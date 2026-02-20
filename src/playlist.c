#include "playlist.h"

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

void playlist_free(Playlist* p)
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

int playlist_load_from_home_videos(Playlist* p, char* out_dir, size_t out_dir_sz)
{
    memset(p, 0, sizeof(*p));

    const char* home = getenv("HOME");
    if (!home) home = "/home/pi";

    snprintf(out_dir, out_dir_sz, "%s/raspberryPi-video-mapper/videos", home);

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

const char* playlist_random(const Playlist* p, const char* avoid_path)
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
