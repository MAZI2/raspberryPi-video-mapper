#include "video.h"
#include <stdio.h>
#include <string.h>

static void setup_tex_params(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void video_reset(Video* v)
{
    memset(v, 0, sizeof(*v));
}

int video_start(Video* v, const char* filename)
{
    video_reset(v);
    snprintf(v->path, sizeof(v->path), "%s", filename);

    char pipe[2048];
    snprintf(pipe, sizeof(pipe),
    "filesrc location=\"%s\" ! "
    "decodebin ! "
    "videoconvert ! "
    "video/x-raw,format=I420 ! "
    "appsink name=sink sync=false max-buffers=1 drop=true",
    filename
);

    GError* err = NULL;
    v->pipeline = gst_parse_launch(pipe, &err);
    if (!v->pipeline) {
        fprintf(stderr, "gst_parse_launch failed: %s\n", err ? err->message : "unknown");
        if (err) g_error_free(err);
        fflush(stderr);
        return 0;
    }

    v->appsink = gst_bin_get_by_name(GST_BIN(v->pipeline), "sink");
    if (!v->appsink) {
        fprintf(stderr, "appsink not found\n");
        fflush(stderr);
        return 0;
    }

    // Force raw I420 at the sink.
    GstCaps* want = gst_caps_from_string("video/x-raw,format=I420");
    gst_app_sink_set_caps((GstAppSink*)v->appsink, want);
    gst_caps_unref(want);

    gst_app_sink_set_emit_signals((GstAppSink*)v->appsink, FALSE);
    gst_app_sink_set_drop((GstAppSink*)v->appsink, TRUE);
    gst_app_sink_set_max_buffers((GstAppSink*)v->appsink, 1);

    v->bus = gst_element_get_bus(v->pipeline);

    if (gst_element_set_state(v->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "Failed to set PLAYING for: %s\n", filename);
        fflush(stderr);
        return 0;
    }

    fprintf(stderr, "Video started (decodebin -> appsink I420): %s\n", filename);
    fflush(stderr);
    v->playing = 1;
    return 1;
}

void video_stop(Video* v)
{
    if (!v) return;

    if (v->pipeline)
        gst_element_set_state(v->pipeline, GST_STATE_NULL);

    if (v->bus)      gst_object_unref(v->bus);
    if (v->appsink)  gst_object_unref(v->appsink);
    if (v->pipeline) gst_object_unref(v->pipeline);

    v->pipeline = NULL;
    v->appsink  = NULL;
    v->bus      = NULL;
    v->playing  = 0;
}

void video_delete_textures(Video* v)
{
    if (!v) return;
    if (v->tex_inited) {
        glDeleteTextures(1, &v->texY);
        glDeleteTextures(1, &v->texU);
        glDeleteTextures(1, &v->texV);
        v->texY = v->texU = v->texV = 0;
        v->tex_inited = 0;
    }
}

void video_poll_bus(Video* v)
{
    if (!v || !v->bus) return;

    while (1) {
        GstMessage* msg = gst_bus_pop(v->bus);
        if (!msg) break;

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = NULL;
            gchar* dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            fprintf(stderr, "GST ERROR (%s): %s\n", v->path, err ? err->message : "unknown");
            if (dbg) fprintf(stderr, "GST DEBUG: %s\n", dbg);
            if (dbg) g_free(dbg);
            if (err) g_error_free(err);
            fflush(stderr);
            break;
        }
        case GST_MESSAGE_EOS:
            gst_element_seek_simple(v->pipeline, GST_FORMAT_TIME,
                (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
            break;
        default:
            break;
        }

        gst_message_unref(msg);
    }
}

static void upload_i420(Video* v, const GstVideoInfo* info, GstBuffer* buffer)
{
    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, info, buffer, GST_MAP_READ))
        return;

    int w = GST_VIDEO_INFO_WIDTH(info);
    int h = GST_VIDEO_INFO_HEIGHT(info);

    const guint8* dataY = (const guint8*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
    const guint8* dataU = (const guint8*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 1);
    const guint8* dataV = (const guint8*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 2);

    int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    int strideU = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
    int strideV = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (!v->tex_inited || v->width != w || v->height != h) {
        if (v->tex_inited) {
            glDeleteTextures(1, &v->texY);
            glDeleteTextures(1, &v->texU);
            glDeleteTextures(1, &v->texV);
            v->tex_inited = 0;
        }

        v->width = w;
        v->height = h;

        glGenTextures(1, &v->texY);
        glBindTexture(GL_TEXTURE_2D, v->texY);
        setup_tex_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

        glGenTextures(1, &v->texU);
        glBindTexture(GL_TEXTURE_2D, v->texU);
        setup_tex_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w/2, h/2, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

        glGenTextures(1, &v->texV);
        glBindTexture(GL_TEXTURE_2D, v->texV);
        setup_tex_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w/2, h/2, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

        v->tex_inited = 1;

        fprintf(stderr, "Textures init (I420) %dx%d strideY=%d strideU=%d strideV=%d\n",
                w, h, strideY, strideU, strideV);
        fflush(stderr);
    }

    glBindTexture(GL_TEXTURE_2D, v->texY);
    for (int y = 0; y < h; y++) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE,
                        dataY + y * strideY);
    }

    glBindTexture(GL_TEXTURE_2D, v->texU);
    for (int y = 0; y < h/2; y++) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w/2, 1,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE,
                        dataU + y * strideU);
    }

    glBindTexture(GL_TEXTURE_2D, v->texV);
    for (int y = 0; y < h/2; y++) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w/2, 1,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE,
                        dataV + y * strideV);
    }

    gst_video_frame_unmap(&frame);
}

void video_update_texture(Video* v)
{
    static int warned_non_i420 = 0;

    if (!v || !v->appsink) return;

    // 5ms timeout (still low-latency, but avoids “always NULL” on some decoders)
    GstSample* sample = gst_app_sink_try_pull_sample((GstAppSink*)v->appsink, 5000000);
    if (!sample) return;

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);

    GstVideoInfo info;
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps))
        goto out;

    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_I420) {
        if (!warned_non_i420) {
            fprintf(stderr, "Unexpected sink format: %s (expected I420)\n",
                    gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&info)));
            fflush(stderr);
            warned_non_i420 = 1;
        }
        goto out;
    }

    GstVideoColorimetry c = info.colorimetry;
    v->video_range = (c.range == GST_VIDEO_COLOR_RANGE_16_235);
    v->bt709       = (c.matrix == GST_VIDEO_COLOR_MATRIX_BT709);

    upload_i420(v, &info, buffer);

out:
    gst_sample_unref(sample);
}
