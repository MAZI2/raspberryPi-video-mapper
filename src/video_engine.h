#pragma once
#include "common.h"
#include "video.h"

typedef struct {
    Video cur;
    Video nxt;
    int transitioning;

    float blend;               // 0..1
    Uint32 xfade_start_ms;
    float xfade_seconds;

    char pending_path[1024];   // requested next
    int pending;               // request queued
} VideoEngine;

void ve_init(VideoEngine* ve);
int  ve_start_current(VideoEngine* ve, const char* path);
void ve_request_transition(VideoEngine* ve, const char* path);
void ve_update(VideoEngine* ve);
void ve_bind_textures_and_blend(VideoEngine* ve, GLint uBlend);
void ve_shutdown(VideoEngine* ve);
