#pragma once
#include "common.h"
#include "app_state.h"

void on_btn3_toggle_edit(void* u);
void on_btn2_toggle_select_move(void* u);
void on_btn1_cycle_corner_only(void* u);

void on_up(void* u);
void on_down(void* u);
void on_left(void* u);
void on_right(void* u);

void move_selected_corner(AppState* s, float dx, float dy);
