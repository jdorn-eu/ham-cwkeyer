// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * audio_alsa.c - Linux audio backend.
 *
 * Included at the end of cwkeyer.c; see platform.h for the contract.
 * The core hands us one generated period at a time; we push it at ALSA and
 * report what the server actually negotiated, because the sample rate it
 * picks is what the renderer counts frames in.
 *
 * The default PCM is "pulse", and an unset PULSE_SINK is pointed at the
 * cw_out combined sink created by cw-mic-mix.sh.  If that sink is not
 * available, the system's default sink is used instead, so a bare run still
 * makes a sound.
 */

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

/*
 * Requested latency.  audio_backend_open() re-reads CW_ALSA_LATENCY_US at
 * open time; the initialiser keeps open_audio() usable on its own, which is
 * how the audio tests drive it.
 */
static unsigned int alsa_latency_us = 20000;

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
            fprintf(stderr, "PULSE_SINK unset or empty; trying cw_out\n");
        } else {
            fprintf(stderr,
                    "warning: could not set PULSE_SINK=cw_out: %s\n",
                    strerror(errno));
        }
    }

    /*
     * If the cw_out sink was only our default, a missing or unusable one is
     * not a reason to refuse to run: fall back to the system's default sink
     * and say so.  An explicit PULSE_SINK still fails loudly.
     */
    for (;;) {
        pcm = NULL;
        err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
        if (err >= 0) {
            err = snd_pcm_set_params(pcm,
                                     SND_PCM_FORMAT_S16_LE,
                                     SND_PCM_ACCESS_RW_INTERLEAVED,
                                     1,
                                     AUDIO_RATE,
                                     1,
                                     alsa_latency_us);

            if (err < 0 && alsa_latency_us < 10000) {
                fprintf(stderr,
                        "warning: ALSA latency %u us failed: %s; "
                        "retrying 10000 us\n",
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
        }

        if (err >= 0)
            break;

        if (forced_sink) {
            fprintf(stderr,
                    "warning: cannot use the cw_out sink (%s); "
                    "using the default sink instead\n",
                    snd_strerror(err));
            (void)unsetenv("PULSE_SINK");
            forced_sink = 0;
            if (pcm) {
                snd_pcm_close(pcm);
                pcm = NULL;
            }
            continue;
        }

        if (pcm) {
            fprintf(stderr,
                    "snd_pcm_set_params failed for '%s': %s\n",
                    dev, snd_strerror(err));
            snd_pcm_close(pcm);
        } else {
            fprintf(stderr,
                    "cannot open ALSA playback device '%s': %s\n",
                    dev, snd_strerror(err));
        }
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

/*
 * Neutral backend entry points.  The PCM handle is backend state: the core
 * renders and the backend paces, and neither needs the other's objects.
 */
static snd_pcm_t *alsa_pcm;
static unsigned int alsa_rate;

int audio_backend_open(unsigned int *actual_rate)
{
    /* Read here rather than at startup: the value is only used to open. */
    alsa_latency_us = parse_uint_env("CW_ALSA_LATENCY_US", 20000, 1000, 100000);

    alsa_pcm = open_audio(actual_rate);
    if (!alsa_pcm)
        return -1;

    alsa_rate = *actual_rate;
    return 0;
}

int audio_backend_run(void)
{
    return audio_loop(alsa_pcm, alsa_rate);
}

void audio_backend_close(void)
{
    if (alsa_pcm) {
        snd_pcm_close(alsa_pcm);
        alsa_pcm = NULL;
    }
}

int audio_backend_supports_mix(void)
{
    return 0;
}

void audio_backend_usage(FILE *out)
{
    fputs("  CW_ALSA_DEVICE=pulse       ALSA PCM to open. Default: pulse\n"
          "  CW_ALSA_LATENCY_US=20000   Requested ALSA latency in us\n"
          "  PULSE_SINK=cw_out          If the device is pulse and PULSE_SINK is\n"
          "                             unset, it defaults to cw_out (created by\n"
          "                             cw-mic-mix.sh)\n", out);
}
