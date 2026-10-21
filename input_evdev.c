// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * input_evdev.c - Linux input backend.
 *
 * Included at the end of cwkeyer.c; see platform.h for the contract.
 * Everything here is evdev: the grab state shared with the signal thread,
 * device discovery and classification for --list-devices, the tap-to-select
 * wizard behind --setup, and the thread that turns KEY_* press/release
 * events into paddle state.
 *
 * Nothing outside this file knows what a grab or an event node is; the core
 * goes through the input_backend_* entry points declared in platform.h.
 */

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "platform.h"

/* The signal thread and evdev thread serialize grab and descriptor changes. */
static pthread_mutex_t evdev_lock = PTHREAD_MUTEX_INITIALIZER;
static int evdev_fd = -1;
static int evdev_grabbed = 0;
static int grab_suspended = 0;

static void release_evdev_grab(void)
{
    pthread_mutex_lock(&evdev_lock);
    if (evdev_fd >= 0 && evdev_grabbed) {
        (void)ioctl(evdev_fd, EVIOCGRAB, 0);
        evdev_grabbed = 0;
    }
    pthread_mutex_unlock(&evdev_lock);
}

static void acquire_evdev_grab(void)
{
    pthread_mutex_lock(&evdev_lock);
    if (do_grab && !grab_suspended && evdev_fd >= 0 && !evdev_grabbed) {
        if (ioctl(evdev_fd, EVIOCGRAB, 1) == 0)
            evdev_grabbed = 1;
        else
            fprintf(stderr, "warning: EVIOCGRAB failed: %s; events may leak to desktop\n",
                    strerror(errno));
    }
    pthread_mutex_unlock(&evdev_lock);
}

/*
 * Neutral backend entry points.  The core only ever calls these, so a
 * different platform can drop exclusive access for SIGTSTP/SIGCONT and
 * describe its grab policy without the core knowing what a grab is.
 */
void input_backend_suspend(void)
{
    pthread_mutex_lock(&evdev_lock);
    grab_suspended = 1;
    pthread_mutex_unlock(&evdev_lock);

    release_evdev_grab();
}

void input_backend_resume(void)
{
    pthread_mutex_lock(&evdev_lock);
    grab_suspended = 0;
    pthread_mutex_unlock(&evdev_lock);

    acquire_evdev_grab();
}

const char *input_backend_grab_description(void)
{
    /* Whether the grab actually took is reported by acquire_evdev_grab(). */
    return do_grab ? "grab requested" : "nograb";
}

int input_backend_grab_possible(void)
{
    return 1;
}

const char *input_backend_devices_noun(void)
{
    return "/dev/input/event*";
}

const char *input_backend_unreadable_hint(void)
{
    return "check your udev rules (see the README)";
}

/*
 * Device capability bitmaps: the kernel reports one bit per key code.  The
 * classification below is a pure function of those bits, which is why it is
 * unit tested without touching a device.
 */
#define BITS_PER_ULONG (8U * sizeof(unsigned long))
#define NBITS(x) (((x) + BITS_PER_ULONG - 1U) / BITS_PER_ULONG)

static int bit_is_set(unsigned int bit, const unsigned long *array)
{
    return !!(array[bit / BITS_PER_ULONG] &
              (1UL << (bit % BITS_PER_ULONG)));
}


/*
 * A general-purpose keyboard reports a full typing keyset; a dedicated CW
 * paddle adapter normally reports only its paddle keys.  The name is used
 * only as a corroborating hint when the key bitmap is inconclusive.
 */
static device_kind_t classify_input_device(const char *name,
                                           unsigned short vendor,
                                           unsigned short product,
                                           const unsigned long *keys)
{
    int typing_keys = bit_is_set(KEY_A, keys) &&
                      bit_is_set(KEY_SPACE, keys) &&
                      bit_is_set(KEY_ENTER, keys);

    if (known_paddle_label(vendor, product) != NULL)
        return DEVICE_KIND_PADDLE;

    /* The HRS PaddleAdapter advertises a full keyboard keyset, so a strong
     * name hint must win over the capability check. */
    if (name && (strcasestr(name, "keyer") || strcasestr(name, "paddle") ||
                 strcasestr(name, "vband") || strcasestr(name, "morse")))
        return DEVICE_KIND_PADDLE;

    if (name && (strcasestr(name, "keyboard") || strcasestr(name, "kbd")))
        return DEVICE_KIND_KEYBOARD;

    if (typing_keys)
        return DEVICE_KIND_KEYBOARD;

    /* Both default paddle keys at once is the strong, reliable signal. */
    if (bit_is_set(KEY_LEFTCTRL, keys) &&
        bit_is_set(KEY_RIGHTCTRL, keys))
        return DEVICE_KIND_PADDLE;

    return DEVICE_KIND_UNKNOWN;
}

static int device_has_default_paddles(const unsigned long *keys)
{
    return bit_is_set(KEY_LEFTCTRL, keys) &&
           bit_is_set(KEY_RIGHTCTRL, keys);
}


/* The numeric suffix of /dev/input/eventN, or -1 for anything else. */
static long device_event_number(const char *path)
{
    const char *base = strrchr(path, '/');
    char *end = NULL;
    long number;

    base = base ? base + 1 : path;

    if (strncmp(base, "event", 5) != 0)
        return -1;

    number = strtol(base + 5, &end, 10);
    if (end == base + 5 || *end != '\0' || number < 0)
        return -1;

    return number;
}


/*
 * Open every /dev/input/event* and keep the descriptor for later polling.
 * The caller owns the returned array and releases it with
 * close_input_devices().
 */
int input_backend_discover(input_device_t **out, size_t *out_count)
{
    glob_t g;
    input_device_t *devices;
    size_t count = 0;
    int rc;

    *out = NULL;
    *out_count = 0;

    rc = glob("/dev/input/event*", 0, NULL, &g);
    if (rc == GLOB_NOMATCH) {
        globfree(&g);
        return 0;
    }

    if (rc != 0) {
        fprintf(stderr, "cannot list /dev/input/event*: %s\n",
                rc == GLOB_NOSPACE ? "out of memory" : "glob failed");
        return -1;
    }

    devices = calloc(g.gl_pathc ? g.gl_pathc : 1, sizeof(*devices));
    if (!devices) {
        fprintf(stderr, "out of memory listing input devices\n");
        globfree(&g);
        return -1;
    }

    for (size_t i = 0; i < g.gl_pathc; i++) {
        const char *path = g.gl_pathv[i];
        input_device_t *d = &devices[count];
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

        copy_device_id(d->id, sizeof(d->id), path);
        d->handle = (void *)(intptr_t)fd;
        d->readable = fd >= 0;

        if (fd < 0) {
            d->kind = DEVICE_KIND_UNKNOWN;
            d->errnum = errno;
            count++;
            continue;
        }

        if (ioctl(fd, EVIOCGNAME(sizeof(d->name)), d->name) < 0)
            snprintf(d->name, sizeof(d->name), "(unnamed)");

        struct input_id id;
        if (ioctl(fd, EVIOCGID, &id) == 0) {
            d->vendor = id.vendor;
            d->product = id.product;
        }
        d->order = (int)device_event_number(path);

        unsigned long keys[NBITS(KEY_MAX + 1)] = {0};
        int has_keys = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0;
        d->kind = has_keys ? classify_input_device(d->name, d->vendor,
                                                    d->product, keys)
                           : DEVICE_KIND_UNKNOWN;
        d->default_paddles = has_keys && device_has_default_paddles(keys);
        count++;
    }

    globfree(&g);
    *out = devices;
    *out_count = count;
    return 0;
}

void input_backend_close_devices(input_device_t *devices, size_t count)
{
    if (!devices)
        return;

    for (size_t i = 0; i < count; i++) {
        if (devices[i].readable)
            close((int)(intptr_t)devices[i].handle);
    }

    free(devices);
}


/*
 * Discard events already queued on the discovered devices, so a key pressed
 * before the prompt cannot select a device.
 */
void input_backend_drain(const input_device_t *devices, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!devices[i].readable)
            continue;

        for (;;) {
            struct input_event ev;
            ssize_t got = read((int)(intptr_t)devices[i].handle, &ev, sizeof(ev));

            if (got == (ssize_t)sizeof(ev))
                continue;
            if (got < 0 && errno == EINTR)
                continue;
            break;
        }
    }
}

/*
 * A paddle adapter sends keyboard-style key codes.  The BTN_* range belongs
 * to mice, touchpads, and gamepads, which are commonly present on the same
 * machine and must not be mistaken for a paddle tap.
 */
static int is_paddle_press(unsigned short code)
{
    return code < BTN_MISC;
}

/*
 * Wait for the next key press on the discovered devices. `only` is a device
 * index for the second (dah) tap, or SIZE_MAX to watch every device.
 * Autorepeat and key releases are ignored. Returns 1 on a press, 0 on
 * timeout, and -1 when there is nothing to poll or poll() fails.
 */
int input_backend_wait_press(const input_device_t *devices, size_t count,
                                 size_t only, int timeout_ms,
                                 size_t *which, int *code)
{
    struct pollfd *pfds;
    size_t *indices;
    size_t nfds = 0;
    int rc;

    if (only != SIZE_MAX && only >= count)
        return -1;

    for (size_t i = 0; i < count; i++) {
        if (devices[i].readable && (only == SIZE_MAX || i == only))
            nfds++;
    }

    if (nfds == 0)
        return -1;

    pfds = calloc(nfds, sizeof(*pfds));
    indices = calloc(nfds, sizeof(*indices));
    if (!pfds || !indices) {
        free(pfds);
        free(indices);
        return -1;
    }

    nfds = 0;
    for (size_t i = 0; i < count; i++) {
        if (!devices[i].readable || (only != SIZE_MAX && i != only))
            continue;

        pfds[nfds].fd = (int)(intptr_t)devices[i].handle;
        pfds[nfds].events = POLLIN;
        indices[nfds] = i;
        nfds++;
    }

    int64_t deadline = mono_ns() + (int64_t)timeout_ms * 1000000LL;

    for (;;) {
        int64_t remaining_ns = deadline - mono_ns();
        int wait_ms;

        if (remaining_ns <= 0) {
            free(pfds);
            free(indices);
            return 0;
        }

        wait_ms = (int)((remaining_ns + 999999LL) / 1000000LL);

        rc = poll(pfds, nfds, wait_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            free(pfds);
            free(indices);
            return -1;
        }

        if (rc == 0) {
            free(pfds);
            free(indices);
            return 0;
        }

        for (size_t i = 0; i < nfds; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
                continue;

            for (;;) {
                struct input_event ev;
                ssize_t got = read(pfds[i].fd, &ev, sizeof(ev));

                if (got == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.value == 1 &&
                        is_paddle_press(ev.code)) {
                        *which = indices[i];
                        *code = (int)ev.code;
                        free(pfds);
                        free(indices);
                        return 1;
                    }
                    continue;
                }

                if (got < 0 && errno == EINTR)
                    continue;
                if (got < 0 && errno == EAGAIN)
                    break;

                /* EOF or a real read error: stop polling this device. */
                pfds[i].fd = -1;
                break;
            }

            if (pfds[i].fd >= 0 &&
                (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)))
                pfds[i].fd = -1;
        }

        /* Only non-press events arrived; keep waiting until the deadline. */
    }
}

/*
 * Wait for `code` to be released on `device`.  A paddle adapter may keep
 * sending presses or autorepeat while a lever is held; the second tap must
 * not start until the first lever is physically open.
 */
int input_backend_wait_release(const input_device_t *device, int code,
                                int timeout_ms)
{
    int64_t deadline;
    struct pollfd pfd;

    if (!device->readable)
        return -1;

    deadline = mono_ns() + (int64_t)timeout_ms * 1000000LL;
    pfd.fd = (int)(intptr_t)device->handle;
    pfd.events = POLLIN;

    for (;;) {
        int64_t remaining_ns = deadline - mono_ns();
        int wait_ms;
        int rc;

        if (remaining_ns <= 0)
            return 0;

        pfd.revents = 0;
        wait_ms = (int)((remaining_ns + 999999LL) / 1000000LL);
        rc = poll(&pfd, 1, wait_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            return 0;

        for (;;) {
            struct input_event ev;
            ssize_t got = read((int)(intptr_t)device->handle, &ev, sizeof(ev));

            if (got == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY && (int)ev.code == code &&
                    ev.value == 0)
                    return 1;
                continue;
            }
            if (got < 0 && errno == EINTR)
                continue;
            if (got < 0 && errno == EAGAIN)
                break;

            return -1;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
    }
}





/*
 * Apply one evdev event. `resync` is non-zero between SYN_DROPPED and the
 * next SYN_REPORT, while the kernel's view of the device is unknown. Return
 * non-zero when a SYN_REPORT closes a resync, so the caller can re-read the
 * device state.
 */
static int evdev_handle_event(const struct input_event *ev, int *resync)
{
    if (*resync) {
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
            *resync = 0;
            return 1;
        }
        return 0;
    }

    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
        /* Event state is unreliable until the next report. */
        clear_input_state();
        *resync = 1;
        return 0;
    }

    /* ev.value: 0 = release, 1 = press, 2 = autorepeat (ignored). */
    if (ev->type == EV_KEY && (ev->value == 0 || ev->value == 1))
        set_key_state((int)ev->code, ev->value);

    return 0;
}

static void warn_if_key_not_supported(int fd, int key_code, const char *which)
{
    unsigned long supported[NBITS(KEY_MAX + 1)] = {0};

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(supported)), supported) < 0) {
        fprintf(stderr,
                "warning: EVIOCGBIT failed while checking %s key support: %s\n",
                which, strerror(errno));
        return;
    }

    if (key_code < 0 || key_code > KEY_MAX ||
        !bit_is_set((unsigned int)key_code, supported)) {
        fprintf(stderr,
                "warning: selected %s key code %d does not appear to be "
                "reported by this device\n",
                which, key_code);
    }
}

static void initialize_paddle_state_from_device(int fd)
{
    unsigned long keys[NBITS(KEY_MAX + 1)] = {0};

    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
        fprintf(stderr,
                "warning: EVIOCGKEY failed; initial paddle state unknown: %s\n",
                strerror(errno));
        return;
    }

    ASTORE(dit_paddle, bit_is_set((unsigned int)dit_code, keys));
    ASTORE(dah_paddle, bit_is_set((unsigned int)dah_code, keys));

    /*
     * Do not set edge latches here. If a paddle is already held at startup,
     * the live state is sufficient.
     */
    ASTORE(dit_edge, 0);
    ASTORE(dah_edge, 0);
}

/*
 * Reads evdev keyboard events and updates paddle state.
 */
void *input_backend_thread(void *arg)
{
    const char *path = (const char *)arg;
    int fd;
    int fatal = 0;
    int resync = 0;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        request_stop(1);
        return NULL;
    }

    pthread_mutex_lock(&evdev_lock);
    evdev_fd = fd;
    pthread_mutex_unlock(&evdev_lock);

    warn_if_key_not_supported(fd, dit_code, "dit");
    warn_if_key_not_supported(fd, dah_code, "dah");
    initialize_paddle_state_from_device(fd);

    acquire_evdev_grab();

    while (keep_running()) {
        struct pollfd pfd;
        int pr;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        pr = poll(&pfd, 1, 100);

        if (pr < 0) {
            if (errno == EINTR)
                continue;

            fprintf(stderr, "poll(%s): %s\n", path, strerror(errno));
            fatal = 1;
            break;
        }

        if (pr == 0)
            continue;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr,
                    "input device %s disappeared or became invalid "
                    "(revents=0x%x)\n",
                    path, pfd.revents);
            fatal = 1;
            break;
        }

        if (pfd.revents & POLLIN) {
            for (;;) {
                struct input_event ev;
                ssize_t n = read(fd, &ev, sizeof(ev));

                if (n == (ssize_t)sizeof(ev)) {
                    if (evdev_handle_event(&ev, &resync))
                        initialize_paddle_state_from_device(fd);
                    continue;
                }

                if (n < 0) {
                    /* EWOULDBLOCK is the same value as EAGAIN on Linux. */
                    if (errno == EAGAIN)
                        break;

                    if (errno == EINTR)
                        continue;

                    fprintf(stderr, "read(%s): %s\n", path, strerror(errno));
                    fatal = 1;
                    goto out;
                }

                if (n == 0) {
                    fprintf(stderr, "read(%s): EOF\n", path);
                    fatal = 1;
                    goto out;
                }

                fprintf(stderr,
                        "short read from %s: got %zd bytes, expected %zu\n",
                        path, n, sizeof(ev));
                fatal = 1;
                goto out;
            }
        }
    }

out:
    clear_input_state();

    pthread_mutex_lock(&evdev_lock);
    if (evdev_grabbed)
        (void)ioctl(fd, EVIOCGRAB, 0);
    evdev_grabbed = 0;
    evdev_fd = -1;
    close(fd);
    pthread_mutex_unlock(&evdev_lock);

    if (fatal)
        request_stop(1);

    return NULL;
}

void input_backend_usage(FILE *out, usage_part_t part, const char *prog)
{
    switch (part) {
    case USAGE_DEVICE_ARG:
        fputs("/dev/input/eventX", out);
        break;

    case USAGE_EXAMPLES:
        fprintf(out,
            "  %s /dev/input/eventX\n"
            "  %s /dev/input/eventX 25 700\n"
            "  %s /dev/input/eventX 25 700 COMMA DOT\n"
            "  %s /dev/input/eventX 25 700 COMMA DOT nograb\n"
            "  %s /dev/input/eventX nograb\n"
            "  %s /dev/input/event3 nograb decode\n"
            "  %s /dev/input/eventX straight\n",
            prog, prog, prog, prog, prog, prog, prog);
        break;

    case USAGE_KEYNOTE:
        fputs("You may also use KEY_ prefixes, e.g. KEY_LEFTCTRL, or raw numeric\n"
              "evdev key codes.\n", out);
        break;

    case USAGE_KEYWORDS:
        /* Mic mixing is cw-mic-mix.sh's job on Linux, not a keyword. */
        break;

    case USAGE_DEVICE_OPTS:
        fputs("            List readable /dev/input/event* devices, grouped as\n"
              "            unknown, keyboard, or paddle, and exit. Colors are\n"
              "            used only on a terminal; set NO_COLOR to disable them.\n"
              "  --setup   Choose the paddle device by tapping the levers, then\n"
              "            start with the default speed, pitch, level, and\n"
              "            weighting. Known paddle adapters skip the tap test. A\n"
              "            device that looks like a keyboard defaults to nograb.\n",
              out);
        break;

    default:
        break;
    }
}
