// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * cwkeyer.c — low-latency iambic CW sidetone keyer
 *
 * This file is the portable core: the Curtis 8044 type B iambic keyer, the
 * RC paddle filter, the weighting, the Morse decoder, the PCM renderer, the
 * terminal handling, argument parsing, and main. It reads a paddle adapter
 * through an input backend and plays a sidetone through an audio backend;
 * both are included at the end of this file, so the core never names a
 * platform API and a port is a new pair of backend files (see platform.h).
 *
 * Key names are portable (CWKEY_*) and map to the platform's own codes; the
 * default mapping is left control for dit and right control for dah.
 *
 * Build:
 *   make        (strict: -Werror plus a broad warning set)
 *   make test   (the hardware-independent suites)
 *
 * Which device to pass and which environment variables apply depends on
 * the backend; the README documents the Linux ones (an evdev node such as
 * /dev/input/event3, CW_ALSA_DEVICE, CW_ALSA_LATENCY_US, PULSE_SINK).  The
 * keyer's own settings are the same everywhere: CW_KEYER_LEVEL,
 * CW_KEYER_WEIGHT_MS, and CW_KEYER_DEBOUNCE_MS, plus the runtime keys.
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
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

#include "platform.h"

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

static int dit_code = CWKEY_LEFTCTRL;
static int dah_code = CWKEY_RIGHTCTRL;
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
 * Press order.  The input thread sees the levers in the order the device
 * reports them, so it stamps each press with a monotonic counter.  A
 * currently-held lever keeps the stamp of the press that closed it, so of
 * two held levers the smaller stamp is the one closed first.  0 means "not
 * pressed this session" (for example a lever already held at startup).
 */
static atomic_uint dit_press_seq = 0;
static atomic_uint dah_press_seq = 0;
static unsigned press_seq = 0;   /* input thread only */

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
                input_backend_suspend();
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
                    input_backend_resume();
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
    fprintf(stderr, "usage: %s ", prog);
    input_backend_usage(stderr, USAGE_DEVICE_ARG, prog);
    fprintf(stderr,
        " [wpm=12] [freq_hz=800] [dit_key] [dah_key] [nograb] [decode] [straight]\n"
        "       %s --help | --version | --list-devices | --setup\n"
        "\n"
        "Examples:\n", prog);
    input_backend_usage(stderr, USAGE_EXAMPLES, prog);
    fprintf(stderr,
        "\n"
        "Known key names include:\n"
        "  LEFTCTRL RIGHTCTRL LEFTSHIFT RIGHTSHIFT LEFTALT RIGHTALT\n"
        "  COMMA DOT PERIOD LEFT RIGHT LEFTBRACE RIGHTBRACE\n"
        "  INSERT DELETE Z X\n"
        "\n");
    input_backend_usage(stderr, USAGE_KEYNOTE, prog);
    fprintf(stderr,
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
        "  --list-devices, -L\n");
    input_backend_usage(stderr, USAGE_DEVICE_OPTS, prog);
    fprintf(stderr,
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
        "  CW_KEYER_LEVEL=70          Sidetone level, percent of full scale\n"
        "  CW_KEYER_DEBOUNCE_MS=10    Paddle RC filter time constant, 0..50 ms\n"
        "  CW_KEYER_WEIGHT_MS=0       Weighting, -25..25 ms; a constant time\n"
        "                             added to each mark and taken from the\n"
        "                             following space (0 is strict ITU timing)\n");
    audio_backend_usage(stderr);
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
 * Maps command-line key names to key codes.  The names are portable; the
 * codes come from the platform's CWKEY_* namespace.
 * Add more names here if needed.
 */
static int lookup_key(const char *name)
{
    static const struct {
        const char *name;
        int code;
    } keymap[] = {
        {"LEFTCTRL",    CWKEY_LEFTCTRL},
        {"RIGHTCTRL",   CWKEY_RIGHTCTRL},
        {"CTRL_L",      CWKEY_LEFTCTRL},
        {"CTRL_R",      CWKEY_RIGHTCTRL},
        {"LCONTROL",    CWKEY_LEFTCTRL},
        {"RCONTROL",    CWKEY_RIGHTCTRL},

        {"LEFTSHIFT",   CWKEY_LEFTSHIFT},
        {"RIGHTSHIFT",  CWKEY_RIGHTSHIFT},
        {"SHIFT_L",     CWKEY_LEFTSHIFT},
        {"SHIFT_R",     CWKEY_RIGHTSHIFT},

        {"LEFTALT",     CWKEY_LEFTALT},
        {"RIGHTALT",    CWKEY_RIGHTALT},
        {"ALT_L",       CWKEY_LEFTALT},
        {"ALT_R",       CWKEY_RIGHTALT},

        {"COMMA",       CWKEY_COMMA},
        {"DOT",         CWKEY_DOT},
        {"PERIOD",      CWKEY_DOT},

        {"LEFTBRACE",   CWKEY_LEFTBRACE},
        {"RIGHTBRACE",  CWKEY_RIGHTBRACE},

        {"LEFT",        CWKEY_LEFT},
        {"RIGHT",       CWKEY_RIGHT},
        {"INSERT",      CWKEY_INSERT},
        {"DELETE",      CWKEY_DELETE},

        {"Z",           CWKEY_Z},
        {"X",           CWKEY_X},

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
     * Also allow raw numeric key codes.
     */
    {
        char *end = NULL;
        long code;

        errno = 0;
        code = strtol(name, &end, 0);

        if (errno == 0 && end != name && *end == '\0' &&
            code >= 0 && code <= CWKEY_MAX) {
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

    /*
     * nanosleep() is relative and, on both Linux and macOS, not affected by
     * changes to the wall clock, which is what the monotonic deadlines around
     * it need. clock_nanosleep() would say so explicitly but is Linux-only.
     */
    while (keep_running()) {
        struct timespec rem;
        int rc = nanosleep(&req, &rem);

        if (rc == 0)
            return;

        if (rc < 0 && errno == EINTR) {
            req = rem;
            continue;
        }

        fprintf(stderr, "nanosleep: %s\n", strerror(errno));
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
 * Input devices, as the core sees them.
 *
 * A backend fills this in and the core only compares and prints it: `id` is
 * the platform's stable name for the device (a /dev/input/event node, a
 * vendor:product:location string), `order` sorts devices within a group, and
 * `handle` is whatever the backend needs to reach the device.  The core never
 * dereferences `handle`; only the backend's primitives do.
 */
struct input_device_t {
    char id[64];
    char name[256];
    unsigned short vendor;
    unsigned short product;
    int order;           /* backend sort key within a group */
    device_kind_t kind;
    int default_paddles;
    int readable;
    int errnum;
    void *handle;        /* backend-private */
};

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

static void copy_device_id(char *dest, size_t size, const char *id)
{
    size_t len = strlen(id);

    if (len >= size)
        len = size - 1;

    memcpy(dest, id, len);
    dest[len] = '\0';
}

/*
 * Find the single known paddle adapter in the discovered list.  More than
 * one known adapter is ambiguous, so the caller falls back to tapping.
 */
static size_t find_known_paddle(const input_device_t *devices, size_t count)
{
    size_t found = SIZE_MAX;

    for (size_t i = 0; i < count; i++) {
        if (!devices[i].readable)
            continue;
        if (known_paddle_by_id(devices[i].vendor, devices[i].product) == NULL)
            continue;
        if (found != SIZE_MAX)
            return SIZE_MAX;
        found = i;
    }

    return found;
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
            device->id);
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

    if (input_backend_discover(&devices, &count) < 0)
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
                CLR(use_color, ANSI_RESET), devices[known_index].id,
                dit, dah);
    } else {
        for (;;) {
            size_t which = SIZE_MAX;
            size_t second = SIZE_MAX;
            int code = 0;
            int rc;

            input_backend_drain(devices, count);
            fprintf(stderr, "  %s→%s Tap the DOT (dit) paddle, then release it.\n",
                    CLR(use_color, ANSI_CYAN), CLR(use_color, ANSI_RESET));
            rc = input_backend_wait_press(devices, count, SIZE_MAX,
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
            rc = input_backend_wait_release(&devices[which], code,
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

            rc = input_backend_wait_press(devices, count, which,
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
            CLR(use_color, ANSI_RESET), devices[selected].id,
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
    copy_device_id(device_path, path_size, devices[selected].id);
    result = 0;

out:
    input_backend_close_devices(devices, count);
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

    if (ea->kind != eb->kind)
        return ea->kind < eb->kind ? -1 : 1;

    if (ea->order != eb->order)
        return ea->order < eb->order ? -1 : 1;

    return strcmp(ea->id, eb->id);
}

int input_backend_list(FILE *out)
{
    input_device_t *devices;
    size_t count;
    int unreadable = 0;
    int current_kind = -1;
    int use_color;

    if (input_backend_discover(&devices, &count) < 0)
        return -1;

    if (count == 0) {
        fprintf(out, "no %s devices found\n", input_backend_devices_noun());
        input_backend_close_devices(devices, count);
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

        if (!devices[i].readable) {
            print_unreadable_line(out, devices[i].id, devices[i].errnum);
            unreadable++;
        } else {
            const char *known = known_paddle_label(devices[i].vendor,
                                                   devices[i].product);
            print_device_line(out, devices[i].id, devices[i].name,
                              devices[i].default_paddles, known);
        }
    }

    if (unreadable) {
        fprintf(out, "\n%d device(s) could not be read; %s.\n",
                unreadable, input_backend_unreadable_hint());
    }

    input_backend_close_devices(devices, count);
    return 0;
}

/*
 * The keyer reads paddles through the input backend, not stdin, but key
 * presses still reach the controlling terminal and the tty line discipline
 * would echo them. Disable echo and canonical mode while running; ISIG is
 * kept so Ctrl+C still works.
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
/*
 * Runtime status display.  On a terminal the line is rewritten in place so
 * repeated control-key changes do not scroll the screen away.
 */
static void write_control_status(FILE *out, const char *status, int is_tty,
                                 size_t *last_len)
{
    size_t len = strlen(status);

    if (!is_tty) {
        fprintf(out, "%s\n", status);
        return;
    }

    fputc('\r', out);
    fputs(status, out);
    for (size_t i = len; i < *last_len; i++)
        fputc(' ', out);

    *last_len = len;
}

static size_t control_status_len = 0;

static void print_control_status(const char *status)
{
    int is_tty = isatty(STDERR_FILENO);

    write_control_status(stderr, status, is_tty, &control_status_len);
    if (is_tty)
        fflush(stderr);
}

static void finish_control_status(void)
{
    if (control_status_len == 0)
        return;

    fputc('\n', stderr);
    control_status_len = 0;
}

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

    if (changed) {
        char status[128];

        snprintf(status, sizeof(status),
                 "[%.1f WPM, %.0f Hz, %d%%, weight %+.1f ms]",
                 ALOAD(wpm), ALOAD(freq_hz), ALOAD(volume_percent),
                 ALOAD(weight_ms));
        print_control_status(status);
    }
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


int main(int argc, char **argv)
{
    pthread_t input_tid;
    pthread_t keyer_tid;
    pthread_t control_tid;
    int control_started = 0;
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
        return input_backend_list(stdout) == 0 ? 0 : 1;
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
        if (input_backend_setup(setup_path, sizeof(setup_path)) < 0)
            return 1;
        device_path = setup_path;
    } else if (argc == 1 && isatty(STDIN_FILENO)) {
        if (input_backend_setup(setup_path, sizeof(setup_path)) < 0)
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
            input_backend_grab_description(),
            straight_mode ? "straight-key passthrough" : "Curtis mode B",
            debounce_ms,
            ALOAD(volume_percent),
            ALOAD(weight_ms),
            (do_decode && !straight_mode) ? ", decode" : "");

    if (audio_backend_open(&actual_rate) < 0) {
        stop_signal_thread();
        restore_terminal();
        return 1;
    }
    audio_rate = actual_rate;

    err = pthread_create(&input_tid, NULL, input_backend_thread, device_path);
    if (err != 0) {
        fprintf(stderr, "pthread_create(input): %s\n", strerror(err));
        stop_signal_thread();
        restore_terminal();
        audio_backend_close();
        return 1;
    }

    err = pthread_create(&keyer_tid, NULL, keyer_thread, NULL);
    if (err != 0) {
        fprintf(stderr, "pthread_create(keyer): %s\n", strerror(err));
        request_stop(1);
        ASTORE(shutdown_wanted, 1);
        pthread_join(input_tid, NULL);
        stop_signal_thread();
        restore_terminal();
        audio_backend_close();
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

    audio_rc = audio_backend_run();
    if (audio_rc < 0)
        request_stop(1);

    request_stop(0);
    ASTORE(shutdown_wanted, 1);
    pthread_join(keyer_tid, NULL);
    if (control_started)
        pthread_join(control_tid, NULL);
    pthread_join(input_tid, NULL);
    stop_signal_thread();

    audio_backend_close();
    restore_terminal();

    finish_control_status();

    /* Only mention the audio health if something actually went wrong. */
    if (audio_underruns || keyer_late_elements)
        fprintf(stderr,
                "audio: %u underrun(s), %u late element(s) "
                "(max %.1f ms)\n",
                audio_underruns, keyer_late_elements,
                (double)keyer_max_overrun_ns / 1000000.0);

    return ALOAD(fatal_error) ? 1 : 0;
}

/*
 * Platform backends.  They are included, not linked: this keeps the whole
 * program in one translation unit so a backend can use the core's statics
 * (and so the test suites can include cwkeyer.c and reach into internals).
 * Everything a backend must provide is declared in platform.h.
 */
#if defined(__APPLE__)
#include "input_hid.c"
#include "audio_coreaudio.c"
#elif defined(__linux__)
#include "input_evdev.c"
#include "audio_alsa.c"
#else
#error "no platform backend for this target"
#endif

