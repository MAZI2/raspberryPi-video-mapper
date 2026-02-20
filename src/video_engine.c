#include "video_engine.h"

static void ve_try_start_next(VideoEngine* ve)
{
    if (!ve->pending) return;
    if (ve->transitioning) return;

    // Start next pipeline
    if (!video_start(&ve->nxt, ve->pending_path)) {
        printf("[VE] Failed to start next: %s\n", ve->pending_path);
        ve->pending = 0;
        fflush(stdout);
        return;
    }

    ve->pending = 0;
    ve->transitioning = 1;
    ve->blend = 0.0f;
    ve->xfade_start_ms = 0; // start only after nxt has first texture

    printf("[VE] Next started: %s (waiting for first frame)\n", ve->nxt.path);
    fflush(stdout);
}

void ve_init(VideoEngine* ve)
{
    memset(ve, 0, sizeof(*ve));
    ve->xfade_seconds = XFADE_SECONDS;
}

int ve_start_current(VideoEngine* ve, const char* path)
{
    if (!video_start(&ve->cur, path)) return 0;
    printf("[VE] Current = %s\n", ve->cur.path);
    fflush(stdout);
    return 1;
}

void ve_request_transition(VideoEngine* ve, const char* path)
{
    if (!path || !path[0]) return;

    // If we’re already transitioning, just replace the pending request.
    snprintf(ve->pending_path, sizeof(ve->pending_path), "%s", path);
    ve->pending = 1;

    printf("[VE] Transition requested -> %s\n", path);
    fflush(stdout);
}

void ve_update(VideoEngine* ve)
{
    // Poll bus for both
    video_poll_bus(&ve->cur);
    if (ve->transitioning) video_poll_bus(&ve->nxt);

    // Update textures
    video_update_texture(&ve->cur);
    if (ve->transitioning) {
        video_update_texture(&ve->nxt);

        // Start fade clock only once nxt has a texture (first frame ready)
        if (ve->xfade_start_ms == 0 && ve->nxt.tex_inited) {
            ve->xfade_start_ms = SDL_GetTicks();
            ve->blend = 0.0f;
            printf("[VE] Crossfade start (%.2fs)\n", ve->xfade_seconds);
            fflush(stdout);
        }

        if (ve->xfade_start_ms != 0) {
            Uint32 now = SDL_GetTicks();
            float t = (now - ve->xfade_start_ms) / 1000.0f;
            float b = t / ve->xfade_seconds;
            if (b < 0.0f) b = 0.0f;
            if (b > 1.0f) b = 1.0f;
            ve->blend = b;

            // Finish transition
            if (ve->blend >= 1.0f) {
                printf("[VE] Crossfade complete. Current <- %s\n", ve->nxt.path);
                fflush(stdout);

                // Stop old current pipeline (cur), swap structs, clear next
                video_stop(&ve->cur);
                video_delete_textures(&ve->cur);

                ve->cur = ve->nxt;
                video_reset(&ve->nxt);

                ve->transitioning = 0;
                ve->blend = 0.0f;
                ve->xfade_start_ms = 0;
            }
        }
    } else {
        // if not transitioning and we have a queued request, start it
        ve_try_start_next(ve);
    }
}

void ve_bind_textures_and_blend(VideoEngine* ve, GLint uBlend)
{
    // Bind A (current) to units 0/1
    if (ve->cur.tex_inited) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ve->cur.texY);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ve->cur.texUV);
    } else {
        // if no frame yet, bind 0
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Bind B (next) to units 2/3
    if (ve->transitioning && ve->nxt.tex_inited) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ve->nxt.texY);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, ve->nxt.texUV);

        glUniform1f(uBlend, ve->blend);
    } else {
        // Not transitioning: bind B = A and blend=0 (cheap, no branching in shader)
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ve->cur.tex_inited ? ve->cur.texY : 0);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, ve->cur.tex_inited ? ve->cur.texUV : 0);

        glUniform1f(uBlend, 0.0f);
    }
}

void ve_shutdown(VideoEngine* ve)
{
    video_stop(&ve->cur);
    video_stop(&ve->nxt);
    // textures deleted in main after GL context exists; we do both here too:
    video_delete_textures(&ve->cur);
    video_delete_textures(&ve->nxt);
    memset(ve, 0, sizeof(*ve));
}
