/*
 * cwkeyer.c — low-latency iambic CW sidetone keyer
 *
 * Reads an evdev keyboard-emulating paddle adapter and generates an ALSA
 * sidetone. Default paddle mapping:
 *
 *   KEY_LEFTCTRL  = dit
 *   KEY_RIGHTCTRL = dah
 *
 * Build:
 *   make   (or: gcc -O2 -Wall -Wextra -o cwkeyer cwkeyer.c -lasound -lpthread -lm)
 *
 * Run:
 *   ./cwkeyer /dev/input/eventX [wpm=12] [freq_hz=800] [dit_key] [dah_key] [nograb] [decode]
 *
 * Examples:
 *   ./cwkeyer /dev/input/eventX
 *   ./cwkeyer /dev/input/eventX 25 700
 *   ./cwkeyer /dev/input/eventX 25 700 COMMA DOT
 *   ./cwkeyer /dev/input/eventX 25 700 COMMA DOT nograb
 *   ./cwkeyer /dev/input/eventX nograb
 *   ./cwkeyer /dev/input/event3 nograb decode
 *
 * Environment:
 *   CW_ALSA_DEVICE=pulse            ALSA PCM to open. Default: "pulse".
 *   CW_ALSA_LATENCY_US=20000        Requested ALSA latency in microseconds.
 *   CW_KEYER_MODE=b                 Curtis iambic mode "a" or "b".
 *   PULSE_SINK=cw_out               If the device is "pulse" and PULSE_SINK is
 *                                   unset or empty, it defaults to "cw_out"
 *                                   (created by cw-mic-mix.sh). Set it
 *                                   explicitly to a real sink if needed.
 *
 * For the mic-mixing setup, the default ALSA PCM is "pulse" and the keyer
 * routes it to cw_out when PULSE_SINK is unset or empty:
 *
 *   ./cwkeyer /dev/input/eventX
 *
 * Set PULSE_SINK explicitly to override that destination, or set
 * CW_ALSA_DEVICE to select a different ALSA PCM.
 */

#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

#define ALOAD(obj) atomic_load_explicit(&(obj), memory_order_relaxed)
#define ASTORE(obj, val) atomic_store_explicit(&(obj), (val), memory_order_relaxed)

#define NS_PER_SEC 1000000000LL

enum {
    AUDIO_RATE = 48000,
    AUDIO_PERIOD_FRAMES = 128
};

static double wpm = 12.0;
static double freq_hz = 800.0;

static int dit_code = KEY_LEFTCTRL;
static int dah_code = KEY_RIGHTCTRL;
static int do_grab = 1;
static int do_decode = 0;
static int do_mode_b = 1;   /* Curtis 8044 mode B is the default */

static unsigned int alsa_latency_us = 20000;

static atomic_int dit_paddle = 0;
static atomic_int dah_paddle = 0;

/*
 * Edge latches prevent very short key-down/key-up pulses from being missed
 * between keyer polling intervals.
 */
static atomic_int dit_edge = 0;
static atomic_int dah_edge = 0;

/* Single producer (keyer thread), single consumer (audio thread). */
enum { MARK_QUEUE_SIZE = 256 };
typedef struct {
    int64_t start_ns;
    int64_t duration_ns;
} mark_t;
static mark_t mark_queue[MARK_QUEUE_SIZE];
static atomic_uint mark_write = 0;
static atomic_uint mark_read = 0;

static atomic_int running = 1;
static atomic_int fatal_error = 0;

static volatile sig_atomic_t stop_requested = 0;

/*
 * The evdev thread owns this descriptor, but the SIGTSTP/SIGCONT handlers must
 * be able to drop and restore an exclusive EVIOCGRAB around a stop. -1 means
 * no device is currently open.
 */
static volatile sig_atomic_t evdev_fd = -1;
static volatile sig_atomic_t evdev_grabbed = 0;

/* Defined with the terminal code below; the signal handlers call them. */
static void disable_terminal_echo(void);
static void restore_terminal(void);

static int keep_running(void)
{
    if (stop_requested)
        ASTORE(running, 0);

    return ALOAD(running);
}

static void request_stop(int fatal)
{
    if (fatal)
        ASTORE(fatal_error, 1);

    ASTORE(running, 0);
}

/*
 * The signal handlers below do the minimum work that cannot be deferred:
 * kill() is async-signal-safe, and tcsetattr()/ioctl() are single syscalls.
 * They deliberately avoid stdio, malloc, and anything that can take a lock.
 */
static void release_evdev_grab(void)
{
    int fd = (int)evdev_fd;

    if (fd >= 0 && evdev_grabbed) {
        ioctl(fd, EVIOCGRAB, 0);
        evdev_grabbed = 0;
    }
}

static void acquire_evdev_grab(void)
{
    int fd = (int)evdev_fd;

    if (do_grab && fd >= 0 && !evdev_grabbed) {
        if (ioctl(fd, EVIOCGRAB, 1) == 0)
            evdev_grabbed = 1;
    }
}

/* SIGINT, SIGTERM, SIGHUP, SIGQUIT: ask the loops to wind down. */
static void handle_stop_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/*
 * Ctrl+Z. Put the terminal and the input device back how the rest of the
 * session expects them, then really stop. SIGSTOP is used rather than
 * re-raising SIGTSTP because the latter is blocked while its own handler
 * runs; SIGSTOP is uncatchable and unblockable, so it takes effect at once.
 * On resume the kernel delivers SIGCONT, whose handler re-establishes our
 * terminal mode and grab (see handle_cont).
 */
static void handle_tstp(int sig)
{
    (void)sig;

    release_evdev_grab();
    restore_terminal();

    kill(getpid(), SIGSTOP);
}

/*
 * SIGCONT arrives after SIGSTOP, including a SIGSTOP sent directly by another
 * process (which bypasses the SIGTSTP handler entirely). Re-assert our
 * terminal mode and grab so the keyer keeps working.
 */
static void handle_cont(int sig)
{
    (void)sig;

    disable_terminal_echo();
    acquire_evdev_grab();
}

/*
 * Last-resort cleanup for fatal signals: leave the terminal usable, then die
 * from the original signal so the exit status and any core dump are preserved.
 */
static void handle_fatal(int sig)
{
    struct sigaction dfl;
    sigset_t mask;

    release_evdev_grab();
    restore_terminal();

    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);

    /* The signal was blocked while this handler ran; unblock it so the
     * re-raise below actually terminates the process. */
    sigemptyset(&mask);
    sigaddset(&mask, sig);
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

    raise(sig);
    _exit(128 + sig);
}

static int set_signal_handler(int sig, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);

    /*
     * Deliberately no SA_RESTART: blocking poll()/ALSA/nanosleep calls should
     * return EINTR so shutdown and suspend are immediate.
     */
    sa.sa_flags = 0;

    if (sigaction(sig, &sa, NULL) < 0) {
        fprintf(stderr, "sigaction(%d): %s\n", sig, strerror(errno));
        return -1;
    }

    return 0;
}

static int install_signal_handlers(void)
{
    if (set_signal_handler(SIGINT, handle_stop_signal) < 0 ||
        set_signal_handler(SIGTERM, handle_stop_signal) < 0 ||
        set_signal_handler(SIGHUP, handle_stop_signal) < 0 ||
        set_signal_handler(SIGQUIT, handle_stop_signal) < 0 ||
        set_signal_handler(SIGTSTP, handle_tstp) < 0 ||
        set_signal_handler(SIGCONT, handle_cont) < 0 ||
        set_signal_handler(SIGSEGV, handle_fatal) < 0 ||
        set_signal_handler(SIGBUS, handle_fatal) < 0 ||
        set_signal_handler(SIGFPE, handle_fatal) < 0 ||
        set_signal_handler(SIGILL, handle_fatal) < 0 ||
        set_signal_handler(SIGABRT, handle_fatal) < 0)
        return -1;

    /*
     * The decoder writes to stdout. If its consumer goes away, writes should
     * fail with EPIPE rather than kill the whole keyer and skip the clean
     * shutdown path. Ignore SIGPIPE and let write errors surface instead.
     */
    if (set_signal_handler(SIGPIPE, SIG_IGN) < 0)
        return -1;

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s /dev/input/eventX [wpm=12] [freq_hz=800] [dit_key] [dah_key] [nograb] [decode]\n"
        "\n"
        "Examples:\n"
        "  %s /dev/input/eventX\n"
        "  %s /dev/input/eventX 25 700\n"
        "  %s /dev/input/eventX 25 700 COMMA DOT\n"
        "  %s /dev/input/eventX 25 700 COMMA DOT nograb\n"
        "  %s /dev/input/eventX nograb\n"
        "  %s /dev/input/event3 nograb decode\n"
        "\n"
        "Known key names include:\n"
        "  LEFTCTRL RIGHTCTRL LEFTSHIFT RIGHTSHIFT LEFTALT RIGHTALT\n"
        "  COMMA DOT PERIOD LEFT RIGHT LEFTBRACE RIGHTBRACE\n"
        "  INSERT DELETE Z X\n"
        "\n"
        "You may also use KEY_ prefixes, e.g. KEY_LEFTCTRL, or raw numeric\n"
        "evdev key codes.\n"
        "\n"
        "Options:\n"
        "  nograb    Do not take exclusive control of the input device.\n"
        "            Useful when testing on your normal keyboard.\n"
        "  decode    Print the keyed morse, decoded to text, without\n"
        "            newlines. Unknown symbols are ignored. A pause of\n"
        "            more than seven dits between characters prints a\n"
        "            space (the ITU word gap).\n"
        "\n"
        "While running, terminal echo is disabled so keystrokes do not\n"
        "print in the window; the previous terminal state is restored\n"
        "on exit.\n"
        "\n"
        "Environment:\n"
        "  CW_ALSA_DEVICE=pulse       ALSA PCM to open. Default: pulse\n"
        "  CW_ALSA_LATENCY_US=20000   Requested ALSA latency in us\n"
        "  CW_KEYER_MODE=b            Curtis iambic mode, a or b\n"
        "  PULSE_SINK=cw_out          If the device is pulse and PULSE_SINK is\n"
        "                             unset, it defaults to cw_out (created by\n"
        "                             cw-mic-mix.sh)\n",
        prog, prog, prog, prog, prog, prog, prog);
}

static double parse_double_arg(const char *s,
                               const char *name,
                               double min_val,
                               double max_val)
{
    char *end = NULL;
    double v;

    errno = 0;
    v = strtod(s, &end);

    if (errno != 0 || end == s || *end != '\0' ||
        !isfinite(v) || v < min_val || v > max_val) {
        fprintf(stderr,
                "invalid %s '%s'; expected finite value in range %.3f..%.3f\n",
                name, s, min_val, max_val);
        exit(1);
    }

    return v;
}

static unsigned int parse_uint_env(const char *name,
                                   unsigned int def,
                                   unsigned int min_val,
                                   unsigned int max_val)
{
    const char *s = getenv(name);
    char *end = NULL;
    unsigned long v;

    if (!s || !*s)
        return def;

    errno = 0;
    v = strtoul(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0' ||
        v < min_val || v > max_val) {
        fprintf(stderr,
                "warning: ignoring invalid %s='%s'; expected %u..%u\n",
                name, s, min_val, max_val);
        return def;
    }

    return (unsigned int)v;
}

/*
 * CW_KEYER_MODE selects the Curtis iambic timing: "a" or "b" (default b).
 */
static void parse_mode_env(void)
{
    const char *s = getenv("CW_KEYER_MODE");

    if (!s || !*s)
        return;

    if (strcasecmp(s, "a") == 0 ||
        strcasecmp(s, "modea") == 0 ||
        strcasecmp(s, "mode_a") == 0) {
        do_mode_b = 0;
    } else if (strcasecmp(s, "b") == 0 ||
               strcasecmp(s, "modeb") == 0 ||
               strcasecmp(s, "mode_b") == 0) {
        do_mode_b = 1;
    } else {
        fprintf(stderr,
                "warning: ignoring invalid CW_KEYER_MODE='%s'; use a or b\n", s);
    }
}

/*
 * Maps command-line key names to evdev codes.
 * Add more names here if needed.
 */
static int lookup_key(const char *name)
{
    static const struct {
        const char *name;
        int code;
    } keymap[] = {
        {"LEFTCTRL",    KEY_LEFTCTRL},
        {"RIGHTCTRL",   KEY_RIGHTCTRL},
        {"CTRL_L",      KEY_LEFTCTRL},
        {"CTRL_R",      KEY_RIGHTCTRL},
        {"LCONTROL",    KEY_LEFTCTRL},
        {"RCONTROL",    KEY_RIGHTCTRL},

        {"LEFTSHIFT",   KEY_LEFTSHIFT},
        {"RIGHTSHIFT",  KEY_RIGHTSHIFT},
        {"SHIFT_L",     KEY_LEFTSHIFT},
        {"SHIFT_R",     KEY_RIGHTSHIFT},

        {"LEFTALT",     KEY_LEFTALT},
        {"RIGHTALT",    KEY_RIGHTALT},
        {"ALT_L",       KEY_LEFTALT},
        {"ALT_R",       KEY_RIGHTALT},

        {"COMMA",       KEY_COMMA},
        {"DOT",         KEY_DOT},
        {"PERIOD",      KEY_DOT},

        {"LEFTBRACE",   KEY_LEFTBRACE},
        {"RIGHTBRACE",  KEY_RIGHTBRACE},

        {"LEFT",        KEY_LEFT},
        {"RIGHT",       KEY_RIGHT},
        {"INSERT",      KEY_INSERT},
        {"DELETE",      KEY_DELETE},

        {"Z",           KEY_Z},
        {"X",           KEY_X},

        {NULL, 0}
    };

    const char *s = name;

    if (strncasecmp(s, "KEY_", 4) == 0)
        s += 4;

    for (int i = 0; keymap[i].name; i++) {
        if (strcasecmp(s, keymap[i].name) == 0)
            return keymap[i].code;
    }

    /*
     * Also allow raw numeric evdev key codes.
     */
    {
        char *end = NULL;
        long code;

        errno = 0;
        code = strtol(name, &end, 0);

        if (errno == 0 && end != name && *end == '\0' &&
            code >= 0 && code <= KEY_MAX) {
            return (int)code;
        }
    }

    fprintf(stderr, "unknown key name '%s'\n", name);
    exit(1);
}

static int64_t mono_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        /*
         * CLOCK_MONOTONIC should not fail on Linux. If it somehow does,
         * request shutdown rather than continuing with invalid timing.
         */
        fprintf(stderr, "clock_gettime(CLOCK_MONOTONIC): %s\n", strerror(errno));
        request_stop(1);
        return 0;
    }

    return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

static void sleep_ns(int64_t ns)
{
    struct timespec req;

    if (ns <= 0)
        return;

    req.tv_sec = (time_t)(ns / NS_PER_SEC);
    req.tv_nsec = (long)(ns % NS_PER_SEC);

    while (keep_running()) {
        struct timespec rem;
        int rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &rem);

        if (rc == 0)
            return;

        if (rc == EINTR) {
            req = rem;
            continue;
        }

        fprintf(stderr, "clock_nanosleep: %s\n", strerror(rc));
        request_stop(1);
        return;
    }
}

/*
 * Curtis 8044 iambic keyer state machine.
 *
 * This is a direct port of the state graph used by UnixCW's libcw, which
 * documents itself as emulating the Curtis 8044, so that keying follows the
 * same rules as a transceiver built around the real chip. The graph tracks
 * "A" and "B" states because Curtis mode B sends one trailing element after
 * a simultaneous squeeze:
 *
 *   - the dot and dash latches are set while their paddle is closed and are
 *     cleared at the end of that element's space if the paddle is open;
 *   - the mode B Curtis latch is set on the leading edge of a simultaneous
 *     squeeze and, once consumed, forces one extra opposite element.
 *
 * keyer_core_step() is called once at the end of every mark and once at the
 * end of every inter-element space, matching how libcw clocks its graph from
 * the generator.
 */
typedef enum {
    KS_IDLE,
    KS_IN_DOT_A,
    KS_IN_DASH_A,
    KS_AFTER_DOT_A,
    KS_AFTER_DASH_A,
    KS_IN_DOT_B,
    KS_IN_DASH_B,
    KS_AFTER_DOT_B,
    KS_AFTER_DASH_B
} keyer_state_t;

typedef struct {
    keyer_state_t state;
    int dot_paddle;
    int dash_paddle;
    int dot_latch;
    int dash_latch;
    int curtis_b_latch;
    int both_closed;
    int mode_b;
} keyer_t;

/* 1 = dit, 0 = dah, -1 = not currently sending a mark. */
static int keyer_symbol(const keyer_t *keyer)
{
    if (keyer->state == KS_IN_DOT_A || keyer->state == KS_IN_DOT_B)
        return 1;

    if (keyer->state == KS_IN_DASH_A || keyer->state == KS_IN_DASH_B)
        return 0;

    return -1;
}

static void keyer_core_step(keyer_t *keyer)
{
    switch (keyer->state) {
    case KS_IN_DOT_A:
        keyer->state = KS_AFTER_DOT_A;
        break;

    case KS_IN_DOT_B:
        keyer->state = KS_AFTER_DOT_B;
        break;

    case KS_IN_DASH_A:
        keyer->state = KS_AFTER_DASH_A;
        break;

    case KS_IN_DASH_B:
        keyer->state = KS_AFTER_DASH_B;
        break;

    case KS_AFTER_DOT_A:
    case KS_AFTER_DOT_B:
        if (!keyer->dot_paddle)
            keyer->dot_latch = 0;

        if (keyer->state == KS_AFTER_DOT_B) {
            /* Mode B: the trailing element after a squeeze is a dash. */
            keyer->state = KS_IN_DASH_A;
        } else if (keyer->dash_latch) {
            if (keyer->curtis_b_latch) {
                keyer->curtis_b_latch = 0;
                keyer->state = KS_IN_DASH_B;
            } else {
                keyer->state = KS_IN_DASH_A;
            }
        } else if (keyer->dot_latch) {
            keyer->state = KS_IN_DOT_A;
        } else {
            keyer->state = KS_IDLE;
        }
        break;

    case KS_AFTER_DASH_A:
    case KS_AFTER_DASH_B:
        if (!keyer->dash_paddle)
            keyer->dash_latch = 0;

        if (keyer->state == KS_AFTER_DASH_B) {
            /* Mode B: the trailing element after a squeeze is a dot. */
            keyer->state = KS_IN_DOT_A;
        } else if (keyer->dot_latch) {
            if (keyer->curtis_b_latch) {
                keyer->curtis_b_latch = 0;
                keyer->state = KS_IN_DOT_B;
            } else {
                keyer->state = KS_IN_DOT_A;
            }
        } else if (keyer->dash_latch) {
            keyer->state = KS_IN_DASH_A;
        } else {
            keyer->state = KS_IDLE;
        }
        break;

    case KS_IDLE:
    default:
        break;
    }
}

/*
 * Record the current paddle values. The dot/dash latches are level-based,
 * but the mode B Curtis latch is edge-triggered so that merely holding a
 * squeeze does not keep queueing trailing elements.
 */
static void keyer_core_paddles(keyer_t *keyer, int dot_down, int dash_down)
{
    dot_down = !!dot_down;
    dash_down = !!dash_down;

    keyer->dot_paddle = dot_down;
    keyer->dash_paddle = dash_down;

    if (dot_down)
        keyer->dot_latch = 1;
    if (dash_down)
        keyer->dash_latch = 1;

    if (keyer->mode_b && dot_down && dash_down && !keyer->both_closed)
        keyer->curtis_b_latch = 1;
    keyer->both_closed = dot_down && dash_down;

    if (keyer->state == KS_IDLE && (dot_down || dash_down)) {
        /*
         * Nudge the machine out of idle. Pretend we are at the end of the
         * opposite space, then step once, matching libcw's initial nudge.
         */
        if (dot_down)
            keyer->state = keyer->curtis_b_latch ? KS_AFTER_DASH_B
                                                 : KS_AFTER_DASH_A;
        else
            keyer->state = keyer->curtis_b_latch ? KS_AFTER_DOT_B
                                                 : KS_AFTER_DOT_A;
        keyer_core_step(keyer);
    }
}

/*
 * Read the live paddle state plus any latched edges, and feed it to the
 * keyer core.
 */
static void sample_keyer_paddles(keyer_t *keyer)
{
    int dit_latched = atomic_exchange_explicit(&dit_edge, 0,
                                               memory_order_relaxed);
    int dah_latched = atomic_exchange_explicit(&dah_edge, 0,
                                               memory_order_relaxed);

    keyer_core_paddles(keyer,
                       ALOAD(dit_paddle) || dit_latched,
                       ALOAD(dah_paddle) || dah_latched);
}

/*
 * Wait until an absolute monotonic deadline while feeding paddle samples
 * into the keyer core. A press anywhere inside a mark or space sets the
 * appropriate latch.
 */
static void watch_keyer_until_ns(int64_t deadline, keyer_t *keyer)
{
    while (keep_running()) {
        int64_t remaining;

        sample_keyer_paddles(keyer);

        remaining = deadline - mono_ns();
        if (remaining <= 0)
            break;

        /*
         * 0.2 ms sampling. This is not a busy-wait, but is still responsive
         * enough for human paddle input.
         */
        if (remaining > 200000LL)
            remaining = 200000LL;

        sleep_ns(remaining);
    }
}

#define BITS_PER_ULONG (8U * sizeof(unsigned long))
#define NBITS(x) (((x) + BITS_PER_ULONG - 1U) / BITS_PER_ULONG)

static int bit_is_set(unsigned int bit, const unsigned long *array)
{
    return !!(array[bit / BITS_PER_ULONG] &
              (1UL << (bit % BITS_PER_ULONG)));
}

static void clear_input_state(void)
{
    ASTORE(dit_paddle, 0);
    ASTORE(dah_paddle, 0);
    ASTORE(dit_edge, 0);
    ASTORE(dah_edge, 0);
}

static void set_key_state(int code, int down)
{
    down = !!down;

    if (code == dit_code) {
        ASTORE(dit_paddle, down);
        if (down)
            ASTORE(dit_edge, 1);
    }

    if (code == dah_code) {
        ASTORE(dah_paddle, down);
        if (down)
            ASTORE(dah_edge, 1);
    }
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
static void *evdev_thread(void *arg)
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

    evdev_fd = fd;

    warn_if_key_not_supported(fd, dit_code, "dit");
    warn_if_key_not_supported(fd, dah_code, "dah");
    initialize_paddle_state_from_device(fd);

    if (do_grab) {
        if (ioctl(fd, EVIOCGRAB, 1) == 0) {
            evdev_grabbed = 1;
        } else {
            fprintf(stderr,
                    "warning: EVIOCGRAB failed: %s; events may leak to desktop\n",
                    strerror(errno));
        }
    }

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
                    if (resync) {
                        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                            initialize_paddle_state_from_device(fd);
                            resync = 0;
                        }
                        continue;
                    }

                    if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
                        /* Event state is unreliable until the next report. */
                        ASTORE(dit_paddle, 0);
                        ASTORE(dah_paddle, 0);
                        ASTORE(dit_edge, 0);
                        ASTORE(dah_edge, 0);
                        resync = 1;
                        continue;
                    }

                    if (ev.type == EV_KEY) {
                        /*
                         * ev.value:
                         *   0 = release
                         *   1 = press
                         *   2 = autorepeat
                         */
                        if (ev.value == 0 || ev.value == 1)
                            set_key_state((int)ev.code, ev.value);
                    }

                    continue;
                }

                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
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

    release_evdev_grab();
    evdev_fd = -1;

    close(fd);

    if (fatal)
        request_stop(1);

    return NULL;
}

/*
 * The keyer reads paddles from evdev, not stdin, but key presses still
 * reach the controlling terminal and the tty line discipline would echo
 * them. Disable echo and canonical mode while running; ISIG is kept so
 * Ctrl+C still works.
 */
static struct termios saved_termios;
static volatile sig_atomic_t termios_modified = 0;

static void disable_terminal_echo(void)
{
    struct termios t;

    /* Already disabled; re-saving here would store the disabled state. */
    if (termios_modified || !isatty(STDIN_FILENO))
        return;

    if (tcgetattr(STDIN_FILENO, &t) < 0)
        return;

    saved_termios = t;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) < 0)
        return;

    termios_modified = 1;
}

static void restore_terminal(void)
{
    if (!termios_modified)
        return;

    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    termios_modified = 0;
}

/*
 * Morse code table for the decode display. Unknown symbols return '\0'.
 */
static const struct {
    char ch;
    const char *code;
} morse_table[] = {
    {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
    {'E', "."},    {'F', "..-."},  {'G', "--."},  {'H', "...."},
    {'I', ".."},   {'J', ".---"}, {'K', "-.-"},  {'L', ".-.."},
    {'M', "--"},   {'N', "-."},   {'O', "---"},  {'P', ".--."},
    {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
    {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"},
    {'Y', "-.--"}, {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
    {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
    {'8', "---.."}, {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
    {'-', "-....-"}, {'(', "-.--."},  {')', "-.--.-"}, {'"', ".-..-."},
    {'\'', ".----."}, {'@', ".--.-."}, {'&', ".-..."}, {'=', "-...-"},
    {0, NULL}
};

static char morse_decode(const char *s, size_t len)
{
    char buf[16];

    if (len >= sizeof(buf))
        return '\0';

    memcpy(buf, s, len);
    buf[len] = '\0';

    for (int i = 0; morse_table[i].code; i++) {
        if (strcmp(morse_table[i].code, buf) == 0)
            return morse_table[i].ch;
    }

    return '\0';
}

static int64_t tone_ramp_ns(int64_t dit_ns)
{
    return dit_ns / 2 < 5000000LL ? dit_ns / 2 : 5000000LL;
}

static int queue_mark(int64_t start_ns, int64_t duration_ns)
{
    unsigned int write_pos = atomic_load_explicit(&mark_write, memory_order_relaxed);
    unsigned int read_pos = atomic_load_explicit(&mark_read, memory_order_acquire);
    if (write_pos - read_pos >= MARK_QUEUE_SIZE) {
        fprintf(stderr, "audio mark queue overflow; stopping keyer\n");
        request_stop(1);
        return -1;
    }

    mark_queue[write_pos % MARK_QUEUE_SIZE] = (mark_t){ start_ns, duration_ns };
    atomic_store_explicit(&mark_write, write_pos + 1, memory_order_release);
    return 0;
}

static int take_mark(mark_t *mark)
{
    unsigned int read_pos = atomic_load_explicit(&mark_read, memory_order_relaxed);
    unsigned int write_pos = atomic_load_explicit(&mark_write, memory_order_acquire);
    if (read_pos == write_pos)
        return 0;

    *mark = mark_queue[read_pos % MARK_QUEUE_SIZE];
    atomic_store_explicit(&mark_read, read_pos + 1, memory_order_release);
    return 1;
}

/*
 * Curtis 8044 iambic keyer driver.
 *
 * The decision logic lives in the keyer core above; this thread only clocks
 * it. Each iteration sends the mark selected by the current IN_* state,
 * samples the paddles through the mark and its one-dit space, then advances
 * the graph at the end of the mark and again at the end of the space.
 */
static void *keyer_thread(void *arg)
{
    (void)arg;

    const int64_t dit_ns = (int64_t)llround(1.2e9 / wpm);
    const int64_t dah_ns = dit_ns * 3;
    const int64_t space_ns = dit_ns;
    /*
     * Idle gaps longer than seven dits between characters are word gaps,
     * per the ITU standard (1 dit between elements, 3 between characters,
     * 7 between words).
     */
    const int64_t word_gap_ns = dit_ns * 7;
    const int64_t char_gap_ns = dit_ns * 3;

    keyer_t keyer = {0};
    keyer.mode_b = do_mode_b;
    keyer.state = KS_IDLE;

    char sym[16];
    size_t sym_len = 0;
    int64_t gap_start = 0;    /* mono_ns() when the current idle gap began */
    int printed_char = 0;     /* at least one decoded character printed */
    int space_emitted = 0;    /* space already printed for this idle gap */

    while (keep_running()) {
        if (keyer.state == KS_IDLE) {
            /* A press here nudges the graph into its first element. */
            sample_keyer_paddles(&keyer);

            if (keyer.state == KS_IDLE) {
                /*
                 * Idle: the symbol being keyed is complete. If the decode
                 * display is active, print it.
                 */
                if (do_decode && sym_len &&
                    mono_ns() - gap_start >= char_gap_ns) {
                    char ch = morse_decode(sym, sym_len);

                    /* Unknown symbols are dropped. */
                    if (ch != '\0') {
                        fputc(ch, stdout);
                        printed_char = 1;
                    }

                    fflush(stdout);
                    sym_len = 0;
                }

                /*
                 * Decode display: an idle gap longer than seven dits (the
                 * ITU word gap) between characters is a word gap; print a
                 * single space for it.
                 */
                if (do_decode && gap_start > 0 &&
                    printed_char && !space_emitted &&
                    (mono_ns() - gap_start) > word_gap_ns) {
                    fputc(' ', stdout);
                    fflush(stdout);
                    space_emitted = 1;
                }

                /*
                 * Idle poll interval. Lower values reduce input-to-keyer
                 * latency but increase wakeups.
                 */
                sleep_ns(200000LL);
                continue;
            }
        }

        /* In an IN_* state: emit this mark. */
        int send_dit = keyer_symbol(&keyer);
        int64_t mark_start = mono_ns();
        int64_t mark_duration = send_dit ? dit_ns : dah_ns;
        int64_t mark_end = mark_start + mark_duration;

        if (queue_mark(mark_start, mark_duration) < 0)
            break;

        watch_keyer_until_ns(mark_end, &keyer);
        keyer_core_step(&keyer);            /* end of mark: IN -> AFTER */

        gap_start = mark_end;
        space_emitted = 0;

        watch_keyer_until_ns(mark_end + space_ns, &keyer);
        keyer_core_step(&keyer);            /* end of space: AFTER -> next */

        if (do_decode && sym_len + 1 < sizeof(sym))
            sym[sym_len++] = send_dit ? '.' : '-';
    }

    return NULL;
}

static int recover_pcm(snd_pcm_t *pcm, int err, const char *where)
{
    int rc;

    if (err == -EINTR)
        return 0;

    rc = snd_pcm_recover(pcm, err, 1);
    if (rc < 0) {
        fprintf(stderr,
                "ALSA %s failed: %s; recover failed: %s\n",
                where, snd_strerror(err), snd_strerror(rc));
        return -1;
    }

    return 0;
}

static snd_pcm_t *open_audio(unsigned int *actual_rate)
{
    const char *dev = getenv("CW_ALSA_DEVICE");
    const char *pulse_sink = getenv("PULSE_SINK");
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw = NULL;
    int forced_sink = 0;
    int err;

    if (!dev || !*dev)
        dev = "pulse";

    /*
     * If using the Pulse ALSA plugin and no sink was explicitly selected,
     * send the keyer to the virtual combined sink created by cw-mic-mix.sh.
     */
    if (strcmp(dev, "pulse") == 0 && (!pulse_sink || !*pulse_sink)) {
        if (setenv("PULSE_SINK", "cw_out", 1) == 0) {
            forced_sink = 1;
            fprintf(stderr, "PULSE_SINK unset or empty; defaulting to cw_out\n");
        } else {
            fprintf(stderr,
                    "warning: could not set PULSE_SINK=cw_out: %s\n",
                    strerror(errno));
        }
    }

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr,
                "cannot open ALSA playback device '%s': %s\n",
                dev, snd_strerror(err));
        if (forced_sink) {
            fprintf(stderr,
                    "PULSE_SINK was defaulted to 'cw_out'. If that sink is "
                    "unavailable, run ./cw-mic-mix.sh or set PULSE_SINK "
                    "to a real sink, e.g. PULSE_SINK=$(pactl get-default-sink)\n");
        }
        return NULL;
    }

    err = snd_pcm_set_params(pcm,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             1,
                             AUDIO_RATE,
                             1,
                             alsa_latency_us);

    if (err < 0 && alsa_latency_us < 10000) {
        fprintf(stderr,
                "warning: ALSA latency %u us failed: %s; retrying 10000 us\n",
                alsa_latency_us, snd_strerror(err));

        alsa_latency_us = 10000;

        err = snd_pcm_set_params(pcm,
                                 SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 1,
                                 AUDIO_RATE,
                                 1,
                                 alsa_latency_us);
    }

    if (err < 0) {
        fprintf(stderr,
                "snd_pcm_set_params failed for '%s': %s\n",
                dev, snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    fprintf(stderr,
            "ALSA device: %s, rate: %u Hz, requested latency: %u us, "
            "write period: %d frames\n",
            dev, AUDIO_RATE, alsa_latency_us, AUDIO_PERIOD_FRAMES);

    /*
     * Log what the server actually negotiated, since it may differ from
     * the request.
     */
    snd_pcm_hw_params_alloca(&hw);
    err = snd_pcm_hw_params_current(pcm, hw);
    if (err < 0 || snd_pcm_hw_params_get_rate(hw, actual_rate, 0) < 0 ||
        *actual_rate == 0) {
        fprintf(stderr, "cannot determine ALSA playback sample rate\n");
        snd_pcm_close(pcm);
        return NULL;
    }

    if (freq_hz >= *actual_rate / 2.0) {
        fprintf(stderr, "tone %.2f Hz is at or above the negotiated "
                "Nyquist frequency (%u Hz sample rate)\n", freq_hz, *actual_rate);
        snd_pcm_close(pcm);
        return NULL;
    }

    snd_pcm_uframes_t buffer = 0, period = 0;
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    snd_pcm_hw_params_get_period_size(hw, &period, 0);
    fprintf(stderr,
            "ALSA achieved: rate %u Hz, buffer %lu frames (%.1f ms), "
            "period %lu frames (%.1f ms)\n",
            *actual_rate, (unsigned long)buffer, 1000.0 * (double)buffer / *actual_rate,
            (unsigned long)period, 1000.0 * (double)period / *actual_rate);

    return pcm;
}

typedef struct {
    uint64_t frame;
    uint64_t anchor_frame;
    uint64_t mark_start;
    uint64_t mark_end;
    uint64_t next_start;
    uint64_t gap_frames;
    uint64_t ramp_frames;
    int64_t anchor_ns;
    mark_t pending;
    int has_pending;
    int has_mark;
    double phase;
} audio_state_t;

static uint64_t ns_to_frames(int64_t ns, unsigned int rate)
{
    return (uint64_t)llround((double)ns * rate / 1e9);
}

static void generate_audio(int16_t *buf, size_t frames,
                           audio_state_t *state, unsigned int rate)
{
    const double two_pi = 2.0 * M_PI;
    const double phase_inc = two_pi * freq_hz / rate;
    /* Match UnixCW's default 70% of the 16-bit PCM amplitude range. */
    const double amplitude = (70 * 32768) / 100;

    for (size_t i = 0; i < frames; i++, state->frame++) {
        double gain = 0.0;

        if (!state->has_pending && take_mark(&state->pending)) {
            state->has_pending = 1;
            /* Re-anchor after a long idle period to avoid accumulating
             * drift between the monotonic and PCM playback clocks. */
            if (!state->has_mark || state->pending.start_ns - state->anchor_ns >
                (int64_t)llround(1.2e9 / wpm) * 7) {
                state->anchor_ns = state->pending.start_ns;
                state->anchor_frame = state->frame;
            }
            state->next_start = state->anchor_frame +
                ns_to_frames(state->pending.start_ns - state->anchor_ns, rate);
            if (state->has_mark &&
                state->next_start < state->mark_end + state->gap_frames)
                state->next_start = state->mark_end + state->gap_frames;
        }

        if (state->has_pending && state->frame >= state->next_start) {
            uint64_t mark_frames = ns_to_frames(state->pending.duration_ns, rate);
            if (!mark_frames)
                mark_frames = 1;
            state->mark_start = state->frame;
            state->mark_end = state->frame + mark_frames;
            state->anchor_ns = state->pending.start_ns;
            state->anchor_frame = state->frame;
            state->ramp_frames = ns_to_frames(tone_ramp_ns(
                (int64_t)llround(1.2e9 / wpm)), rate);
            if (state->ramp_frames > mark_frames / 2)
                state->ramp_frames = mark_frames / 2;
            state->has_mark = 1;
            state->has_pending = 0;
        }

        if (state->has_mark && state->frame < state->mark_end) {
            uint64_t from_start = state->frame - state->mark_start;
            uint64_t from_end = state->mark_end - state->frame - 1;
            if (state->ramp_frames && from_start < state->ramp_frames)
                gain = (1.0 - cos(M_PI * (double)from_start / (double)state->ramp_frames)) * 0.5;
            else if (state->ramp_frames && from_end < state->ramp_frames)
                gain = (1.0 - cos(M_PI * (double)from_end / (double)state->ramp_frames)) * 0.5;
            else
                gain = 1.0;
        }

        buf[i] = gain ? (int16_t)(gain * amplitude * sin(state->phase)) : 0;
        state->phase += phase_inc;
        if (state->phase >= two_pi)
            state->phase -= two_pi;
    }
}

static int wait_pcm(snd_pcm_t *pcm)
{
    int rc = snd_pcm_wait(pcm, 20);
    return rc < 0 ? recover_pcm(pcm, rc, "wait") : 0;
}

static int pcm_write_all(snd_pcm_t *pcm,
                         const int16_t *buf,
                         snd_pcm_uframes_t frames)
{
    snd_pcm_uframes_t off = 0;

    while (off < frames && keep_running()) {
        snd_pcm_sframes_t n;

        n = snd_pcm_writei(pcm, buf + off, frames - off);

        if (n == -EAGAIN) {
            if (wait_pcm(pcm) < 0)
                return -1;
            continue;
        }

        if (n < 0) {
            if (recover_pcm(pcm, (int)n, "write") < 0)
                return -1;

            continue;
        }

        if (n == 0) {
            if (wait_pcm(pcm) < 0)
                return -1;
            continue;
        }

        off += (snd_pcm_uframes_t)n;
    }

    return 0;
}

static int audio_loop(snd_pcm_t *pcm, unsigned int actual_rate)
{
    int16_t buf[AUDIO_PERIOD_FRAMES];

    audio_state_t state = {0};
    state.gap_frames = ns_to_frames((int64_t)llround(1.2e9 / wpm), actual_rate);

    int rc = 0;

    while (keep_running()) {
        generate_audio(buf, AUDIO_PERIOD_FRAMES, &state, actual_rate);

        if (pcm_write_all(pcm, buf, AUDIO_PERIOD_FRAMES) < 0) {
            rc = -1;
            break;
        }
    }

    /*
     * Drop rather than drain: on Ctrl+C or device failure, stop promptly
     * instead of playing out queued audio. This runs on the audio thread,
     * the only thread using the PCM handle by now, so no lock is needed.
     */
    snd_pcm_drop(pcm);

    return rc;
}

int main(int argc, char **argv)
{
    pthread_t evdev_tid;
    pthread_t keyer_tid;
    snd_pcm_t *pcm;
    int err;
    int field = 0;
    int audio_rc;
    unsigned int actual_rate = 0;

    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(argv[0]);
        return 0;
    }

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /*
     * Flexible positional parser:
     *
     *   non-nograb arg 0: wpm
     *   non-nograb arg 1: frequency
     *   non-nograb arg 2: dit key
     *   non-nograb arg 3: dah key
     *
     * "nograb" may appear anywhere after the device path.
     */
    for (int i = 2; i < argc; i++) {
        if (strcasecmp(argv[i], "nograb") == 0) {
            do_grab = 0;
            continue;
        }

        if (strcasecmp(argv[i], "decode") == 0) {
            do_decode = 1;
            continue;
        }

        switch (field) {
        case 0:
            wpm = parse_double_arg(argv[i], "wpm", 1.0, 300.0);
            break;

        case 1:
            freq_hz = parse_double_arg(argv[i],
                                       "freq_hz",
                                       1.0,
                                       (double)AUDIO_RATE / 2.0 - 1.0);
            break;

        case 2:
            dit_code = lookup_key(argv[i]);
            break;

        case 3:
            dah_code = lookup_key(argv[i]);
            break;

        default:
            usage(argv[0]);
            return 1;
        }

        field++;
    }

    if (dit_code == dah_code) {
        fprintf(stderr, "dit_key and dah_key must be different\n");
        return 1;
    }

    alsa_latency_us = parse_uint_env("CW_ALSA_LATENCY_US",
                                     20000,
                                     1000,
                                     100000);
    parse_mode_env();

    if (install_signal_handlers() < 0)
        return 1;

    /* Safety net for exit paths (and future ones) that skip the explicit
     * restore below; it is a no-op when the terminal was never changed. */
    if (atexit(restore_terminal) != 0)
        fprintf(stderr, "warning: atexit(restore_terminal) failed\n");

    disable_terminal_echo();

    fprintf(stderr,
            "CW keyer: %.2f WPM, %.2f Hz, dit code %d, dah code %d, %s, "
            "Curtis mode %c%s\n",
            wpm,
            freq_hz,
            dit_code,
            dah_code,
            do_grab ? "EVIOCGRAB enabled" : "nograb",
            do_mode_b ? 'B' : 'A',
            do_decode ? ", decode" : "");

    pcm = open_audio(&actual_rate);
    if (!pcm) {
        restore_terminal();
        return 1;
    }

    err = pthread_create(&evdev_tid, NULL, evdev_thread, argv[1]);
    if (err != 0) {
        fprintf(stderr, "pthread_create(evdev): %s\n", strerror(err));
        restore_terminal();
        snd_pcm_close(pcm);
        return 1;
    }

    err = pthread_create(&keyer_tid, NULL, keyer_thread, NULL);
    if (err != 0) {
        fprintf(stderr, "pthread_create(keyer): %s\n", strerror(err));
        request_stop(1);
        restore_terminal();
        pthread_join(evdev_tid, NULL);
        snd_pcm_close(pcm);
        return 1;
    }

    audio_rc = audio_loop(pcm, actual_rate);
    if (audio_rc < 0)
        request_stop(1);

    request_stop(0);
    pthread_join(keyer_tid, NULL);
    pthread_join(evdev_tid, NULL);

    snd_pcm_close(pcm);
    restore_terminal();

    return ALOAD(fatal_error) ? 1 : 0;
}
