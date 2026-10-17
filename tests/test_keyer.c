// SPDX-License-Identifier: GPL-3.0-or-later
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

typedef struct {
    char seq[32];
    size_t len;
} seq_t;

static void emit(seq_t *s, const keyer_t *keyer)
{
    int sym = keyer_symbol(keyer);

    if (sym >= 0 && s->len + 1 < sizeof(s->seq))
        s->seq[s->len++] = sym ? '.' : '-';
    s->seq[s->len] = '\0';
}

/* Clock one mark plus its inter-element space. */
static void advance(keyer_t *keyer)
{
    keyer_core_step(keyer);
    keyer_core_step(keyer);
}

static int expect_seq(const char *name, const seq_t *got, const char *want)
{
    if (strcmp(got->seq, want) == 0)
        return 0;

    fprintf(stderr, "iambic %s: expected %s, got %s\n", name, want, got->seq);
    return 1;
}

/*
 * Reference behaviour from the Curtis 8044 application note and DJ5IL's
 * "All About Squeeze-Keying":
 *
 *   N: tap dash then dot and release during the dash gives "-." in both
 *      modes (dash memory).
 *   K/C: squeeze and release during the second dash. Type A completes the
 *      dash -> "-." + dash = "-.-" (K). Type B also emits the pending
 *      opposite dot -> "-.-." (C).
 */
static int run_iambic_tests(void)
{
    int failures = 0;

    for (int mode = 0; mode <= 1; mode++) {
        keyer_t k;
        seq_t s;

        /* Lone dit and lone dah stop after one element. */
        memset(&k, 0, sizeof(k));
        k.mode_b = mode;
        s = (seq_t){0};
        keyer_core_paddles(&k, 1, 0);
        emit(&s, &k);
        keyer_core_paddles(&k, 0, 0);
        advance(&k);
        if (k.state != KY_IDLE) {
            fprintf(stderr, "iambic: lone dit did not stop\n");
            failures++;
        }
        failures += expect_seq("lone dit", &s, ".");

        memset(&k, 0, sizeof(k));
        k.mode_b = mode;
        s = (seq_t){0};
        keyer_core_paddles(&k, 0, 1);
        emit(&s, &k);
        keyer_core_paddles(&k, 0, 0);
        advance(&k);
        if (k.state != KY_IDLE) {
            fprintf(stderr, "iambic: lone dah did not stop\n");
            failures++;
        }
        failures += expect_seq("lone dah", &s, "-");

        /* N: dash, then dot, released during the dash. */
        memset(&k, 0, sizeof(k));
        k.mode_b = mode;
        s = (seq_t){0};
        keyer_core_paddles(&k, 0, 1);
        emit(&s, &k);
        keyer_core_paddles(&k, 1, 1);
        keyer_core_paddles(&k, 0, 0);
        advance(&k);
        emit(&s, &k);
        advance(&k);
        emit(&s, &k);
        failures += expect_seq("N", &s, "-.");

        /* A: dot, then dash, released during the dot. */
        memset(&k, 0, sizeof(k));
        k.mode_b = mode;
        s = (seq_t){0};
        keyer_core_paddles(&k, 1, 0);
        emit(&s, &k);
        keyer_core_paddles(&k, 1, 1);
        keyer_core_paddles(&k, 0, 0);
        advance(&k);
        emit(&s, &k);
        advance(&k);
        emit(&s, &k);
        failures += expect_seq("A", &s, ".-");

        /* K/C: squeeze, release during the second dash. */
        memset(&k, 0, sizeof(k));
        k.mode_b = mode;
        s = (seq_t){0};
        keyer_core_paddles(&k, 0, 1);
        emit(&s, &k);
        keyer_core_paddles(&k, 1, 1);
        advance(&k);
        emit(&s, &k);
        advance(&k);
        emit(&s, &k);
        keyer_core_paddles(&k, 0, 0);
        advance(&k);
        emit(&s, &k);
        advance(&k);
        emit(&s, &k);
        failures += expect_seq("K/C", &s, mode ? "-.-." : "-.-");

        /* Held levers repeat; a held squeeze alternates. */
        memset(&k, 0, sizeof(k));
        k.mode_b = mode;
        s = (seq_t){0};
        keyer_core_paddles(&k, 1, 0);
        for (int i = 0; i < 4; i++) {
            emit(&s, &k);
            advance(&k);
        }
        failures += expect_seq("hold dit", &s, "....");

        memset(&k, 0, sizeof(k));
        k.mode_b = mode;
        s = (seq_t){0};
        keyer_core_paddles(&k, 0, 1);
        for (int i = 0; i < 4; i++) {
            emit(&s, &k);
            advance(&k);
        }
        failures += expect_seq("hold dah", &s, "----");

        memset(&k, 0, sizeof(k));
        k.mode_b = mode;
        s = (seq_t){0};
        keyer_core_paddles(&k, 1, 1);
        for (int i = 0; i < 6; i++) {
            emit(&s, &k);
            advance(&k);
        }
        failures += expect_seq("squeeze alternates", &s, ".-.-.-");
    }

    return failures;
}

/*
 * Debouncing: a level change is accepted only after it has been stable for
 * the hold time, so contact bounce and short glitches never reach the core.
 */
static int run_debounce_tests(void)
{
    int failures = 0;
    const int64_t hold = 5000000LL; /* 5 ms */
    debounce_t d;
    int transitions;
    int prev;
    int64_t t;

    /* A glitch shorter than the hold time is rejected. */
    d = (debounce_t){0};
    debounce_update(&d, 1, 0, hold);
    debounce_update(&d, 0, 1000000, hold);
    if (d.stable != 0) {
        fprintf(stderr, "debounce: short glitch was accepted\n");
        failures++;
    }

    /* A held press is accepted only once the hold time has elapsed. */
    d = (debounce_t){0};
    debounce_update(&d, 1, 0, hold);
    if (debounce_update(&d, 1, hold - 1, hold) != 0) {
        fprintf(stderr, "debounce: press accepted too early\n");
        failures++;
    }
    if (debounce_update(&d, 1, hold, hold) != 1) {
        fprintf(stderr, "debounce: press not accepted after hold\n");
        failures++;
    }

    /* A burst of bounce on close yields exactly one transition. */
    d = (debounce_t){0};
    transitions = 0;
    prev = 0;
    t = 0;
    for (int i = 0; i < 12; i++, t += 250000) {
        int st = debounce_update(&d, (i % 2 == 0) ? 1 : 0, t, hold);
        if (st != prev) { transitions++; prev = st; }
    }
    for (int i = 0; i < 40; i++, t += 250000) {
        int st = debounce_update(&d, 1, t, hold);
        if (st != prev) { transitions++; prev = st; }
    }
    if (transitions != 1 || prev != 1) {
        fprintf(stderr, "debounce: close bounce gave %d transitions\n", transitions);
        failures++;
    }

    /* A burst of bounce on open also yields exactly one transition. */
    d = (debounce_t){.stable = 1, .candidate = 1, .candidate_ns = 0};
    transitions = 0;
    prev = 1;
    t = 0;
    for (int i = 0; i < 12; i++, t += 250000) {
        int st = debounce_update(&d, (i % 2 == 0) ? 0 : 1, t, hold);
        if (st != prev) { transitions++; prev = st; }
    }
    for (int i = 0; i < 40; i++, t += 250000) {
        int st = debounce_update(&d, 0, t, hold);
        if (st != prev) { transitions++; prev = st; }
    }
    if (transitions != 1 || prev != 0) {
        fprintf(stderr, "debounce: open bounce gave %d transitions\n", transitions);
        failures++;
    }

    /* Hold 0 disables debouncing. */
    d = (debounce_t){0};
    if (debounce_update(&d, 1, 0, 0) != 1) {
        fprintf(stderr, "debounce: hold 0 did not pass the level through\n");
        failures++;
    }

    /*
     * Through the sample path: a glitch must not start an element, and a
     * held press must start exactly one.
     */
    debounce_ns = hold;

    keyer_t k = {0};
    k.mode_b = 1;
    keyer_sample(&k, 1, 0, 0);
    keyer_sample(&k, 0, 0, 1000000);
    keyer_sample(&k, 0, 0, hold * 2);
    if (keyer_symbol(&k) != -1 || k.state != KY_IDLE) {
        fprintf(stderr, "debounce: glitch started an element\n");
        failures++;
    }

    k = (keyer_t){0};
    k.mode_b = 1;
    keyer_sample(&k, 1, 0, 0);
    keyer_sample(&k, 1, 0, hold);
    if (keyer_symbol(&k) != 1) {
        fprintf(stderr, "debounce: held press did not start a dit\n");
        failures++;
    }
    keyer_sample(&k, 1, 0, hold + 1000000);
    if (keyer_symbol(&k) != 1 || k.state != KY_IN_DOT) {
        fprintf(stderr, "debounce: held press started a second element\n");
        failures++;
    }

    return failures;
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

    failures += run_iambic_tests();
    failures += run_debounce_tests();

    if (failures)
        fprintf(stderr, "%d test(s) failed\n", failures);
    return failures ? 1 : 0;
}
