#include "common.h"
#include "app_state.h"
#include "gpio_helpers.h"
#include "input_actions.h"
#include "project_profile.h"
#include "project_runtime.h"
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
    ProjectRuntime* runtime;
    VideoEngine* ve_fg;
    VideoEngine* ve_bg;
} Btn1Context;

static void gl_check(const char* where)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "[GL] error 0x%x at %s\n", (unsigned)e, where);
        fflush(stderr);
    }
}

static void on_btn1_edit_or_show_action(void* u)
{
    Btn1Context* ctx = (Btn1Context*)u;
    AppState* st = ctx->st;

    if (!debounce_ok(&st->last_btn1)) {
        return;
    }

    if (st->edit_mode) {
        st->selected_ui = (st->selected_ui + 1) % 4;
        printf("[BTN1] SELECT %s\n", corner_name_ui(st->selected_ui));
        print_status(st);
        return;
    }

    project_runtime_on_action(ctx->runtime, ctx->ve_fg, ctx->ve_bg);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    ProjectRuntime runtime;
    project_runtime_init(&runtime);

    fprintf(stderr, "[BOOT] mapping_video_keystone starting\n");
    fprintf(stderr, "[BOOT] project runtime: %s\n", runtime.project_name);
    fflush(stderr);

    signal(SIGINT, handle_sigint);
    srand((unsigned int)time(NULL));

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
    GLint uTexA = glGetUniformLocation(program, "uTexA");
    GLint uRange = glGetUniformLocation(program, "uVideoRange");
    GLint u709 = glGetUniformLocation(program, "uBT709");
    GLint uAlpha = glGetUniformLocation(program, "uAlpha");

    if (uTexY >= 0) glUniform1i(uTexY, 0);
    if (uTexU >= 0) glUniform1i(uTexU, 1);
    if (uTexV >= 0) glUniform1i(uTexV, 2);
    if (uTexA >= 0) glUniform1i(uTexA, 3);
    if (uAlpha >= 0) glUniform1f(uAlpha, 1.0f);

    AppState st;
    memset(&st, 0, sizeof(st));
    st.vertices = vertices;
    st.numVerts = numVerts;
    st.numIndices = numIndices;
    st.vbo = vbo;
    st.edit_mode = 0;
    st.selected_ui = 0;
    st.moveSpeed = 0.02f;

    st.corners[C_BL][0] = -1.0f; st.corners[C_BL][1] = -1.0f;
    st.corners[C_BR][0] =  1.0f; st.corners[C_BR][1] = -1.0f;
    st.corners[C_TR][0] =  1.0f; st.corners[C_TR][1] =  1.0f;
    st.corners[C_TL][0] = -1.0f; st.corners[C_TL][1] =  1.0f;
    rebuild_mesh_from_corners(&st);
    print_status(&st);

    if (!project_runtime_prepare(&runtime, argc, argv)) {
        fprintf(stderr, "Failed to prepare runtime for profile '%s'\n", runtime.project_name[0] ? runtime.project_name : "core");
        fflush(stderr);
        project_runtime_shutdown(&runtime);
        return 1;
    }

    VideoEngine ve_fg;
    VideoEngine ve_bg;
    int use_background_layer = runtime.use_background_layer;

    ve_init(&ve_fg);
    ve_init(&ve_bg);
    ve_set_prefer_alpha(&ve_fg, runtime.foreground_prefer_alpha);
    ve_set_prefer_alpha(&ve_bg, runtime.background_prefer_alpha);

#if PROJECT_DISABLE_XFADE
    ve_set_xfade_seconds(&ve_fg, 0.0f);
#else
    if (runtime.foreground_xfade_seconds >= 0.0f) {
        ve_set_xfade_seconds(&ve_fg, runtime.foreground_xfade_seconds);
    }
#endif

    if (!runtime.initial_video[0] ||
        !ve_start_current_opts(&ve_fg, runtime.initial_video, runtime.initial_loop)) {
        fprintf(stderr, "Failed to start foreground video: %s\n",
                runtime.initial_video[0] ? runtime.initial_video : "(null)");
        fflush(stderr);
    }

    if (use_background_layer) {
        if (!runtime.background_video[0] ||
            !ve_start_current_opts(&ve_bg, runtime.background_video, 1)) {
            fprintf(stderr, "Failed to start background video: %s\n",
                    runtime.background_video[0] ? runtime.background_video : "(null)");
            fflush(stderr);
        } else {
            printf("[PROJECT] Background layer -> %s\n", runtime.background_video);
        }
    }

    project_runtime_after_start(&runtime, &ve_fg, &ve_bg);

    const char* consumer = "mapping_video_keystone";
    GpioLine* line_btn1 = gpio_request_line(GPIO_BTN1, consumer);
    GpioLine* line_btn3 = gpio_request_line(GPIO_BTN3, consumer);
    GpioLine* line_up = gpio_request_line(GPIO_UP, consumer);
    GpioLine* line_down = gpio_request_line(GPIO_DOWN, consumer);
    GpioLine* line_left = gpio_request_line(GPIO_LEFT, consumer);
    GpioLine* line_right = gpio_request_line(GPIO_RIGHT, consumer);

    Btn1Context btn1_ctx = {
        .st = &st,
        .runtime = &runtime,
        .ve_fg = &ve_fg,
        .ve_bg = &ve_bg
    };

    glClearColor(0.f, 0.f, 0.f, 1.f);
    fprintf(stderr, "[BOOT] entering main loop\n");
    fflush(stderr);

    while (keepRunning) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                keepRunning = 0;
            }
        }

        if (use_background_layer) {
            ve_update(&ve_bg);
        }
        ve_update(&ve_fg);
        project_runtime_update(&runtime, &ve_fg, &ve_bg);
        project_runtime_maintenance(&runtime, &ve_fg, &ve_bg);

        gpio_process_events(line_btn3, on_btn3_toggle_edit, &st);
        gpio_process_events(line_btn1, on_btn1_edit_or_show_action, &btn1_ctx);
        gpio_process_events(line_up, on_up, &st);
        gpio_process_events(line_down, on_down, &st);
        gpio_process_events(line_left, on_left, &st);
        gpio_process_events(line_right, on_right, &st);

        glUseProgram(program);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glClear(GL_COLOR_BUFFER_BIT);

        if (use_background_layer && ve_bg.cur.tex_inited) {
            glDisable(GL_BLEND);
            if (uAlpha >= 0) glUniform1f(uAlpha, 1.0f);
            if (uRange >= 0) glUniform1i(uRange, ve_bg.cur.video_range);
            if (u709 >= 0) glUniform1i(u709, ve_bg.cur.bt709);
            ve_bind_video_textures(&ve_bg.cur, uTexY, uTexU, uTexV, uTexA);
            glDrawElements(GL_TRIANGLES, (GLsizei)st.numIndices, GL_UNSIGNED_SHORT, 0);
        }

        if (ve_fg.cur.tex_inited) {
            if (use_background_layer) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            if (uAlpha >= 0) glUniform1f(uAlpha, 1.0f);
            if (uRange >= 0) glUniform1i(uRange, ve_fg.cur.video_range);
            if (u709 >= 0) glUniform1i(u709, ve_fg.cur.bt709);
            ve_bind_video_textures(&ve_fg.cur, uTexY, uTexU, uTexV, uTexA);
            glDrawElements(GL_TRIANGLES, (GLsizei)st.numIndices, GL_UNSIGNED_SHORT, 0);
        }

        if (ve_fg.transitioning && ve_fg.nxt.tex_inited) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            if (uAlpha >= 0) glUniform1f(uAlpha, ve_fg.blend);
            if (uRange >= 0) glUniform1i(uRange, ve_fg.nxt.video_range);
            if (u709 >= 0) glUniform1i(u709, ve_fg.nxt.bt709);

            ve_bind_video_textures(&ve_fg.nxt, uTexY, uTexU, uTexV, uTexA);
            glDrawElements(GL_TRIANGLES, (GLsizei)st.numIndices, GL_UNSIGNED_SHORT, 0);
        }

        SDL_GL_SwapWindow(window);
    }

    gpio_release_line(line_btn1);
    gpio_release_line(line_btn3);
    gpio_release_line(line_up);
    gpio_release_line(line_down);
    gpio_release_line(line_left);
    gpio_release_line(line_right);

    ve_shutdown(&ve_fg);
    ve_shutdown(&ve_bg);
    project_runtime_shutdown(&runtime);

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
