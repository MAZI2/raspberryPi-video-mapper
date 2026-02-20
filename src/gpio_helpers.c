#include "gpio_helpers.h"

struct gpiod_line* request_line_events(struct gpiod_chip* chip, unsigned int offset, const char* consumer)
{
    struct gpiod_line* line = gpiod_chip_get_line(chip, offset);
    if (!line) { printf("gpiod: failed get GPIO %u\n", offset); return NULL; }
    if (gpiod_line_request_both_edges_events(line, consumer) < 0) {
        printf("gpiod: failed request events GPIO %u\n", offset);
        return NULL;
    }
    return line;
}

void process_line_events(struct gpiod_line* line, void (*on_press)(void*), void* user)
{
    if (!line) return;

    struct timespec timeout = {0, 0};
    while (1) {
        int ready = gpiod_line_event_wait(line, &timeout);
        if (ready <= 0) break;

        struct gpiod_line_event ev;
        if (gpiod_line_event_read(line, &ev) < 0) break;

        if (ev.event_type == GPIOD_LINE_EVENT_RISING_EDGE) {
            if (on_press) on_press(user);
        }
    }
}
