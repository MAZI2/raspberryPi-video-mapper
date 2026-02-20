// ============================================================
// mapping_video_keystone (modular build)
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
// ============================================================

#include "common.h"
#include "shaders.h"
#include "app_state.h"
#include "gpio_helpers.h"
#include "playlist.h"
#include "video_engine.h"
#include "input_actions.h"

volatile int keepRunning = 1;

void handle_sigint(int sig) { (void)sig; keepRunning = 0; }

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
