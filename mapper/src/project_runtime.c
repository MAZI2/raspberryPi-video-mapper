#include "project_runtime.h"

#include "playlist.h"
#include "project_support.h"

static void project_runtime_set_name(ProjectRuntime* runtime)
{
    snprintf(runtime->project_name, sizeof(runtime->project_name), "%s", "core");
}

void project_runtime_init(ProjectRuntime* runtime)
{
    memset(runtime, 0, sizeof(*runtime));
    project_runtime_set_name(runtime);
    runtime->initial_loop = 1;
    runtime->foreground_prefer_alpha = 1;
    runtime->background_prefer_alpha = 0;
    runtime->foreground_xfade_seconds = -1.0f;
}

int project_runtime_prepare(ProjectRuntime* runtime, int argc, char** argv)
{
    Playlist playlist;

    memset(&playlist, 0, sizeof(playlist));

    (void)mapper_detect_media_root(runtime->media_root, sizeof(runtime->media_root));

    if (runtime->media_root[0]) {
        printf("[MEDIA] root=%s\n", runtime->media_root);
    } else {
        printf("[MEDIA] root not found\n");
    }

    if (argc >= 2 && argv[1] && argv[1][0]) {
        snprintf(runtime->initial_video, sizeof(runtime->initial_video), "%s", argv[1]);
        return 1;
    }

    if (!runtime->media_root[0]) {
        fprintf(stderr, "[CORE] No initial video path provided and no media root found\n");
        return 0;
    }

    if (!playlist_load_from_dir(&playlist, runtime->media_root)) {
        fprintf(stderr, "[CORE] No videos found in media root: %s\n", runtime->media_root);
        return 0;
    }

    if (!playlist_first(&playlist)) {
        playlist_free(&playlist);
        fprintf(stderr, "[CORE] Playlist is empty\n");
        return 0;
    }

    snprintf(runtime->initial_video, sizeof(runtime->initial_video), "%s", playlist_first(&playlist));
    playlist_free(&playlist);
    return 1;
}

void project_runtime_after_start(ProjectRuntime* runtime, VideoEngine* ve_fg, VideoEngine* ve_bg)
{
    (void)runtime;
    (void)ve_fg;
    (void)ve_bg;
}

void project_runtime_on_action(ProjectRuntime* runtime, VideoEngine* ve_fg, VideoEngine* ve_bg)
{
    (void)runtime;
    (void)ve_fg;
    (void)ve_bg;
    printf("[CORE] No project action configured\n");
}

void project_runtime_update(ProjectRuntime* runtime, VideoEngine* ve_fg, VideoEngine* ve_bg)
{
    (void)runtime;
    (void)ve_fg;
    (void)ve_bg;
}

void project_runtime_maintenance(ProjectRuntime* runtime, VideoEngine* ve_fg, VideoEngine* ve_bg)
{
    (void)runtime;
    (void)ve_fg;
    (void)ve_bg;
}

void project_runtime_draw_overlay(ProjectRuntime* runtime, int viewport_w, int viewport_h)
{
    (void)runtime;
    (void)viewport_w;
    (void)viewport_h;
}

void project_runtime_shutdown(ProjectRuntime* runtime)
{
    (void)runtime;
}
