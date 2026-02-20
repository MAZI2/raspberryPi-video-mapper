#include "gpio_helpers.h"
#include <poll.h>

GpioLine* gpio_request_line(unsigned int offset, const char* consumer)
{
    struct gpiod_line_settings* settings = gpiod_line_settings_new();
    struct gpiod_line_config* config = gpiod_line_config_new();
    struct gpiod_request_config* rconfig = gpiod_request_config_new();

    if (!settings || !config || !rconfig) return NULL;

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_RISING);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

    gpiod_line_config_add_line_settings(config, &offset, 1, settings);
    gpiod_request_config_set_consumer(rconfig, consumer);

    struct gpiod_chip* chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) return NULL;

    struct gpiod_line_request* req =
        gpiod_chip_request_lines(chip, rconfig, config);

    gpiod_chip_close(chip);

    if (!req) return NULL;

    GpioLine* l = calloc(1, sizeof(GpioLine));
    l->req = req;
    l->fd = gpiod_line_request_get_fd(req);
    l->buf = gpiod_edge_event_buffer_new(8);

    return l;
}

void gpio_release_line(GpioLine* l)
{
    if (!l) return;
    if (l->buf) gpiod_edge_event_buffer_free(l->buf);
    if (l->req) gpiod_line_request_release(l->req);
    free(l);
}

void gpio_process_events(GpioLine* l, void (*on_press)(void*), void* user)
{
    if (!l) return;

    struct pollfd pfd = {
        .fd = l->fd,
        .events = POLLIN
    };

    int ret = poll(&pfd, 1, 0);
    if (ret <= 0) return;

    int n = gpiod_line_request_read_edge_events(l->req, l->buf, 8);
    for (int i = 0; i < n; i++) {
        struct gpiod_edge_event* ev =
            gpiod_edge_event_buffer_get_event(l->buf, i);

        if (gpiod_edge_event_get_event_type(ev)
            == GPIOD_EDGE_EVENT_RISING_EDGE) {
            if (on_press) on_press(user);
        }
    }
}
