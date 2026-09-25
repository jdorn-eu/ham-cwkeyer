/* cwkeyer.c — low-latency iambic CW sidetone keyer
 * Reads a keyboard-emulating paddle adapter (Ctrl_L = dit, Ctrl_R = dah)
 * directly from evdev and drives an ALSA sidetone through an iambic keyer.
 *
 * build: gcc -O2 -o cwkeyer cwkeyer.c -lasound -lpthread -lm
 * run:   ./cwkeyer /dev/input/eventX [wpm=20] [freq_hz=600]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static atomic_int dit_paddle = 0;
static atomic_int dah_paddle = 0;
static atomic_int key_on = 0;
static atomic_int running = 1;

static void handle_sigint(int sig) { (void)sig; running = 0; }

static void sleep_ns(long ns) {
    struct timespec ts = { .tv_sec = ns / 1000000000L, .tv_nsec = ns % 1000000000L };
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

/* Reads the adapter's keyboard events and updates paddle state. */
static void *evdev_thread(void *arg) {
    const char *path = (const char *)arg;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(1); }
    if (ioctl(fd, EVIOCGRAB, 1) < 0)
        fprintf(stderr, "warning: EVIOCGRAB failed (%s) - events may leak to the desktop\n", strerror(errno));

    struct input_event ev;
    while (running) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) break;
        if (ev.type == EV_KEY && ev.value != 2) { /* ignore autorepeat */
            if (ev.code == KEY_LEFTCTRL)  atomic_store(&dit_paddle, ev.value);
            if (ev.code == KEY_RIGHTCTRL) atomic_store(&dah_paddle, ev.value);
        }
    }
    close(fd);
    return NULL;
}

/* Iambic keyer: self-completing elements, alternates on squeeze. */
static void *keyer_thread(void *arg) {
    (void)arg;
    long dit_ns = (long)(1.2 / wpm * 1e9);   /* PARIS timing: dit(ms) = 1200/wpm */
    long dah_ns = dit_ns * 3;
    long space_ns = dit_ns;
    int last_was_dit = 0;

    while (running) {
        int dit = atomic_load(&dit_paddle);
        int dah = atomic_load(&dah_paddle);
        if (dit || dah) {
            int do_dit = (dit && dah) ? !last_was_dit : dit;
            atomic_store(&key_on, 1);
            sleep_ns(do_dit ? dit_ns : dah_ns);
            atomic_store(&key_on, 0);
            last_was_dit = do_dit;
            sleep_ns(space_ns);
        } else {
            sleep_ns(1000000L); /* 1ms idle poll */
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /dev/input/eventX [wpm=20] [freq_hz=600]\n", argv[0]);
        return 1;
    }
    if (argc > 2) wpm = atof(argv[2]);
    if (argc > 3) freq = atof(argv[3]);

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
