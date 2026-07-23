#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct target {
    int x;
    int y;
};

static void draw_target(
    Display *display,
    Window window,
    GC graphics,
    int width,
    int height,
    const struct target *target,
    size_t index,
    size_t total
)
{
    char message[128];

    XClearWindow(display, window);
    snprintf(
        message,
        sizeof(message),
        "Touch the crosshair (%zu/%zu)",
        index + 1,
        total
    );
    XDrawString(display, window, graphics, 145, 24, message, (int)strlen(message));
    XDrawLine(
        display,
        window,
        graphics,
        target->x - 18,
        target->y,
        target->x + 18,
        target->y
    );
    XDrawLine(
        display,
        window,
        graphics,
        target->x,
        target->y - 18,
        target->x,
        target->y + 18
    );
    XDrawArc(
        display,
        window,
        graphics,
        target->x - 10,
        target->y - 10,
        20,
        20,
        0,
        360 * 64
    );
    XDrawRectangle(display, window, graphics, 0, 0, width - 1, height - 1);
    XFlush(display);
}

int main(void)
{
    static const struct target targets[] = {
        {40, 40},
        {440, 40},
        {440, 232},
        {40, 232},
        {240, 136},
    };
    const char *display_name = getenv("DISPLAY");
    const char *input_path = getenv("Q2_INPUT_DEVICE");
    Display *display;
    int screen;
    int width;
    int height;
    Window root;
    Window window;
    XSetWindowAttributes attributes;
    GC graphics;
    int input_fd;
    int raw_x = 0;
    int raw_y = 0;
    int touch_started = 0;
    size_t target_index = 0;

    if (display_name == NULL)
        display_name = ":0";
    if (input_path == NULL)
        input_path = "/dev/input/event0";

    display = XOpenDisplay(display_name);
    if (display == NULL) {
        fprintf(stderr, "Cannot open X display %s\n", display_name);
        return EXIT_FAILURE;
    }

    screen = DefaultScreen(display);
    width = DisplayWidth(display, screen);
    height = DisplayHeight(display, screen);
    root = RootWindow(display, screen);
    attributes.override_redirect = True;
    window = XCreateWindow(
        display,
        root,
        0,
        0,
        (unsigned int)width,
        (unsigned int)height,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWOverrideRedirect,
        &attributes
    );
    XStoreName(display, window, "Q2 touchscreen calibration");
    XMapRaised(display, window);
    graphics = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, graphics, WhitePixel(display, screen));

    input_fd = open(input_path, O_RDONLY | O_CLOEXEC);
    if (input_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", input_path, strerror(errno));
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

    draw_target(
        display,
        window,
        graphics,
        width,
        height,
        &targets[target_index],
        target_index,
        sizeof(targets) / sizeof(targets[0])
    );

    while (target_index < sizeof(targets) / sizeof(targets[0])) {
        struct input_event events[32];
        ssize_t bytes = read(input_fd, events, sizeof(events));
        size_t event_index;
        size_t event_count;

        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Input read failed: %s\n", strerror(errno));
            close(input_fd);
            XCloseDisplay(display);
            return EXIT_FAILURE;
        }

        event_count = (size_t)bytes / sizeof(events[0]);
        for (event_index = 0; event_index < event_count; event_index++) {
            const struct input_event *event = &events[event_index];
            if (event->type == EV_ABS && event->code == ABS_X)
                raw_x = event->value;
            else if (event->type == EV_ABS && event->code == ABS_Y)
                raw_y = event->value;
            else if (event->type == EV_KEY && event->code == BTN_TOUCH) {
                if (event->value != 0) {
                    touch_started = 1;
                } else if (touch_started) {
                    printf(
                        "CAL target=%d,%d raw=%d,%d\n",
                        targets[target_index].x,
                        targets[target_index].y,
                        raw_x,
                        raw_y
                    );
                    fflush(stdout);
                    touch_started = 0;
                    target_index++;
                    if (target_index < sizeof(targets) / sizeof(targets[0])) {
                        {
                            struct timespec pause_time = {
                                .tv_sec = 0,
                                .tv_nsec = 250000000L,
                            };
                            nanosleep(&pause_time, NULL);
                        }
                        draw_target(
                            display,
                            window,
                            graphics,
                            width,
                            height,
                            &targets[target_index],
                            target_index,
                            sizeof(targets) / sizeof(targets[0])
                        );
                    }
                }
            }
        }
    }

    XClearWindow(display, window);
    XDrawString(display, window, graphics, 170, 136, "Calibration complete", 20);
    XFlush(display);
    sleep(2);

    close(input_fd);
    XFreeGC(display, graphics);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return EXIT_SUCCESS;
}
