#pragma once

#include <GLES2/gl2.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

typedef struct {
    GstElement* pipeline;
    GstElement* appsink;
    GstBus* bus;

    int width;
    int height;

    // I420 textures
    GLuint texY;
    GLuint texU;
    GLuint texV;

    int tex_inited;

    int video_range; // 1 = video range
    int bt709;       // 1 = BT.709

    // Packed staging buffers used when decoder strides are padded.
    guint8* upload_y;
    guint8* upload_u;
    guint8* upload_v;
    size_t upload_y_size;
    size_t upload_u_size;
    size_t upload_v_size;

    char path[1024];
    int playing;
} Video;

void video_reset(Video* v);
int  video_start(Video* v, const char* filename);
void video_stop(Video* v);
void video_delete_textures(Video* v);
void video_poll_bus(Video* v);
void video_update_texture(Video* v);
