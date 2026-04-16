#include "video_engine.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

static void ve_discard_prepared_locked(VideoEngine* ve)
{
    if (!ve->prep_ready)
        return;
    video_stop(&ve->prep_video);
    video_reset(&ve->prep_video);
    ve->prep_ready = 0;
}

static int ve_prepare_thread_fn(void* userdata)
{
    VideoEngine* ve = (VideoEngine*)userdata;
    char path[1024];
    int loop_on_eos = 1;
    Video tmp;

    SDL_LockMutex(ve->prep_mutex);
    snprintf(path, sizeof(path), "%s", ve->prep_path);
    loop_on_eos = ve->prep_loop_on_eos;
    SDL_UnlockMutex(ve->prep_mutex);

    int ok = video_start_with_options(&tmp, path, loop_on_eos);

    SDL_LockMutex(ve->prep_mutex);
    ve->prep_inflight = 0;
    if (ok) {
        ve_discard_prepared_locked(ve);
        ve->prep_video = tmp;
        ve->prep_ready = 1;
    } else {
        ve->prep_failed = 1;
    }
    SDL_UnlockMutex(ve->prep_mutex);
    return 0;
}

static int ve_start_prepare_async(VideoEngine* ve, const char* path, int loop_on_eos)
{
    if (!ve->prep_mutex)
        return 0;

    SDL_LockMutex(ve->prep_mutex);
    snprintf(ve->prep_path, sizeof(ve->prep_path), "%s", path);
    ve->prep_loop_on_eos = loop_on_eos ? 1 : 0;
    ve->prep_failed = 0;
    ve->prep_inflight = 1;
    SDL_UnlockMutex(ve->prep_mutex);

    ve->prep_thread = SDL_CreateThread(ve_prepare_thread_fn, "ve_prepare", ve);
    if (!ve->prep_thread) {
        SDL_LockMutex(ve->prep_mutex);
        ve->prep_inflight = 0;
        ve->prep_failed = 1;
        SDL_UnlockMutex(ve->prep_mutex);
        return 0;
    }
    return 1;
}

/* ================= Engine lifecycle ================= */

void ve_init(VideoEngine* ve)
{
    memset(ve, 0, sizeof(*ve));
    ve->xfade_seconds = XFADE_SECONDS;
    ve->pending_loop_on_eos = 1;
    ve->prep_mutex = SDL_CreateMutex();
}

int ve_start_current(VideoEngine* ve, const char* path)
{
    return ve_start_current_opts(ve, path, 1);
}

int ve_start_current_opts(VideoEngine* ve, const char* path, int loop_on_eos)
{
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

static void ve_try_start_next(VideoEngine* ve)
{
    int prep_inflight = 0;
    int prep_match = 0;
    int prep_ready = 0;

    if (!ve->pending || ve->transitioning)
        return;

    if (ve->prep_mutex) {
        SDL_LockMutex(ve->prep_mutex);
        prep_inflight = ve->prep_inflight;
        SDL_UnlockMutex(ve->prep_mutex);
    }

    if (ve->prep_thread && !prep_inflight) {
        SDL_WaitThread(ve->prep_thread, NULL);
        ve->prep_thread = NULL;
    }

    if (ve->prep_mutex) {
        SDL_LockMutex(ve->prep_mutex);
        prep_ready = ve->prep_ready;
        prep_match = prep_ready &&
                     (strcmp(ve->prep_path, ve->pending_path) == 0) &&
                     (ve->prep_loop_on_eos == ve->pending_loop_on_eos);

        if (prep_match) {
            ve->nxt = ve->prep_video;
            video_reset(&ve->prep_video);
            ve->prep_ready = 0;
        }
        if (prep_ready && !prep_match)
            ve_discard_prepared_locked(ve);

        prep_inflight = ve->prep_inflight;
        SDL_UnlockMutex(ve->prep_mutex);
    }

    if (prep_match) {
        ve->pending = 0;
        ve->transitioning = 1;
        ve->blend = 0.0f;
        ve->xfade_start_ms = 0;

        printf("[VE] Next started (async): %s\n", ve->nxt.path);
        fflush(stdout);
        return;
    }

    if (prep_inflight)
        return;

    if (!ve_start_prepare_async(ve, ve->pending_path, ve->pending_loop_on_eos)) {
        if (!video_start_with_options(&ve->nxt, ve->pending_path, ve->pending_loop_on_eos)) {
            ve->pending = 0;
            return;
        }

        ve->pending = 0;
        ve->transitioning = 1;
        ve->blend = 0.0f;
        ve->xfade_start_ms = 0;

        printf("[VE] Next started (sync fallback): %s\n", ve->nxt.path);
        fflush(stdout);
    }
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
    if (ve->prep_thread) {
        SDL_WaitThread(ve->prep_thread, NULL);
        ve->prep_thread = NULL;
    }
    if (ve->prep_mutex) {
        SDL_LockMutex(ve->prep_mutex);
        ve_discard_prepared_locked(ve);
        SDL_UnlockMutex(ve->prep_mutex);
        SDL_DestroyMutex(ve->prep_mutex);
        ve->prep_mutex = NULL;
    }

    video_stop(&ve->cur);
    video_stop(&ve->nxt);
    video_delete_textures(&ve->cur);
    video_delete_textures(&ve->nxt);
    memset(ve, 0, sizeof(*ve));
}
