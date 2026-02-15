// ============================================================
// mapping_video_keystone.c
//
// NV12 (no videoconvert) + GLES2 keystone mesh + GPIO edit UI
// PLUS: Random video switching from ~/videos with smooth crossfade
//
// Key behavior (no new GPIOs required):
//   - BTN3 (GPIO27): toggle EDIT mode
//   - When EDIT ON:
//       BTN2 toggles SELECT <-> MOVE
//       BTN1 cycles corners (SELECT mode only)
//       Arrows move corner (MOVE mode only)
//   - When EDIT OFF:
//       BTN1 (GPIO17) triggers RANDOM VIDEO crossfade from ~/videos
//
// Run (log to file):
//   SDL_VIDEODRIVER=kmsdrm ./mapping_video_keystone /home/pi/videos/any.mp4 > log.txt 2>&1
//
// Compile:
//   gcc mapping_video_keystone.c -o mapping_video_keystone \
//     `pkg-config --cflags --libs sdl2 gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 libgpiod` \
//     -lGLESv2
// ============================================================

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <gpiod.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <dirent.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

/* ================= CONFIG ================= */

#define GRID_X 16
#define GRID_Y 9

// MiniMAD board GPIOs (BCM)
#define GPIO_BTN1   17  // cycle corner (EDIT+SELECT), random video (EDIT OFF)
#define GPIO_BTN2   18  // toggle SELECT<->MOVE (EDIT ON)
#define GPIO_BTN3   27  // toggle EDIT mode
#define GPIO_UP     24
#define GPIO_DOWN   22
#define GPIO_LEFT   25
#define GPIO_RIGHT  23

volatile int keepRunning = 1;

// Debounce (ms)
#define DEBOUNCE_MS 120

// Crossfade duration (seconds)
#define XFADE_SECONDS 0.60f

// Corner order for homography: BL, BR, TR, TL
typedef enum { C_BL=0, C_BR=1, C_TR=2, C_TL=3 } CornerSq;

// UI cycle order: TL, TR, BL, BR
static const int UI_TO_SQ_CORNER[4] = { C_TL, C_TR, C_BL, C_BR };

static const char* corner_name_ui(int uiIdx) {
    switch (uiIdx) { case 0: return "TL"; case 1: return "TR"; case 2: return "BL"; case 3: return "BR"; default: return "?"; }
}

static void handle_sigint(int sig) { (void)sig; keepRunning = 0; }

/* ================= SHADERS ================= */

// Vertex shader: pass position + texcoord
static const char* vertex_shader_src =
    "attribute vec2 aPos;"
    "attribute vec2 aTex;"
    "varying vec2 vTex;"
    "void main(){ vTex=aTex; gl_Position=vec4(aPos,0.0,1.0); }";

/*
  Fragment shader:
    - Decode NV12 for stream A: (uTexY_A, uTexUV_A)
    - Decode NV12 for stream B: (uTexY_B, uTexUV_B)
    - Blend with uBlend (0..1): output = mix(A, B, uBlend)
*/
static const char* fragment_shader_src =
    "precision mediump float;"
    "varying vec2 vTex;"
    "uniform sampler2D uTexY_A;"
    "uniform sampler2D uTexUV_A;"
    "uniform sampler2D uTexY_B;"
    "uniform sampler2D uTexUV_B;"
    "uniform float uBlend;"
    ""
    "vec3 nv12_to_rgb(sampler2D tY, sampler2D tUV, vec2 tc){"
    "  float y  = texture2D(tY,  tc).r;"
    "  vec4  uv = texture2D(tUV, tc);"
    "  float u = uv.r - 0.5;"
    "  float v = uv.a - 0.5;"
    "  float Y = 1.1643 * (y - 0.0625);"   // video range
    "  float R = Y + 1.7927 * v;"
    "  float G = Y - 0.2132 * u - 0.5329 * v;"
    "  float B = Y + 2.1124 * u;"
    "  return vec3(R,G,B);"
    "}"
    ""
    "void main(){"
    "  vec2 tc = vec2(vTex.x, 1.0 - vTex.y);"
    "  vec3 A = nv12_to_rgb(uTexY_A, uTexUV_A, tc);"
    "  vec3 B = nv12_to_rgb(uTexY_B, uTexUV_B, tc);"
    "  vec3 C = mix(A, B, clamp(uBlend, 0.0, 1.0));"
    "  gl_FragColor = vec4(C, 1.0);"
    "}";

static GLuint compile_shader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char buffer[1024];
        glGetShaderInfoLog(shader, sizeof(buffer), NULL, buffer);
        printf("Shader compile error: %s\n", buffer);
        fflush(stdout);
    }
    return shader;
}

/* ================= HOMOGRAPHY / MESH ================= */

/*
  Compute homography H mapping unit square (u,v) to quad (x,y).
  Corner mapping order:
    (0,0)->BL, (1,0)->BR, (1,1)->TR, (0,1)->TL
  H row-major:
    [ a b c
      d e f
      g h 1 ]
*/
static void homography_square_to_quad(
    float x0, float y0,
    float x1, float y1,
    float x2, float y2,
    float x3, float y3,
    float H[9]
) {
    float dx1 = x1 - x2;
    float dx2 = x3 - x2;
    float dx3 = x0 - x1 + x2 - x3;

    float dy1 = y1 - y2;
    float dy2 = y3 - y2;
    float dy3 = y0 - y1 + y2 - y3;

    float a,b,c,d,e,f,g,h;

    if (dx3 == 0.0f && dy3 == 0.0f) {
        a = x1 - x0;  b = x3 - x0;  c = x0;
        d = y1 - y0;  e = y3 - y0;  f = y0;
        g = 0.0f;     h = 0.0f;
    } else {
        float det = dx1 * dy2 - dx2 * dy1;
        if (det == 0.0f) det = 1e-6f;

        g = (dx3 * dy2 - dx2 * dy3) / det;
        h = (dx1 * dy3 - dx3 * dy1) / det;

        a = x1 - x0 + g * x1;
        b = x3 - x0 + h * x3;
        c = x0;

        d = y1 - y0 + g * y1;
        e = y3 - y0 + h * y3;
        f = y0;
    }

    H[0]=a; H[1]=b; H[2]=c;
    H[3]=d; H[4]=e; H[5]=f;
    H[6]=g; H[7]=h; H[8]=1.0f;
}

static void apply_homography(const float H[9], float u, float v, float* x, float* y)
{
    float a=H[0], b=H[1], c=H[2];
    float d=H[3], e=H[4], f=H[5];
    float g=H[6], h=H[7];

    float denom = g*u + h*v + 1.0f;
    if (denom == 0.0f) denom = 1e-6f;

    *x = (a*u + b*v + c) / denom;
    *y = (d*u + e*v + f) / denom;
}

/* ================= APP STATE ================= */

typedef struct {
    int edit_mode;
    int select_mode;
    int selected_ui;   // 0..3 (TL,TR,BL,BR)
    float moveSpeed;

    float corners[4][2]; // BL,BR,TR,TL
    float H[9];

    float* vertices;
    int numVerts;
    int numIndices;
    GLuint vbo;

    Uint32 last_btn1, last_btn2, last_btn3;
    Uint32 last_up, last_down, last_left, last_right;
} AppState;

static void print_status(AppState* s)
{
    int sq = UI_TO_SQ_CORNER[s->selected_ui];
    float cx = s->corners[sq][0];
    float cy = s->corners[sq][1];

    printf("\n====================\n");
    printf("EDIT MODE : %s\n", s->edit_mode ? "ON" : "OFF");
    printf("SUBMODE   : %s\n", s->select_mode ? "SELECT" : "MOVE");
    printf("SELECTED  : %s  (x=%.3f, y=%.3f)\n", corner_name_ui(s->selected_ui), cx, cy);
    printf("CORNERS   : TL(%.3f,%.3f) TR(%.3f,%.3f) BL(%.3f,%.3f) BR(%.3f,%.3f)\n",
           s->corners[C_TL][0], s->corners[C_TL][1],
           s->corners[C_TR][0], s->corners[C_TR][1],
           s->corners[C_BL][0], s->corners[C_BL][1],
           s->corners[C_BR][0], s->corners[C_BR][1]);
    printf("====================\n");
    fflush(stdout);
}

static void rebuild_mesh_from_corners(AppState* s)
{
    homography_square_to_quad(
        s->corners[C_BL][0], s->corners[C_BL][1],
        s->corners[C_BR][0], s->corners[C_BR][1],
        s->corners[C_TR][0], s->corners[C_TR][1],
        s->corners[C_TL][0], s->corners[C_TL][1],
        s->H
    );

    int v = 0;
    for (int y = 0; y < GRID_Y; y++) {
        for (int x = 0; x < GRID_X; x++) {
            float fx = (float)x / (GRID_X - 1);
            float fy = (float)y / (GRID_Y - 1);

            float px, py;
            apply_homography(s->H, fx, fy, &px, &py);

            s->vertices[v++] = px;
            s->vertices[v++] = py;
            s->vertices[v++] = fx;
            s->vertices[v++] = fy;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, s->numVerts * 4 * sizeof(float), s->vertices);
}

static int debounce_ok(Uint32* last_ms)
{
    Uint32 now = SDL_GetTicks();
    if (now - *last_ms < DEBOUNCE_MS) return 0;
    *last_ms = now;
    return 1;
}

/* ================= GPIO HELPERS ================= */

static struct gpiod_line* request_line_events(struct gpiod_chip* chip, unsigned int offset, const char* consumer)
{
    struct gpiod_line* line = gpiod_chip_get_line(chip, offset);
    if (!line) { printf("gpiod: failed get GPIO %u\n", offset); return NULL; }
    if (gpiod_line_request_both_edges_events(line, consumer) < 0) {
        printf("gpiod: failed request events GPIO %u\n", offset);
        return NULL;
    }
    return line;
}

// PRESS on your board = RISING edge
static void process_line_events(struct gpiod_line* line, void (*on_press)(void*), void* user)
{
    if (!line) return;

    struct timespec timeout = {0, 0};
    while (1) {
        int ready = gpiod_line_event_wait(line, &timeout);
        if (ready <= 0) break;

        struct gpiod_line_event ev;
        if (gpiod_line_event_read(line, &ev) < 0) break;

        if (ev.event_type == GPIOD_LINE_EVENT_RISING_EDGE) {
            if (on_press) on_press(user);
        }
    }
}

/* ================= PLAYLIST (~/videos) ================= */

typedef struct {
    char** items;
    int count;
    int cap;
} Playlist;

static int ends_with_ci(const char* s, const char* ext)
{
    size_t ls = strlen(s), le = strlen(ext);
    if (ls < le) return 0;
    const char* a = s + (ls - le);
    for (size_t i = 0; i < le; i++) {
        char ca = a[i], ce = ext[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (ce >= 'A' && ce <= 'Z') ce = (char)(ce - 'A' + 'a');
        if (ca != ce) return 0;
    }
    return 1;
}

static int is_video_file(const char* name)
{
    return ends_with_ci(name, ".mp4") || ends_with_ci(name, ".mov") ||
           ends_with_ci(name, ".mkv") || ends_with_ci(name, ".m4v") ||
           ends_with_ci(name, ".ts");
}

static void playlist_free(Playlist* p)
{
    if (!p) return;
    for (int i = 0; i < p->count; i++) free(p->items[i]);
    free(p->items);
    memset(p, 0, sizeof(*p));
}

static void playlist_add(Playlist* p, const char* fullpath)
{
    if (p->count >= p->cap) {
        int ncap = (p->cap == 0) ? 8 : p->cap * 2;
        char** nitems = (char**)realloc(p->items, (size_t)ncap * sizeof(char*));
        if (!nitems) return;
        p->items = nitems;
        p->cap = ncap;
    }
    p->items[p->count++] = strdup(fullpath);
}

static int playlist_load_from_home_videos(Playlist* p, char* out_dir, size_t out_dir_sz)
{
    memset(p, 0, sizeof(*p));

    const char* home = getenv("HOME");
    if (!home) home = "/home/pi";

    snprintf(out_dir, out_dir_sz, "%s/videos", home);

    DIR* d = opendir(out_dir);
    if (!d) {
        printf("Playlist: failed to open dir: %s\n", out_dir);
        return 0;
    }

    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_video_file(de->d_name)) continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", out_dir, de->d_name);

        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            playlist_add(p, full);
        }
    }
    closedir(d);

    if (p->count == 0) {
        printf("Playlist: no videos found in %s\n", out_dir);
        return 0;
    }

    printf("Playlist: loaded %d video(s) from %s\n", p->count, out_dir);
    for (int i = 0; i < p->count; i++) {
        printf("  [%d] %s\n", i, p->items[i]);
    }
    return 1;
}

static const char* playlist_random(const Playlist* p, const char* avoid_path)
{
    if (!p || p->count <= 0) return NULL;
    if (p->count == 1) return p->items[0];

    // try a few times to avoid repeating the current file
    for (int tries = 0; tries < 8; tries++) {
        int idx = rand() % p->count;
        const char* cand = p->items[idx];
        if (!avoid_path || strcmp(cand, avoid_path) != 0) return cand;
    }
    // fallback
    return p->items[rand() % p->count];
}

/* ================= VIDEO (NV12 appsink -> GL textures) ================= */

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

static void video_reset(Video* v)
{
    memset(v, 0, sizeof(*v));
}

static int video_start(Video* v, const char* filename)
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

static void video_stop(Video* v)
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

static void video_delete_textures(Video* v)
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

static void video_poll_bus(Video* v)
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

static void video_update_texture(Video* v)
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

/* ================= TRANSITION ENGINE ================= */

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

static void ve_init(VideoEngine* ve)
{
    memset(ve, 0, sizeof(*ve));
    ve->xfade_seconds = XFADE_SECONDS;
}

static int ve_start_current(VideoEngine* ve, const char* path)
{
    if (!video_start(&ve->cur, path)) return 0;
    printf("[VE] Current = %s\n", ve->cur.path);
    fflush(stdout);
    return 1;
}

static void ve_request_transition(VideoEngine* ve, const char* path)
{
    if (!path || !path[0]) return;

    // If we’re already transitioning, just replace the pending request.
    snprintf(ve->pending_path, sizeof(ve->pending_path), "%s", path);
    ve->pending = 1;

    printf("[VE] Transition requested -> %s\n", path);
    fflush(stdout);
}

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

static void ve_update(VideoEngine* ve)
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

static void ve_bind_textures_and_blend(VideoEngine* ve, GLint uBlend)
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

static void ve_shutdown(VideoEngine* ve)
{
    video_stop(&ve->cur);
    video_stop(&ve->nxt);
    // textures deleted in main after GL context exists; we do both here too:
    video_delete_textures(&ve->cur);
    video_delete_textures(&ve->nxt);
    memset(ve, 0, sizeof(*ve));
}

/* ================= BUTTON ACTIONS ================= */

static void on_btn3_toggle_edit(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_btn3)) return;

    s->edit_mode = !s->edit_mode;
    if (s->edit_mode) s->select_mode = 1;

    printf("[BTN3] EDIT %s\n", s->edit_mode ? "ON" : "OFF");
    print_status(s);
}

static void on_btn2_toggle_select_move(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_btn2)) return;
    if (!s->edit_mode) return;

    s->select_mode = !s->select_mode;
    printf("[BTN2] MODE %s\n", s->select_mode ? "SELECT" : "MOVE");
    print_status(s);
}

static void on_btn1_cycle_corner_only(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_btn1)) return;
    if (!s->edit_mode || !s->select_mode) return;

    s->selected_ui = (s->selected_ui + 1) % 4;
    printf("[BTN1] SELECT %s\n", corner_name_ui(s->selected_ui));
    print_status(s);
}

static void move_selected_corner(AppState* s, float dx, float dy)
{
    int sq = UI_TO_SQ_CORNER[s->selected_ui];
    s->corners[sq][0] += dx;
    s->corners[sq][1] += dy;
    rebuild_mesh_from_corners(s);

    printf("[MOVE] %s dx=%.3f dy=%.3f\n", corner_name_ui(s->selected_ui), dx, dy);
    print_status(s);
}

static void on_up(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_up)) return;
    if (!s->edit_mode || s->select_mode) return;
    move_selected_corner(s, 0.0f, s->moveSpeed);
}
static void on_down(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_down)) return;
    if (!s->edit_mode || s->select_mode) return;
    move_selected_corner(s, 0.0f, -s->moveSpeed);
}
static void on_left(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_left)) return;
    if (!s->edit_mode || s->select_mode) return;
    move_selected_corner(s, -s->moveSpeed, 0.0f);
}
static void on_right(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_right)) return;
    if (!s->edit_mode || s->select_mode) return;
    move_selected_corner(s, s->moveSpeed, 0.0f);
}

/* ================= MAIN ================= */

int main(int argc, char** argv)
{
    // Ensure tail -f shows prints immediately
    setvbuf(stdout, NULL, _IONBF, 0);

    signal(SIGINT, handle_sigint);

    if (argc < 2) {
        printf("Usage: %s /path/to/video.mp4\n", argv[0]);
        printf("Note: this initial file is only used to start playback.\n");
        return 1;
    }

    srand((unsigned int)time(NULL));

    const char* initial_video_path = argv[1];

    // GStreamer init
    gst_init(NULL, NULL);

    // SDL / GL init (KMSDRM)
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_Window* window = SDL_CreateWindow(
        "Mapping Video Keystone (NV12 + Crossfade)",
        0, 0, 1920, 1080,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN
    );
    if (!window) { printf("Window creation failed: %s\n", SDL_GetError()); return -1; }

    SDL_ShowCursor(SDL_DISABLE);

    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) { printf("GL context failed: %s\n", SDL_GetError()); return -1; }

    printf("Renderer: %s\n", glGetString(GL_RENDERER));
    printf("Version : %s\n", glGetString(GL_VERSION));

    // Shaders/program
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint link_ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &link_ok);
    if (!link_ok) {
        char buffer[1024];
        glGetProgramInfoLog(program, sizeof(buffer), NULL, buffer);
        printf("Program link error: %s\n", buffer);
        return -1;
    }

    glUseProgram(program);

    // Mesh buffers
    int numVerts = GRID_X * GRID_Y;
    int numIndices = (GRID_X - 1) * (GRID_Y - 1) * 6;

    float* vertices = (float*)malloc((size_t)numVerts * 4 * sizeof(float));
    unsigned int* indices = (unsigned int*)malloc((size_t)numIndices * sizeof(unsigned int));
    if (!vertices || !indices) { printf("Out of memory\n"); return -1; }

    int ii = 0;
    for (int y = 0; y < GRID_Y - 1; y++) {
        for (int x = 0; x < GRID_X - 1; x++) {
            int topLeft = y * GRID_X + x;
            int topRight = topLeft + 1;
            int bottomLeft = topLeft + GRID_X;
            int bottomRight = bottomLeft + 1;

            indices[ii++] = topLeft;   indices[ii++] = bottomLeft; indices[ii++] = topRight;
            indices[ii++] = topRight;  indices[ii++] = bottomLeft; indices[ii++] = bottomRight;
        }
    }

    GLuint vbo, ebo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)numVerts * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)numIndices * sizeof(unsigned int), indices, GL_STATIC_DRAW);

    GLint aPos = glGetAttribLocation(program, "aPos");
    GLint aTex = glGetAttribLocation(program, "aTex");
    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), 0);
    glEnableVertexAttribArray(aTex);
    glVertexAttribPointer(aTex, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));

    // Texture uniforms: A uses units 0/1, B uses units 2/3
    GLint uTexY_A  = glGetUniformLocation(program, "uTexY_A");
    GLint uTexUV_A = glGetUniformLocation(program, "uTexUV_A");
    GLint uTexY_B  = glGetUniformLocation(program, "uTexY_B");
    GLint uTexUV_B = glGetUniformLocation(program, "uTexUV_B");
    GLint uBlend   = glGetUniformLocation(program, "uBlend");

    glUniform1i(uTexY_A, 0);
    glUniform1i(uTexUV_A, 1);
    glUniform1i(uTexY_B, 2);
    glUniform1i(uTexUV_B, 3);
    glUniform1f(uBlend, 0.0f);

    // GPIO init
    struct gpiod_chip* chip = gpiod_chip_open_by_name("gpiochip0");
    if (!chip) { printf("gpiod: failed to open gpiochip0\n"); return -1; }

    const char* consumer = "mapping_video_keystone_nv12_xfade";
    struct gpiod_line* line_btn1  = request_line_events(chip, GPIO_BTN1,  consumer);
    struct gpiod_line* line_btn2  = request_line_events(chip, GPIO_BTN2,  consumer);
    struct gpiod_line* line_btn3  = request_line_events(chip, GPIO_BTN3,  consumer);
    struct gpiod_line* line_up    = request_line_events(chip, GPIO_UP,    consumer);
    struct gpiod_line* line_down  = request_line_events(chip, GPIO_DOWN,  consumer);
    struct gpiod_line* line_left  = request_line_events(chip, GPIO_LEFT,  consumer);
    struct gpiod_line* line_right = request_line_events(chip, GPIO_RIGHT, consumer);

    // App state
    AppState st;
    memset(&st, 0, sizeof(st));
    st.vertices = vertices;
    st.numVerts = numVerts;
    st.numIndices = numIndices;
    st.vbo = vbo;

    st.edit_mode = 0;
    st.select_mode = 1;
    st.selected_ui = 0;     // TL
    st.moveSpeed = 0.02f;

    // Default corners: full screen
    st.corners[C_BL][0] = -1.0f; st.corners[C_BL][1] = -1.0f;
    st.corners[C_BR][0] =  1.0f; st.corners[C_BR][1] = -1.0f;
    st.corners[C_TR][0] =  1.0f; st.corners[C_TR][1] =  1.0f;
    st.corners[C_TL][0] = -1.0f; st.corners[C_TL][1] =  1.0f;

    rebuild_mesh_from_corners(&st);
    print_status(&st);

    // Playlist
    Playlist pl;
    char videos_dir[512];
    if (!playlist_load_from_home_videos(&pl, videos_dir, sizeof(videos_dir))) {
        // still continue with the initial file; random switching will be disabled
        memset(&pl, 0, sizeof(pl));
        printf("Playlist: random switching disabled (no videos loaded)\n");
    }

    // Video engine
    VideoEngine ve;
    ve_init(&ve);

    // Start current video: if initial path is in playlist, fine; if not, still plays
    if (!ve_start_current(&ve, initial_video_path)) {
        printf("Failed to start initial video.\n");
        return -1;
    }

    printf("\nControls:\n");
    printf("  BTN3(GPIO27): toggle EDIT mode\n");
    printf("  When EDIT ON:\n");
    printf("    BTN1(GPIO17): cycle corner (only in SELECT mode)\n");
    printf("    BTN2(GPIO18): toggle SELECT<->MOVE\n");
    printf("    Arrows(GPIO24/22/25/23): move corner (only in MOVE mode)\n");
    printf("  When EDIT OFF:\n");
    printf("    BTN1(GPIO17): RANDOM VIDEO crossfade from %s\n", (pl.count > 0 ? videos_dir : "(disabled)"));
    printf("  ESC (keyboard): exit\n\n");

    glClearColor(0, 0, 0, 1);

    // Main loop
    while (keepRunning)
    {
        // ESC emergency exit
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                keepRunning = 0;
        }

        // Update video engine (poll bus, pull frames, handle crossfade)
        ve_update(&ve);

        // GPIO event processing:
        // - BTN3 always toggles edit
        // - BTN2 only matters in edit mode
        // - BTN1: either cycles corner (edit/select) or triggers random video (edit off)
        process_line_events(line_btn3,  on_btn3_toggle_edit,        &st);
        process_line_events(line_btn2,  on_btn2_toggle_select_move, &st);

        // BTN1 special behavior based on edit mode
        if (line_btn1) {
            struct timespec timeout = {0, 0};
            while (1) {
                int ready = gpiod_line_event_wait(line_btn1, &timeout);
                if (ready <= 0) break;

                struct gpiod_line_event ev;
                if (gpiod_line_event_read(line_btn1, &ev) < 0) break;

                if (ev.event_type == GPIOD_LINE_EVENT_RISING_EDGE) {
                    if (!debounce_ok(&st.last_btn1)) continue;

                    if (st.edit_mode && st.select_mode) {
                        // original behavior: cycle corners in SELECT mode
                        st.selected_ui = (st.selected_ui + 1) % 4;
                        printf("[BTN1] SELECT %s\n", corner_name_ui(st.selected_ui));
                        print_status(&st);
                    } else if (!st.edit_mode) {
                        // new behavior: random transition
                        if (pl.count > 0) {
                            const char* next = playlist_random(&pl, ve.cur.path[0] ? ve.cur.path : NULL);
                            printf("[BTN1] RANDOM -> %s\n", next ? next : "(null)");
                            fflush(stdout);
                            if (next) ve_request_transition(&ve, next);
                        } else {
                            printf("[BTN1] RANDOM requested, but playlist empty\n");
                            fflush(stdout);
                        }
                    } else {
                        // edit mode but not SELECT => ignore (matches original intent)
                        printf("[BTN1] (ignored) EDIT ON but not in SELECT mode\n");
                        fflush(stdout);
                    }
                }
            }
        }

        // Movement buttons
        process_line_events(line_up,    on_up,    &st);
        process_line_events(line_down,  on_down,  &st);

	if (line_left) {
	    struct timespec timeout = {0, 0};
	    while (1) {
		int ready = gpiod_line_event_wait(line_left, &timeout);
		if (ready <= 0) break;

		struct gpiod_line_event ev;
		if (gpiod_line_event_read(line_left, &ev) < 0) break;

		if (ev.event_type == GPIOD_LINE_EVENT_RISING_EDGE) {

		    if (!debounce_ok(&st.last_left))
			continue;

		    if (st.edit_mode) {
			// Original behavior: move corner left
			if (!st.select_mode) {
			    move_selected_corner(&st, -st.moveSpeed, 0.0f);
			} else {
			    printf("[LEFT] ignored (EDIT ON but SELECT mode)\n");
			    fflush(stdout);
			}
		    } else {
			// NEW behavior: random video transition
			if (pl.count > 0) {
			    const char* next = playlist_random(&pl, ve.cur.path[0] ? ve.cur.path : NULL);
			    printf("[LEFT] RANDOM -> %s\n", next ? next : "(null)");
			    fflush(stdout);
			    if (next)
				ve_request_transition(&ve, next);
			} else {
			    printf("[LEFT] RANDOM requested, but playlist empty\n");
			    fflush(stdout);
			}
		    }
		}
	    }
	}


        process_line_events(line_right, on_right, &st);

        // Render
        glClear(GL_COLOR_BUFFER_BIT);

        // Bind textures and blend value
        ve_bind_textures_and_blend(&ve, uBlend);

        glDrawElements(GL_TRIANGLES, st.numIndices, GL_UNSIGNED_INT, 0);
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    ve_shutdown(&ve);
    playlist_free(&pl);

    if (line_btn1)  gpiod_line_release(line_btn1);
    if (line_btn2)  gpiod_line_release(line_btn2);
    if (line_btn3)  gpiod_line_release(line_btn3);
    if (line_up)    gpiod_line_release(line_up);
    if (line_down)  gpiod_line_release(line_down);
    if (line_left)  gpiod_line_release(line_left);
    if (line_right) gpiod_line_release(line_right);
    if (chip)       gpiod_chip_close(chip);

    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);
    glDeleteProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    free(vertices);
    free(indices);

    return 0;
}

