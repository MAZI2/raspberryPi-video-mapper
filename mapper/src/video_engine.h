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
    int pending_loop_on_eos;

    // Async prepare path to avoid blocking render loop on gst_parse_launch/set_state.
    SDL_Thread* prep_thread;
    SDL_mutex* prep_mutex;
    Video prep_video;
    char prep_path[1024];
    int prep_loop_on_eos;
    int prep_inflight;
    int prep_ready;
    int prep_failed;
} VideoEngine;

void ve_init(VideoEngine* ve);
int  ve_start_current(VideoEngine* ve, const char* path);
int  ve_start_current_opts(VideoEngine* ve, const char* path, int loop_on_eos);
void ve_request_transition(VideoEngine* ve, const char* path);
void ve_request_transition_opts(VideoEngine* ve, const char* path, int loop_on_eos);
void ve_prefetch_transition(VideoEngine* ve, const char* path);
void ve_prefetch_transition_opts(VideoEngine* ve, const char* path, int loop_on_eos);
void ve_update(VideoEngine* ve);
void ve_shutdown(VideoEngine* ve);
void ve_set_xfade_seconds(VideoEngine* ve, float seconds);
int  ve_current_eos(VideoEngine* ve);
void ve_bind_video_textures(Video* v,
                            GLint uTexY,
                            GLint uTexU,
                            GLint uTexV,
                            GLint uTexA);
