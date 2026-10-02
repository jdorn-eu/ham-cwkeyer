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
 *   ./cwkeyer /dev/input/eventX [wpm=20] [freq_hz=600] [dit_key] [dah_key] [nograb] [decode]
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
 *   PULSE_SINK=cw_out               If the device is "pulse" and PULSE_SINK is
 *                                   unset, it defaults to "cw_out" (created by
 *                                   cw-mic-mix.sh). Set it explicitly to a real
 *                                   sink if you haven't run that script.
 *
 * For your mic-mixing setup, keeping ALSA device "default" is intentional:
 *
 *   PULSE_SINK=cw_out ./cwkeyer /dev/input/eventX
 *
 * This works when your ALSA default device is routed through PulseAudio or
 * PipeWire-Pulse.
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

static double wpm = 20.0;
static double freq_hz = 600.0;

static int dit_code = KEY_LEFTCTRL;
static int dah_code = KEY_RIGHTCTRL;
static int do_grab = 1;
static int do_decode = 0;

static unsigned int alsa_latency_us = 20000;

static atomic_int dit_paddle = 0;
static atomic_int dah_paddle = 0;

/*
 * Edge latches prevent very short key-down/key-up pulses from being missed
 * between keyer polling intervals.
 */
static atomic_int dit_edge = 0;
static atomic_int dah_edge = 0;

static atomic_int key_on = 0;
static atomic_int running = 1;
static atomic_int fatal_error = 0;

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

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

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    /*
     * Deliberately no SA_RESTART: blocking poll()/ALSA calls should wake up
     * promptly on Ctrl+C.
     */
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        fprintf(stderr, "sigaction(SIGINT): %s\n", strerror(errno));
        return -1;
    }

    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        fprintf(stderr, "sigaction(SIGTERM): %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s /dev/input/eventX [wpm=20] [freq_hz=600] [dit_key] [dah_key] [nograb] [decode]\n"
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

static void sample_paddles(int *dit_seen, int *dah_seen)
{
    int dit_latched = atomic_exchange_explicit(&dit_edge, 0,
                                               memory_order_relaxed);
    int dah_latched = atomic_exchange_explicit(&dah_edge, 0,
                                               memory_order_relaxed);

    if (ALOAD(dit_paddle) || dit_latched)
        *dit_seen = 1;

    if (ALOAD(dah_paddle) || dah_latched)
        *dah_seen = 1;
}

/*
 * Wait for ns nanoseconds while continuously OR-ing live and edge-latched
 * paddle state into dit_seen/dah_seen.
 *
 * This implements paddle memory during both mark and following inter-element
 * space.
 */
static void watch_ns(int64_t ns, int *dit_seen, int *dah_seen)
{
    int64_t deadline = mono_ns() + ns;

    while (keep_running()) {
        int64_t remaining;

        sample_paddles(dit_seen, dah_seen);

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
    ASTORE(key_on, 0);
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

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        request_stop(1);
        return NULL;
    }

    warn_if_key_not_supported(fd, dit_code, "dit");
    warn_if_key_not_supported(fd, dah_code, "dah");
    initialize_paddle_state_from_device(fd);

    if (do_grab) {
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
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

    if (do_grab) {
        if (ioctl(fd, EVIOCGRAB, 0) < 0) {
            fprintf(stderr,
                    "warning: EVIOCGRAB release failed: %s\n",
                    strerror(errno));
        }
    }

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
static int termios_modified = 0;

static void disable_terminal_echo(void)
{
    struct termios t;

    if (!isatty(STDIN_FILENO))
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
    if (termios_modified)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

/*
 * Morse code table for the decode display. Unknown symbols report '?',
 * which the caller drops.
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
    {'-', ".-.-."},  {'(', "-.--."},  {')', "-.--.-"}, {'"', ".-..-"},
    {'\'', ".----."}, {'@', ".--.-."}, {'&', "....-."}, {'=', "-...-"},
    {0, NULL}
};

static char morse_decode(const char *s, size_t len)
{
    char buf[16];

    if (len >= sizeof(buf))
        return '?';

    memcpy(buf, s, len);
    buf[len] = '\0';

    for (int i = 0; morse_table[i].code; i++) {
        if (strcmp(morse_table[i].code, buf) == 0)
            return morse_table[i].ch;
    }

    return '?';
}

/*
 * Curtis-8044-style iambic Mode B keyer.
 *
 * Decision priority:
 *
 *   1. Queued one-shot opposite-paddle memory element.
 *   2. Genuine live squeeze: alternate relative to previous element.
 *   3. Single live/requested paddle: repeat that element.
 *
 * The edge latches prevent short key-down pulses from being missed while idle
 * or during timing windows.
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

    int last_was_dit = 0;
    int dit_mem = 0;
    int dah_mem = 0;
    char sym[16];
    size_t sym_len = 0;
    int64_t gap_start = 0;    /* mono_ns() when the current idle gap began */
    int printed_char = 0;     /* at least one decoded character printed */
    int space_emitted = 0;    /* space already printed for this idle gap */

    while (keep_running()) {
        int dit_event;
        int dah_event;
        int dit_live;
        int dah_live;
        int dit_req;
        int dah_req;
        int send_dit;
        int seen_dit;
        int seen_dah;

        /*
         * Consume idle-time edge latches. If a very short press occurred
         * between iterations, this still starts an element.
         */
        dit_event = atomic_exchange_explicit(&dit_edge, 0,
                                             memory_order_relaxed);
        dah_event = atomic_exchange_explicit(&dah_edge, 0,
                                             memory_order_relaxed);

        dit_live = ALOAD(dit_paddle);
        dah_live = ALOAD(dah_paddle);

        dit_req = dit_live || dit_event;
        dah_req = dah_live || dah_event;

        if (dit_mem || dah_mem) {
            send_dit = dit_mem ? 1 : 0;
            dit_mem = 0;
            dah_mem = 0;
        } else if (dit_req && dah_req) {
            send_dit = !last_was_dit;
        } else if (dit_req) {
            send_dit = 1;
        } else if (dah_req) {
            send_dit = 0;
        } else {
            /*
             * Idle: the symbol being keyed is complete. If the decode
             * display is active, print it.
             */
            if (sym_len) {
                if (do_decode) {
                    char ch = morse_decode(sym, sym_len);

                    /* Unknown symbols are dropped. */
                    if (ch != '?') {
                        fputc(ch, stdout);
                        printed_char = 1;
                    }

                    fflush(stdout);
                }
                sym_len = 0;
            }

            /*
             * Decode display: an idle gap longer than seven dits (the ITU
             * word gap) between characters is a word gap; print a single
             * space for it.
             */
            if (do_decode && gap_start > 0 &&
                printed_char && !space_emitted &&
                (mono_ns() - gap_start) > word_gap_ns) {
                fputc(' ', stdout);
                fflush(stdout);
                space_emitted = 1;
            }

            /*
             * Idle poll interval. Lower values reduce input-to-keyer latency
             * but increase wakeups.
             */
            sleep_ns(200000LL);
            continue;
        }

        /*
         * Include the start-of-element requests in the memory window.
         */
        seen_dit = dit_req;
        seen_dah = dah_req;

        ASTORE(key_on, 1);
        watch_ns(send_dit ? dit_ns : dah_ns, &seen_dit, &seen_dah);

        ASTORE(key_on, 0);
        last_was_dit = send_dit;
        gap_start = mono_ns();
        space_emitted = 0;

        watch_ns(space_ns, &seen_dit, &seen_dah);

        if (sym_len + 1 < sizeof(sym))
            sym[sym_len++] = send_dit ? '.' : '-';

        /*
         * Curtis Mode B one-shot memory: latch only the opposite paddle.
         */
        if (send_dit)
            dah_mem = seen_dah;
        else
            dit_mem = seen_dit;
    }

    ASTORE(key_on, 0);
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

static snd_pcm_t *open_audio(void)
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
        if (setenv("PULSE_SINK", "cw_out", 0) == 0) {
            forced_sink = 1;
            fprintf(stderr, "PULSE_SINK not set; defaulting to cw_out\n");
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
                    "PULSE_SINK was defaulted to 'cw_out', which does not "
                    "exist.\n"
                    "Run ./cw-mic-mix.sh first, or set PULSE_SINK to a real "
                    "sink,\n"
                    "e.g. PULSE_SINK=$(pactl get-default-sink)\n");
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
    if (hw && snd_pcm_hw_params_current(pcm, hw) == 0) {
        unsigned int rate = 0;
        snd_pcm_uframes_t buffer = 0, period = 0;

        snd_pcm_hw_params_get_rate(hw, &rate, 0);
        snd_pcm_hw_params_get_buffer_size(hw, &buffer);
        snd_pcm_hw_params_get_period_size(hw, &period, 0);

        if (rate) {
            fprintf(stderr,
                    "ALSA achieved: rate %u Hz, buffer %lu frames (%.1f ms), "
                    "period %lu frames (%.1f ms)\n",
                    rate, (unsigned long)buffer, 1000.0 * buffer / rate,
                    (unsigned long)period, 1000.0 * period / rate);
        }
    }

    return pcm;
}

static void generate_audio(int16_t *buf,
                           size_t frames,
                           double *phase,
                           double *env,
                           double phase_inc,
                           double env_step)
{
    const double two_pi = 2.0 * M_PI;
    const double amplitude = 12000.0;

    for (size_t i = 0; i < frames; i++) {
        double target = ALOAD(key_on) ? 1.0 : 0.0;
        double sample;

        if (*env < target) {
            *env += env_step;
            if (*env > target)
                *env = target;
        } else if (*env > target) {
            *env -= env_step;
            if (*env < target)
                *env = target;
        }

        sample = (*env) * amplitude * sin(*phase);

        if (sample > 32767.0)
            sample = 32767.0;
        else if (sample < -32768.0)
            sample = -32768.0;

        buf[i] = (int16_t)sample;

        *phase += phase_inc;
        if (*phase >= two_pi)
            *phase -= two_pi;
    }
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
            snd_pcm_wait(pcm, 20);
            continue;
        }

        if (n < 0) {
            if (recover_pcm(pcm, (int)n, "write") < 0)
                return -1;

            continue;
        }

        if (n == 0) {
            snd_pcm_wait(pcm, 20);
            continue;
        }

        off += (snd_pcm_uframes_t)n;
    }

    return 0;
}

static int audio_loop(snd_pcm_t *pcm)
{
    int16_t buf[AUDIO_PERIOD_FRAMES];

    double phase = 0.0;
    double env = 0.0;

    const double phase_inc = 2.0 * M_PI * freq_hz / (double)AUDIO_RATE;

    /*
     * 3 ms ramp to avoid clicks.
     */
    const double env_step = 1.0 / ((double)AUDIO_RATE * 0.003);

    int rc = 0;

    while (keep_running()) {
        generate_audio(buf,
                       AUDIO_PERIOD_FRAMES,
                       &phase,
                       &env,
                       phase_inc,
                       env_step);

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

    if (install_signal_handlers() < 0)
        return 1;

    disable_terminal_echo();

    fprintf(stderr,
            "CW keyer: %.2f WPM, %.2f Hz, dit code %d, dah code %d, %s%s\n",
            wpm,
            freq_hz,
            dit_code,
            dah_code,
            do_grab ? "EVIOCGRAB enabled" : "nograb",
            do_decode ? ", decode" : "");

    pcm = open_audio();
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

    audio_rc = audio_loop(pcm);
    if (audio_rc < 0)
        request_stop(1);

    request_stop(0);
    ASTORE(key_on, 0);

    pthread_join(keyer_tid, NULL);
    pthread_join(evdev_tid, NULL);

    snd_pcm_close(pcm);
    restore_terminal();

    return ALOAD(fatal_error) ? 1 : 0;
}
