#include "common.h"
#include "video_engine.h"
#include "shaders.h"

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <gst/gst.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

/* ================= CONFIG ================= */

#define GRID_X 16
#define GRID_Y 9

/* ================= GL helpers ================= */

static void gl_check(const char* where)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "[GL] error 0x%x at %s\n", (unsigned)e, where);
        fflush(stderr);
    }
}

/* ================= MAIN ================= */

int main(int argc, char** argv)
{
    /* Print to stderr so you ALWAYS see it even if stdout redirect/buffering is weird */
    fprintf(stderr, "[BOOT] mapping_video_keystone starting\n");
    fflush(stderr);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s /path/to/video.mp4\n", argv[0]);
        return 1;
    }

    signal(SIGINT, handle_sigint);
    srand((unsigned int)time(NULL));

    const char* initial_video = argv[1];

    /* ---------- GStreamer ---------- */
    gst_init(NULL, NULL);

    /* ---------- SDL / GLES ---------- */
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_Window* window = SDL_CreateWindow(
        "Video Mapper (baseline draw test)",
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

    /* IMPORTANT: set viewport explicitly (kmsdrm path can default to 0x0) */
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) { dw = 1920; dh = 1080; }
    glViewport(0, 0, dw, dh);

    fprintf(stderr, "Renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "Version : %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "Viewport: %dx%d\n", dw, dh);
    fflush(stderr);

    /* ---------- Shaders ---------- */
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

    /* ---------- Mesh (FULLSCREEN GRID) ---------- */
    const int numVerts   = GRID_X * GRID_Y;
    const int numIndices = (GRID_X - 1) * (GRID_Y - 1) * 6;

    float* vertices = (float*)malloc((size_t)numVerts * 4 * sizeof(float));
    GLushort* indices = (GLushort*)malloc((size_t)numIndices * sizeof(GLushort));

    if (!vertices || !indices) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    /* Fill vertices: position + texcoord */
    int v = 0;
    for (int y = 0; y < GRID_Y; y++) {
        for (int x = 0; x < GRID_X; x++) {
            float fx = (float)x / (GRID_X - 1);
            float fy = (float)y / (GRID_Y - 1);

            vertices[v++] = fx * 2.0f - 1.0f; /* aPos.x */
            vertices[v++] = fy * 2.0f - 1.0f; /* aPos.y */
            vertices[v++] = fx;               /* aTex.x */
            vertices[v++] = fy;               /* aTex.y */
        }
    }

    /* Fill indices (GL_UNSIGNED_SHORT for GLES2 portability) */
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
    glBufferData(GL_ARRAY_BUFFER,
                 (size_t)numVerts * 4 * sizeof(float),
                 vertices,
                 GL_STATIC_DRAW);

    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (size_t)numIndices * sizeof(GLushort),
                 indices,
                 GL_STATIC_DRAW);

    GLint aPos = glGetAttribLocation(program, "aPos");
    GLint aTex = glGetAttribLocation(program, "aTex");

    if (aPos < 0 || aTex < 0) {
        fprintf(stderr, "Attribs not found: aPos=%d aTex=%d\n", aPos, aTex);
        fflush(stderr);
    }

    glEnableVertexAttribArray((GLuint)aPos);
    glVertexAttribPointer((GLuint)aPos, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray((GLuint)aTex);
    glVertexAttribPointer((GLuint)aTex, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void*)(2 * sizeof(float)));

    gl_check("after vertex attrib setup");

    /* ---------- Uniforms (may be unused in your red-debug shader) ---------- */
    GLint uTexY  = glGetUniformLocation(program, "uTexY");
    GLint uTexU  = glGetUniformLocation(program, "uTexU");
    GLint uTexV  = glGetUniformLocation(program, "uTexV");
    GLint uRange = glGetUniformLocation(program, "uVideoRange");
    GLint u709   = glGetUniformLocation(program, "uBT709");
    GLint uAlpha = glGetUniformLocation(program, "uAlpha"); /* if you add it later */

    if (uTexY >= 0) glUniform1i(uTexY, 0);
    if (uTexU >= 0) glUniform1i(uTexU, 1);
    if (uTexV >= 0) glUniform1i(uTexV, 2);
    if (uRange >= 0) glUniform1i(uRange, 1);
    if (u709 >= 0)   glUniform1i(u709, 1);
    if (uAlpha >= 0) glUniform1f(uAlpha, 1.0f);

    /* Clear to a slightly gray so you can distinguish “nothing drawn” vs black */
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    /* ---------- Video engine (won’t matter until shader reads textures) ---------- */
    VideoEngine ve;
    ve_init(&ve);
    if (!ve_start_current(&ve, initial_video)) {
        fprintf(stderr, "Failed to start video: %s\n", initial_video);
        fflush(stderr);
        /* Continue anyway so we can still validate drawing */
    }

    fprintf(stderr, "[BOOT] entering main loop\n");
    fflush(stderr);

    /* ---------- Main Loop ---------- */
    while (keepRunning) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_KEYDOWN &&
            e.key.keysym.sym == SDLK_ESCAPE)
            keepRunning = 0;
    }

    ve_update(&ve);
    fprintf(stderr, "tex_inited=%d transitioning=%d\n",
            ve.cur.tex_inited, ve.transitioning);
    fflush(stderr);

    glUseProgram(program);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);

    glClear(GL_COLOR_BUFFER_BIT);

// Draw current
if (ve.cur.tex_inited) {
    glDisable(GL_BLEND);

    glUniform1f(uAlpha, 1.0f);
    glUniform1i(uRange, ve.cur.video_range);
    glUniform1i(u709,   ve.cur.bt709);

    ve_bind_video_textures(&ve.cur, uTexY, uTexU, uTexV);

    glDrawElements(GL_TRIANGLES,
                   (GLsizei)numIndices,
                   GL_UNSIGNED_SHORT,
                   0);
}
        
// Draw next on top during transition (crossfade)
if (ve.transitioning && ve.nxt.tex_inited) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (uAlpha >= 0) glUniform1f(uAlpha, ve.blend);
    if (uRange >= 0) glUniform1i(uRange, ve.nxt.video_range);
    if (u709 >= 0)   glUniform1i(u709,   ve.nxt.bt709);

    ve_bind_video_textures(&ve.nxt, uTexY, uTexU, uTexV);

    glDrawElements(GL_TRIANGLES, (GLsizei)numIndices, GL_UNSIGNED_SHORT, 0);
    gl_check("draw next");
}
                gl_check("after glDrawElements");

        SDL_GL_SwapWindow(window);
    }

    /* ---------- Cleanup ---------- */
    ve_shutdown(&ve);

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
