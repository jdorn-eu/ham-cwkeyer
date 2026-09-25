/* cwkeyer.c — low-latency iambic CW sidetone keyer
 * Reads a keyboard-emulating paddle adapter (Ctrl_L = dit, Ctrl_R = dah)
 * directly from evdev and drives an ALSA sidetone through an iambic keyer.
 *
 * build: gcc -O2 -o cwkeyer cwkeyer.c -lasound -lpthread -lm
 * run:   ./cwkeyer /dev/input/eventX [wpm=20] [freq_hz=600] [dit_key] [dah_key] [nograb]
 *
 * dit_key/dah_key name evdev keys (see keymap[] below), default LEFTCTRL/RIGHTCTRL.
 * "nograb" skips exclusive device grab - use this to test on your regular
 * keyboard so it keeps working normally for everything else while testing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <alsa/asoundlib.h>

static double wpm = 20.0;
static double freq = 600.0;
static const unsigned int rate = 48000;
static int dit_code = KEY_LEFTCTRL;
static int dah_code = KEY_RIGHTCTRL;
static int do_grab = 1;

static atomic_int dit_paddle = 0;
static atomic_int dah_paddle = 0;
static atomic_int key_on = 0;
static atomic_int running = 1;

static void handle_sigint(int sig) { (void)sig; running = 0; }

/* Maps a command-line key name to an evdev code. Add more here if needed. */
static int lookup_key(const char *name) {
    static const struct { const char *name; int code; } keymap[] = {
        {"LEFTCTRL", KEY_LEFTCTRL},   {"RIGHTCTRL", KEY_RIGHTCTRL},
        {"LEFTSHIFT", KEY_LEFTSHIFT}, {"RIGHTSHIFT", KEY_RIGHTSHIFT},
        {"LEFTALT", KEY_LEFTALT},     {"RIGHTALT", KEY_RIGHTALT},
        {"COMMA", KEY_COMMA},         {"DOT", KEY_DOT},
        {"LEFTBRACE", KEY_LEFTBRACE}, {"RIGHTBRACE", KEY_RIGHTBRACE},
        {"LEFT", KEY_LEFT},           {"RIGHT", KEY_RIGHT},
        {"INSERT", KEY_INSERT},       {"DELETE", KEY_DELETE},
        {"Z", KEY_Z},                 {"X", KEY_X},
        {NULL, 0}
    };
    for (int i = 0; keymap[i].name; i++)
        if (strcasecmp(name, keymap[i].name) == 0) return keymap[i].code;
    fprintf(stderr, "unknown key name '%s'\n", name);
    exit(1);
}

static void sleep_ns(long ns) {
    struct timespec ts = { .tv_sec = ns / 1000000000L, .tv_nsec = ns % 1000000000L };
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

/* Busy-waits for `ns` nanoseconds, OR-ing live paddle state into *dit_seen /
 * *dah_seen the whole time. Used to sample paddle contacts throughout an
 * element's mark and its following inter-element space - the mechanism
 * that gives Curtis-mode-B iambic keyers their paddle memory. */
static void watch_ns(long ns, int *dit_seen, int *dah_seen) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (atomic_load(&dit_paddle)) *dit_seen = 1;
        if (atomic_load(&dah_paddle)) *dah_seen = 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000000000L + (now.tv_nsec - start.tv_nsec);
        if (elapsed >= ns) break;
        struct timespec poll = { .tv_sec = 0, .tv_nsec = 200000L }; /* 0.2ms poll */
        clock_nanosleep(CLOCK_MONOTONIC, 0, &poll, NULL);
    }
}

/* Reads the adapter's keyboard events and updates paddle state. */
static void *evdev_thread(void *arg) {
    const char *path = (const char *)arg;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(1); }
    if (do_grab) {
        if (ioctl(fd, EVIOCGRAB, 1) < 0)
            fprintf(stderr, "warning: EVIOCGRAB failed (%s) - events may leak to the desktop\n", strerror(errno));
    }

    struct input_event ev;
    while (running) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) break;
        if (ev.type == EV_KEY && ev.value != 2) { /* ignore autorepeat */
            if (ev.code == dit_code) atomic_store(&dit_paddle, ev.value);
            if (ev.code == dah_code) atomic_store(&dah_paddle, ev.value);
        }
    }
    close(fd);
    return NULL;
}

/* Curtis 8044-style iambic Mode B keyer.
 *
 * Decision priority each cycle:
 *   1. A queued Mode-B element (opposite paddle was touched during the
 *      previous element's window) is sent directly - it never enters the
 *      alternation branch, so a single stray/overlap touch can only ever
 *      insert ONE extra element, never sustained oscillation.
 *   2. A genuine LIVE squeeze (both paddles down right now) alternates.
 *   3. A single live paddle just repeats itself.
 */
static void *keyer_thread(void *arg) {
    (void)arg;
    long dit_ns = (long)(1.2 / wpm * 1e9);   /* PARIS timing: dit(ms) = 1200/wpm */
    long dah_ns = dit_ns * 3;
    long space_ns = dit_ns;
    int last_was_dit = 0;
    int dit_mem = 0, dah_mem = 0; /* one-shot opposite-paddle memory */

    while (running) {
        int dit_live = atomic_load(&dit_paddle);
        int dah_live = atomic_load(&dah_paddle);
        int send_dit;

        if (dit_mem || dah_mem) {
            send_dit = dit_mem ? 1 : 0;   /* queued element: no alternation */
            dit_mem = dah_mem = 0;
        } else if (dit_live && dah_live) {
            send_dit = !last_was_dit;     /* genuine squeeze: alternate */
        } else if (dit_live) {
            send_dit = 1;
        } else if (dah_live) {
            send_dit = 0;
        } else {
            sleep_ns(1000000L);           /* 1ms idle poll */
            continue;
        }

        atomic_store(&key_on, 1);
        int seen_dit = 0, seen_dah = 0;
        watch_ns(send_dit ? dit_ns : dah_ns, &seen_dit, &seen_dah);
        atomic_store(&key_on, 0);
        last_was_dit = send_dit;

        watch_ns(space_ns, &seen_dit, &seen_dah);

        /* Latch memory only for the paddle opposite what we just sent. */
        if (send_dit) dah_mem = seen_dah;
        else           dit_mem = seen_dit;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s /dev/input/eventX [wpm=20] [freq_hz=600] [dit_key] [dah_key] [nograb]\n"
            "  dit_key/dah_key: LEFTCTRL RIGHTCTRL COMMA DOT LEFT RIGHT LEFTBRACE\n"
            "                   RIGHTBRACE LEFTSHIFT RIGHTSHIFT LEFTALT RIGHTALT\n"
            "                   INSERT DELETE Z X   (default LEFTCTRL RIGHTCTRL)\n"
            "  nograb: literal word 'nograb' - don't take exclusive control of the\n"
            "          device; use this when testing on your main keyboard so it\n"
            "          keeps working normally (test keys will also be typed wherever\n"
            "          focus is, so point focus at an empty terminal/editor)\n",
            argv[0]);
        return 1;
    }
    if (argc > 2) wpm = atof(argv[2]);
    if (argc > 3) freq = atof(argv[3]);
    if (argc > 4) dit_code = lookup_key(argv[4]);
    if (argc > 5) dah_code = lookup_key(argv[5]);
    if (argc > 6 && strcmp(argv[6], "nograb") == 0) do_grab = 0;

    signal(SIGINT, handle_sigint);

    pthread_t t_evdev, t_keyer;
    pthread_create(&t_evdev, NULL, evdev_thread, argv[1]);
    pthread_create(&t_keyer, NULL, keyer_thread, NULL);

    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "cannot open ALSA playback device\n");
        return 1;
    }
    /* mono, 48kHz, ~10ms total buffer latency */
    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                        1, rate, 1, 10000);

    const int period = 240; /* 5ms @ 48kHz — lower this for tighter latency */
    int16_t buf[period];
    double phase = 0.0, env = 0.0;
    double phase_inc = 2.0 * M_PI * freq / rate;
    double env_step = 1.0 / (rate * 0.003); /* 3ms click-free ramp */

    while (running) {
        int on = atomic_load(&key_on);
        double target = on ? 1.0 : 0.0;
        for (int i = 0; i < period; i++) {
            if (env < target) env += env_step; else if (env > target) env -= env_step;
            if (env < 0) env = 0;
            if (env > 1) env = 1;
            buf[i] = (int16_t)(env * 12000.0 * sin(phase));
            phase += phase_inc;
            if (phase > 2 * M_PI) phase -= 2 * M_PI;
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, period);
        if (w < 0) snd_pcm_recover(pcm, (int)w, 1);
    }

    snd_pcm_close(pcm);
    return 0;
}
