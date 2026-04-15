#pragma once
#include "common.h"

typedef struct {
    struct gpiod_line_request* req;
    struct gpiod_edge_event_buffer* buf;
    int fd;
} GpioLine;

GpioLine* gpio_request_line(unsigned int offset, const char* consumer);
void gpio_release_line(GpioLine* l);

/* Non-blocking: calls on_press(user) for each rising edge */
void gpio_process_events(GpioLine* l, void (*on_press)(void*), void* user);
