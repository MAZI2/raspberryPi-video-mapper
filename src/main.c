#include "common.h"
#include "app_state.h"
#include "gpio_helpers.h"
#include "input_actions.h"
#include "playlist.h"
#include "shaders.h"
#include "video_engine.h"

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <gst/gst.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

typedef struct {
    AppState* st;
    Playlist* pl;
    VideoEngine* ve;
} Btn1Context;

static void gl_check(const char* where)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "[GL] error 0x%x at %s\n", (unsigned)e, where);
        fflush(stderr);
    }
}

static void on_btn1_edit_or_random(void* u)
{
    Btn1Context* ctx = (Btn1Context*)u;
    AppState* st = ctx->st;
    Playlist* pl = ctx->pl;
    VideoEngine* ve = ctx->ve;

    if (!debounce_ok(&st->last_btn1))
        return;

    if (st->edit_mode) {
        if (st->select_mode) {
            st->selected_ui = (st->selected_ui + 1) % 4;
            printf("[BTN1] SELECT %s\n", corner_name_ui(st->selected_ui));
            print_status(st);
        }
        return;
    }

    if (pl->count <= 0) {
        printf("[BTN1] RANDOM requested, but playlist is empty\n");
        fflush(stdout);
        return;
    }

    const char* next = playlist_random(pl, ve->cur.path[0] ? ve->cur.path : NULL);
    printf("[BTN1] RANDOM -> %s\n", next ? next : "(null)");
    fflush(stdout);

    if (next)
        ve_request_transition(ve, next);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "[BOOT] mapping_video_keystone starting\n");
    fflush(stderr);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s /path/to/video.mp4\n", argv[0]);
        return 1;
    }

    signal(SIGINT, handle_sigint);
    srand((unsigned int)time(NULL));

    const char* initial_video = argv[1];

    gst_init(NULL, NULL);

    SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_Window* window = SDL_CreateWindow(
        "Mapping Video Keystone",
        0, 0, 1920, 1080,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN
    );
    if (!window) {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_ShowCursor(SDL_DISABLE);

    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        fprintf(stderr, "GL context creation failed: %s\n", SDL_GetError());
        return 1;
    }

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) {
        dw = 1920;
        dh = 1080;
    }
    glViewport(0, 0, dw, dh);

    fprintf(stderr, "Renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "Version : %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "Viewport: %dx%d\n", dw, dh);
    fflush(stderr);

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "Program link error: %s\n", log);
        fflush(stderr);
        return 1;
    }

    glUseProgram(program);
    gl_check("after glUseProgram");

    const int numVerts = GRID_X * GRID_Y;
    const int numIndices = (GRID_X - 1) * (GRID_Y - 1) * 6;

    float* vertices = (float*)malloc((size_t)numVerts * 4 * sizeof(float));
    GLushort* indices = (GLushort*)malloc((size_t)numIndices * sizeof(GLushort));
    if (!vertices || !indices) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    int ii = 0;
    for (int y = 0; y < GRID_Y - 1; y++) {
        for (int x = 0; x < GRID_X - 1; x++) {
            int tl = y * GRID_X + x;
            int tr = tl + 1;
            int bl = tl + GRID_X;
            int br = bl + 1;

            indices[ii++] = (GLushort)tl;
            indices[ii++] = (GLushort)bl;
            indices[ii++] = (GLushort)tr;

            indices[ii++] = (GLushort)tr;
            indices[ii++] = (GLushort)bl;
            indices[ii++] = (GLushort)br;
        }
    }

    GLuint vbo = 0, ebo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)numVerts * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (size_t)numIndices * sizeof(GLushort),
                 indices,
                 GL_STATIC_DRAW);

    GLint aPos = glGetAttribLocation(program, "aPos");
    GLint aTex = glGetAttribLocation(program, "aTex");
    if (aPos < 0 || aTex < 0) {
        fprintf(stderr, "Shader attributes missing: aPos=%d aTex=%d\n", aPos, aTex);
        return 1;
    }

    glEnableVertexAttribArray((GLuint)aPos);
    glVertexAttribPointer((GLuint)aPos, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray((GLuint)aTex);
    glVertexAttribPointer((GLuint)aTex, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void*)(2 * sizeof(float)));

    GLint uTexY = glGetUniformLocation(program, "uTexY");
    GLint uTexU = glGetUniformLocation(program, "uTexU");
    GLint uTexV = glGetUniformLocation(program, "uTexV");
    GLint uRange = glGetUniformLocation(program, "uVideoRange");
    GLint u709 = glGetUniformLocation(program, "uBT709");
    GLint uAlpha = glGetUniformLocation(program, "uAlpha");

    if (uTexY >= 0) glUniform1i(uTexY, 0);
    if (uTexU >= 0) glUniform1i(uTexU, 1);
    if (uTexV >= 0) glUniform1i(uTexV, 2);
    if (uAlpha >= 0) glUniform1f(uAlpha, 1.0f);

    AppState st;
    memset(&st, 0, sizeof(st));
    st.vertices = vertices;
    st.numVerts = numVerts;
    st.numIndices = numIndices;
    st.vbo = vbo;
    st.edit_mode = 0;
    st.select_mode = 1;
    st.selected_ui = 0;
    st.moveSpeed = 0.02f;

    st.corners[C_BL][0] = -1.0f; st.corners[C_BL][1] = -1.0f;
    st.corners[C_BR][0] =  1.0f; st.corners[C_BR][1] = -1.0f;
    st.corners[C_TR][0] =  1.0f; st.corners[C_TR][1] =  1.0f;
    st.corners[C_TL][0] = -1.0f; st.corners[C_TL][1] =  1.0f;
    rebuild_mesh_from_corners(&st);
    print_status(&st);

    Playlist pl;
    char videos_dir[512];
    if (!playlist_load_from_home_videos(&pl, videos_dir, sizeof(videos_dir))) {
        memset(&pl, 0, sizeof(pl));
    }

    VideoEngine ve;
    ve_init(&ve);
    if (!ve_start_current(&ve, initial_video)) {
        fprintf(stderr, "Failed to start video: %s\n", initial_video);
        fflush(stderr);
    }

    const char* consumer = "mapping_video_keystone";
    GpioLine* line_btn1 = gpio_request_line(GPIO_BTN1, consumer);
    GpioLine* line_btn2 = gpio_request_line(GPIO_BTN2, consumer);
    GpioLine* line_btn3 = gpio_request_line(GPIO_BTN3, consumer);
    GpioLine* line_up = gpio_request_line(GPIO_UP, consumer);
    GpioLine* line_down = gpio_request_line(GPIO_DOWN, consumer);
    GpioLine* line_left = gpio_request_line(GPIO_LEFT, consumer);
    GpioLine* line_right = gpio_request_line(GPIO_RIGHT, consumer);

    Btn1Context btn1_ctx = {
        .st = &st,
        .pl = &pl,
        .ve = &ve
    };

    glClearColor(0.f, 0.f, 0.f, 1.f);
    fprintf(stderr, "[BOOT] entering main loop\n");
    fflush(stderr);

    while (keepRunning) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                keepRunning = 0;
        }

        ve_update(&ve);

        gpio_process_events(line_btn3, on_btn3_toggle_edit, &st);
        gpio_process_events(line_btn2, on_btn2_toggle_select_move, &st);
        gpio_process_events(line_btn1, on_btn1_edit_or_random, &btn1_ctx);
        gpio_process_events(line_up, on_up, &st);
        gpio_process_events(line_down, on_down, &st);
        gpio_process_events(line_left, on_left, &st);
        gpio_process_events(line_right, on_right, &st);

        glUseProgram(program);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glClear(GL_COLOR_BUFFER_BIT);

        if (ve.cur.tex_inited) {
            glDisable(GL_BLEND);
            if (uAlpha >= 0) glUniform1f(uAlpha, 1.0f);
            if (uRange >= 0) glUniform1i(uRange, ve.cur.video_range);
            if (u709 >= 0) glUniform1i(u709, ve.cur.bt709);
            ve_bind_video_textures(&ve.cur, uTexY, uTexU, uTexV);
            glDrawElements(GL_TRIANGLES, (GLsizei)st.numIndices, GL_UNSIGNED_SHORT, 0);
        }

        if (ve.transitioning && ve.nxt.tex_inited) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            if (uAlpha >= 0) glUniform1f(uAlpha, ve.blend);
            if (uRange >= 0) glUniform1i(uRange, ve.nxt.video_range);
            if (u709 >= 0) glUniform1i(u709, ve.nxt.bt709);

            ve_bind_video_textures(&ve.nxt, uTexY, uTexU, uTexV);
            glDrawElements(GL_TRIANGLES, (GLsizei)st.numIndices, GL_UNSIGNED_SHORT, 0);
        }

        gl_check("after draw");
        SDL_GL_SwapWindow(window);
    }

    gpio_release_line(line_btn1);
    gpio_release_line(line_btn2);
    gpio_release_line(line_btn3);
    gpio_release_line(line_up);
    gpio_release_line(line_down);
    gpio_release_line(line_left);
    gpio_release_line(line_right);

    ve_shutdown(&ve);
    playlist_free(&pl);

    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);
    glDeleteProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    free(vertices);
    free(indices);

    return 0;
}
