#pragma once
#include "common.h"

struct gpiod_line* request_line_events(struct gpiod_chip* chip, unsigned int offset, const char* consumer);

// PRESS on your board = RISING edge
void process_line_events(struct gpiod_line* line, void (*on_press)(void*), void* user);
