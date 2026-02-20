#pragma once
#include "common.h"

typedef struct {
    char** items;
    int count;
    int cap;
} Playlist;

void playlist_free(Playlist* p);
int playlist_load_from_home_videos(Playlist* p, char* out_dir, size_t out_dir_sz);
const char* playlist_random(const Playlist* p, const char* avoid_path);
