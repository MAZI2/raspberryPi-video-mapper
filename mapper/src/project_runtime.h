#pragma once

#include "video_engine.h"

#define PROJECT_RUNTIME_NAME_MAX 64
#define PROJECT_RUNTIME_PATH_MAX 1024

typedef struct {
    char project_name[PROJECT_RUNTIME_NAME_MAX];
    char media_root[PROJECT_RUNTIME_PATH_MAX];
    char initial_video[PROJECT_RUNTIME_PATH_MAX];
    char background_video[PROJECT_RUNTIME_PATH_MAX];
    int initial_loop;
    int use_background_layer;
    int foreground_prefer_alpha;
    int background_prefer_alpha;
    float foreground_xfade_seconds;
    void* user;
} ProjectRuntime;

void project_runtime_init(ProjectRuntime* runtime);
int project_runtime_prepare(ProjectRuntime* runtime, int argc, char** argv);
void project_runtime_after_start(ProjectRuntime* runtime, VideoEngine* ve_fg, VideoEngine* ve_bg);
void project_runtime_on_action(ProjectRuntime* runtime, VideoEngine* ve_fg, VideoEngine* ve_bg);
void project_runtime_update(ProjectRuntime* runtime, VideoEngine* ve_fg, VideoEngine* ve_bg);
void project_runtime_maintenance(ProjectRuntime* runtime, VideoEngine* ve_fg, VideoEngine* ve_bg);
void project_runtime_shutdown(ProjectRuntime* runtime);
