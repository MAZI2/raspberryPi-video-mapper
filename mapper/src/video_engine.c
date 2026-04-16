#include "video_engine.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

static void ve_apply_prefer_alpha(VideoEngine* ve)
{
    int prefer_alpha;

    if (!ve)
        return;

    prefer_alpha = ve->prefer_alpha ? 1 : 0;
    ve->cur.prefer_alpha = prefer_alpha;
    ve->nxt.prefer_alpha = prefer_alpha;
    ve->prep.prefer_alpha = prefer_alpha;
}

static void ve_discard_prepared(VideoEngine* ve)
{
    if (!ve->prep_ready)
        return;

    video_stop(&ve->prep);
    video_delete_textures(&ve->prep);
    video_reset(&ve->prep);
    ve->prep_path[0] = '\0';
    ve->prep_loop_on_eos = 0;
    ve->prep_ready = 0;
}

static int ve_promote_prepared(VideoEngine* ve)
{
    if (!ve->prep_ready)
        return 0;

    if (strcmp(ve->prep_path, ve->pending_path) != 0 ||
        ve->prep_loop_on_eos != ve->pending_loop_on_eos) {
        return 0;
    }

    ve->nxt = ve->prep;
    video_reset(&ve->prep);
    ve->prep_path[0] = '\0';
    ve->prep_loop_on_eos = 0;
    ve->prep_ready = 0;

    if (ve->nxt.pipeline) {
        gst_element_seek_simple(ve->nxt.pipeline, GST_FORMAT_TIME,
            (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
        gst_element_set_state(ve->nxt.pipeline, GST_STATE_PLAYING);
        ve->nxt.eos_hit = 0;
        ve->nxt.playing = 1;
    }

    return 1;
}

/* ================= Engine lifecycle ================= */

void ve_init(VideoEngine* ve)
{
    memset(ve, 0, sizeof(*ve));
    ve->xfade_seconds = XFADE_SECONDS;
    ve->pending_loop_on_eos = 1;
    ve->prefer_alpha = 1;
    ve_apply_prefer_alpha(ve);
}

int ve_start_current(VideoEngine* ve, const char* path)
{
    return ve_start_current_opts(ve, path, 1);
}

int ve_start_current_opts(VideoEngine* ve, const char* path, int loop_on_eos)
{
    ve->cur.prefer_alpha = ve->prefer_alpha ? 1 : 0;
    if (!video_start_with_options(&ve->cur, path, loop_on_eos))
        return 0;

    printf("[VE] Current = %s\n", ve->cur.path);
    fflush(stdout);
    return 1;
}

void ve_request_transition(VideoEngine* ve, const char* path)
{
    ve_request_transition_opts(ve, path, 1);
}

void ve_request_transition_opts(VideoEngine* ve, const char* path, int loop_on_eos)
{
    if (!path || !path[0]) return;

    snprintf(ve->pending_path, sizeof(ve->pending_path), "%s", path);
    ve->pending = 1;
    ve->pending_loop_on_eos = loop_on_eos ? 1 : 0;

    printf("[VE] Transition requested -> %s (loop=%d)\n", path, ve->pending_loop_on_eos);
    fflush(stdout);
}

void ve_prepare_transition(VideoEngine* ve, const char* path)
{
    ve_prepare_transition_opts(ve, path, 1);
}

void ve_prepare_transition_opts(VideoEngine* ve, const char* path, int loop_on_eos)
{
    if (!ve || !path || !path[0] || ve->transitioning || ve->pending)
        return;

    if (ve->prep_ready) {
        if (strcmp(ve->prep_path, path) == 0 &&
            ve->prep_loop_on_eos == (loop_on_eos ? 1 : 0)) {
            return;
        }
        ve_discard_prepared(ve);
    }

    ve->prep.prefer_alpha = ve->prefer_alpha ? 1 : 0;
    if (!video_start_with_options(&ve->prep, path, loop_on_eos))
        return;

    if (ve->prep.pipeline) {
        gst_element_set_state(ve->prep.pipeline, GST_STATE_PAUSED);
        gst_element_seek_simple(ve->prep.pipeline, GST_FORMAT_TIME,
            (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
    }

    snprintf(ve->prep_path, sizeof(ve->prep_path), "%s", path);
    ve->prep_loop_on_eos = loop_on_eos ? 1 : 0;
    ve->prep_ready = 1;

    printf("[VE] Prepared next: %s (loop=%d)\n", ve->prep_path, ve->prep_loop_on_eos);
    fflush(stdout);
}

static void ve_try_start_next(VideoEngine* ve)
{
    if (!ve->pending || ve->transitioning)
        return;

    if (ve_promote_prepared(ve)) {
        ve->pending = 0;
        ve->transitioning = 1;
        ve->blend = 0.0f;
        ve->xfade_start_ms = 0;

        printf("[VE] Next started (prepared): %s\n", ve->nxt.path);
        fflush(stdout);
        return;
    }

    ve->nxt.prefer_alpha = ve->prefer_alpha ? 1 : 0;
    if (!video_start_with_options(&ve->nxt, ve->pending_path, ve->pending_loop_on_eos)) {
        ve->pending = 0;
        return;
    }

    ve->pending = 0;
    ve->transitioning = 1;
    ve->blend = 0.0f;
    ve->xfade_start_ms = 0;

    printf("[VE] Next started: %s\n", ve->nxt.path);
    fflush(stdout);
}

void ve_update(VideoEngine* ve)
{
    video_poll_bus(&ve->cur);
    if (ve->transitioning)
        video_poll_bus(&ve->nxt);

    video_update_texture(&ve->cur);
    if (ve->transitioning)
        video_update_texture(&ve->nxt);

    if (ve->transitioning) {
        if (ve->xfade_start_ms == 0 && ve->nxt.tex_inited) {
            ve->xfade_start_ms = SDL_GetTicks();
            ve->blend = 0.0f;
        }

        if (ve->xfade_seconds <= 0.0f && ve->nxt.tex_inited) {
            video_stop(&ve->cur);
            video_delete_textures(&ve->cur);

            ve->cur = ve->nxt;
            video_reset(&ve->nxt);

            ve->transitioning = 0;
            ve->blend = 0.0f;
            ve->xfade_start_ms = 0;

            printf("[VE] Transition complete (hard cut)\n");
            fflush(stdout);
            return;
        }

        if (ve->xfade_start_ms != 0) {
            Uint32 now = SDL_GetTicks();
            float t = (now - ve->xfade_start_ms) / 1000.0f;
            ve->blend = t / ve->xfade_seconds;

            if (ve->blend >= 1.0f) {
                video_stop(&ve->cur);
                video_delete_textures(&ve->cur);

                ve->cur = ve->nxt;
                video_reset(&ve->nxt);

                ve->transitioning = 0;
                ve->blend = 0.0f;
                ve->xfade_start_ms = 0;

                printf("[VE] Transition complete\n");
                fflush(stdout);
            }
        }
    } else {
        ve_try_start_next(ve);
    }
}

void ve_set_xfade_seconds(VideoEngine* ve, float seconds)
{
    if (!ve)
        return;
    ve->xfade_seconds = seconds;
}

void ve_set_prefer_alpha(VideoEngine* ve, int prefer_alpha)
{
    if (!ve)
        return;

    ve->prefer_alpha = prefer_alpha ? 1 : 0;
    ve_apply_prefer_alpha(ve);
}

int ve_current_eos(VideoEngine* ve)
{
    if (!ve || ve->transitioning)
        return 0;
    return video_consume_eos(&ve->cur);
}

/* ================= Rendering helpers ================= */

/*
   Binds textures for ONE video.
   Blending is handled outside via glBlendFunc.
*/
void ve_bind_video_textures(Video* v,
                            GLint uTexY,
                            GLint uTexU,
                            GLint uTexV,
                            GLint uTexA)
{
    if (!v->tex_inited)
        return;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v->texY);
    glUniform1i(uTexY, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, v->texU);
    glUniform1i(uTexU, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, v->texV);
    glUniform1i(uTexV, 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, v->texA);
    glUniform1i(uTexA, 3);
}

/* ================= Shutdown ================= */

void ve_shutdown(VideoEngine* ve)
{
    ve_discard_prepared(ve);
    video_stop(&ve->cur);
    video_stop(&ve->nxt);
    video_delete_textures(&ve->cur);
    video_delete_textures(&ve->nxt);
    memset(ve, 0, sizeof(*ve));
}
