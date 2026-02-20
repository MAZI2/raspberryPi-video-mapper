#include "video.h"

void video_reset(Video* v)
{
    memset(v, 0, sizeof(*v));
}

int video_start(Video* v, const char* filename)
{
    video_reset(v);
    snprintf(v->path, sizeof(v->path), "%s", filename);

    // NV12 path: no videoconvert, no RGB
    // sync=false keeps latency low and makes transitions feel immediate
    char pipe[2048];
    snprintf(pipe, sizeof(pipe),
        "filesrc location=\"%s\" ! qtdemux ! h264parse ! v4l2h264dec "
        "! video/x-raw,format=NV12 "
        "! appsink name=sink sync=false max-buffers=1 drop=true",
        filename
    );

    GError* err = NULL;
    v->pipeline = gst_parse_launch(pipe, &err);
    if (!v->pipeline) {
        printf("gst_parse_launch failed: %s\n", err ? err->message : "unknown");
        if (err) g_error_free(err);
        return 0;
    }

    v->appsink = gst_bin_get_by_name(GST_BIN(v->pipeline), "sink");
    if (!v->appsink) {
        printf("appsink not found in pipeline\n");
        return 0;
    }

    gst_app_sink_set_emit_signals((GstAppSink*)v->appsink, FALSE);
    gst_app_sink_set_drop((GstAppSink*)v->appsink, TRUE);
    gst_app_sink_set_max_buffers((GstAppSink*)v->appsink, 1);

    v->bus = gst_element_get_bus(v->pipeline);

    GstStateChangeReturn r = gst_element_set_state(v->pipeline, GST_STATE_PLAYING);
    if (r == GST_STATE_CHANGE_FAILURE) {
        printf("Failed to set pipeline PLAYING for: %s\n", filename);
        return 0;
    }

    v->playing = 1;
    printf("Video started (NV12): %s\n", filename);
    fflush(stdout);
    return 1;
}

void video_stop(Video* v)
{
    if (!v) return;

    if (v->pipeline) {
        gst_element_set_state(v->pipeline, GST_STATE_NULL);
    }
    if (v->bus) gst_object_unref(v->bus);
    if (v->appsink) gst_object_unref(v->appsink);
    if (v->pipeline) gst_object_unref(v->pipeline);

    // textures deleted separately (needs GL context)
    v->pipeline = NULL;
    v->appsink = NULL;
    v->bus = NULL;
    v->playing = 0;
}

void video_delete_textures(Video* v)
{
    if (!v) return;
    if (v->tex_inited) {
        glDeleteTextures(1, &v->texY);
        glDeleteTextures(1, &v->texUV);
        v->tex_inited = 0;
        v->texY = 0;
        v->texUV = 0;
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
                printf("GST ERROR (%s): %s\n", v->path, err ? err->message : "unknown");
                if (dbg) printf("GST DEBUG: %s\n", dbg);
                if (err) g_error_free(err);
                if (dbg) g_free(dbg);
                fflush(stdout);
                break;
            }
            case GST_MESSAGE_EOS: {
                // Loop: seek to start
                gst_element_seek_simple(v->pipeline, GST_FORMAT_TIME,
                    (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
                break;
            }
            default:
                break;
        }
        gst_message_unref(msg);
    }
}

static void upload_nv12_frame(Video* v, const GstVideoInfo* info, GstBuffer* buffer)
{
    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, info, buffer, GST_MAP_READ)) return;

    int w = GST_VIDEO_INFO_WIDTH(info);
    int h = GST_VIDEO_INFO_HEIGHT(info);

    int strideY  = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    int strideUV = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);

    const guint8* dataY  = (const guint8*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
    const guint8* dataUV = (const guint8*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 1);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (!v->tex_inited || v->width != w || v->height != h) {
        v->width = w;
        v->height = h;

        if (!v->tex_inited) {
            glGenTextures(1, &v->texY);
            glGenTextures(1, &v->texUV);
        }

        glBindTexture(GL_TEXTURE_2D, v->texY);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glBindTexture(GL_TEXTURE_2D, v->texUV);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, w/2, h/2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        v->tex_inited = 1;
        printf("Textures init (NV12) %dx%d strideY=%d strideUV=%d for %s\n", w, h, strideY, strideUV, v->path);
        fflush(stdout);
    }

    // Upload Y plane
    glBindTexture(GL_TEXTURE_2D, v->texY);
    if (strideY == w) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, dataY);
    } else {
        const unsigned char* row = (const unsigned char*)dataY;
        for (int y = 0; y < h; y++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, row);
            row += strideY;
        }
    }

    // Upload UV plane (half res, 2 bytes per texel)
    glBindTexture(GL_TEXTURE_2D, v->texUV);
    int uvw = w / 2;
    int uvh = h / 2;
    int tightUV = uvw * 2;
    if (strideUV == tightUV) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvw, uvh, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, dataUV);
    } else {
        const unsigned char* row = (const unsigned char*)dataUV;
        for (int y = 0; y < uvh; y++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, uvw, 1, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, row);
            row += strideUV;
        }
    }

    gst_video_frame_unmap(&frame);
}

void video_update_texture(Video* v)
{
    if (!v || !v->appsink) return;

    // non-blocking pull
    GstSample* sample = gst_app_sink_try_pull_sample((GstAppSink*)v->appsink, 0);
    if (!sample) return;

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);

    GstVideoInfo info;
    if (caps && gst_video_info_from_caps(&info, caps) && buffer) {
        upload_nv12_frame(v, &info, buffer);
    }

    gst_sample_unref(sample);
}
