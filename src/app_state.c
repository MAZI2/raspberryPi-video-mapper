#include "app_state.h"
#include "homography.h"

const int UI_TO_SQ_CORNER[4] = { C_TL, C_TR, C_BL, C_BR };

const char* corner_name_ui(int uiIdx) {
    switch (uiIdx) { case 0: return "TL"; case 1: return "TR"; case 2: return "BL"; case 3: return "BR"; default: return "?"; }
}

void print_status(AppState* s)
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

void rebuild_mesh_from_corners(AppState* s)
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

int debounce_ok(Uint32* last_ms)
{
    Uint32 now = SDL_GetTicks();
    if (now - *last_ms < DEBOUNCE_MS) return 0;
    *last_ms = now;
    return 1;
}
