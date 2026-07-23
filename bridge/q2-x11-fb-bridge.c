#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DISPLAY ":0"
#define DEFAULT_FB "/dev/fb0"
#define DEFAULT_INPUT "/dev/input/event0"
#define DEFAULT_SPLASH "/usr/local/share/klipperscreen-q2/klipperscreen-splash.bgra"
#define DEFAULT_FPS 20
#define SPLASH_WIDTH 480U
#define SPLASH_HEIGHT 272U
#define SPLASH_MIN_MS 600LL
#define SPLASH_MAX_MS 15000LL

static volatile sig_atomic_t running = 1;

struct touch_matrix {
    int enabled;
    double x_raw_x;
    double x_raw_y;
    double x_offset;
    double y_raw_x;
    double y_raw_y;
    double y_offset;
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

static unsigned int field_mask(const struct fb_bitfield *field)
{
    if (field->length == 0)
        return 0;
    if (field->length >= 32)
        return UINT32_MAX;
    return ((1U << field->length) - 1U) << field->offset;
}

static int lowest_set_bit(unsigned long mask)
{
    int shift = 0;
    while (mask != 0 && (mask & 1UL) == 0) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static int set_bit_count(unsigned long mask)
{
    int count = 0;
    while (mask != 0) {
        count += (int)(mask & 1UL);
        mask >>= 1;
    }
    return count;
}

static uint8_t component_to_u8(uint32_t pixel, unsigned long mask)
{
    int shift;
    int bits;
    uint32_t value;
    uint32_t maximum;

    if (mask == 0)
        return 0;
    shift = lowest_set_bit(mask);
    bits = set_bit_count(mask);
    value = (pixel & mask) >> shift;
    maximum = bits >= 32 ? UINT32_MAX : ((1U << bits) - 1U);
    return (uint8_t)((value * 255U + maximum / 2U) / maximum);
}

static uint32_t u8_to_component(uint8_t value, const struct fb_bitfield *field)
{
    uint32_t maximum;
    uint32_t scaled;

    if (field->length == 0)
        return 0;
    maximum = field->length >= 32 ? UINT32_MAX : ((1U << field->length) - 1U);
    scaled = ((uint32_t)value * maximum + 127U) / 255U;
    return scaled << field->offset;
}

static int map_axis(int raw, const struct input_absinfo *axis, int pixels)
{
    long long numerator;
    int range = axis->maximum - axis->minimum;

    if (range <= 0 || pixels <= 1)
        return 0;
    if (raw < axis->minimum)
        raw = axis->minimum;
    if (raw > axis->maximum)
        raw = axis->maximum;
    numerator = (long long)(raw - axis->minimum) * (pixels - 1);
    return (int)(numerator / range);
}

static int rounded_and_clamped(double value, int pixels)
{
    int rounded = (int)(value >= 0.0 ? value + 0.5 : value - 0.5);

    if (rounded < 0)
        return 0;
    if (rounded >= pixels)
        return pixels - 1;
    return rounded;
}

static int parse_touch_matrix(const char *value, struct touch_matrix *matrix)
{
    int consumed = 0;

    if (value == NULL || *value == '\0')
        return 0;
    if (sscanf(
            value,
            " %lf , %lf , %lf , %lf , %lf , %lf %n",
            &matrix->x_raw_x,
            &matrix->x_raw_y,
            &matrix->x_offset,
            &matrix->y_raw_x,
            &matrix->y_raw_y,
            &matrix->y_offset,
            &consumed
        ) != 6 ||
        value[consumed] != '\0') {
        fprintf(stderr, "Invalid Q2_TOUCH_MATRIX: %s\n", value);
        return -1;
    }
    matrix->enabled = 1;
    fprintf(
        stderr,
        "Touch matrix: %.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n",
        matrix->x_raw_x,
        matrix->x_raw_y,
        matrix->x_offset,
        matrix->y_raw_x,
        matrix->y_raw_y,
        matrix->y_offset
    );
    return 1;
}

static void map_touch(
    int raw_x,
    int raw_y,
    const struct input_absinfo *x_axis,
    const struct input_absinfo *y_axis,
    int width,
    int height,
    const struct touch_matrix *matrix,
    int *x,
    int *y
)
{
    if (!matrix->enabled) {
        *x = map_axis(raw_x, x_axis, width);
        *y = map_axis(raw_y, y_axis, height);
        return;
    }
    *x = rounded_and_clamped(
        matrix->x_raw_x * raw_x +
        matrix->x_raw_y * raw_y +
        matrix->x_offset,
        width
    );
    *y = rounded_and_clamped(
        matrix->y_raw_x * raw_x +
        matrix->y_raw_y * raw_y +
        matrix->y_offset,
        height
    );
}

static void print_ximage_info(const XImage *image)
{
    fprintf(
        stderr,
        "X image: %dx%d, depth=%d, bpp=%d, stride=%d, masks=%08lx/%08lx/%08lx\n",
        image->width,
        image->height,
        image->depth,
        image->bits_per_pixel,
        image->bytes_per_line,
        image->red_mask,
        image->green_mask,
        image->blue_mask
    );
}

static int open_framebuffer(
    const char *path,
    int *fd,
    uint8_t **memory,
    size_t *memory_length,
    struct fb_fix_screeninfo *fixed,
    struct fb_var_screeninfo *variable
)
{
    *fd = open(path, O_RDWR | O_CLOEXEC);
    if (*fd < 0) {
        fprintf(stderr, "Cannot open framebuffer %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ioctl(*fd, FBIOGET_FSCREENINFO, fixed) < 0 ||
        ioctl(*fd, FBIOGET_VSCREENINFO, variable) < 0) {
        fprintf(stderr, "Cannot query framebuffer %s: %s\n", path, strerror(errno));
        close(*fd);
        *fd = -1;
        return -1;
    }
    if (variable->bits_per_pixel != 32) {
        fprintf(stderr, "Unsupported framebuffer bpp: %u\n", variable->bits_per_pixel);
        close(*fd);
        *fd = -1;
        return -1;
    }

    *memory_length = fixed->smem_len;
    *memory = mmap(NULL, *memory_length, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (*memory == MAP_FAILED) {
        fprintf(stderr, "Cannot map framebuffer %s: %s\n", path, strerror(errno));
        *memory = NULL;
        close(*fd);
        *fd = -1;
        return -1;
    }

    fprintf(
        stderr,
        "Framebuffer: %ux%u, virtual=%ux%u, bpp=%u, stride=%u, "
        "rgba=%u/%u %u/%u %u/%u %u/%u\n",
        variable->xres,
        variable->yres,
        variable->xres_virtual,
        variable->yres_virtual,
        variable->bits_per_pixel,
        fixed->line_length,
        variable->red.offset,
        variable->red.length,
        variable->green.offset,
        variable->green.length,
        variable->blue.offset,
        variable->blue.length,
        variable->transp.offset,
        variable->transp.length
    );
    return 0;
}

static int open_touch(
    const char *path,
    int *fd,
    struct input_absinfo *x_axis,
    struct input_absinfo *y_axis
)
{
    *fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (*fd < 0) {
        fprintf(stderr, "Cannot open touchscreen %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ioctl(*fd, EVIOCGABS(ABS_X), x_axis) < 0 ||
        ioctl(*fd, EVIOCGABS(ABS_Y), y_axis) < 0) {
        fprintf(stderr, "Cannot query touchscreen axes: %s\n", strerror(errno));
        close(*fd);
        *fd = -1;
        return -1;
    }
    fprintf(
        stderr,
        "Touchscreen: X=%d..%d, Y=%d..%d\n",
        x_axis->minimum,
        x_axis->maximum,
        y_axis->minimum,
        y_axis->maximum
    );
    return 0;
}

static void copy_frame(
    const XImage *image,
    uint8_t *framebuffer,
    const struct fb_fix_screeninfo *fixed,
    const struct fb_var_screeninfo *variable
)
{
    unsigned int width = variable->xres;
    unsigned int height = variable->yres;
    unsigned int destination_red = field_mask(&variable->red);
    unsigned int destination_green = field_mask(&variable->green);
    unsigned int destination_blue = field_mask(&variable->blue);
    unsigned int y;

    if ((unsigned int)image->width < width)
        width = (unsigned int)image->width;
    if ((unsigned int)image->height < height)
        height = (unsigned int)image->height;

    if (image->bits_per_pixel == 32 &&
        image->red_mask == destination_red &&
        image->green_mask == destination_green &&
        image->blue_mask == destination_blue) {
        for (y = 0; y < height; y++) {
            uint8_t *destination =
                framebuffer +
                (size_t)(y + variable->yoffset) * fixed->line_length +
                (size_t)variable->xoffset * 4U;
            const uint8_t *source =
                (const uint8_t *)image->data + (size_t)y * image->bytes_per_line;
            memcpy(destination, source, (size_t)width * 4U);
        }
        return;
    }

    for (y = 0; y < height; y++) {
        uint32_t *destination = (uint32_t *)(
            framebuffer +
            (size_t)(y + variable->yoffset) * fixed->line_length +
            (size_t)variable->xoffset * 4U
        );
        const uint32_t *source = (const uint32_t *)(
            (const uint8_t *)image->data + (size_t)y * image->bytes_per_line
        );
        unsigned int x;
        for (x = 0; x < width; x++) {
            uint8_t red = component_to_u8(source[x], image->red_mask);
            uint8_t green = component_to_u8(source[x], image->green_mask);
            uint8_t blue = component_to_u8(source[x], image->blue_mask);
            destination[x] =
                u8_to_component(red, &variable->red) |
                u8_to_component(green, &variable->green) |
                u8_to_component(blue, &variable->blue);
        }
    }
}

static int show_splash(
    const char *path,
    uint8_t *framebuffer,
    const struct fb_fix_screeninfo *fixed,
    const struct fb_var_screeninfo *variable
)
{
    const size_t row_bytes = SPLASH_WIDTH * 4U;
    const size_t expected_bytes = row_bytes * SPLASH_HEIGHT;
    uint8_t *pixels;
    FILE *file;
    unsigned int y;
    int trailing;

    if (variable->xres != SPLASH_WIDTH ||
        variable->yres != SPLASH_HEIGHT ||
        variable->red.offset != 16 || variable->red.length != 8 ||
        variable->green.offset != 8 || variable->green.length != 8 ||
        variable->blue.offset != 0 || variable->blue.length != 8) {
        fprintf(stderr, "Splash image does not match the framebuffer format\n");
        return -1;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Cannot open splash image %s: %s\n", path, strerror(errno));
        return -1;
    }
    pixels = malloc(expected_bytes);
    if (pixels == NULL) {
        fprintf(stderr, "Cannot allocate splash image buffer\n");
        fclose(file);
        return -1;
    }
    if (fread(pixels, 1, expected_bytes, file) != expected_bytes) {
        fprintf(stderr, "Splash image %s has the wrong size\n", path);
        free(pixels);
        fclose(file);
        return -1;
    }
    trailing = fgetc(file);
    fclose(file);
    if (trailing != EOF) {
        fprintf(stderr, "Splash image %s has trailing data\n", path);
        free(pixels);
        return -1;
    }

    for (y = 0; y < SPLASH_HEIGHT; y++) {
        uint8_t *destination =
            framebuffer +
            (size_t)(y + variable->yoffset) * fixed->line_length +
            (size_t)variable->xoffset * 4U;
        memcpy(destination, pixels + (size_t)y * row_bytes, row_bytes);
    }
    free(pixels);
    fprintf(stderr, "KlipperScreen splash displayed\n");
    return 0;
}

static int image_has_content(const XImage *image)
{
    unsigned long background = XGetPixel((XImage *)image, 0, 0);
    int background_red = component_to_u8((uint32_t)background, image->red_mask);
    int background_green = component_to_u8((uint32_t)background, image->green_mask);
    int background_blue = component_to_u8((uint32_t)background, image->blue_mask);
    int different_samples = 0;
    int y;

    /*
     * Xvfb starts as a uniform black root window. Keep the framebuffer splash
     * until GTK has painted a meaningfully varied KlipperScreen frame.
     */
    for (y = 0; y < image->height; y += 4) {
        int x;
        for (x = 0; x < image->width; x += 4) {
            unsigned long pixel = XGetPixel((XImage *)image, x, y);
            int red = component_to_u8((uint32_t)pixel, image->red_mask);
            int green = component_to_u8((uint32_t)pixel, image->green_mask);
            int blue = component_to_u8((uint32_t)pixel, image->blue_mask);
            int distance =
                abs(red - background_red) +
                abs(green - background_green) +
                abs(blue - background_blue);

            if (distance >= 36 && ++different_samples >= 60)
                return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *display_name = getenv("DISPLAY");
    const char *fb_path = getenv("Q2_FB_DEVICE");
    const char *input_path = getenv("Q2_INPUT_DEVICE");
    const char *splash_path = getenv("Q2_SPLASH_IMAGE");
    const char *fps_value = getenv("Q2_BRIDGE_FPS");
    const char *touch_matrix_value = getenv("Q2_TOUCH_MATRIX");
    int probe_only = argc == 2 && strcmp(argv[1], "--probe") == 0;
    int fps = fps_value != NULL ? atoi(fps_value) : DEFAULT_FPS;
    int frame_interval;
    Display *display = NULL;
    Window root;
    int screen;
    int xtest_event_base;
    int xtest_error_base;
    int xtest_major;
    int xtest_minor;
    XShmSegmentInfo shared = {0};
    XImage *image = NULL;
    int fb_fd = -1;
    int input_fd = -1;
    uint8_t *fb_memory = NULL;
    size_t fb_length = 0;
    struct fb_fix_screeninfo fixed = {0};
    struct fb_var_screeninfo variable = {0};
    struct input_absinfo x_axis = {0};
    struct input_absinfo y_axis = {0};
    struct touch_matrix touch_matrix = {0};
    int raw_x = 0;
    int raw_y = 0;
    int button_state = 0;
    int sent_button_state = 0;
    long long next_frame;
    long long splash_started = 0;
    int splash_visible = 0;
    int result = EXIT_FAILURE;

    if (argc > 2 || (argc == 2 && !probe_only)) {
        fprintf(stderr, "Usage: %s [--probe]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (display_name == NULL || *display_name == '\0')
        display_name = DEFAULT_DISPLAY;
    if (fb_path == NULL || *fb_path == '\0')
        fb_path = DEFAULT_FB;
    if (input_path == NULL || *input_path == '\0')
        input_path = DEFAULT_INPUT;
    if (splash_path == NULL || *splash_path == '\0')
        splash_path = DEFAULT_SPLASH;
    if (fps < 1 || fps > 60)
        fps = DEFAULT_FPS;
    if (parse_touch_matrix(touch_matrix_value, &touch_matrix) < 0)
        return EXIT_FAILURE;
    frame_interval = 1000 / fps;

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);

    display = XOpenDisplay(display_name);
    if (display == NULL) {
        fprintf(stderr, "Cannot open X display %s\n", display_name);
        goto cleanup;
    }
    if (!XShmQueryExtension(display)) {
        fprintf(stderr, "X display does not support MIT-SHM\n");
        goto cleanup;
    }
    if (!XTestQueryExtension(
            display,
            &xtest_event_base,
            &xtest_error_base,
            &xtest_major,
            &xtest_minor
        )) {
        fprintf(stderr, "X display does not support XTEST\n");
        goto cleanup;
    }

    screen = DefaultScreen(display);
    root = RootWindow(display, screen);
    image = XShmCreateImage(
        display,
        DefaultVisual(display, screen),
        (unsigned int)DefaultDepth(display, screen),
        ZPixmap,
        NULL,
        &shared,
        (unsigned int)DisplayWidth(display, screen),
        (unsigned int)DisplayHeight(display, screen)
    );
    if (image == NULL) {
        fprintf(stderr, "Cannot create shared X image\n");
        goto cleanup;
    }
    shared.shmid = shmget(
        IPC_PRIVATE,
        (size_t)image->bytes_per_line * image->height,
        IPC_CREAT | 0600
    );
    if (shared.shmid < 0) {
        fprintf(stderr, "Cannot allocate shared X image: %s\n", strerror(errno));
        goto cleanup;
    }
    shared.shmaddr = shmat(shared.shmid, NULL, 0);
    if (shared.shmaddr == (char *)-1) {
        shared.shmaddr = NULL;
        fprintf(stderr, "Cannot attach shared X image: %s\n", strerror(errno));
        goto cleanup;
    }
    image->data = shared.shmaddr;
    shared.readOnly = False;
    if (!XShmAttach(display, &shared)) {
        fprintf(stderr, "Cannot attach shared image to X display\n");
        goto cleanup;
    }
    XSync(display, False);
    print_ximage_info(image);

    if (!XShmGetImage(display, root, image, 0, 0, AllPlanes)) {
        fprintf(stderr, "Cannot capture X root window\n");
        goto cleanup;
    }
    if (probe_only) {
        fprintf(stderr, "Probe successful\n");
        result = EXIT_SUCCESS;
        goto cleanup;
    }

    if (open_framebuffer(
            fb_path,
            &fb_fd,
            &fb_memory,
            &fb_length,
            &fixed,
            &variable
        ) < 0)
        goto cleanup;
    if (open_touch(input_path, &input_fd, &x_axis, &y_axis) < 0)
        goto cleanup;

    splash_visible =
        show_splash(splash_path, fb_memory, &fixed, &variable) == 0;
    splash_started = monotonic_ms();
    next_frame = monotonic_ms();
    while (running) {
        struct pollfd input_poll = {
            .fd = input_fd,
            .events = POLLIN,
            .revents = 0,
        };
        long long now = monotonic_ms();
        int timeout = next_frame > now ? (int)(next_frame - now) : 0;
        int poll_result = poll(&input_poll, 1, timeout);

        if (poll_result < 0 && errno != EINTR) {
            fprintf(stderr, "Touchscreen poll failed: %s\n", strerror(errno));
            goto cleanup;
        }
        if (poll_result > 0 && (input_poll.revents & POLLIN)) {
            struct input_event events[32];
            ssize_t bytes;
            while ((bytes = read(input_fd, events, sizeof(events))) > 0) {
                size_t count = (size_t)bytes / sizeof(events[0]);
                size_t index;
                for (index = 0; index < count; index++) {
                    const struct input_event *event = &events[index];
                    if (event->type == EV_ABS && event->code == ABS_X)
                        raw_x = event->value;
                    else if (event->type == EV_ABS && event->code == ABS_Y)
                        raw_y = event->value;
                    else if (event->type == EV_KEY && event->code == BTN_TOUCH)
                        button_state = event->value != 0;
                    else if (event->type == EV_SYN && event->code == SYN_REPORT) {
                        int x;
                        int y;
                        map_touch(
                            raw_x,
                            raw_y,
                            &x_axis,
                            &y_axis,
                            DisplayWidth(display, screen),
                            DisplayHeight(display, screen),
                            &touch_matrix,
                            &x,
                            &y
                        );
                        XTestFakeMotionEvent(display, screen, x, y, CurrentTime);
                        if (button_state != sent_button_state) {
                            XTestFakeButtonEvent(display, 1, button_state, CurrentTime);
                            fprintf(
                                stderr,
                                "Touch %s at %d,%d (raw %d,%d)\n",
                                button_state ? "press" : "release",
                                x,
                                y,
                                raw_x,
                                raw_y
                            );
                            sent_button_state = button_state;
                        }
                        XFlush(display);
                    }
                }
            }
            if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "Touchscreen read failed: %s\n", strerror(errno));
                goto cleanup;
            }
        }

        now = monotonic_ms();
        if (now >= next_frame) {
            if (!XShmGetImage(display, root, image, 0, 0, AllPlanes)) {
                fprintf(stderr, "Cannot capture X root window\n");
                goto cleanup;
            }
            if (splash_visible) {
                long long splash_elapsed = now - splash_started;
                if ((splash_elapsed >= SPLASH_MIN_MS && image_has_content(image)) ||
                    splash_elapsed >= SPLASH_MAX_MS) {
                    copy_frame(image, fb_memory, &fixed, &variable);
                    splash_visible = 0;
                    fprintf(
                        stderr,
                        "KlipperScreen splash hidden after %lld ms\n",
                        splash_elapsed
                    );
                }
            } else {
                copy_frame(image, fb_memory, &fixed, &variable);
            }
            next_frame = now + frame_interval;
        }
    }

    result = EXIT_SUCCESS;

cleanup:
    if (display != NULL && sent_button_state) {
        XTestFakeButtonEvent(display, 1, False, CurrentTime);
        XFlush(display);
    }
    if (fb_memory != NULL)
        munmap(fb_memory, fb_length);
    if (input_fd >= 0)
        close(input_fd);
    if (fb_fd >= 0)
        close(fb_fd);
    if (display != NULL && shared.shmaddr != NULL)
        XShmDetach(display, &shared);
    if (image != NULL) {
        image->data = NULL;
        XDestroyImage(image);
    }
    if (shared.shmaddr != NULL)
        shmdt(shared.shmaddr);
    if (shared.shmid >= 0)
        shmctl(shared.shmid, IPC_RMID, NULL);
    if (display != NULL)
        XCloseDisplay(display);
    return result;
}
