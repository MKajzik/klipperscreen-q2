#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_INPUT "/dev/input/event0"
#define QIDI_SERVICE "qidi-client.service"
#define KLIPPERSCREEN_SERVICE "KlipperScreen.service"
#define DISPLAY_HELPER "/usr/local/sbin/q2-display-mode"

#define TOP_EDGE_Y 36
#define BOTTOM_EDGE_Y 235
#define MIN_VERTICAL_TRAVEL 180
#define MIN_GESTURE_MS 120
#define MAX_GESTURE_MS 2200
#define COOLDOWN_MS 3000

static volatile sig_atomic_t running = 1;

struct touch_state {
    int raw_x;
    int raw_y;
    int button_down;
    int tracking;
    int start_x;
    int start_y;
    int last_x;
    int last_y;
    long long started_ms;
};

enum gesture_direction {
    GESTURE_NONE = 0,
    GESTURE_UP,
    GESTURE_DOWN,
};

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static long long monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int absolute_value(int value)
{
    return value < 0 ? -value : value;
}

static int run_and_wait(
    const char *program,
    const char *arg1,
    const char *arg2,
    const char *arg3
)
{
    pid_t child = fork();
    int status;

    if (child < 0) {
        fprintf(stderr, "Cannot fork %s: %s\n", program, strerror(errno));
        return -1;
    }
    if (child == 0) {
        if (arg3 != NULL)
            execl(program, program, arg1, arg2, arg3, (char *)NULL);
        else if (arg2 != NULL)
            execl(program, program, arg1, arg2, (char *)NULL);
        else if (arg1 != NULL)
            execl(program, program, arg1, (char *)NULL);
        else
            execl(program, program, (char *)NULL);
        fprintf(stderr, "Cannot execute %s: %s\n", program, strerror(errno));
        _exit(127);
    }

    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "Cannot wait for %s: %s\n", program, strerror(errno));
            return -1;
        }
    }
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int service_is_active(const char *service)
{
    return run_and_wait("/bin/systemctl", "is-active", "--quiet", service) == 0;
}

static int switch_display(enum gesture_direction direction)
{
    const char *mode;
    const char *expected_service;

    if (direction == GESTURE_UP) {
        mode = "klipperscreen";
        expected_service = QIDI_SERVICE;
    } else {
        mode = "qidi";
        expected_service = KLIPPERSCREEN_SERVICE;
    }

    if (!service_is_active(expected_service)) {
        fprintf(
            stderr,
            "Ignoring %s swipe: %s is not active\n",
            direction == GESTURE_UP ? "up" : "down",
            expected_service
        );
        return 0;
    }

    fprintf(
        stderr,
        "Switching display to %s after full-screen %s swipe\n",
        mode,
        direction == GESTURE_UP ? "up" : "down"
    );
    return run_and_wait(DISPLAY_HELPER, mode, NULL, NULL);
}

static enum gesture_direction classify_gesture(
    const struct touch_state *touch,
    long long ended_ms
)
{
    int delta_x = touch->last_x - touch->start_x;
    int delta_y = touch->last_y - touch->start_y;
    int vertical_travel = absolute_value(delta_y);
    int horizontal_travel = absolute_value(delta_x);
    long long duration = ended_ms - touch->started_ms;

    if (duration < MIN_GESTURE_MS || duration > MAX_GESTURE_MS)
        return GESTURE_NONE;
    if (vertical_travel < MIN_VERTICAL_TRAVEL)
        return GESTURE_NONE;
    if (horizontal_travel * 4 > vertical_travel * 3)
        return GESTURE_NONE;

    fprintf(
        stderr,
        "Vertical stroke: %d,%d -> %d,%d, delta=%d,%d, duration=%lldms\n",
        touch->start_x,
        touch->start_y,
        touch->last_x,
        touch->last_y,
        delta_x,
        delta_y,
        duration
    );

    if (touch->start_y >= BOTTOM_EDGE_Y &&
        touch->last_y <= TOP_EDGE_Y &&
        delta_y < 0)
        return GESTURE_UP;

    if (touch->start_y <= TOP_EDGE_Y &&
        touch->last_y >= BOTTOM_EDGE_Y &&
        delta_y > 0)
        return GESTURE_DOWN;

    return GESTURE_NONE;
}

static int process_report(
    struct touch_state *touch,
    long long *cooldown_until_ms
)
{
    long long now = monotonic_ms();

    if (touch->button_down && !touch->tracking) {
        touch->tracking = 1;
        touch->start_x = touch->raw_x;
        touch->start_y = touch->raw_y;
        touch->last_x = touch->raw_x;
        touch->last_y = touch->raw_y;
        touch->started_ms = now;
        return 0;
    }

    if (touch->button_down && touch->tracking) {
        touch->last_x = touch->raw_x;
        touch->last_y = touch->raw_y;
        return 0;
    }

    if (!touch->button_down && touch->tracking) {
        enum gesture_direction direction;

        touch->last_x = touch->raw_x;
        touch->last_y = touch->raw_y;
        touch->tracking = 0;
        direction = classify_gesture(touch, now);
        if (direction == GESTURE_NONE || now < *cooldown_until_ms)
            return 0;

        *cooldown_until_ms = now + COOLDOWN_MS;
        if (switch_display(direction) != 0) {
            fprintf(stderr, "Display switch failed\n");
            return -1;
        }
    }
    return 0;
}

int main(void)
{
    const char *input_path = getenv("Q2_INPUT_DEVICE");
    struct touch_state touch = {
        .raw_x = -1,
        .raw_y = -1,
    };
    long long cooldown_until_ms = 0;
    int input_fd;

    if (input_path == NULL || *input_path == '\0')
        input_path = DEFAULT_INPUT;

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);

    input_fd = open(input_path, O_RDONLY | O_CLOEXEC);
    if (input_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", input_path, strerror(errno));
        return EXIT_FAILURE;
    }
    fprintf(
        stderr,
        "Q2 display gesture daemon listening on %s; "
        "bottom-to-top => KlipperScreen, top-to-bottom => QIDI\n",
        input_path
    );

    while (running) {
        struct input_event events[32];
        ssize_t bytes = read(input_fd, events, sizeof(events));
        size_t event_count;
        size_t index;

        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Input read failed: %s\n", strerror(errno));
            close(input_fd);
            return EXIT_FAILURE;
        }
        if (bytes == 0)
            continue;

        event_count = (size_t)bytes / sizeof(events[0]);
        for (index = 0; index < event_count; index++) {
            const struct input_event *event = &events[index];

            if (event->type == EV_ABS &&
                (event->code == ABS_X || event->code == ABS_MT_POSITION_X))
                touch.raw_x = event->value;
            else if (event->type == EV_ABS &&
                     (event->code == ABS_Y || event->code == ABS_MT_POSITION_Y))
                touch.raw_y = event->value;
            else if (event->type == EV_KEY && event->code == BTN_TOUCH)
                touch.button_down = event->value != 0;
            else if (event->type == EV_SYN && event->code == SYN_REPORT &&
                     touch.raw_x >= 0 && touch.raw_y >= 0)
                process_report(&touch, &cooldown_until_ms);
        }
    }

    close(input_fd);
    return EXIT_SUCCESS;
}
