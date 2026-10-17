// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Audio-layer tests. These use ALSA's built-in "null" PCM device, which is
 * part of alsa-lib and needs no sound server or hardware, so they exercise
 * open_audio(), pcm_write_all() and audio_loop() for real.
 */
int cwkeyer_program_main(int argc, char **argv);
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

static int failures;

static void redirect_stderr(int *saved)
{
    *saved = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
}

static void restore_stderr(int saved)
{
    if (saved >= 0) {
        dup2(saved, STDERR_FILENO);
        close(saved);
    }
}

static void *loop_thread(void *arg)
{
    snd_pcm_t *pcm = arg;

    audio_loop(pcm, AUDIO_RATE);
    return NULL;
}

int main(void)
{
    const char *null_device = "null";
    unsigned int rate = 0;
    snd_pcm_t *pcm;
    int saved;

    /* Valid device: open, then write one generated period for real. */
    setenv("CW_ALSA_DEVICE", null_device, 1);
    pcm = open_audio(&rate);
    if (!pcm) {
        fprintf(stderr, "audio: open_audio(\"null\") failed\n");
        return 1;
    }
    if (rate == 0) {
        fprintf(stderr, "audio: negotiated rate is zero\n");
        failures++;
    }

    audio_state_t st = {0};
    st.gap_frames = ns_to_frames(100000000LL, rate);
    queue_mark(1000000000LL, 100000000LL);

    int16_t buf[AUDIO_PERIOD_FRAMES];
    for (int i = 0; i < 4; i++) {
        generate_audio(buf, AUDIO_PERIOD_FRAMES, &st, rate);
        if (pcm_write_all(pcm, buf, AUDIO_PERIOD_FRAMES) < 0) {
            fprintf(stderr, "audio: pcm_write_all failed\n");
            failures++;
            break;
        }
    }

    /* Run the real audio loop in a thread, then stop it. */
    ASTORE(running, 1);
    stop_requested = 0;

    pthread_t tid;
    if (pthread_create(&tid, NULL, loop_thread, pcm) != 0) {
        fprintf(stderr, "audio: pthread_create failed\n");
        snd_pcm_close(pcm);
        return 1;
    }

    struct timespec pause = {0, 50000000}; /* 50 ms */
    nanosleep(&pause, NULL);
    ASTORE(running, 0);
    pthread_join(tid, NULL);

    /* An EPIPE is counted as an underrun and recovered. */
    audio_underruns = 0;
    redirect_stderr(&saved);
    int rc = recover_pcm(pcm, -EPIPE, "test");
    restore_stderr(saved);
    if (rc != 0 || audio_underruns != 1) {
        fprintf(stderr, "audio: recover_pcm(EPIPE) not counted\n");
        failures++;
    }

    snd_pcm_close(pcm);

    /* A non-existent device must fail cleanly. */
    setenv("CW_ALSA_DEVICE", "cwkeyer_no_such_device", 1);
    redirect_stderr(&saved);
    pcm = open_audio(&rate);
    restore_stderr(saved);
    if (pcm) {
        fprintf(stderr, "audio: bad device unexpectedly opened\n");
        snd_pcm_close(pcm);
        failures++;
    }

    /* A tone at or above Nyquist must be rejected before playback. */
    setenv("CW_ALSA_DEVICE", null_device, 1);
    freq_hz = (double)AUDIO_RATE / 2.0;
    redirect_stderr(&saved);
    pcm = open_audio(&rate);
    restore_stderr(saved);
    freq_hz = 800.0;
    if (pcm) {
        fprintf(stderr, "audio: Nyquist tone unexpectedly accepted\n");
        snd_pcm_close(pcm);
        failures++;
    }

    if (failures == 0)
        printf("audio tests passed\n");
    else
        fprintf(stderr, "audio: %d test(s) failed\n", failures);

    return failures ? 1 : 0;
}
