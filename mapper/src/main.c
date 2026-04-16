#include "common.h"
#include "app_state.h"
#include "gpio_helpers.h"
#include "input_actions.h"
#include "playlist.h"
#include "project_profile.h"
#include "shaders.h"
#include "video_engine.h"

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <gst/gst.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    MODE_CORE_RANDOM = 0,
    MODE_FREUND = 1,
    MODE_OMAHEN_8BALL = 2
} ProjectMode;

typedef enum {
    FREUND_STATE_BACKGROUND = 0,
    FREUND_STATE_TRANSITION = 1,
    FREUND_STATE_LOOP = 2
} FreundState;

typedef enum {
    OMAHEN_STATE_IDLE = 0,
    OMAHEN_STATE_TRANSITION = 1,
    OMAHEN_STATE_ANSWER = 2
} OmahenState;

typedef struct {
    int step;
    const char* transition_path;
    const char* loop_path;
} FreundPair;

typedef struct {
    Playlist background;
    Playlist loops;
    Playlist transitions;
    FreundPair* pairs;
    int pair_count;
    int active_pair_idx;
    FreundState state;
} FreundShow;

typedef struct {
    Playlist idle;
    Playlist transitions;
    Playlist answers;
    OmahenState state;
    char pending_answer[1024];
} OmahenShow;

typedef struct {
    ProjectMode mode;
    char media_root[1024];
    Playlist core_playlist;
    FreundShow freund;
    OmahenShow omahen;
} ShowContext;

typedef struct {
    AppState* st;
    ShowContext* show;
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

static const char* path_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

static int path_is_dir(const char* path)
{
    struct stat st;
    return (path && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static int find_videos_under_root(const char* root, char* out, size_t out_sz)
{
    char candidate[1024];

    snprintf(candidate, sizeof(candidate), "%s/videos", root);
    if (path_is_dir(candidate)) {
        snprintf(out, out_sz, "%s", candidate);
        return 1;
    }

    snprintf(candidate, sizeof(candidate), "%s/raspberryPi-video-mapper/videos", root);
    if (path_is_dir(candidate)) {
        snprintf(out, out_sz, "%s", candidate);
        return 1;
    }

    return 0;
}

static int resolve_usb_label_device(const char* label, char* out_dev, size_t out_dev_sz)
{
    char by_label[1024];
    char resolved[PATH_MAX];

    if (!label || !label[0]) {
        return 0;
    }

    snprintf(by_label, sizeof(by_label), "/dev/disk/by-label/%s", label);
    if (!realpath(by_label, resolved)) {
        return 0;
    }

    snprintf(out_dev, out_dev_sz, "%s", resolved);
    return 1;
}

static int fs_source_matches_device(const char* fs_source, const char* wanted_dev)
{
    char resolved[PATH_MAX];

    if (!wanted_dev || !wanted_dev[0]) {
        return 1;
    }

    if (strcmp(fs_source, wanted_dev) == 0) {
        return 1;
    }

    if (realpath(fs_source, resolved) && strcmp(resolved, wanted_dev) == 0) {
        return 1;
    }

    return 0;
}

static int run_cmd_wait(char* const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return 0;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int find_mount_for_device(const char* wanted_dev, char* out_mount, size_t out_mount_sz)
{
    FILE* mounts = setmntent("/proc/mounts", "r");
    if (!mounts) {
        return 0;
    }

    int found = 0;
    struct mntent* ent;
    while ((ent = getmntent(mounts)) != NULL) {
        if (fs_source_matches_device(ent->mnt_fsname, wanted_dev)) {
            snprintf(out_mount, out_mount_sz, "%s", ent->mnt_dir);
            found = 1;
            break;
        }
    }

    endmntent(mounts);
    return found;
}

static int ensure_device_mounted(const char* wanted_dev, char* out_mount, size_t out_mount_sz)
{
    if (!wanted_dev || !wanted_dev[0]) {
        return 0;
    }

    if (find_mount_for_device(wanted_dev, out_mount, out_mount_sz)) {
        return 1;
    }

    if (mkdir("/run/mapper-usb", 0755) != 0 && errno != EEXIST) {
        return 0;
    }

    const char* base = strrchr(wanted_dev, '/');
    base = base ? (base + 1) : wanted_dev;

    char mount_point[1024];
    snprintf(mount_point, sizeof(mount_point), "/run/mapper-usb/%s", base);
    if (mkdir(mount_point, 0755) != 0 && errno != EEXIST) {
        return 0;
    }

    char* cmd1[] = { "mount", (char*)wanted_dev, mount_point, NULL };
    if (!run_cmd_wait(cmd1)) {
        char* cmd2[] = { "mount", "-o", "ro", (char*)wanted_dev, mount_point, NULL };
        if (!run_cmd_wait(cmd2)) {
            return 0;
        }
    }

    return find_mount_for_device(wanted_dev, out_mount, out_mount_sz);
}

static int parse_step_prefix(const char* path, int* out_step)
{
    const char* name = path_basename(path);
    if (!name || !isdigit((unsigned char)name[0])) {
        return 0;
    }

    long value = 0;
    int i = 0;
    while (name[i] && isdigit((unsigned char)name[i])) {
        value = (value * 10) + (name[i] - '0');
        if (value > 1000000) {
            return 0;
        }
        i++;
    }

    if (name[i] != '-') {
        return 0;
    }

    *out_step = (int)value;
    return 1;
}

static int freund_pair_cmp(const void* a, const void* b)
{
    const FreundPair* pa = (const FreundPair*)a;
    const FreundPair* pb = (const FreundPair*)b;
    return pa->step - pb->step;
}

static ProjectMode project_mode_from_profile(void)
{
    if (strcasecmp(PROJECT_PROFILE, "freund") == 0) {
        return MODE_FREUND;
    }
    if (strcasecmp(PROJECT_PROFILE, "omahen") == 0 ||
        strcasecmp(PROJECT_PROFILE, "omahen-8ball") == 0 ||
        strcasecmp(PROJECT_PROFILE, "8ball") == 0) {
        return MODE_OMAHEN_8BALL;
    }
    return MODE_CORE_RANDOM;
}

static const char* project_mode_name(ProjectMode mode)
{
    switch (mode) {
    case MODE_FREUND: return "freund";
    case MODE_OMAHEN_8BALL: return "omahen-8ball";
    default: return "core";
    }
}

static int detect_media_root(char* out, size_t out_sz)
{
    const char* env_root = getenv("MAPPER_MEDIA_ROOT");
    const char* usb_label = getenv("MAPPER_USB_LABEL");
    char wanted_usb_dev[PATH_MAX];
    char mounted_dir[1024];
    wanted_usb_dev[0] = '\0';
    mounted_dir[0] = '\0';

    if (env_root && env_root[0] && path_is_dir(env_root)) {
        snprintf(out, out_sz, "%s", env_root);
        return 1;
    }

    if (usb_label && usb_label[0]) {
        if (resolve_usb_label_device(usb_label, wanted_usb_dev, sizeof(wanted_usb_dev))) {
            printf("[MEDIA] USB label filter: %s -> %s\n", usb_label, wanted_usb_dev);
            if (ensure_device_mounted(wanted_usb_dev, mounted_dir, sizeof(mounted_dir))) {
                printf("[MEDIA] USB mounted at: %s\n", mounted_dir);
                if (find_videos_under_root(mounted_dir, out, out_sz)) {
                    return 1;
                }
            } else {
                printf("[MEDIA] Failed to auto-mount %s (need root privileges?)\n", wanted_usb_dev);
            }
        } else {
            printf("[MEDIA] USB label '%s' is not currently resolvable\n", usb_label);
        }
    }

    FILE* mounts = setmntent("/proc/mounts", "r");
    if (mounts) {
        struct mntent* ent;
        while ((ent = getmntent(mounts)) != NULL) {
            if (wanted_usb_dev[0] && !fs_source_matches_device(ent->mnt_fsname, wanted_usb_dev)) {
                continue;
            }
            if (find_videos_under_root(ent->mnt_dir, out, out_sz)) {
                endmntent(mounts);
                return 1;
            }
        }
        endmntent(mounts);
    }

    const char* home = getenv("HOME");
    if (!home) {
        home = "/home/pi";
    }

    char candidate[1024];

    snprintf(candidate, sizeof(candidate), "%s", home);
    if (find_videos_under_root(candidate, out, out_sz)) {
        return 1;
    }

    snprintf(candidate, sizeof(candidate), "%s", "/opt/raspberryPi-video-mapper");
    if (find_videos_under_root(candidate, out, out_sz)) {
        return 1;
    }

    out[0] = '\0';
    return 0;
}

static int load_playlist_from_subdir(Playlist* p, const char* media_root, const char* subdir)
{
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", media_root, subdir);
    return playlist_load_from_dir(p, full);
}

static void show_context_init(ShowContext* show)
{
    memset(show, 0, sizeof(*show));
    show->mode = project_mode_from_profile();
}

static void show_context_free(ShowContext* show)
{
    playlist_free(&show->core_playlist);

    playlist_free(&show->freund.background);
    playlist_free(&show->freund.loops);
    playlist_free(&show->freund.transitions);
    free(show->freund.pairs);
    show->freund.pairs = NULL;

    playlist_free(&show->omahen.idle);
    playlist_free(&show->omahen.transitions);
    playlist_free(&show->omahen.answers);
}

static int freund_find_loop_for_step(const Playlist* loops, int step, const char** out_path)
{
    for (int i = 0; i < loops->count; i++) {
        int loop_step = 0;
        if (parse_step_prefix(loops->items[i], &loop_step) && loop_step == step) {
            *out_path = loops->items[i];
            return 1;
        }
    }
    return 0;
}

static int freund_build_pairs(FreundShow* freund)
{
    if (freund->transitions.count <= 0 || freund->loops.count <= 0) {
        return 0;
    }

    FreundPair* pairs = (FreundPair*)calloc((size_t)freund->transitions.count, sizeof(FreundPair));
    if (!pairs) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < freund->transitions.count; i++) {
        int step = 0;
        const char* loop_path = NULL;
        if (!parse_step_prefix(freund->transitions.items[i], &step)) {
            continue;
        }
        if (!freund_find_loop_for_step(&freund->loops, step, &loop_path)) {
            continue;
        }

        pairs[count].step = step;
        pairs[count].transition_path = freund->transitions.items[i];
        pairs[count].loop_path = loop_path;
        count++;
    }

    if (count <= 0) {
        free(pairs);
        return 0;
    }

    qsort(pairs, (size_t)count, sizeof(FreundPair), freund_pair_cmp);

    freund->pairs = pairs;
    freund->pair_count = count;
    freund->active_pair_idx = -1;
    freund->state = FREUND_STATE_BACKGROUND;
    return 1;
}

static int show_prepare_freund(ShowContext* show, const char** initial_video, int* initial_loop)
{
    if (!load_playlist_from_subdir(&show->freund.background, show->media_root, "freund/BACKGROUND")) {
        fprintf(stderr, "[FREUND] Missing videos in freund/BACKGROUND\n");
        return 0;
    }
    if (!load_playlist_from_subdir(&show->freund.loops, show->media_root, "freund/LOOP")) {
        fprintf(stderr, "[FREUND] Missing videos in freund/LOOP\n");
        return 0;
    }
    if (!load_playlist_from_subdir(&show->freund.transitions, show->media_root, "freund/TRANSITION")) {
        fprintf(stderr, "[FREUND] Missing videos in freund/TRANSITION\n");
        return 0;
    }
    if (!freund_build_pairs(&show->freund)) {
        fprintf(stderr, "[FREUND] No usable transition/loop pairs (n-*.mp4) found\n");
        return 0;
    }

    show->freund.active_pair_idx = 0;
    show->freund.state = FREUND_STATE_TRANSITION;
    *initial_video = show->freund.pairs[0].transition_path;
    *initial_loop = 0;

    printf("[FREUND] Loaded %d pair(s), background=%s, initial transition=%s\n",
           show->freund.pair_count,
           playlist_first(&show->freund.background) ? playlist_first(&show->freund.background) : "(null)",
           *initial_video ? *initial_video : "(null)");
    return (*initial_video != NULL);
}

static int show_prepare_omahen(ShowContext* show, const char** initial_video, int* initial_loop)
{
    if (!load_playlist_from_subdir(&show->omahen.idle, show->media_root, "omahen/IDLE")) {
        fprintf(stderr, "[OMAHEN] Missing videos in omahen/IDLE\n");
        return 0;
    }
    if (!load_playlist_from_subdir(&show->omahen.transitions, show->media_root, "omahen/TRANSITION")) {
        fprintf(stderr, "[OMAHEN] Missing videos in omahen/TRANSITION\n");
        return 0;
    }
    if (!load_playlist_from_subdir(&show->omahen.answers, show->media_root, "omahen/ANSWER")) {
        fprintf(stderr, "[OMAHEN] Missing videos in omahen/ANSWER\n");
        return 0;
    }

    *initial_video = playlist_first(&show->omahen.idle);
    *initial_loop = 1;
    show->omahen.state = OMAHEN_STATE_IDLE;
    show->omahen.pending_answer[0] = '\0';

    printf("[OMAHEN] Loaded idle=%d transition=%d answer=%d\n",
           show->omahen.idle.count,
           show->omahen.transitions.count,
           show->omahen.answers.count);
    return (*initial_video != NULL);
}

static int show_prepare_core_random(ShowContext* show, int argc, char** argv, const char** initial_video, int* initial_loop)
{
    if (argc >= 2 && argv[1] && argv[1][0]) {
        *initial_video = argv[1];
        *initial_loop = 1;
    }

    if (show->media_root[0]) {
        (void)playlist_load_from_dir(&show->core_playlist, show->media_root);
    }

    if (!*initial_video) {
        *initial_video = playlist_first(&show->core_playlist);
        *initial_loop = 1;
    }

    if (!*initial_video) {
        fprintf(stderr, "[CORE] No initial video provided and no videos found in media root\n");
        return 0;
    }

    return 1;
}

static int show_prepare(ShowContext* show, int argc, char** argv, const char** initial_video, int* initial_loop)
{
    *initial_video = NULL;
    *initial_loop = 1;

    (void)detect_media_root(show->media_root, sizeof(show->media_root));

    if (show->media_root[0]) {
        printf("[MEDIA] root=%s\n", show->media_root);
    } else {
        printf("[MEDIA] root not found\n");
    }

    switch (show->mode) {
    case MODE_FREUND:
        if (!show->media_root[0]) {
            fprintf(stderr, "[FREUND] MAPPER_MEDIA_ROOT (or fallback videos path) is required\n");
            return 0;
        }
        return show_prepare_freund(show, initial_video, initial_loop);

    case MODE_OMAHEN_8BALL:
        if (!show->media_root[0]) {
            fprintf(stderr, "[OMAHEN] MAPPER_MEDIA_ROOT (or fallback videos path) is required\n");
            return 0;
        }
        return show_prepare_omahen(show, initial_video, initial_loop);

    case MODE_CORE_RANDOM:
    default:
        return show_prepare_core_random(show, argc, argv, initial_video, initial_loop);
    }
}

static void freund_request_pair(ShowContext* show, VideoEngine* ve, int pair_idx)
{
    if (pair_idx < 0 || pair_idx >= show->freund.pair_count) {
        return;
    }

    show->freund.active_pair_idx = pair_idx;
    show->freund.state = FREUND_STATE_TRANSITION;

    const FreundPair* pair = &show->freund.pairs[pair_idx];
    printf("[FREUND] Step %d: transition -> %s\n", pair->step, pair->transition_path);
    ve_request_transition_opts(ve, pair->transition_path, 0);
}

static void freund_request_loop_for_active(ShowContext* show, VideoEngine* ve)
{
    int idx = show->freund.active_pair_idx;
    if (idx < 0 || idx >= show->freund.pair_count) {
        return;
    }

    const FreundPair* pair = &show->freund.pairs[idx];
    show->freund.state = FREUND_STATE_LOOP;

    printf("[FREUND] Step %d: loop -> %s\n", pair->step, pair->loop_path);
    ve_request_transition_opts(ve, pair->loop_path, 1);
}

static void show_after_start(ShowContext* show, VideoEngine* ve)
{
    (void)show;
    (void)ve;
}

static void show_on_btn1_non_edit(ShowContext* show, VideoEngine* ve)
{
    if (show->mode == MODE_CORE_RANDOM) {
        if (show->core_playlist.count <= 0) {
            printf("[BTN1] RANDOM requested, but playlist is empty\n");
            return;
        }

        const char* next = playlist_random(&show->core_playlist, ve->cur.path[0] ? ve->cur.path : NULL);
        printf("[BTN1] RANDOM -> %s\n", next ? next : "(null)");
        if (next) {
            ve_request_transition_opts(ve, next, 1);
        }
        return;
    }

    if (show->mode == MODE_FREUND) {
        if (show->freund.state != FREUND_STATE_LOOP || show->freund.pair_count <= 0) {
            printf("[BTN1] FREUND click ignored (state=%d)\n", (int)show->freund.state);
            return;
        }

        int next_idx = (show->freund.active_pair_idx + 1) % show->freund.pair_count;
        freund_request_pair(show, ve, next_idx);
        return;
    }

    if (show->mode == MODE_OMAHEN_8BALL) {
        if (show->omahen.state != OMAHEN_STATE_IDLE) {
            printf("[BTN1] OMAHEN click ignored (state=%d)\n", (int)show->omahen.state);
            return;
        }

        const char* transition = playlist_random(&show->omahen.transitions, NULL);
        const char* answer = playlist_random(&show->omahen.answers, NULL);
        if (!transition || !answer) {
            printf("[BTN1] OMAHEN missing transition/answer clips\n");
            return;
        }

        snprintf(show->omahen.pending_answer, sizeof(show->omahen.pending_answer), "%s", answer);
        show->omahen.state = OMAHEN_STATE_TRANSITION;
        printf("[OMAHEN] transition -> %s\n", transition);
        ve_request_transition_opts(ve, transition, 0);
        return;
    }
}

static void show_update(ShowContext* show, VideoEngine* ve)
{
    if (!ve_current_eos(ve)) {
        return;
    }

    if (show->mode == MODE_FREUND) {
        if (show->freund.state == FREUND_STATE_TRANSITION) {
            freund_request_loop_for_active(show, ve);
        }
        return;
    }

    if (show->mode == MODE_OMAHEN_8BALL) {
        if (show->omahen.state == OMAHEN_STATE_TRANSITION) {
            if (show->omahen.pending_answer[0]) {
                show->omahen.state = OMAHEN_STATE_ANSWER;
                printf("[OMAHEN] answer -> %s\n", show->omahen.pending_answer);
                ve_request_transition_opts(ve, show->omahen.pending_answer, 0);
            }
            return;
        }

        if (show->omahen.state == OMAHEN_STATE_ANSWER) {
            const char* idle = playlist_first(&show->omahen.idle);
            if (idle) {
                show->omahen.state = OMAHEN_STATE_IDLE;
                show->omahen.pending_answer[0] = '\0';
                printf("[OMAHEN] idle -> %s\n", idle);
                ve_request_transition_opts(ve, idle, 1);
            }
        }
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

    show_on_btn1_non_edit(ctx->show, ctx->ve);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "[BOOT] mapping_video_keystone starting\n");
    fprintf(stderr, "[BOOT] project profile: %s\n", PROJECT_PROFILE);
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
    st.select_mode = 0;
    st.selected_ui = 0;
    st.moveSpeed = 0.02f;

    st.corners[C_BL][0] = -1.0f; st.corners[C_BL][1] = -1.0f;
    st.corners[C_BR][0] =  1.0f; st.corners[C_BR][1] = -1.0f;
    st.corners[C_TR][0] =  1.0f; st.corners[C_TR][1] =  1.0f;
    st.corners[C_TL][0] = -1.0f; st.corners[C_TL][1] =  1.0f;
    rebuild_mesh_from_corners(&st);
    print_status(&st);

    ShowContext show;
    show_context_init(&show);

    const char* initial_video = NULL;
    int initial_loop = 1;
    if (!show_prepare(&show, argc, argv, &initial_video, &initial_loop)) {
        fprintf(stderr, "Failed to prepare show for mode '%s'\n", project_mode_name(show.mode));
        fflush(stderr);
        return 1;
    }

    VideoEngine ve_fg;
    VideoEngine ve_bg;
    int use_background_layer = (show.mode == MODE_FREUND);
    ve_init(&ve_fg);
    ve_init(&ve_bg);

#if PROJECT_DISABLE_XFADE
    ve_set_xfade_seconds(&ve_fg, 0.0f);
#endif

    if (!ve_start_current_opts(&ve_fg, initial_video, initial_loop)) {
        fprintf(stderr, "Failed to start foreground video: %s\n", initial_video ? initial_video : "(null)");
        fflush(stderr);
    }

    if (use_background_layer) {
        const char* bg_video = playlist_first(&show.freund.background);
        if (!bg_video || !ve_start_current_opts(&ve_bg, bg_video, 1)) {
            fprintf(stderr, "Failed to start background video: %s\n", bg_video ? bg_video : "(null)");
            fflush(stderr);
        } else {
            printf("[FREUND] Background layer -> %s\n", bg_video);
        }
    }

    show_after_start(&show, &ve_fg);

    const char* consumer = "mapping_video_keystone";
    GpioLine* line_btn1 = gpio_request_line(GPIO_BTN1, consumer);
    GpioLine* line_btn3 = gpio_request_line(GPIO_BTN3, consumer);
    GpioLine* line_up = gpio_request_line(GPIO_UP, consumer);
    GpioLine* line_down = gpio_request_line(GPIO_DOWN, consumer);
    GpioLine* line_left = gpio_request_line(GPIO_LEFT, consumer);
    GpioLine* line_right = gpio_request_line(GPIO_RIGHT, consumer);

    Btn1Context btn1_ctx = {
        .st = &st,
        .show = &show,
        .ve = &ve_fg
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

        ve_update(&ve_fg);
        if (use_background_layer) {
            ve_update(&ve_bg);
        }
        show_update(&show, &ve_fg);

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
    show_context_free(&show);

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
