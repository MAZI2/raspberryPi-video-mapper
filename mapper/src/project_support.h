#pragma once

#include "playlist.h"

int mapper_path_is_dir(const char* path);
const char* mapper_path_basename(const char* path);
int mapper_find_videos_under_root(const char* root, char* out, size_t out_sz);
int mapper_detect_media_root(char* out, size_t out_sz);
int mapper_load_playlist_from_subdir(Playlist* p, const char* media_root, const char* subdir);
