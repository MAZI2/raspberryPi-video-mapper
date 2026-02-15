// ==============================
// Mapping Video Keystone (NV12 + Mesh + GPIO Debug)
// Raspberry Pi 3 optimized: NV12 upload + shader YUV->RGB
// Restores original GPIO button debug printing + corner editing.
// ==============================

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <gpiod.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

/* ================= CONFIG ================= */

#define GRID_X 16
#define GRID_Y 9

// MiniMAD board GPIOs (BCM)
#define GPIO_BTN1   17  // cycle corner
#define GPIO_BTN2   18  // toggle SELECT<->MOVE
#define GPIO_BTN3   27  // toggle EDIT mode
#define GPIO_UP     24
#define GPIO_DOWN   22
#define GPIO_LEFT   25
#define GPIO_RIGHT  23

volatile int keepRunning = 1;

// Debounce (ms)
#define DEBOUNCE_MS 120

// Corner order for homography: BL, BR, TR, TL
typedef enum { C_BL=0, C_BR=1, C_TR=2, C_TL=3 } CornerSq;

// UI cycle order: TL, TR, BL, BR
static const int UI_TO_SQ_CORNER[4] = { C_TL, C_TR, C_BL, C_BR };

static const char* corner_name_ui(int uiIdx) {
    switch (uiIdx) { case 0: return "TL"; case 1: return "TR"; case 2: return "BL"; case 3: return "BR"; default: return "?"; }
}

static void handle_sigint(int sig) { (void)sig; keepRunning = 0; }

/* ================= SHADERS ================= */

// Vertex shader: pass through position + texcoord
static const char* vertex_shader_src =
    "attribute vec2 aPos;"
    "attribute vec2 aTex;"
    "varying vec2 vTex;"
    "void main(){ vTex=aTex; gl_Position=vec4(aPos,0.0,1.0); }";

// Fragment shader: NV12 -> RGB
// Notes:
// - uTexY: GL_LUMINANCE texture full res
// - uTexUV: GL_LUMINANCE_ALPHA texture half res (U in .r, V in .a)
static const char* fragment_shader_src =
    "precision mediump float;"
    "varying vec2 vTex;"
    "uniform sampler2D uTexY;"
    "uniform sampler2D uTexUV;"
    "void main(){"
    "  vec2 tc = vec2(vTex.x, 1.0 - vTex.y);"
    "  float y  = texture2D(uTexY,  tc).r;"
    "  vec4  uv = texture2D(uTexUV, tc);"
    "  float u = uv.r - 0.5;"
    "  float v = uv.a - 0.5;"
    "  float Y = 1.1643 * (y - 0.0625);"   // video range
    "  float R = Y + 1.7927 * v;"
    "  float G = Y - 0.2132 * u - 0.5329 * v;"
    "  float B = Y + 2.1124 * u;"
    "  gl_FragColor = vec4(R, G, B, 1.0);"
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

/* ================= GPIO helpers ================= */

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

/* ================= Button actions ================= */

static void on_btn3_toggle_edit(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_btn3)) return;

    s->edit_mode = !s->edit_mode;
    if (s->edit_mode) s->select_mode = 1;

    printf("[BTN3] EDIT %s\n", s->edit_mode ? "ON" : "OFF");
    print_status(s);
}

static void on_btn1_cycle_corner(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_btn1)) return;
    if (!s->edit_mode || !s->select_mode) return;

    s->selected_ui = (s->selected_ui + 1) % 4;
    printf("[BTN1] SELECT %s\n", corner_name_ui(s->selected_ui));
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

/* ================= GStreamer -> appsink video (NV12) ================= */

typedef struct {
    GstElement* pipeline;
    GstElement* appsink;
    GstBus* bus;

    int width;
    int height;

    GLuint texY;
    GLuint texUV;
    int tex_inited;
} Video;

static int video_start(Video* v, const char* filename)
{
    memset(v, 0, sizeof(*v));

    // NV12 path: no videoconvert, no RGB
    char pipe[2048];
    snprintf(pipe, sizeof(pipe),
        "filesrc location=\"%s\" ! qtdemux ! h264parse ! v4l2h264dec "
        "! video/x-raw,format=NV12 "
        "! appsink name=sink sync=true max-buffers=1 drop=true",
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

    // appsink pull mode
    gst_app_sink_set_emit_signals((GstAppSink*)v->appsink, FALSE);
    gst_app_sink_set_drop((GstAppSink*)v->appsink, TRUE);
    gst_app_sink_set_max_buffers((GstAppSink*)v->appsink, 1);

    v->bus = gst_element_get_bus(v->pipeline);

    GstStateChangeReturn r = gst_element_set_state(v->pipeline, GST_STATE_PLAYING);
    if (r == GST_STATE_CHANGE_FAILURE) {
        printf("Failed to set pipeline PLAYING\n");
        return 0;
    }

    printf("Video pipeline started (NV12): %s\n", filename);
    fflush(stdout);
    return 1;
}

static void video_stop(Video* v)
{
    if (v->pipeline) {
        gst_element_set_state(v->pipeline, GST_STATE_NULL);
    }
    if (v->bus) gst_object_unref(v->bus);
    if (v->appsink) gst_object_unref(v->appsink);
    if (v->pipeline) gst_object_unref(v->pipeline);

    // Textures live in GL context; delete in main after stop if desired.
    memset(v, 0, sizeof(*v));
}

// Upload NV12 planes to two textures, handling stride safely.
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

        // Y texture
        glBindTexture(GL_TEXTURE_2D, v->texY);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // UV texture (half res, U in L, V in A)
        glBindTexture(GL_TEXTURE_2D, v->texUV);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, w/2, h/2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        v->tex_inited = 1;
        printf("Video textures init NV12 %dx%d strideY=%d strideUV=%d\n", w, h, strideY, strideUV);
        fflush(stdout);
    }

    // Upload Y
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

    // Upload UV
    glBindTexture(GL_TEXTURE_2D, v->texUV);
    int uvw = w / 2;
    int uvh = h / 2;
    int tightUV = uvw * 2; // bytes per UV row
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

// Poll bus for EOS/ERROR. On EOS, loop back to start.
static void video_poll_bus(Video* v)
{
    if (!v->bus) return;

    while (1) {
        GstMessage* msg = gst_bus_pop(v->bus);
        if (!msg) break;

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = NULL;
                gchar* dbg = NULL;
                gst_message_parse_error(msg, &err, &dbg);
                printf("GST ERROR: %s\n", err ? err->message : "unknown");
                if (dbg) printf("GST DEBUG: %s\n", dbg);
                if (err) g_error_free(err);
                if (dbg) g_free(dbg);
                fflush(stdout);
                break;
            }
            case GST_MESSAGE_EOS: {
                printf("GST EOS: looping\n");
                fflush(stdout);
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

// Try to pull one sample (non-blocking). If available, upload into textures.
static void video_update_texture(Video* v)
{
    if (!v->appsink) return;

    // Non-blocking: render loop is the clock; keeps latency low.
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

/* ================= Main ================= */

int main(int argc, char** argv)
{
    // Ensure tail -f shows prints immediately
    setvbuf(stdout, NULL, _IONBF, 0);

    signal(SIGINT, handle_sigint);

    if (argc < 2) {
        printf("Usage: %s /path/to/video.mp4\n", argv[0]);
        return 1;
    }

    const char* video_path = argv[1];

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
        "Mapping Video Keystone (NV12)",
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
    glUseProgram(program);

    // Mesh buffers
    int numVerts = GRID_X * GRID_Y;
    int numIndices = (GRID_X - 1) * (GRID_Y - 1) * 6;

    float* vertices = (float*)malloc(numVerts * 4 * sizeof(float));
    unsigned int* indices = (unsigned int*)malloc(numIndices * sizeof(unsigned int));
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
    glBufferData(GL_ARRAY_BUFFER, numVerts * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, numIndices * sizeof(unsigned int), indices, GL_STATIC_DRAW);

    GLint aPos = glGetAttribLocation(program, "aPos");
    GLint aTex = glGetAttribLocation(program, "aTex");
    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), 0);
    glEnableVertexAttribArray(aTex);
    glVertexAttribPointer(aTex, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));

    GLint uTexY  = glGetUniformLocation(program, "uTexY");
    GLint uTexUV = glGetUniformLocation(program, "uTexUV");
    glUniform1i(uTexY, 0);
    glUniform1i(uTexUV, 1);

    // GPIO init
    struct gpiod_chip* chip = gpiod_chip_open_by_name("gpiochip0");
    if (!chip) { printf("gpiod: failed to open gpiochip0\n"); return -1; }

    const char* consumer = "mapping_video_keystone_nv12";
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

    // Start video
    Video vid;
    if (!video_start(&vid, video_path)) {
        printf("Failed to start video.\n");
        return -1;
    }

    printf("\nControls:\n");
    printf("  BTN3(GPIO27): toggle EDIT mode\n");
    printf("  When EDIT ON:\n");
    printf("    BTN1(GPIO17): cycle corner (only in SELECT mode)\n");
    printf("    BTN2(GPIO18): toggle SELECT<->MOVE\n");
    printf("    Arrows(GPIO24/22/25/23): move corner (only in MOVE mode)\n");
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

        // Update video textures
        video_poll_bus(&vid);
        video_update_texture(&vid);

        // Handle GPIO editing (debug printing happens in handlers)
        process_line_events(line_btn3,  on_btn3_toggle_edit,        &st);
        process_line_events(line_btn2,  on_btn2_toggle_select_move, &st);
        process_line_events(line_btn1,  on_btn1_cycle_corner,       &st);
        process_line_events(line_up,    on_up,    &st);
        process_line_events(line_down,  on_down,  &st);
        process_line_events(line_left,  on_left,  &st);
        process_line_events(line_right, on_right, &st);

        // Render
        glClear(GL_COLOR_BUFFER_BIT);

        if (vid.tex_inited) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, vid.texY);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, vid.texUV);
        }

        glDrawElements(GL_TRIANGLES, st.numIndices, GL_UNSIGNED_INT, 0);
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    video_stop(&vid);

    if (vid.tex_inited) {
        glDeleteTextures(1, &vid.texY);
        glDeleteTextures(1, &vid.texUV);
    }

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

