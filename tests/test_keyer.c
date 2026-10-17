/* Hardware-independent regression tests for decoding and PCM mark timing. */
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

static int check_code(char expected, const char *code)
{
    char actual = morse_decode(code, strlen(code));
    if (actual == expected)
        return 0;
    fprintf(stderr, "decode %s: expected %d, got %d\n", code, expected, actual);
    return 1;
}

static void render_blocked(int16_t *samples, size_t frames,
                           audio_state_t *state, unsigned int rate)
{
    for (size_t off = 0; off < frames; off += AUDIO_PERIOD_FRAMES) {
        size_t count = frames - off;
        if (count > AUDIO_PERIOD_FRAMES)
            count = AUDIO_PERIOD_FRAMES;
        generate_audio(samples + off, count, state, rate);
    }
}

int main(void)
{
    int failures = 0;
    int16_t samples[4800];
    audio_state_t state = {0};
    const int64_t start_ns = 1000000000LL;
    state.gap_frames = 4800; /* One dit at 12 WPM and 48 kHz. */

    failures += check_code('?', "..--..");
    failures += check_code('-', "-....-");
    failures += check_code('"', ".-..-.");
    failures += check_code('&', ".-...");
    failures += check_code('\0', "------...");

    if (tone_ramp_ns(100000000) != 5000000LL ||
        tone_ramp_ns(4000000) != 2000000LL) {
        fprintf(stderr, "incorrect ramp duration\n");
        failures++;
    }

    /* Enqueue the next mark early: rendering must retain a full mark and
     * insert the one-dit gap, irrespective of when the event was received. */
    queue_mark(start_ns, 100000000LL);
    queue_mark(start_ns + 100000000LL, 100000000LL);
    render_blocked(samples, 4800, &state, AUDIO_RATE);
    if (state.mark_start != 0 || state.mark_end != 4800 ||
        samples[0] != 0 || samples[4799] != 0 ||
        samples[15] < 200 || samples[15] > 300) {
        fprintf(stderr, "first mark length or raised-cosine envelope is wrong\n");
        failures++;
    }

    render_blocked(samples, 4800, &state, AUDIO_RATE);
    for (size_t i = 0; i < 4800; i++) {
        if (samples[i]) {
            fprintf(stderr, "gap contains a nonzero PCM sample\n");
            failures++;
            break;
        }
    }
    render_blocked(samples, 4800, &state, AUDIO_RATE);
    if (state.mark_start != 9600 || state.mark_end != 14400 ||
        samples[0] != 0 || samples[4799] != 0) {
        fprintf(stderr, "second mark was not sample-aligned after gap\n");
        failures++;
    }

    /* A late event must not truncate its mark to catch up with the clock. */
    render_blocked(samples, 4800, &state, AUDIO_RATE);
    queue_mark(start_ns + 300000000LL, 100000000LL);
    render_blocked(samples, 4800, &state, AUDIO_RATE);
    if (state.mark_start != 19200 || state.mark_end != 24000 ||
        samples[0] != 0 || samples[4799] != 0) {
        fprintf(stderr, "late mark did not retain its full duration\n");
        failures++;
    }

    /* At the maximum supported speed, use 2 ms ramps within a 4 ms dit. */
    wpm = 300;
    audio_state_t fast = {0};
    fast.gap_frames = 192;
    queue_mark(start_ns + 1000000000LL, 4000000LL);
    render_blocked(samples, 192, &fast, AUDIO_RATE);
    if (fast.mark_end != 192 || fast.ramp_frames != 96 ||
        samples[0] != 0 || samples[191] != 0) {
        fprintf(stderr, "high-speed dit envelope exceeds the mark\n");
        failures++;
    }

    if (failures)
        fprintf(stderr, "%d test(s) failed\n", failures);
    return failures ? 1 : 0;
}
