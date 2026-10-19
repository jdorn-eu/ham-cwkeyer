// SPDX-License-Identifier: GPL-3.0-or-later
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
 *   ./cwkeyer --list-devices
 *   ./cwkeyer /dev/input/eventX [wpm=12] [freq_hz=800] [dit_key] [dah_key] [nograb] [decode] [straight]
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
 *   CW_KEYER_LEVEL=70               Sidetone level, percent of full scale.
 *   CW_KEYER_DEBOUNCE_MS=10         Paddle RC filter time constant, 0..50 ms.
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
#include <glob.h>
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

/* Set by the build (see Makefile); "unknown" for an unversioned build. */
#ifndef CWKEYER_VERSION
#define CWKEYER_VERSION "unknown"
#endif

enum {
    AUDIO_RATE = 48000,
    AUDIO_PERIOD_FRAMES = 128
};

/* At 40 WPM a dit is 30 ms: even +/-25 ms weighting stays positive. */
#define MAX_WPM 40.0
static _Atomic double wpm = 12.0;
static _Atomic double freq_hz = 800.0;

/* Sidetone level, percent of full-scale PCM. */
static _Atomic int volume_percent = 70;

/* Negotiated output rate; set before the control thread starts. */
static unsigned int audio_rate = AUDIO_RATE;

static int dit_code = KEY_LEFTCTRL;
static int dah_code = KEY_RIGHTCTRL;
static int do_grab = 1;
static int do_decode = 0;

/*
 * Straight-key passthrough.  When set, the paddles key the sidetone directly
 * (either lever closes the key) instead of driving the iambic core.  This is
 * the software equivalent of the 8044ABM's separate MAN. KEY input.
 */
static int straight_mode = 0;
static atomic_int straight_down = 0;

/*
 * Weighting: a constant time added to every mark and taken from the space
 * that follows it, so the element period stays constant.  This matches the
 * 8044ABM's RC weight control, which the application note describes as
 * stretching each element "by a constant amount regardless of what speed is
 * being used" -- i.e. not a ratio.  0 ms is strict ITU timing.
 */
static _Atomic double weight_ms = 0.0;

/*
 * Paddle RC filter time constant.  This models the 8044's external 1M/0.01uF
 * input network, which is roughly 10 ms; 0 disables the filter.
 */
static unsigned int debounce_ms = 10;
static int64_t debounce_ns = 10000000LL;

static unsigned int alsa_latency_us = 20000;

/*
 * Diagnostics gathered during a run and reported at exit. Each counter is
 * written by one thread and only read after that thread has joined.
 */
static unsigned int audio_underruns = 0;
static unsigned int keyer_late_elements = 0;
static int64_t keyer_max_overrun_ns = 0;
static int64_t keyer_late_threshold_ns = 1000000LL;   /* 1 ms */

static atomic_int dit_paddle = 0;
static atomic_int dah_paddle = 0;

/*
 * Edge latches prevent very short key-down/key-up pulses from being missed
 * between keyer polling intervals.
 */
static atomic_int dit_edge = 0;
static atomic_int dah_edge = 0;

/*
 * Press order.  The evdev thread sees the levers in kernel order, so it
 * stamps each press with a monotonic counter.  A currently-held lever keeps
 * the stamp of the press that closed it, so of two held levers the smaller
 * stamp is the one closed first.  0 means "not pressed this session" (for
 * example a lever already held at startup).
 */
static atomic_uint dit_press_seq = 0;
static atomic_uint dah_press_seq = 0;
static unsigned press_seq = 0;   /* evdev thread only */

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

static atomic_int stop_requested = 0;

/* The signal thread and evdev thread serialize grab and descriptor changes. */
static pthread_mutex_t evdev_lock = PTHREAD_MUTEX_INITIALIZER;
static int evdev_fd = -1;
static int evdev_grabbed = 0;
static int grab_suspended = 0;

/* A signal-waiting thread handles normal stop/continue signals outside a
 * signal handler. shutdown_wanted is set before joining it. */
static atomic_int shutdown_wanted = 0;
static pthread_t signal_tid;
static sigset_t signal_set;
static sigset_t old_signal_mask;
static pthread_mutex_t terminal_lock = PTHREAD_MUTEX_INITIALIZER;
static int terminal_suspended = 0;
static int signal_started = 0;

/* Defined with the terminal code below; the signal thread calls them. */
static void disable_terminal_echo(void);
static void restore_terminal(void);

static int keep_running(void)
{
    if (ALOAD(stop_requested))
        ASTORE(running, 0);

    return ALOAD(running);
}

static void request_stop(int fatal)
{
    if (fatal)
        ASTORE(fatal_error, 1);

    ASTORE(running, 0);
}

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

static void *signal_thread(void *arg)
{
    int suspended = 0;
    (void)arg;

    for (;;) {
        int sig;
        int rc = sigwait(&signal_set, &sig);
        if (rc != 0) {
            fprintf(stderr, "sigwait: %s\n", strerror(rc));
            request_stop(1);
            break;
        }
        if (ALOAD(shutdown_wanted))
            break;

        switch (sig) {
        case SIGINT:
        case SIGTERM:
        case SIGHUP:
        case SIGQUIT:
            ASTORE(stop_requested, 1);
            request_stop(0);
            break;
        case SIGTSTP:
            if (!suspended && keep_running()) {
                suspended = 1;
                pthread_mutex_lock(&terminal_lock);
                terminal_suspended = 1;
                pthread_mutex_unlock(&terminal_lock);
                pthread_mutex_lock(&evdev_lock);
                grab_suspended = 1;
                pthread_mutex_unlock(&evdev_lock);
                release_evdev_grab();
                restore_terminal();
                /* SIGSTOP suspends all threads; SIGCONT will be consumed here
                 * after the process resumes. Do not suspend if shutdown won
                 * the race while we were restoring the terminal. */
                if (!ALOAD(shutdown_wanted) && keep_running())
                    (void)kill(getpid(), SIGSTOP);
            }
            break;
        case SIGCONT:
            if (suspended) {
                suspended = 0;
                if (!ALOAD(shutdown_wanted)) {
                    pthread_mutex_lock(&terminal_lock);
                    terminal_suspended = 0;
                    pthread_mutex_unlock(&terminal_lock);
                    disable_terminal_echo();
                    pthread_mutex_lock(&evdev_lock);
                    grab_suspended = 0;
                    pthread_mutex_unlock(&evdev_lock);
                    acquire_evdev_grab();
                }
            }
            break;
        default:
            break;
        }
    }
    return NULL;
}

static void stop_signal_thread(void)
{
    if (!signal_started)
        return;
    ASTORE(shutdown_wanted, 1);
    /* Wake sigwait even if no external signal arrived. */
    (void)pthread_kill(signal_tid, SIGCONT);
    pthread_join(signal_tid, NULL);
    signal_started = 0;
    /* Leave the mask in place until process exit: a late stop/continue must
     * not get its default action between joining and terminal restoration. */
}

static int install_signal_handlers(void)
{
    struct sigaction sa = {0};
    int rc;

    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);
    sigaddset(&signal_set, SIGHUP);
    sigaddset(&signal_set, SIGQUIT);
    sigaddset(&signal_set, SIGTSTP);
    sigaddset(&signal_set, SIGCONT);

    /* Block before creating any other threads; children inherit the mask. */
    rc = pthread_sigmask(SIG_BLOCK, &signal_set, &old_signal_mask);
    if (rc != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(rc));
        return -1;
    }

    /* Broken decode output must fail with EPIPE, not terminate the keyer. */
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        fprintf(stderr, "sigaction(SIGPIPE): %s\n", strerror(errno));
        (void)pthread_sigmask(SIG_SETMASK, &old_signal_mask, NULL);
        return -1;
    }

    ASTORE(shutdown_wanted, 0);
    rc = pthread_create(&signal_tid, NULL, signal_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_create(signal): %s\n", strerror(rc));
        (void)pthread_sigmask(SIG_SETMASK, &old_signal_mask, NULL);
        return -1;
    }
    signal_started = 1;
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s /dev/input/eventX [wpm=12] [freq_hz=800] [dit_key] [dah_key] [nograb] [decode] [straight]\n"
        "       %s --help | --version | --list-devices | --setup\n"
        "\n"
        "Examples:\n"
        "  %s /dev/input/eventX\n"
        "  %s /dev/input/eventX 25 700\n"
        "  %s /dev/input/eventX 25 700 COMMA DOT\n"
        "  %s /dev/input/eventX 25 700 COMMA DOT nograb\n"
        "  %s /dev/input/eventX nograb\n"
        "  %s /dev/input/event3 nograb decode\n"
        "  %s /dev/input/eventX straight\n"
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
        "  straight  Use the paddles as a straight key: either lever\n"
        "            keys the sidetone directly, with no iambic timing\n"
        "            or memory. The RC paddle filter still applies.\n"
        "            decode is ignored in this mode.\n"
        "  --help    Print this help and exit.\n"
        "  --version Print the version and exit.\n"
        "  --list-devices, -L\n"
        "            List readable /dev/input/event* devices, grouped as\n"
        "            unknown, keyboard, or paddle, and exit. Colors are\n"
        "            used only on a terminal; set NO_COLOR to disable them.\n"
        "  --setup   Choose the paddle device by tapping the levers, then\n"
        "            start with the default speed, pitch, level, and\n"
        "            weighting. A device that looks like a keyboard\n"
        "            defaults to nograb.\n"
        "\n"
        "With no arguments on an interactive terminal, cwkeyer runs the\n"
        "same tap-to-select setup and then starts with the defaults.\n"
        "Without a terminal it prints this help.\n"
        "\n"
        "While running, terminal echo is disabled so keystrokes do not\n"
        "print in the window; the previous terminal state is restored\n"
        "on exit.  If stdin is readable, [ ] change speed, - = change\n"
        "pitch, 9 0 change the level, and , . change the weighting.\n"
        "\n"
        "Environment:\n"
        "  CW_ALSA_DEVICE=pulse       ALSA PCM to open. Default: pulse\n"
        "  CW_ALSA_LATENCY_US=20000   Requested ALSA latency in us\n"
        "  CW_KEYER_LEVEL=70          Sidetone level, percent of full scale\n"
        "  CW_KEYER_DEBOUNCE_MS=10    Paddle RC filter time constant, 0..50 ms\n"
        "  CW_KEYER_WEIGHT_MS=0       Weighting, -25..25 ms; a constant time\n"
        "                             added to each mark and taken from the\n"
        "                             following space (0 is strict ITU timing)\n"
        "  PULSE_SINK=cw_out          If the device is pulse and PULSE_SINK is\n"
        "                             unset, it defaults to cw_out (created by\n"
        "                             cw-mic-mix.sh)\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
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

static double parse_double_env(const char *name,
                               double def,
                               double min_val,
                               double max_val)
{
    const char *s = getenv(name);
    char *end = NULL;
    double v;

    if (!s || !*s)
        return def;

    errno = 0;
    v = strtod(s, &end);

    if (errno != 0 || end == s || *end != '\0' ||
        v < min_val || v > max_val) {
        fprintf(stderr,
                "warning: ignoring invalid %s='%s'; expected %.1f..%.1f\n",
                name, s, min_val, max_val);
        return def;
    }

    return v;
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
 * Curtis 8044 iambic keyer core.
 *
 * This project implements the type B behaviour (the Accu-keyer / 8044ABM
 * "B"): the dot and dash latches are set whenever their lever is closed and
 * cleared at the end of that element if the lever is open.  Only the latches
 * select elements, so releasing a squeeze leaves the opposite latch set and
 * one extra element goes out.  This is what most transceivers mean by
 * "Curtis mode B".  Type A is deliberately out of scope.
 *
 * keyer_core_step() is called once at the end of every mark and once at the
 * end of every inter-element space.
 *
 * References: Curtis Electro Devices, "8044 Series Keyer-on-a-Chip
 * Application Note"; K. Fischer, DJ5IL, "All About Squeeze-Keying".
 */
typedef enum {
    KY_IDLE,
    KY_IN_DOT,
    KY_IN_DASH,
    KY_AFTER_DOT,
    KY_AFTER_DASH
} keyer_state_t;

/*
 * Which lever was closed first when both are down in the same sample.  The
 * 8044 starts the stream with that lever's element ("the stream commences
 * with the element ... corresponding to whichever key was closed first").
 * KY_FIRST_UNKNOWN falls back to the dot, which is what a caller that has
 * no ordering information (e.g. a unit test) gets.
 */
typedef enum {
    KY_FIRST_UNKNOWN = 0,
    KY_FIRST_DOT,
    KY_FIRST_DASH
} keyer_first_t;

/*
 * Contact filtering.  The 8044 uses an external RC low-pass on each paddle
 * input; the recommended 1M/0.01uF network has a time constant of about
 * 10 ms.  The same idea is modelled here with a first-order filter on the
 * sampled lever level, followed by a small Schmitt-trigger hysteresis.  A
 * pulse shorter than the threshold time is ignored, just as it would not
 * charge the real capacitor past the chip's input threshold.
 */
typedef struct {
    double level;       /* filtered lever level, 0.0 .. 1.0 */
    int state;          /* debounced logic level */
    int64_t last_ns;    /* time of the previous update */
    int initialized;
} debounce_t;

typedef struct {
    keyer_state_t state;

    int dot_paddle;
    int dash_paddle;

    /* Transient input: which lever closed first in this sample. */
    keyer_first_t first_lever;

    /* Mode B level latches. */
    int dot_latch;
    int dash_latch;

    /* Input RC filtering. */
    debounce_t dit_debounce;
    debounce_t dah_debounce;
} keyer_t;

/* 1 = dit, 0 = dah, -1 = not currently sending a mark. */
static int keyer_symbol(const keyer_t *keyer)
{
    if (keyer->state == KY_IN_DOT)
        return 1;

    if (keyer->state == KY_IN_DASH)
        return 0;

    return -1;
}

/*
 * Advance the graph: end of a mark moves IN_* to the matching AFTER_*;
 * end of an inter-element space selects the next element (or idle).
 */
static void keyer_core_step(keyer_t *keyer)
{
    if (keyer->state == KY_IN_DOT) {
        keyer->state = KY_AFTER_DOT;
        return;
    }

    if (keyer->state == KY_IN_DASH) {
        keyer->state = KY_AFTER_DASH;
        return;
    }

    if (keyer->state == KY_AFTER_DOT) {
        if (!keyer->dot_paddle)
            keyer->dot_latch = 0;

        if (keyer->dash_latch)
            keyer->state = KY_IN_DASH;
        else if (keyer->dot_latch)
            keyer->state = KY_IN_DOT;
        else
            keyer->state = KY_IDLE;
        return;
    }

    if (keyer->state == KY_AFTER_DASH) {
        if (!keyer->dash_paddle)
            keyer->dash_latch = 0;

        if (keyer->dot_latch)
            keyer->state = KY_IN_DOT;
        else if (keyer->dash_latch)
            keyer->state = KY_IN_DASH;
        else
            keyer->state = KY_IDLE;
        return;
    }
}

/*
 * Record the current paddle values and set the mode B level latches.
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

    if (keyer->state == KY_IDLE && (dot_down || dash_down)) {
        /*
         * Nudge the machine out of idle. Pretend we are at the end of the
         * opposite element's space so the step picks the right first
         * element. The first element follows the lever that was closed
         * first; if only one lever is down that is also the answer, and if
         * both arrived together without a known order the dot wins.
         */
        int dot_first;

        if (!dash_down)
            dot_first = 1;
        else if (!dot_down)
            dot_first = 0;
        else
            dot_first = keyer->first_lever != KY_FIRST_DASH;

        if (dot_first)
            keyer->state = KY_AFTER_DASH;
        else
            keyer->state = KY_AFTER_DOT;
        keyer_core_step(keyer);
    }
}

/*
 * Update one paddle's RC filter.  `tau_ns` is the time constant; 0 disables
 * filtering.  The threshold is 0.5 with a small amount of hysteresis so a
 * noisy level near the threshold cannot chatter.
 */
static int debounce_update(debounce_t *d, int raw, int64_t now, int64_t tau_ns)
{
    double target;
    double alpha;
    int64_t dt;

    raw = !!raw;

    if (tau_ns <= 0) {
        d->level = raw ? 1.0 : 0.0;
        d->state = raw;
        d->last_ns = now;
        d->initialized = 1;
        return d->state;
    }

    if (!d->initialized) {
        /* The real capacitor starts discharged, so a lever already held at
         * startup takes the normal filter delay to register. */
        d->level = 0.0;
        d->state = 0;
        d->last_ns = now;
        d->initialized = 1;
    }

    dt = now - d->last_ns;
    if (dt < 0)
        dt = 0;
    d->last_ns = now;

    target = raw ? 1.0 : 0.0;
    alpha = 1.0 - exp(-(double)dt / (double)tau_ns);
    d->level += (target - d->level) * alpha;

    if (d->level >= 0.55)
        d->state = 1;
    else if (d->level <= 0.45)
        d->state = 0;

    return d->state;
}

/*
 * Feed one filtered sample to the keyer core. The RC filter runs in the
 * keyer's own time base, so short bounce is smoothed before it can reach the
 * core.
 */
static void keyer_sample(keyer_t *keyer, int dit_raw, int dah_raw, int64_t now)
{
    int dit = debounce_update(&keyer->dit_debounce, dit_raw, now, debounce_ns);
    int dah = debounce_update(&keyer->dah_debounce, dah_raw, now, debounce_ns);

    keyer_core_paddles(keyer, dit, dah);
}

/*
 * Read the live paddle state plus any latched edges, filter it, and feed it
 * to the keyer core. The edge latches preserve a press that lands between
 * keyer samples when filtering is disabled; with the RC filter enabled, a
 * pulse that short is normally filtered out, just as the chip would ignore
 * it.
 */
static void sample_keyer_paddles(keyer_t *keyer)
{
    int dit_latched = atomic_exchange_explicit(&dit_edge, 0,
                                               memory_order_relaxed);
    int dah_latched = atomic_exchange_explicit(&dah_edge, 0,
                                               memory_order_relaxed);
    unsigned dit_seq = atomic_load_explicit(&dit_press_seq,
                                            memory_order_relaxed);
    unsigned dah_seq = atomic_load_explicit(&dah_press_seq,
                                            memory_order_relaxed);

    /*
     * Record which lever was closed first, for the one case where it
     * matters: both levers already registered in the same sample while the
     * core is idle.  Otherwise the first lever starts its element before
     * the second is ever seen, so the order is already implicit.
     */
    if (dit_seq < dah_seq)
        keyer->first_lever = KY_FIRST_DOT;
    else if (dah_seq < dit_seq)
        keyer->first_lever = KY_FIRST_DASH;
    else
        keyer->first_lever = KY_FIRST_UNKNOWN;

    keyer_sample(keyer,
                 ALOAD(dit_paddle) || dit_latched,
                 ALOAD(dah_paddle) || dah_latched,
                 mono_ns());
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
 * A general-purpose keyboard reports a full typing keyset; a dedicated CW
 * paddle adapter normally reports only its paddle keys.  The name is used
 * only as a corroborating hint when the key bitmap is inconclusive.
 */
static device_kind_t classify_input_device(const char *name,
                                           const unsigned long *keys)
{
    int typing_keys = bit_is_set(KEY_A, keys) &&
                      bit_is_set(KEY_SPACE, keys) &&
                      bit_is_set(KEY_ENTER, keys);

    if (typing_keys)
        return DEVICE_KIND_KEYBOARD;

    if (name && (strcasestr(name, "keyboard") || strcasestr(name, "kbd")))
        return DEVICE_KIND_KEYBOARD;

    /* Both default paddle keys at once is the strong, reliable signal. */
    if (bit_is_set(KEY_LEFTCTRL, keys) &&
        bit_is_set(KEY_RIGHTCTRL, keys))
        return DEVICE_KIND_PADDLE;

    if (name && (strcasestr(name, "keyer") || strcasestr(name, "paddle") ||
                 strcasestr(name, "vband") || strcasestr(name, "morse")))
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
                              int default_paddles)
{
    fprintf(out, "  %s  \"%s\"%s\n",
            path, (name && *name) ? name : "(unnamed)",
            default_paddles ?
                "  (default paddle keys: LEFTCTRL RIGHTCTRL)" : "");
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

        unsigned long keys[NBITS(KEY_MAX + 1)] = {0};
        int has_keys = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0;
        d->kind = has_keys ? classify_input_device(d->name, keys)
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

    flush_stdin();

    for (;;) {
        size_t len = 0;

        fprintf(stderr, "%s [%s] ", question, default_yes ? "Y/n" : "y/N");
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

        fprintf(stderr, "Please answer y or n.\n");
    }
}

/*
 * Interactive device selection.  Tap the two levers to learn both key codes
 * and the device, then confirm the grab policy.  Speed, pitch, level, and
 * weighting keep their normal defaults and runtime controls.
 */
static int run_setup_wizard(char *device_path, size_t path_size)
{
    input_device_t *devices = NULL;
    size_t count = 0;
    size_t selected = SIZE_MAX;
    int dit = 0;
    int dah = 0;
    int straight = 0;
    int result = -1;

    if (discover_input_devices(&devices, &count) < 0)
        return -1;

    if (count == 0) {
        fprintf(stderr, "no /dev/input/event* devices found\n");
        goto out;
    }

    for (;;) {
        size_t which = SIZE_MAX;
        size_t second = SIZE_MAX;
        int code = 0;
        int rc;

        drain_device_events(devices, count);
        fprintf(stderr,
                "Tap the DOT (dit) paddle. Release it after the tap.\n"
                "  (Ctrl+C cancels setup)\n");
        rc = wait_for_paddle_press(devices, count, SIZE_MAX,
                                   SETUP_TAP_TIMEOUT_MS, &which, &code);
        if (rc < 0) {
            fprintf(stderr,
                    "no readable input devices; check your udev rules "
                    "(see the README)\n");
            goto out;
        }
        if (rc == 0) {
            fprintf(stderr,
                    "no paddle press detected within %d seconds\n",
                    SETUP_TAP_TIMEOUT_MS / 1000);
            if (prompt_yes_no("Try again?", 1) != 1)
                goto out;
            continue;
        }

        flush_stdin();
        fprintf(stderr, "Detected %s (%s), key %d\n",
                devices[which].name, devices[which].path, code);

        drain_device_events(devices, count);
        fprintf(stderr,
                "Now tap the DASH (dah) paddle on the same device.\n");
        rc = wait_for_paddle_press(devices, count, which,
                                   SETUP_TAP_TIMEOUT_MS, &second, &dah);
        if (rc < 0) {
            fprintf(stderr, "cannot poll the selected input device\n");
            goto out;
        }
        if (rc == 0) {
            fprintf(stderr,
                    "no paddle press detected within %d seconds\n",
                    SETUP_TAP_TIMEOUT_MS / 1000);
            if (prompt_yes_no("Try again?", 1) != 1)
                goto out;
            continue;
        }

        flush_stdin();
        selected = second;
        dit = code;
        straight = 0;

        if (dit == dah) {
            int straight_choice;

            fprintf(stderr,
                    "Both taps used key %d; this looks like a straight key.\n",
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

    fprintf(stderr, "Using %s (%s), detected as %s.\n",
            devices[selected].name, devices[selected].path,
            device_kind_name(devices[selected].kind));

    int default_grab = devices[selected].kind == DEVICE_KIND_PADDLE;
    int grab = prompt_yes_no("Take exclusive control of this device?",
                             default_grab);
    if (grab < 0)
        goto out;

    if (grab && devices[selected].kind == DEVICE_KIND_KEYBOARD) {
        int sure;

        fprintf(stderr,
                "warning: grabbing a keyboard hides it from the desktop and "
                "terminal; Ctrl+C may not reach cwkeyer.\n");
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

static int list_input_devices(FILE *out)
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
            print_device_line(out, devices[i].path, devices[i].name,
                              devices[i].default_paddles);
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

static void clear_input_state(void)
{
    ASTORE(dit_paddle, 0);
    ASTORE(dah_paddle, 0);
    ASTORE(dit_edge, 0);
    ASTORE(dah_edge, 0);
    ASTORE(dit_press_seq, 0);
    ASTORE(dah_press_seq, 0);
    press_seq = 0;
}

static void set_key_state(int code, int down)
{
    down = !!down;

    if (code == dit_code) {
        ASTORE(dit_paddle, down);
        if (down) {
            ASTORE(dit_edge, 1);
            ASTORE(dit_press_seq, ++press_seq);
        }
    }

    if (code == dah_code) {
        ASTORE(dah_paddle, down);
        if (down) {
            ASTORE(dah_edge, 1);
            ASTORE(dah_press_seq, ++press_seq);
        }
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

/*
 * The keyer reads paddles from evdev, not stdin, but key presses still
 * reach the controlling terminal and the tty line discipline would echo
 * them. Disable echo and canonical mode while running; ISIG is kept so
 * Ctrl+C still works.
 */
static struct termios saved_termios;
static int termios_modified = 0; /* protected by terminal_lock */

static void disable_terminal_echo(void)
{
    struct termios t;

    pthread_mutex_lock(&terminal_lock);
    if (terminal_suspended || termios_modified || !isatty(STDIN_FILENO))
        goto out;
    if (tcgetattr(STDIN_FILENO, &t) < 0)
        goto out;

    saved_termios = t;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
    /* Keep the original state available even if tcsetattr partly fails. */
    termios_modified = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
        (void)tcflush(STDIN_FILENO, TCIFLUSH);
out:
    pthread_mutex_unlock(&terminal_lock);
}

static void restore_terminal(void)
{
    pthread_mutex_lock(&terminal_lock);
    if (termios_modified) {
        (void)tcflush(STDIN_FILENO, TCIFLUSH);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios) == 0)
            termios_modified = 0;
    }
    pthread_mutex_unlock(&terminal_lock);
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

/*
 * State for the optional decode display.
 */
typedef struct {
    char sym[16];
    size_t sym_len;
    int64_t gap_start;    /* mono_ns() when the current idle gap began */
    int printed_char;     /* at least one decoded character printed */
    int space_emitted;    /* space already printed for this idle gap */
} decode_state_t;

/* Append one element to the symbol being keyed. */
static void decode_add_symbol(decode_state_t *d, int send_dit)
{
    if (d->sym_len + 1 < sizeof(d->sym))
        d->sym[d->sym_len++] = send_dit ? '.' : '-';
}

/*
 * Called while the keyer is idle. After a character gap the accumulated
 * symbol is printed (unknown symbols are dropped); after a word gap a single
 * space is printed. `out` is normally stdout, but a stream is passed in so
 * the logic can be tested.
 */
static void decode_idle(decode_state_t *d, int enable, int64_t now,
                        int64_t char_gap_ns, int64_t word_gap_ns, FILE *out)
{
    if (!enable)
        return;

    if (d->sym_len && now - d->gap_start >= char_gap_ns) {
        char ch = morse_decode(d->sym, d->sym_len);

        /* Unknown symbols are dropped. */
        if (ch != '\0') {
            fputc(ch, out);
            d->printed_char = 1;
        }

        fflush(out);
        d->sym_len = 0;
    }

    if (d->gap_start > 0 && d->printed_char && !d->space_emitted &&
        (now - d->gap_start) > word_gap_ns) {
        fputc(' ', out);
        fflush(out);
        d->space_emitted = 1;
    }
}

static int64_t tone_ramp_ns(int64_t dit_ns)
{
    return dit_ns / 2 < 5000000LL ? dit_ns / 2 : 5000000LL;
}

/*
 * The space following an element: the nominal one-dit gap reduced (or, for
 * negative weighting, extended) by the weighting offset, never negative.
 * The element period (mark + space) stays constant.
 */
static int64_t keyer_space_ns(void)
{
    int64_t dit_ns = (int64_t)llround(1.2e9 / ALOAD(wpm));
    int64_t space_ns = dit_ns - (int64_t)llround(ALOAD(weight_ms) * 1e6);

    return space_ns < 0 ? 0 : space_ns;
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

    keyer_t keyer = {0};
    keyer.state = KY_IDLE;

    decode_state_t dec = {0};
    int64_t prev_expected_start = 0;
    int64_t last_dit_ns = -1;

    while (keep_running()) {
        /*
         * Straight-key passthrough: poll the debounced paddles and publish
         * the combined key state for the audio thread.  No iambic core, no
         * mark queue, no decode.
         */
        if (straight_mode) {
            int dit_latched = atomic_exchange_explicit(&dit_edge, 0,
                                                       memory_order_relaxed);
            int dah_latched = atomic_exchange_explicit(&dah_edge, 0,
                                                       memory_order_relaxed);
            int64_t now = mono_ns();
            int dit = debounce_update(&keyer.dit_debounce,
                                      ALOAD(dit_paddle) || dit_latched,
                                      now, debounce_ns);
            int dah = debounce_update(&keyer.dah_debounce,
                                      ALOAD(dah_paddle) || dah_latched,
                                      now, debounce_ns);

            ASTORE(straight_down, dit || dah);
            sleep_ns(200000LL);
            continue;
        }

        /* Read the live speed each time so runtime changes apply to the next
         * element without restarting the thread. */
        const int64_t dit_ns = (int64_t)llround(1.2e9 / wpm);
        const int64_t dah_ns = dit_ns * 3;
        const int64_t space_ns = keyer_space_ns();
        const int64_t weight_ns = dit_ns - space_ns;
        /*
         * Idle gaps longer than seven dits between characters are word gaps,
         * per the ITU standard (1 dit between elements, 3 between characters,
         * 7 between words).
         */
        const int64_t word_gap_ns = dit_ns * 7;
        const int64_t char_gap_ns = dit_ns * 3;

        if (dit_ns != last_dit_ns) {
            /* A speed change makes the previous element's nominal start
             * meaningless for the overrun diagnostic. */
            prev_expected_start = 0;
            last_dit_ns = dit_ns;
        }
        if (keyer.state == KY_IDLE) {
            /* A new burst; don't measure this start against the last one. */
            prev_expected_start = 0;

            /* A press here nudges the graph into its first element. */
            sample_keyer_paddles(&keyer);

            if (keyer.state == KY_IDLE) {
                decode_idle(&dec, do_decode, mono_ns(),
                            char_gap_ns, word_gap_ns, stdout);

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
        int64_t mark_duration = (send_dit ? dit_ns : dah_ns) + weight_ns;
        if (mark_duration < 1000000LL)
            mark_duration = 1000000LL;   /* keep the tone audible */
        int64_t mark_end = mark_start + mark_duration;

        /*
         * Compare this element's start with where the previous element's
         * nominal mark + space should have ended. A large positive overrun
         * means scheduling delayed the keyer and the gap grew.
         */
        if (prev_expected_start != 0) {
            int64_t overrun = mark_start - prev_expected_start;

            if (overrun > keyer_max_overrun_ns)
                keyer_max_overrun_ns = overrun;
            if (overrun > keyer_late_threshold_ns)
                keyer_late_elements++;
        }
        prev_expected_start = mark_end + space_ns;

        if (queue_mark(mark_start, mark_duration) < 0)
            break;

        watch_keyer_until_ns(mark_end, &keyer);
        keyer_core_step(&keyer);            /* end of mark: IN -> AFTER */

        dec.gap_start = mark_end;
        dec.space_emitted = 0;

        watch_keyer_until_ns(mark_end + space_ns, &keyer);
        keyer_core_step(&keyer);            /* end of space: AFTER -> next */

        if (do_decode)
            decode_add_symbol(&dec, send_dit);
    }

    return NULL;
}

static int recover_pcm(snd_pcm_t *pcm, int err, const char *where)
{
    int rc;

    if (err == -EINTR)
        return 0;

    if (err == -EPIPE) {
        audio_underruns++;
        if (audio_underruns <= 3)
            fprintf(stderr,
                    "warning: ALSA underrun #%u during %s; recovering\n",
                    audio_underruns, where);
    }

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
    double phase_inc;
    double amplitude;
    double straight_gain;
    double straight_step;
    int straight_init;
} audio_state_t;

static uint64_t ns_to_frames(int64_t ns, unsigned int rate)
{
    return (uint64_t)llround((double)ns * rate / 1e9);
}

static void generate_audio(int16_t *buf, size_t frames,
                           audio_state_t *state, unsigned int rate)
{
    const double two_pi = 2.0 * M_PI;

    /*
     * Straight-key passthrough: gate the tone directly from the paddle
     * state instead of the mark queue.  A short raised-cosine ramp is
     * applied by slewing the gain one step per frame, so key-down/key-up
     * cannot click.  Level and pitch still follow the runtime controls.
     */
    if (straight_mode) {
        uint64_t rf = ns_to_frames(tone_ramp_ns(
            (int64_t)llround(1.2e9 / ALOAD(wpm))), rate);
        if (!rf)
            rf = 1;
        state->straight_step = 1.0 / (double)rf;
        state->phase_inc = two_pi * ALOAD(freq_hz) / rate;
        state->amplitude = (ALOAD(volume_percent) * 32767.0) / 100.0;
    }

    for (size_t i = 0; i < frames; i++, state->frame++) {
        double gain = 0.0;

        if (straight_mode) {
            double target = ALOAD(straight_down) ? 1.0 : 0.0;

            if (state->straight_gain < target) {
                state->straight_gain += state->straight_step;
                if (state->straight_gain > target)
                    state->straight_gain = target;
            } else if (state->straight_gain > target) {
                state->straight_gain -= state->straight_step;
                if (state->straight_gain < target)
                    state->straight_gain = target;
            }

            buf[i] = (int16_t)(state->straight_gain * state->amplitude *
                               sin(state->phase));
            state->phase += state->phase_inc;
            if (state->phase >= two_pi)
                state->phase -= two_pi;
            continue;
        }

        if (!state->has_pending && take_mark(&state->pending)) {
            state->has_pending = 1;
            /* Re-anchor after a long idle period to avoid accumulating
             * drift between the monotonic and PCM playback clocks. */
            if (!state->has_mark || state->pending.start_ns - state->anchor_ns >
                (int64_t)llround(1.2e9 / ALOAD(wpm)) * 7) {
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
            /* Speed, pitch, and level changes take effect at the start of
             * the next mark, so a change cannot step the tone mid-envelope. */
            state->gap_frames = ns_to_frames(keyer_space_ns(), rate);
            state->ramp_frames = ns_to_frames(tone_ramp_ns(
                (int64_t)llround(1.2e9 / ALOAD(wpm))), rate);
            if (state->ramp_frames > mark_frames / 2)
                state->ramp_frames = mark_frames / 2;
            state->phase_inc = two_pi * ALOAD(freq_hz) / rate;
            state->amplitude = (ALOAD(volume_percent) * 32767.0) / 100.0;
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

        /* gain is exactly 0 during silence, so this is 0 then. */
        buf[i] = (int16_t)(gain * state->amplitude * sin(state->phase));
        state->phase += state->phase_inc;
        if (state->phase >= two_pi)
            state->phase -= two_pi;
    }
}

/*
 * Runtime control from stdin.  These keys are easy to reach while keying
 * and do not collide with the default paddle keys:
 *
 *   [ / ]   slower / faster
 *   - / =   lower / higher pitch
 *   9 / 0   quieter / louder
 *
 * They only work when stdin is a readable terminal or pipe.  Startup values
 * come from the command line and environment as before.
 */
static void control_key(char key)
{
    int changed = 0;
    double value;
    int volume;

    switch (key) {
    case '[':
        value = ALOAD(wpm) - 1.0;
        if (value < 1.0)
            value = 1.0;
        wpm = value;
        changed = 1;
        break;

    case ']':
        value = ALOAD(wpm) + 1.0;
        if (value > MAX_WPM)
            value = MAX_WPM;
        wpm = value;
        changed = 1;
        break;

    case '-':
        value = ALOAD(freq_hz) - 10.0;
        if (value < 1.0)
            value = 1.0;
        freq_hz = value;
        changed = 1;
        break;

    case '=':
    case '+':
        value = ALOAD(freq_hz) + 10.0;
        if (value > (double)audio_rate / 2.0 - 1.0)
            value = (double)audio_rate / 2.0 - 1.0;
        freq_hz = value;
        changed = 1;
        break;

    case '9':
        volume = ALOAD(volume_percent) - 5;
        if (volume < 0)
            volume = 0;
        volume_percent = volume;
        changed = 1;
        break;

    case '0':
        volume = ALOAD(volume_percent) + 5;
        if (volume > 100)
            volume = 100;
        volume_percent = volume;
        changed = 1;
        break;

    case ',':
        value = ALOAD(weight_ms) - 1.0;
        if (value < -25.0)
            value = -25.0;
        weight_ms = value;
        changed = 1;
        break;

    case '.':
        value = ALOAD(weight_ms) + 1.0;
        if (value > 25.0)
            value = 25.0;
        weight_ms = value;
        changed = 1;
        break;

    default:
        break;
    }

    if (changed)
        fprintf(stderr, "[%.1f WPM, %.0f Hz, %d%%, weight %+.1f ms]\n",
                ALOAD(wpm), ALOAD(freq_hz), ALOAD(volume_percent),
                ALOAD(weight_ms));
}

static void *control_thread(void *arg)
{
    char buf[64];

    (void)arg;

    while (keep_running()) {
        struct pollfd pfd;
        ssize_t n;
        int pr;

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;

        pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;

        /* Pipes can report POLLIN and POLLHUP together after the writer
         * closes. Drain readable controls before treating the hangup as EOF. */
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;
            continue;
        }

        n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            break;
        }
        if (n == 0)
            break; /* EOF or closed stdin: no runtime controls. */

        for (ssize_t i = 0; i < n; i++)
            control_key(buf[i]);
    }

    return NULL;
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
    state.gap_frames = ns_to_frames(keyer_space_ns(), actual_rate);

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
    pthread_t control_tid;
    int control_started = 0;
    snd_pcm_t *pcm;
    int err;
    int field = 0;
    int audio_rc;
    unsigned int actual_rate = 0;
    char setup_path[64];
    char *device_path = NULL;

    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(argv[0]);
        return 0;
    }

    if (argc == 2 &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("cwkeyer %s\n", CWKEYER_VERSION);
        return 0;
    }

    if (argc == 2 &&
        (strcmp(argv[1], "--list-devices") == 0 ||
         strcmp(argv[1], "-L") == 0)) {
        return list_input_devices(stdout) == 0 ? 0 : 1;
    }

    if (argc >= 2 && strcmp(argv[1], "--setup") == 0) {
        if (argc != 2) {
            usage(argv[0]);
            return 1;
        }
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr, "--setup needs an interactive terminal on stdin\n");
            return 1;
        }
        if (run_setup_wizard(setup_path, sizeof(setup_path)) < 0)
            return 1;
        device_path = setup_path;
    } else if (argc == 1 && isatty(STDIN_FILENO)) {
        if (run_setup_wizard(setup_path, sizeof(setup_path)) < 0)
            return 1;
        device_path = setup_path;
    } else {
        if (argc < 2) {
            usage(argv[0]);
            return 1;
        }
        device_path = argv[1];
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

        if (strcasecmp(argv[i], "straight") == 0) {
            straight_mode = 1;
            continue;
        }

        switch (field) {
        case 0:
            wpm = parse_double_arg(argv[i], "wpm", 1.0, MAX_WPM);
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

    if (dit_code == dah_code && !straight_mode) {
        fprintf(stderr, "dit_key and dah_key must be different\n");
        return 1;
    }

    alsa_latency_us = parse_uint_env("CW_ALSA_LATENCY_US",
                                     20000,
                                     1000,
                                     100000);
    debounce_ms = parse_uint_env("CW_KEYER_DEBOUNCE_MS", 10, 0, 50);
    debounce_ns = (int64_t)debounce_ms * 1000000LL;
    volume_percent = (int)parse_uint_env("CW_KEYER_LEVEL", 70, 0, 100);
    weight_ms = parse_double_env("CW_KEYER_WEIGHT_MS", 0.0, -25.0, 25.0);

    if (install_signal_handlers() < 0)
        return 1;

    /* Safety net for exit paths (and future ones) that skip the explicit
     * restore below; it is a no-op when the terminal was never changed. */
    if (atexit(restore_terminal) != 0)
        fprintf(stderr, "warning: atexit(restore_terminal) failed\n");

    disable_terminal_echo();

    if (straight_mode && do_decode)
        fprintf(stderr,
                "warning: decode is ignored in straight-key mode\n");

    fprintf(stderr, "input device: %s\n", device_path);
    fprintf(stderr,
            "CW keyer: %.2f WPM, %.2f Hz, dit code %d, dah code %d, %s, "
            "%s, RC filter %u ms, level %d%%, weight %+.1f ms%s\n",
            ALOAD(wpm),
            ALOAD(freq_hz),
            dit_code,
            dah_code,
            do_grab ? "EVIOCGRAB enabled" : "nograb",
            straight_mode ? "straight-key passthrough" : "Curtis mode B",
            debounce_ms,
            ALOAD(volume_percent),
            ALOAD(weight_ms),
            (do_decode && !straight_mode) ? ", decode" : "");

    pcm = open_audio(&actual_rate);
    if (!pcm) {
        stop_signal_thread();
        restore_terminal();
        return 1;
    }
    audio_rate = actual_rate;

    err = pthread_create(&evdev_tid, NULL, evdev_thread, device_path);
    if (err != 0) {
        fprintf(stderr, "pthread_create(evdev): %s\n", strerror(err));
        stop_signal_thread();
        restore_terminal();
        snd_pcm_close(pcm);
        return 1;
    }

    err = pthread_create(&keyer_tid, NULL, keyer_thread, NULL);
    if (err != 0) {
        fprintf(stderr, "pthread_create(keyer): %s\n", strerror(err));
        request_stop(1);
        ASTORE(shutdown_wanted, 1);
        pthread_join(evdev_tid, NULL);
        stop_signal_thread();
        restore_terminal();
        snd_pcm_close(pcm);
        return 1;
    }

    err = pthread_create(&control_tid, NULL, control_thread, NULL);
    if (err != 0) {
        fprintf(stderr,
                "warning: pthread_create(control): %s; runtime controls "
                "disabled\n",
                strerror(err));
    } else {
        control_started = 1;
    }

    audio_rc = audio_loop(pcm, actual_rate);
    if (audio_rc < 0)
        request_stop(1);

    request_stop(0);
    ASTORE(shutdown_wanted, 1);
    pthread_join(keyer_tid, NULL);
    if (control_started)
        pthread_join(control_tid, NULL);
    pthread_join(evdev_tid, NULL);
    stop_signal_thread();

    snd_pcm_close(pcm);
    restore_terminal();

    /* Only mention the audio health if something actually went wrong. */
    if (audio_underruns || keyer_late_elements)
        fprintf(stderr,
                "audio: %u ALSA underrun(s), %u late element(s) "
                "(max %.1f ms)\n",
                audio_underruns, keyer_late_elements,
                (double)keyer_max_overrun_ns / 1000000.0);

    return ALOAD(fatal_error) ? 1 : 0;
}
