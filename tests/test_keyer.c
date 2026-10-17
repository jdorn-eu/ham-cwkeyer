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

static int expect_symbol(keyer_t *keyer, int expected)
{
    int got = keyer_symbol(keyer);

    if (got == expected)
        return 0;

    fprintf(stderr, "iambic: expected %c, got %c\n",
            expected ? '.' : '-', got < 0 ? ' ' : (got ? '.' : '-'));
    return 1;
}

/* Advance the graph through one mark and its inter-element space. */
static void keyer_play(keyer_t *keyer)
{
    keyer_core_step(keyer);
    keyer_core_step(keyer);
}

/*
 * Curtis 8044 behavior, cross-checked against UnixCW's libcw state graph.
 * The decisive case is mode B's trailing element after a simultaneous
 * squeeze: a quick squeeze must key dot-dash-dot, where mode A keys only
 * dot-dash.
 */
static int run_iambic_tests(void)
{
    int failures = 0;
    keyer_t k;

    /* A lone dit and a lone dah stop after one element. */
    memset(&k, 0, sizeof(k));
    k.mode_b = 1;
    keyer_core_paddles(&k, 1, 0);
    failures += expect_symbol(&k, 1);
    keyer_core_paddles(&k, 0, 0);
    keyer_play(&k);
    if (k.state != KS_IDLE) {
        fprintf(stderr, "iambic: lone dit did not stop\n");
        failures++;
    }

    memset(&k, 0, sizeof(k));
    k.mode_b = 1;
    keyer_core_paddles(&k, 0, 1);
    failures += expect_symbol(&k, 0);
    keyer_core_paddles(&k, 0, 0);
    keyer_play(&k);
    if (k.state != KS_IDLE) {
        fprintf(stderr, "iambic: lone dah did not stop\n");
        failures++;
    }

    /* Mode B squeeze released during the first element: . - . */
    memset(&k, 0, sizeof(k));
    k.mode_b = 1;
    keyer_core_paddles(&k, 1, 1);
    failures += expect_symbol(&k, 1);
    keyer_core_paddles(&k, 0, 0);
    keyer_core_step(&k);
    keyer_core_step(&k);
    failures += expect_symbol(&k, 0);
    keyer_core_step(&k);
    keyer_core_step(&k);
    failures += expect_symbol(&k, 1);
    keyer_core_step(&k);
    keyer_core_step(&k);
    if (k.state != KS_IDLE) {
        fprintf(stderr, "iambic: mode B squeeze did not stop\n");
        failures++;
    }

    /* The same squeeze released during the dash also gives . - . */
    memset(&k, 0, sizeof(k));
    k.mode_b = 1;
    keyer_core_paddles(&k, 1, 1);
    failures += expect_symbol(&k, 1);
    keyer_core_step(&k);
    keyer_core_step(&k);
    failures += expect_symbol(&k, 0);
    keyer_core_paddles(&k, 0, 0);
    keyer_core_step(&k);
    keyer_core_step(&k);
    failures += expect_symbol(&k, 1);
    keyer_core_step(&k);
    keyer_core_step(&k);
    if (k.state != KS_IDLE) {
        fprintf(stderr, "iambic: late mode B squeeze did not stop\n");
        failures++;
    }

    /* Mode A squeeze stops after . - with no trailing element. */
    memset(&k, 0, sizeof(k));
    k.mode_b = 0;
    keyer_core_paddles(&k, 1, 1);
    failures += expect_symbol(&k, 1);
    keyer_core_paddles(&k, 0, 0);
    keyer_core_step(&k);
    keyer_core_step(&k);
    failures += expect_symbol(&k, 0);
    keyer_core_step(&k);
    keyer_core_step(&k);
    if (k.state != KS_IDLE) {
        fprintf(stderr, "iambic: mode A squeeze must stop after .-\n");
        failures++;
    }

    /* Holding a squeeze alternates and does not repeat the mode B element. */
    memset(&k, 0, sizeof(k));
    k.mode_b = 1;
    keyer_core_paddles(&k, 1, 1);
    for (int i = 0; i < 8; i++) {
        failures += expect_symbol(&k, i % 2 == 0 ? 1 : 0);
        keyer_play(&k);
    }

    /* Holding one paddle repeats that element. */
    memset(&k, 0, sizeof(k));
    k.mode_b = 1;
    keyer_core_paddles(&k, 1, 0);
    for (int i = 0; i < 4; i++) {
        failures += expect_symbol(&k, 1);
        keyer_play(&k);
    }

    memset(&k, 0, sizeof(k));
    k.mode_b = 1;
    keyer_core_paddles(&k, 0, 1);
    for (int i = 0; i < 4; i++) {
        failures += expect_symbol(&k, 0);
        keyer_play(&k);
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

    if (failures)
        fprintf(stderr, "%d test(s) failed\n", failures);
    return failures ? 1 : 0;
}
