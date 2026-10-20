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
    return do_grab ? "EVIOCGRAB enabled" : "nograb";
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
 * Input-device discovery for --list-devices (and, later, the setup wizard).
 *
 * classify_input_device() is deliberately a pure function of the device
 * name and its evdev key bitmap so it can be unit tested; the syscall layer
 * around it stays thin.  The classification is a hint, never a hard policy:
 * the user can always override it.
 */
typedef enum {
    DEVICE_KIND_UNKNOWN = 0,
    DEVICE_KIND_KEYBOARD,
    DEVICE_KIND_PADDLE
} device_kind_t;

static const char *device_kind_name(device_kind_t kind)
{
    switch (kind) {
    case DEVICE_KIND_KEYBOARD:
        return "keyboard";
    case DEVICE_KIND_PADDLE:
        return "paddle";
    case DEVICE_KIND_UNKNOWN:
    default:
        return "unknown";
    }
}

/*
 * Known USB paddle adapters, matched by vendor/product ID.  Add new
 * devices here; the label is shown by --list-devices and in the setup
 * summary.
 */
typedef struct {
    unsigned short vendor;
    unsigned short product;
    int dit_code;
    int dah_code;
    const char *label;
} known_paddle_t;

static const known_paddle_t known_paddles[] = {
    /* hamradio.solutions / HRS PaddleAdapter (VBand). */
    {0x04d8, 0x01a6, CWKEY_LEFTCTRL, CWKEY_RIGHTCTRL, "HRS PaddleAdapter"},
};

static const known_paddle_t *known_paddle_by_id(unsigned short vendor,
                                                unsigned short product)
{
    for (size_t i = 0; i < sizeof(known_paddles) / sizeof(known_paddles[0]);
         i++) {
        if (known_paddles[i].vendor == vendor &&
            known_paddles[i].product == product)
            return &known_paddles[i];
    }

    return NULL;
}

static const char *known_paddle_label(unsigned short vendor,
                                      unsigned short product)
{
    const known_paddle_t *known = known_paddle_by_id(vendor, product);

    return known ? known->label : NULL;
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

/* One row per device, printed under its group header. */
static void print_device_line(FILE *out, const char *path, const char *name,
                              int default_paddles, const char *known_label)
{
    fprintf(out, "  %s  \"%s\"%s",
            path, (name && *name) ? name : "(unnamed)",
            default_paddles ?
                "  (default paddle keys: LEFTCTRL RIGHTCTRL)" : "");
    if (known_label)
        fprintf(out, "  (known: %s)", known_label);
    fputc('\n', out);
}

static void print_unreadable_line(FILE *out, const char *path, int errnum)
{
    fprintf(out, "  %s  [unreadable: %s]\n", path, strerror(errnum));
}

static int stream_supports_color(FILE *out)
{
    const char *term;

    if (getenv("NO_COLOR") != NULL)
        return 0;

    term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0)
        return 0;

    return isatty(fileno(out)) == 1;
}

static const char *device_kind_color(device_kind_t kind, int use_color)
{
    if (!use_color)
        return "";

    switch (kind) {
    case DEVICE_KIND_PADDLE:
        return "\033[1;32m";   /* bold green */
    case DEVICE_KIND_KEYBOARD:
        return "\033[36m";     /* cyan */
    case DEVICE_KIND_UNKNOWN:
    default:
        return "\033[2m";      /* dim */
    }
}

static void print_device_group(FILE *out, device_kind_t kind, int use_color)
{
    fputs(device_kind_color(kind, use_color), out);
    fputs(device_kind_name(kind), out);
    if (use_color)
        fputs("\033[0m", out);
    fputc('\n', out);
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

typedef struct {
    char path[64];
    char name[256];
    unsigned short vendor;
    unsigned short product;
    device_kind_t kind;
    int default_paddles;
    int fd;              /* -1 when the device could not be opened */
    int unreadable;
    int errnum;
} input_device_t;

static void copy_device_path(char *dest, size_t size, const char *path)
{
    size_t len = strlen(path);

    if (len >= size)
        len = size - 1;

    memcpy(dest, path, len);
    dest[len] = '\0';
}

/*
 * Open every /dev/input/event* and keep the descriptor for later polling.
 * The caller owns the returned array and releases it with
 * close_input_devices().
 */
static int discover_input_devices(input_device_t **out, size_t *out_count)
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

        copy_device_path(d->path, sizeof(d->path), path);
        d->fd = fd;

        if (fd < 0) {
            d->kind = DEVICE_KIND_UNKNOWN;
            d->unreadable = 1;
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

static void close_input_devices(input_device_t *devices, size_t count)
{
    if (!devices)
        return;

    for (size_t i = 0; i < count; i++) {
        if (devices[i].fd >= 0)
            close(devices[i].fd);
    }

    free(devices);
}

/*
 * Find the single known paddle adapter in the discovered list.  More than
 * one known adapter is ambiguous, so the caller falls back to tapping.
 */
static size_t find_known_paddle(const input_device_t *devices, size_t count)
{
    size_t found = SIZE_MAX;

    for (size_t i = 0; i < count; i++) {
        if (devices[i].fd < 0)
            continue;
        if (known_paddle_by_id(devices[i].vendor, devices[i].product) == NULL)
            continue;
        if (found != SIZE_MAX)
            return SIZE_MAX;
        found = i;
    }

    return found;
}

/*
 * Discard events already queued on the discovered devices, so a key pressed
 * before the prompt cannot select a device.
 */
static void drain_device_events(const input_device_t *devices, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (devices[i].fd < 0)
            continue;

        for (;;) {
            struct input_event ev;
            ssize_t got = read(devices[i].fd, &ev, sizeof(ev));

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
static int wait_for_paddle_press(const input_device_t *devices, size_t count,
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
        if (devices[i].fd >= 0 && (only == SIZE_MAX || i == only))
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
        if (devices[i].fd < 0 || (only != SIZE_MAX && i != only))
            continue;

        pfds[nfds].fd = devices[i].fd;
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
static int wait_for_key_release(const input_device_t *device, int code,
                                int timeout_ms)
{
    int64_t deadline;
    struct pollfd pfd;

    if (device->fd < 0)
        return -1;

    deadline = mono_ns() + (int64_t)timeout_ms * 1000000LL;
    pfd.fd = device->fd;
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
            ssize_t got = read(device->fd, &ev, sizeof(ev));

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

/* Setup output styling.  Colors are dropped when stderr is not a terminal. */
#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_DIM    "\033[2m"
#define ANSI_GREEN  "\033[1;32m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_TITLE  "\033[1;36m"
#define CLR(use, code) ((use) ? (code) : "")

#define SETUP_RELEASE_TIMEOUT_MS 5000
#define SETUP_TAP_TIMEOUT_MS 15000

static void flush_stdin(void)
{
    if (isatty(STDIN_FILENO))
        (void)tcflush(STDIN_FILENO, TCIFLUSH);
}

/*
 * Read one yes/no answer without stdio buffering.  This keeps the answer
 * logic testable through a pipe and leaves stdin ready for the runtime
 * control thread.
 */
static int prompt_yes_no(const char *question, int default_yes)
{
    char line[64];
    int use_color = stream_supports_color(stderr);

    flush_stdin();

    for (;;) {
        size_t len = 0;

        fprintf(stderr, "%s%s%s [%s%s%s] ",
                CLR(use_color, ANSI_BOLD), question, CLR(use_color, ANSI_RESET),
                CLR(use_color, ANSI_DIM), default_yes ? "Y/n" : "y/N",
                CLR(use_color, ANSI_RESET));
        fflush(stderr);

        for (;;) {
            char ch;
            ssize_t got = read(STDIN_FILENO, &ch, 1);

            if (got == 1) {
                if (ch == '\n' || ch == '\r')
                    break;
                if (len + 1 < sizeof(line))
                    line[len++] = ch;
                continue;
            }
            if (got < 0 && errno == EINTR)
                continue;
            if (got < 0 || len == 0) {
                fputc('\n', stderr);
                return -1;
            }
            break;
        }

        line[len] = '\0';

        char *answer = line;
        while (*answer == ' ' || *answer == '\t')
            answer++;

        if (*answer == '\0')
            return default_yes;
        if (*answer == 'y' || *answer == 'Y')
            return 1;
        if (*answer == 'n' || *answer == 'N')
            return 0;

        fprintf(stderr, "%sPlease answer y or n.%s\n",
                CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET));
    }
}

/*
 * Report a detected lever the same way for both taps.
 */
static void print_detected_key(FILE *out, const input_device_t *device,
                               const char *lever, int code, int use_color)
{
    fprintf(out, "\n  %s✓%s %s key %d on %s%s%s (%s)\n",
            CLR(use_color, ANSI_GREEN), CLR(use_color, ANSI_RESET),
            lever, code,
            CLR(use_color, ANSI_BOLD), device->name, CLR(use_color, ANSI_RESET),
            device->path);
}

/*
 * Interactive device selection.  Tap the two levers to learn both key codes
 * and the device, then confirm the grab policy.  Speed, pitch, level, and
 * weighting keep their normal defaults and runtime controls.
 */
int input_backend_setup(char *device_path, size_t path_size)
{
    input_device_t *devices = NULL;
    size_t count = 0;
    size_t selected = SIZE_MAX;
    int dit = 0;
    int dah = 0;
    int straight = 0;
    int result = -1;
    int use_color = stream_supports_color(stderr);

    if (discover_input_devices(&devices, &count) < 0)
        return -1;

    if (count == 0) {
        fprintf(stderr, "\n  %s⚠%s No /dev/input/event* devices found.\n",
                CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET));
        goto out;
    }

    fprintf(stderr, "\n  %s◆ CW keyer setup%s\n\n",
            CLR(use_color, ANSI_TITLE), CLR(use_color, ANSI_RESET));

    size_t known_index = find_known_paddle(devices, count);

    if (known_index != SIZE_MAX) {
        const known_paddle_t *adapter =
            known_paddle_by_id(devices[known_index].vendor,
                               devices[known_index].product);

        if (!adapter)
            goto out;

        selected = known_index;
        dit = adapter->dit_code;
        dah = adapter->dah_code;
        straight = dit == dah;

        fprintf(stderr,
                "  %s✓%s Known paddle adapter: %s%s%s (%s)\n"
                "    Using its default dot/dash keys (%d/%d); no tap test "
                "needed.\n",
                CLR(use_color, ANSI_GREEN), CLR(use_color, ANSI_RESET),
                CLR(use_color, ANSI_BOLD), devices[known_index].name,
                CLR(use_color, ANSI_RESET), devices[known_index].path,
                dit, dah);
    } else {
        for (;;) {
            size_t which = SIZE_MAX;
            size_t second = SIZE_MAX;
            int code = 0;
            int rc;

            drain_device_events(devices, count);
            fprintf(stderr, "  %s→%s Tap the DOT (dit) paddle, then release it.\n",
                    CLR(use_color, ANSI_CYAN), CLR(use_color, ANSI_RESET));
            rc = wait_for_paddle_press(devices, count, SIZE_MAX,
                                       SETUP_TAP_TIMEOUT_MS, &which, &code);
            if (rc < 0) {
                fprintf(stderr,
                        "\n  %s⚠%s No readable input devices; check your udev "
                        "rules (see the README).\n",
                        CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET));
                goto out;
            }
            if (rc == 0) {
                fprintf(stderr,
                        "\n  %s⚠%s No paddle press detected within %d seconds.\n",
                        CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET),
                        SETUP_TAP_TIMEOUT_MS / 1000);
                if (prompt_yes_no("Try again?", 1) != 1)
                    goto out;
                continue;
            }

            flush_stdin();
            print_detected_key(stderr, &devices[which], "DOT (dit)", code,
                               use_color);

            fprintf(stderr,
                    "\n  %s→%s Release the DOT paddle, then tap the DASH (dah) "
                    "paddle on the same device.\n",
                    CLR(use_color, ANSI_CYAN), CLR(use_color, ANSI_RESET));
            rc = wait_for_key_release(&devices[which], code,
                                      SETUP_RELEASE_TIMEOUT_MS);
            if (rc < 0) {
                fprintf(stderr,
                        "\n  %s⚠%s Cannot poll the selected input device.\n",
                        CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET));
                goto out;
            }
            if (rc == 0) {
                fprintf(stderr,
                        "\n  %s⚠%s Did not see the DOT paddle release.\n",
                        CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET));
                if (prompt_yes_no("Try again?", 1) != 1)
                    goto out;
                continue;
            }

            rc = wait_for_paddle_press(devices, count, which,
                                       SETUP_TAP_TIMEOUT_MS, &second, &dah);
            if (rc < 0) {
                fprintf(stderr,
                        "\n  %s⚠%s Cannot poll the selected input device.\n",
                        CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET));
                goto out;
            }
            if (rc == 0) {
                fprintf(stderr,
                        "\n  %s⚠%s No paddle press detected within %d seconds.\n",
                        CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET),
                        SETUP_TAP_TIMEOUT_MS / 1000);
                if (prompt_yes_no("Try again?", 1) != 1)
                    goto out;
                continue;
            }

            flush_stdin();
            print_detected_key(stderr, &devices[second], "DASH (dah)", dah,
                               use_color);
            selected = second;
            dit = code;
            straight = 0;

            if (dit == dah) {
                int straight_choice;

                fprintf(stderr,
                        "\n  %s◆%s Both taps used key %d; that looks like a "
                        "straight key.\n",
                        CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET),
                        dit);
                straight_choice = prompt_yes_no("Use straight-key mode?", 1);
                if (straight_choice < 0)
                    goto out;
                if (!straight_choice)
                    continue;
                straight = 1;
                dah = dit;
            }

            int use_it = prompt_yes_no("Use this device?", 1);
            if (use_it < 0)
                goto out;
            if (!use_it)
                continue;
            break;
        }
    }

    const char *known = known_paddle_label(devices[selected].vendor,
                                           devices[selected].product);
    fprintf(stderr, "\n  %s●%s Using %s%s%s (%s), detected as %s",
            CLR(use_color, ANSI_GREEN), CLR(use_color, ANSI_RESET),
            CLR(use_color, ANSI_BOLD), devices[selected].name,
            CLR(use_color, ANSI_RESET), devices[selected].path,
            device_kind_name(devices[selected].kind));
    if (known)
        fprintf(stderr, " (%s)", known);
    fputc('\n', stderr);

    int default_grab = devices[selected].kind == DEVICE_KIND_PADDLE;
    int grab = prompt_yes_no("Take exclusive control of this device?",
                             default_grab);
    if (grab < 0)
        goto out;

    if (grab && devices[selected].kind == DEVICE_KIND_KEYBOARD) {
        int sure;

        fprintf(stderr,
                "\n  %s⚠%s Grabbing a keyboard hides it from the desktop and "
                "terminal; Ctrl+C may not reach cwkeyer.\n",
                CLR(use_color, ANSI_YELLOW), CLR(use_color, ANSI_RESET));
        sure = prompt_yes_no("Continue with exclusive grab?", 0);
        if (sure < 0)
            goto out;
        if (!sure)
            grab = 0;
    }

    dit_code = dit;
    dah_code = dah;
    straight_mode = straight;
    do_grab = grab;
    copy_device_path(device_path, path_size, devices[selected].path);
    result = 0;

out:
    close_input_devices(devices, count);
    return result;
}

/*
 * Unknown devices first, then keyboards, then paddles, and within a group
 * in natural event-number order (event2 before event10).
 */
static int compare_input_devices(const void *a, const void *b)
{
    const input_device_t *ea = a;
    const input_device_t *eb = b;
    long na;
    long nb;

    if (ea->kind != eb->kind)
        return ea->kind < eb->kind ? -1 : 1;

    na = device_event_number(ea->path);
    nb = device_event_number(eb->path);
    if (na != nb)
        return na < nb ? -1 : 1;

    return strcmp(ea->path, eb->path);
}

int input_backend_list(FILE *out)
{
    input_device_t *devices;
    size_t count;
    int unreadable = 0;
    int current_kind = -1;
    int use_color;

    if (discover_input_devices(&devices, &count) < 0)
        return -1;

    if (count == 0) {
        fprintf(out, "no /dev/input/event* devices found\n");
        close_input_devices(devices, count);
        return 0;
    }

    use_color = stream_supports_color(out);
    qsort(devices, count, sizeof(*devices), compare_input_devices);

    for (size_t i = 0; i < count; i++) {
        if ((int)devices[i].kind != current_kind) {
            if (current_kind >= 0)
                fputc('\n', out);
            current_kind = (int)devices[i].kind;
            print_device_group(out, devices[i].kind, use_color);
        }

        if (devices[i].unreadable) {
            print_unreadable_line(out, devices[i].path, devices[i].errnum);
            unreadable++;
        } else {
            const char *known = known_paddle_label(devices[i].vendor,
                                                   devices[i].product);
            print_device_line(out, devices[i].path, devices[i].name,
                              devices[i].default_paddles, known);
        }
    }

    if (unreadable) {
        fprintf(out,
                "\n%d device(s) could not be read; check your udev rules "
                "(see the README).\n", unreadable);
    }

    close_input_devices(devices, count);
    return 0;
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
