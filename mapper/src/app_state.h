#pragma once
#include "common.h"

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

void print_status(AppState* s);
void rebuild_mesh_from_corners(AppState* s);
int debounce_ok(Uint32* last_ms);
