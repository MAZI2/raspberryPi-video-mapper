#include "input_actions.h"

void on_btn3_toggle_edit(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_btn3)) return;

    s->edit_mode = !s->edit_mode;

    printf("[BTN3] EDIT %s\n", s->edit_mode ? "ON" : "OFF");
    print_status(s);
}

void move_selected_corner(AppState* s, float dx, float dy)
{
    int sq = UI_TO_SQ_CORNER[s->selected_ui];
    s->corners[sq][0] += dx;
    s->corners[sq][1] += dy;
    rebuild_mesh_from_corners(s);

    printf("[MOVE] %s dx=%.3f dy=%.3f\n", corner_name_ui(s->selected_ui), dx, dy);
    print_status(s);
}

void on_up(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_up)) return;
    if (!s->edit_mode) return;
    move_selected_corner(s, 0.0f, s->moveSpeed);
}

void on_down(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_down)) return;
    if (!s->edit_mode) return;
    move_selected_corner(s, 0.0f, -s->moveSpeed);
}

void on_left(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_left)) return;
    if (!s->edit_mode) return;
    move_selected_corner(s, -s->moveSpeed, 0.0f);
}

void on_right(void* u)
{
    AppState* s = (AppState*)u;
    if (!debounce_ok(&s->last_right)) return;
    if (!s->edit_mode) return;
    move_selected_corner(s, s->moveSpeed, 0.0f);
}
