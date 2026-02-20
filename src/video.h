#pragma once
#include "common.h"

typedef struct {
    GstElement* pipeline;
    GstElement* appsink;
    GstBus* bus;

    int width;
    int height;

    GLuint texY;
    GLuint texUV;
    int tex_inited;

    char path[1024];
    int playing;
} Video;

void video_reset(Video* v);
int  video_start(Video* v, const char* filename);
void video_stop(Video* v);
void video_delete_textures(Video* v);
void video_poll_bus(Video* v);
void video_update_texture(Video* v);
