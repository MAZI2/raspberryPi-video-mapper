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
    GLuint texA;

    int tex_inited;

    int video_range; // 1 = video range
    int bt709;       // 1 = BT.709

    // Packed staging buffers used when decoder strides are padded.
    guint8* upload_y;
    guint8* upload_u;
    guint8* upload_v;
    guint8* upload_a;
    size_t upload_y_size;
    size_t upload_u_size;
    size_t upload_v_size;
    size_t upload_a_size;
    int alpha_opaque;
    int prefer_alpha;

    char path[1024];
    int playing;
    int loop_on_eos;
    int eos_hit;
} Video;

void video_reset(Video* v);
int  video_start_with_options(Video* v, const char* filename, int loop_on_eos);
int  video_start(Video* v, const char* filename);
void video_stop(Video* v);
void video_delete_textures(Video* v);
void video_poll_bus(Video* v);
void video_update_texture(Video* v);
int  video_consume_eos(Video* v);
