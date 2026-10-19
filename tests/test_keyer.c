// SPDX-License-Identifier: GPL-3.0-or-later
/* Hardware-independent regression tests for decoding and PCM mark timing. */
int cwkeyer_program_main(int argc, char **argv);
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

#include <sys/wait.h>

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
 * "All About Squeeze-Keying", for the type B logic this project implements:
 *
 *   N: tap dash then dot and release during the dash gives "-." (dash
 *      memory).
 *   A: tap dot then dash and release during the dot gives ".-" (dot
 *      memory).
 *   C: squeeze and release during the second dash leaves the opposite
 *      latch set and emits the extra dot -> "-.-.".
 */
static int run_iambic_tests(void)
{
    int failures = 0;
    keyer_t k;
    seq_t s;

    /* Lone dit and lone dah stop after one element. */
    memset(&k, 0, sizeof(k));
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

    /* C: squeeze, release during the second dash. */
    memset(&k, 0, sizeof(k));
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
    failures += expect_seq("C", &s, "-.-.");

    /* Held levers repeat; a held squeeze alternates. */
    memset(&k, 0, sizeof(k));
    s = (seq_t){0};
    keyer_core_paddles(&k, 1, 0);
    for (int i = 0; i < 4; i++) {
        emit(&s, &k);
        advance(&k);
    }
    failures += expect_seq("hold dit", &s, "....");

    memset(&k, 0, sizeof(k));
    s = (seq_t){0};
    keyer_core_paddles(&k, 0, 1);
    for (int i = 0; i < 4; i++) {
        emit(&s, &k);
        advance(&k);
    }
    failures += expect_seq("hold dah", &s, "----");

    memset(&k, 0, sizeof(k));
    s = (seq_t){0};
    keyer_core_paddles(&k, 1, 1);
    for (int i = 0; i < 6; i++) {
        emit(&s, &k);
        advance(&k);
    }
    failures += expect_seq("squeeze alternates", &s, ".-.-.-");

    /*
     * First-lever-first: when both levers arrive in the same sample, the
     * starting element follows the lever that was closed first.  The dot is
     * only the fallback when the order is unknown.
     */
    memset(&k, 0, sizeof(k));
    s = (seq_t){0};
    k.first_lever = KY_FIRST_DOT;
    keyer_core_paddles(&k, 1, 1);
    emit(&s, &k);
    failures += expect_seq("first lever dit", &s, ".");

    memset(&k, 0, sizeof(k));
    s = (seq_t){0};
    k.first_lever = KY_FIRST_DASH;
    keyer_core_paddles(&k, 1, 1);
    emit(&s, &k);
    failures += expect_seq("first lever dah", &s, "-");

    memset(&k, 0, sizeof(k));
    s = (seq_t){0};
    k.first_lever = KY_FIRST_UNKNOWN;
    keyer_core_paddles(&k, 1, 1);
    emit(&s, &k);
    failures += expect_seq("first lever unknown", &s, ".");

    return failures;
}

/*
 * Realistic operator usage: key every entry in the decode table with a
 * plain single-lever ("slap") gesture and check both the emitted elements
 * and the decoded character.  The iambic squeeze semantics are
 * character-independent and covered by the N/A/C reference cases, so they
 * are not repeated per character here.
 */
static void key_slap(const char *code, seq_t *s)
{
    keyer_t k;

    memset(&k, 0, sizeof(k));
    s->len = 0;
    s->seq[0] = '\0';

    for (const char *p = code; *p; p++) {
        int dot = (*p == '.');

        keyer_core_paddles(&k, dot, !dot);   /* close the element's lever */
        emit(s, &k);
        keyer_core_step(&k);                 /* end of mark */
        keyer_core_paddles(&k, 0, 0);        /* release during the space */
        keyer_core_step(&k);                 /* end of space */
    }
}

static int run_alphabet_tests(void)
{
    int failures = 0;
    int entries = 0;

    for (int i = 0; morse_table[i].code; i++) {
        char ch = morse_table[i].ch;
        const char *code = morse_table[i].code;
        seq_t s = {0};

        entries++;

        /* The decode table round-trips. */
        if (morse_decode(code, strlen(code)) != ch) {
            fprintf(stderr, "alphabet: '%c' (%s) did not decode back\n",
                    ch, code);
            failures++;
        }

        /* A single-lever gesture keys exactly that character. */
        key_slap(code, &s);
        if (strcmp(s.seq, code) != 0) {
            fprintf(stderr, "alphabet: '%c' keyed as %s, want %s\n",
                    ch, s.seq, code);
            failures++;
        }

        /* And what was keyed decodes back to it. */
        if (morse_decode(s.seq, s.len) != ch) {
            fprintf(stderr, "alphabet: '%c' did not survive key+decode\n", ch);
            failures++;
        }
    }

    /* 26 letters + 10 digits + 12 punctuation marks. */
    if (entries != 48) {
        fprintf(stderr, "alphabet: decode table has %d entries, expected 48\n",
                entries);
        failures++;
    }

    return failures;
}

/*
 * RC filtering: the lever level is smoothed with the same first-order filter
 * as the 8044's external network, so short glitches and contact bounce never
 * reach the core as a full logic transition.
 */
static int run_debounce_tests(void)
{
    int failures = 0;
    const int64_t tau = 5000000LL; /* 5 ms */
    debounce_t d;
    int transitions;
    int prev;
    int64_t t;

    /* A glitch shorter than the filter time is rejected. */
    d = (debounce_t){0};
    debounce_update(&d, 1, 0, tau);
    debounce_update(&d, 0, 500000, tau);
    debounce_update(&d, 0, 2000000, tau);
    if (d.state != 0) {
        fprintf(stderr, "debounce: short glitch was accepted\n");
        failures++;
    }

    /* A held press must charge the filter past its threshold. */
    d = (debounce_t){0};
    if (debounce_update(&d, 1, 0, tau) != 0) {
        fprintf(stderr, "debounce: press accepted at the first sample\n");
        failures++;
    }
    if (debounce_update(&d, 1, 3000000, tau) != 0) {
        fprintf(stderr, "debounce: press accepted too early\n");
        failures++;
    }
    if (debounce_update(&d, 1, 5000000, tau) != 1) {
        fprintf(stderr, "debounce: press not accepted after the RC delay\n");
        failures++;
    }
    if (debounce_update(&d, 0, 6000000, tau) != 1) {
        fprintf(stderr, "debounce: release accepted too early\n");
        failures++;
    }
    if (debounce_update(&d, 0, 8000000, tau) != 0) {
        fprintf(stderr, "debounce: release not accepted after the RC delay\n");
        failures++;
    }

    /* A burst of bounce on close yields exactly one transition. */
    d = (debounce_t){0};
    transitions = 0;
    prev = 0;
    t = 0;
    for (int i = 0; i < 10; i++, t += 500000) {
        int st = debounce_update(&d, (i % 2 == 0) ? 1 : 0, t, tau);
        if (st != prev) { transitions++; prev = st; }
    }
    for (int i = 0; i < 20; i++, t += 500000) {
        int st = debounce_update(&d, 1, t, tau);
        if (st != prev) { transitions++; prev = st; }
    }
    if (transitions != 1 || prev != 1) {
        fprintf(stderr, "debounce: close bounce gave %d transitions\n", transitions);
        failures++;
    }

    /* A burst of bounce on open also yields exactly one transition. */
    d = (debounce_t){0};
    debounce_update(&d, 1, 0, tau);
    debounce_update(&d, 1, tau * 3, tau);
    transitions = 0;
    prev = d.state;
    t = tau * 3;
    for (int i = 0; i < 10; i++, t += 500000) {
        int st = debounce_update(&d, (i % 2 == 0) ? 0 : 1, t, tau);
        if (st != prev) { transitions++; prev = st; }
    }
    for (int i = 0; i < 20; i++, t += 500000) {
        int st = debounce_update(&d, 0, t, tau);
        if (st != prev) { transitions++; prev = st; }
    }
    if (transitions != 1 || prev != 0) {
        fprintf(stderr, "debounce: open bounce gave %d transitions\n", transitions);
        failures++;
    }

    /* Time constant 0 disables filtering. */
    d = (debounce_t){0};
    if (debounce_update(&d, 1, 0, 0) != 1) {
        fprintf(stderr, "debounce: disabled filter did not pass a press\n");
        failures++;
    }
    if (debounce_update(&d, 0, 1, 0) != 0) {
        fprintf(stderr, "debounce: disabled filter did not pass a release\n");
        failures++;
    }

    /*
     * Through the sample path: a glitch must not start an element, and a
     * held press must start exactly one.
     */
    debounce_ns = tau;

    keyer_t k = {0};
    keyer_sample(&k, 1, 0, 0);
    keyer_sample(&k, 0, 0, 1000000);
    keyer_sample(&k, 0, 0, 2000000);
    if (keyer_symbol(&k) != -1 || k.state != KY_IDLE) {
        fprintf(stderr, "debounce: glitch started an element\n");
        failures++;
    }

    k = (keyer_t){0};
    keyer_sample(&k, 1, 0, 0);
    keyer_sample(&k, 1, 0, 3000000);
    if (keyer_symbol(&k) != -1) {
        fprintf(stderr, "debounce: held press started too early\n");
        failures++;
    }
    keyer_sample(&k, 1, 0, 6000000);
    if (keyer_symbol(&k) != 1) {
        fprintf(stderr, "debounce: held press did not start a dit\n");
        failures++;
    }
    keyer_sample(&k, 1, 0, 7000000);
    if (keyer_symbol(&k) != 1 || k.state != KY_IN_DOT) {
        fprintf(stderr, "debounce: held press started a second element\n");
        failures++;
    }

    return failures;
}

/* ---------------- evdev event parsing ---------------- */

static struct input_event make_event(unsigned short type, unsigned short code,
                                     int value)
{
    struct input_event e;

    memset(&e, 0, sizeof(e));
    e.type = type;
    e.code = code;
    e.value = value;
    return e;
}

static int run_evdev_tests(void)
{
    int failures = 0;
    int resync;
    struct input_event e;

    dit_code = KEY_LEFTCTRL;
    dah_code = KEY_RIGHTCTRL;

    /* A press sets the paddle and its edge latch. */
    clear_input_state();
    resync = 0;
    e = make_event(EV_KEY, KEY_LEFTCTRL, 1);
    evdev_handle_event(&e, &resync);
    if (ALOAD(dit_paddle) != 1 || ALOAD(dit_edge) != 1 || resync) {
        fprintf(stderr, "evdev: dit press not registered\n");
        failures++;
    }

    /* A release clears the paddle. */
    e = make_event(EV_KEY, KEY_LEFTCTRL, 0);
    evdev_handle_event(&e, &resync);
    if (ALOAD(dit_paddle) != 0) {
        fprintf(stderr, "evdev: dit release not registered\n");
        failures++;
    }

    /* Autorepeat (value 2) is ignored. */
    clear_input_state();
    e = make_event(EV_KEY, KEY_RIGHTCTRL, 2);
    evdev_handle_event(&e, &resync);
    if (ALOAD(dah_paddle) || ALOAD(dah_edge)) {
        fprintf(stderr, "evdev: autorepeat treated as a press\n");
        failures++;
    }

    /* Unrelated keys are ignored. */
    e = make_event(EV_KEY, KEY_A, 1);
    evdev_handle_event(&e, &resync);
    if (ALOAD(dit_paddle) || ALOAD(dah_paddle)) {
        fprintf(stderr, "evdev: unrelated key changed a paddle\n");
        failures++;
    }

    /* SYN_DROPPED clears state and suppresses events until SYN_REPORT. */
    clear_input_state();
    ASTORE(dit_paddle, 1);
    ASTORE(dah_edge, 1);
    e = make_event(EV_SYN, SYN_DROPPED, 0);
    evdev_handle_event(&e, &resync);
    if (resync != 1 || ALOAD(dit_paddle) || ALOAD(dah_edge)) {
        fprintf(stderr, "evdev: SYN_DROPPED did not clear and resync\n");
        failures++;
    }
    e = make_event(EV_KEY, KEY_LEFTCTRL, 1);
    if (evdev_handle_event(&e, &resync) || ALOAD(dit_paddle)) {
        fprintf(stderr, "evdev: event applied during resync\n");
        failures++;
    }
    e = make_event(EV_SYN, SYN_REPORT, 0);
    if (!evdev_handle_event(&e, &resync) || resync) {
        fprintf(stderr, "evdev: SYN_REPORT did not end resync\n");
        failures++;
    }

    clear_input_state();
    return failures;
}

/* ---------------- press ordering ---------------- */

/*
 * The evdev thread stamps each press with a monotonic counter, and the
 * keyer uses the stamps to start a same-sample squeeze with the lever that
 * was closed first.
 */
static int run_press_order_tests(void)
{
    int failures = 0;
    int64_t saved_debounce = debounce_ns;
    keyer_t k;

    dit_code = KEY_LEFTCTRL;
    dah_code = KEY_RIGHTCTRL;
    debounce_ns = 0;   /* raw levels: what we set is what the core samples */

    /* Dit pressed first: the lower stamp, and it keys first. */
    clear_input_state();
    set_key_state(dit_code, 1);
    set_key_state(dah_code, 1);
    if (ALOAD(dit_press_seq) >= ALOAD(dah_press_seq)) {
        fprintf(stderr, "press order: first dit did not get the lower stamp\n");
        failures++;
    }
    k = (keyer_t){0};
    sample_keyer_paddles(&k);
    if (k.first_lever != KY_FIRST_DOT || k.state != KY_IN_DOT) {
        fprintf(stderr, "press order: dit-first squeeze did not start on a dit\n");
        failures++;
    }

    /* Dah pressed first: the reverse. */
    clear_input_state();
    set_key_state(dah_code, 1);
    set_key_state(dit_code, 1);
    k = (keyer_t){0};
    sample_keyer_paddles(&k);
    if (k.first_lever != KY_FIRST_DASH || k.state != KY_IN_DASH) {
        fprintf(stderr, "press order: dah-first squeeze did not start on a dah\n");
        failures++;
    }

    /* A lever held before the other keeps the earlier stamp. */
    clear_input_state();
    set_key_state(dah_code, 1);
    k = (keyer_t){0};
    sample_keyer_paddles(&k);          /* dah starts on its own */
    set_key_state(dit_code, 1);
    sample_keyer_paddles(&k);          /* dit joins the squeeze */
    if (k.first_lever != KY_FIRST_DASH) {
        fprintf(stderr, "press order: held dah was not kept as first\n");
        failures++;
    }

    /* clear_input_state resets the stamps. */
    clear_input_state();
    if (ALOAD(dit_press_seq) != 0 || ALOAD(dah_press_seq) != 0) {
        fprintf(stderr, "press order: stamps survived clear_input_state\n");
        failures++;
    }

    debounce_ns = saved_debounce;
    return failures;
}

/* ---------------- decode display ---------------- */

static int run_decode_tests(void)
{
    int failures = 0;
    const int64_t dit = 100000000LL;
    const int64_t char_gap = dit * 3;
    const int64_t word_gap = dit * 7;
    char *buf = NULL;
    size_t buflen = 0;

    FILE *out = open_memstream(&buf, &buflen);
    if (!out) {
        fprintf(stderr, "decode: open_memstream failed\n");
        return 1;
    }

    decode_state_t d = {0};
    const int64_t base = 1000000LL;
    d.gap_start = base;         /* as the driver sets it at the mark end */
    decode_add_symbol(&d, 1);   /* . */
    decode_add_symbol(&d, 0);   /* - */
    decode_add_symbol(&d, 1);   /* . */

    decode_idle(&d, 1, base + char_gap - 1, char_gap, word_gap, out);
    if (buflen != 0) {
        fprintf(stderr, "decode: printed before the character gap\n");
        failures++;
    }

    decode_idle(&d, 1, base + char_gap, char_gap, word_gap, out);
    if (strcmp(buf, "R") != 0) {
        fprintf(stderr, "decode: want R, got '%s'\n", buf);
        failures++;
    }

    decode_idle(&d, 1, base + word_gap + 1, char_gap, word_gap, out);
    if (strcmp(buf, "R ") != 0) {
        fprintf(stderr, "decode: want 'R ', got '%s'\n", buf);
        failures++;
    }

    decode_idle(&d, 1, base + word_gap + 1 + dit, char_gap, word_gap, out);
    if (strcmp(buf, "R ") != 0) {
        fprintf(stderr, "decode: word gap printed more than one space\n");
        failures++;
    }

    fclose(out);
    free(buf);

    /* Unknown symbols are dropped silently. */
    buf = NULL;
    buflen = 0;
    out = open_memstream(&buf, &buflen);
    if (!out) {
        fprintf(stderr, "decode: open_memstream failed\n");
        return 1;
    }
    decode_state_t u = {0};
    u.gap_start = base;
    for (int i = 0; i < 6; i++)
        decode_add_symbol(&u, 0);   /* "------" is not a character */
    decode_idle(&u, 1, base + char_gap, char_gap, word_gap, out);
    if (u.sym_len != 0 || strcmp(buf, "") != 0) {
        fprintf(stderr, "decode: unknown symbol not dropped (got '%s')\n", buf);
        failures++;
    }
    fclose(out);
    free(buf);

    return failures;
}

/* ---------------- driver (keyer_thread) ---------------- */

static int collect_marks(int64_t *durs, int max, int wait_ms)
{
    int n = 0;
    struct timespec ts = {0, 1000000}; /* 1 ms */

    for (int waited = 0; waited < wait_ms && n < max; waited++) {
        mark_t m;
        while (take_mark(&m) && n < max)
            durs[n++] = m.duration_ns;
        nanosleep(&ts, NULL);
    }

    return n;
}

static int run_driver_tests(void)
{
    int failures = 0;
    const int64_t dit_ns = 20000000LL;   /* 60 wpm */
    const int64_t dah_ns = 60000000LL;
    int64_t durs[32];
    pthread_t tid;
    int n;

    wpm = 60.0;
    debounce_ns = 0;      /* test the sequencing, not the debounce */
    do_decode = 0;
    stop_requested = 0;

    /* Count any positive overrun so the timing diagnostic is exercised. */
    keyer_late_threshold_ns = 0;
    keyer_late_elements = 0;

    /* Holding dit repeats dits. */
    atomic_store_explicit(&mark_read, 0, memory_order_relaxed);
    atomic_store_explicit(&mark_write, 0, memory_order_relaxed);
    clear_input_state();
    ASTORE(dit_paddle, 1);
    ASTORE(dah_paddle, 0);
    ASTORE(running, 1);

    if (pthread_create(&tid, NULL, keyer_thread, NULL) != 0) {
        fprintf(stderr, "driver: pthread_create failed\n");
        return 1;
    }
    n = collect_marks(durs, 32, 250);
    ASTORE(running, 0);
    pthread_join(tid, NULL);

    if (n < 3) {
        fprintf(stderr, "driver: held dit produced only %d marks\n", n);
        failures++;
    }
    for (int i = 0; i < n; i++) {
        if (durs[i] != dit_ns) {
            fprintf(stderr, "driver: held dit mark %d was %lld ns\n",
                    i, (long long)durs[i]);
            failures++;
            break;
        }
    }

    /* Holding both alternates dit and dah. */
    atomic_store_explicit(&mark_read, 0, memory_order_relaxed);
    atomic_store_explicit(&mark_write, 0, memory_order_relaxed);
    clear_input_state();
    ASTORE(dit_paddle, 1);
    ASTORE(dah_paddle, 1);
    ASTORE(running, 1);

    if (pthread_create(&tid, NULL, keyer_thread, NULL) != 0) {
        fprintf(stderr, "driver: pthread_create failed\n");
        return 1;
    }
    n = collect_marks(durs, 32, 400);
    ASTORE(running, 0);
    pthread_join(tid, NULL);

    if (n < 4) {
        fprintf(stderr, "driver: squeeze produced only %d marks\n", n);
        failures++;
    }
    for (int i = 0; i + 1 < n; i++) {
        int64_t want = (i % 2 == 0) ? dit_ns : dah_ns;
        if (durs[i] != want) {
            fprintf(stderr, "driver: squeeze mark %d was %lld ns\n",
                    i, (long long)durs[i]);
            failures++;
            break;
        }
    }

    if (keyer_late_elements == 0) {
        fprintf(stderr, "driver: no keyer timing overruns observed\n");
        failures++;
    }
    keyer_late_threshold_ns = 1000000LL;

    clear_input_state();
    return failures;
}

/* ---------------- argument parsing and small helpers ---------------- */

static const char *g_bad_arg;

static void call_parse_wpm(void)
{
    parse_double_arg(g_bad_arg, "wpm", 1.0, MAX_WPM);
}

static void call_lookup(void)
{
    lookup_key(g_bad_arg);
}

/* Fork and check that fn() exits with the expected status. */
static int child_exits_with(int want, void (*fn)(void))
{
    pid_t pid = fork();

    if (pid < 0)
        return 0;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        fn();
        _exit(0);
    }

    int status;
    if (waitpid(pid, &status, 0) != pid)
        return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == want;
}

static int quiet_stderr(void)
{
    int saved = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    return saved;
}

static void unquiet_stderr(int saved)
{
    if (saved >= 0) {
        dup2(saved, STDERR_FILENO);
        close(saved);
    }
}

static int nearly(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

static int run_arg_tests(void)
{
    int failures = 0;
    int saved;
    unsigned long bits[NBITS(KEY_MAX + 1)];

    /* Valid doubles. */
    if (!nearly(parse_double_arg("25", "wpm", 1.0, MAX_WPM), 25.0))
        failures++;
    if (!nearly(parse_double_arg("12.5", "wpm", 1.0, MAX_WPM), 12.5))
        failures++;
    if (!nearly(parse_double_arg("1", "wpm", 1.0, MAX_WPM), 1.0))
        failures++;

    if (!nearly(parse_double_arg("40", "wpm", 1.0, MAX_WPM), MAX_WPM))
        failures++;

    /* Invalid doubles call exit(1). */
    g_bad_arg = "0";
    failures += !child_exits_with(1, call_parse_wpm);
    g_bad_arg = "40.1";
    failures += !child_exits_with(1, call_parse_wpm);
    g_bad_arg = "abc";
    failures += !child_exits_with(1, call_parse_wpm);
    g_bad_arg = "inf";
    failures += !child_exits_with(1, call_parse_wpm);
    g_bad_arg = "";
    failures += !child_exits_with(1, call_parse_wpm);

    /* parse_uint_env: valid, out of range, invalid, and unset. */
    setenv("CW_TEST_UINT", "25", 1);
    if (parse_uint_env("CW_TEST_UINT", 5, 0, 50) != 25)
        failures++;
    setenv("CW_TEST_UINT", "999", 1);
    saved = quiet_stderr();
    if (parse_uint_env("CW_TEST_UINT", 5, 0, 50) != 5)
        failures++;
    unquiet_stderr(saved);
    setenv("CW_TEST_UINT", "x", 1);
    saved = quiet_stderr();
    if (parse_uint_env("CW_TEST_UINT", 7, 0, 50) != 7)
        failures++;
    unquiet_stderr(saved);
    unsetenv("CW_TEST_UINT");
    if (parse_uint_env("CW_TEST_UINT", 3, 0, 50) != 3)
        failures++;

    /* lookup_key: names, case, numeric, and unknown. */
    if (lookup_key("COMMA") != KEY_COMMA)
        failures++;
    if (lookup_key("KEY_LEFTCTRL") != KEY_LEFTCTRL)
        failures++;
    if (lookup_key("leftctrl") != KEY_LEFTCTRL)
        failures++;
    if (lookup_key("42") != 42)
        failures++;
    if (lookup_key("0x2a") != 42)
        failures++;
    g_bad_arg = "NOT_A_KEY";
    failures += !child_exits_with(1, call_lookup);

    /* bit_is_set. */
    memset(bits, 0, sizeof(bits));
    bits[KEY_LEFTCTRL / BITS_PER_ULONG] |= 1UL << (KEY_LEFTCTRL % BITS_PER_ULONG);
    if (!bit_is_set(KEY_LEFTCTRL, bits))
        failures++;
    if (bit_is_set(KEY_RIGHTCTRL, bits))
        failures++;

    /* ns_to_frames rounding. */
    if (ns_to_frames(1000000000LL, 48000) != 48000)
        failures++;
    if (ns_to_frames(0, 48000) != 0)
        failures++;

    return failures;
}

/* ---------------- runtime controls ---------------- */

static int run_control_tests(void)
{
    int failures = 0;
    int saved = quiet_stderr();
    double saved_wpm = ALOAD(wpm);
    double saved_freq = ALOAD(freq_hz);
    int saved_volume = ALOAD(volume_percent);
    double saved_weight = ALOAD(weight_ms);
    const double freq_max = (double)audio_rate / 2.0 - 1.0;

    wpm = 12.0;
    freq_hz = 800.0;
    volume_percent = 70;
    weight_ms = 0.0;

    control_key(']');
    if (!nearly(ALOAD(wpm), 13.0))
        failures++;
    control_key('[');
    if (!nearly(ALOAD(wpm), 12.0))
        failures++;

    wpm = MAX_WPM;
    control_key(']');
    if (!nearly(ALOAD(wpm), MAX_WPM))
        failures++;
    wpm = 1.0;
    control_key('[');
    if (!nearly(ALOAD(wpm), 1.0))
        failures++;

    freq_hz = 800.0;
    control_key('=');
    if (!nearly(ALOAD(freq_hz), 810.0))
        failures++;
    control_key('-');
    if (!nearly(ALOAD(freq_hz), 800.0))
        failures++;
    freq_hz = 1.0;
    control_key('-');
    if (!nearly(ALOAD(freq_hz), 1.0))
        failures++;
    freq_hz = freq_max;
    control_key('=');
    if (!nearly(ALOAD(freq_hz), freq_max))
        failures++;

    volume_percent = 70;
    control_key('0');
    if (ALOAD(volume_percent) != 75)
        failures++;
    control_key('9');
    if (ALOAD(volume_percent) != 70)
        failures++;
    volume_percent = 100;
    control_key('0');
    if (ALOAD(volume_percent) != 100)
        failures++;
    volume_percent = 0;
    control_key('9');
    if (ALOAD(volume_percent) != 0)
        failures++;

    weight_ms = 0.0;
    control_key('.');
    if (!nearly(ALOAD(weight_ms), 1.0))
        failures++;
    control_key(',');
    if (!nearly(ALOAD(weight_ms), 0.0))
        failures++;
    weight_ms = 25.0;
    control_key('.');
    if (!nearly(ALOAD(weight_ms), 25.0))
        failures++;
    weight_ms = -25.0;
    control_key(',');
    if (!nearly(ALOAD(weight_ms), -25.0))
        failures++;

    control_key('x');
    if (!nearly(ALOAD(wpm), 1.0) || !nearly(ALOAD(freq_hz), freq_max) ||
        ALOAD(volume_percent) != 0)
        failures++;

    wpm = saved_wpm;
    freq_hz = saved_freq;
    volume_percent = saved_volume;
    weight_ms = saved_weight;
    unquiet_stderr(saved);

    return failures;
}

/* A pipe with queued data and a closed writer reports POLLIN | POLLHUP.
 * The controls must be consumed before the control thread exits. */
static int run_control_pipe_test(void)
{
    int fds[2];
    int saved_stdin;
    pthread_t tid;
    int failures = 0;
    int saved_err = quiet_stderr();

    if (pipe(fds) < 0) {
        unquiet_stderr(saved_err);
        return 1;
    }
    saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin < 0 || write(fds[1], "]]", 2) != 2 ||
        dup2(fds[0], STDIN_FILENO) < 0) {
        if (saved_stdin >= 0)
            close(saved_stdin);
        close(fds[0]);
        close(fds[1]);
        unquiet_stderr(saved_err);
        return 1;
    }
    close(fds[0]);
    close(fds[1]);

    wpm = 12.0;
    ASTORE(running, 1);
    stop_requested = 0;
    if (pthread_create(&tid, NULL, control_thread, NULL) != 0)
        failures++;
    else
        pthread_join(tid, NULL);
    if (!nearly(ALOAD(wpm), 14.0))
        failures++;

    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    wpm = 12.0;
    unquiet_stderr(saved_err);
    if (failures)
        fprintf(stderr, "control: closed pipe lost pending keys\n");
    return failures;
}

/* ---------------- main() startup paths ---------------- */

static int run_main_tests(void)
{
    int failures = 0;
    char p0[] = "cwkeyer";
    char p1[] = "--help";
    char p2[] = "--version";
    char p3[] = "cwkeyer_no_such_device";
    char p4[] = "25";
    char p5[] = "700";
    char p6[] = "COMMA";
    char p7[] = "DOT";
    char p8[] = "nograb";
    char p9[] = "decode";

    char *help_argv[] = {p0, p1, NULL};
    char *ver_argv[] = {p0, p2, NULL};
    char *noargs_argv[] = {p0, NULL};
    char *run_argv[] = {p0, p3, p4, p5, p6, p7, p8, p9, NULL};

    int saved_out = dup(STDOUT_FILENO);
    int saved_err = quiet_stderr();
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0)
        dup2(devnull, STDOUT_FILENO);

    if (cwkeyer_program_main(2, help_argv) != 0)
        failures++;
    if (cwkeyer_program_main(2, ver_argv) != 0)
        failures++;
    if (cwkeyer_program_main(1, noargs_argv) != 1)
        failures++;

    /*
     * Full startup with a non-existent input device. The evdev thread fails
     * to open it and asks the program to stop, so main exercises argument
     * parsing, audio setup (ALSA "null"), thread startup and joins, and
     * returns the fatal status.
     */
    setenv("CW_ALSA_DEVICE", "null", 1);
    ASTORE(running, 1);
    stop_requested = 0;
    ASTORE(fatal_error, 0);

    int saved_in = dup(STDIN_FILENO);
    int devnull_in = open("/dev/null", O_RDONLY);
    if (devnull_in >= 0)
        dup2(devnull_in, STDIN_FILENO);

    int rc = cwkeyer_program_main(8, run_argv);

    if (saved_in >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }
    if (devnull_in >= 0)
        close(devnull_in);
    if (rc != 1)
        failures++;

    /* Flush while still pointed at /dev/null: stdout is block-buffered when
     * piped, so a late flush would otherwise leak to the real stdout. */
    fflush(stdout);
    fflush(stderr);

    if (devnull >= 0) {
        dup2(saved_out, STDOUT_FILENO);
        close(devnull);
    }
    unquiet_stderr(saved_err);
    if (saved_out >= 0)
        close(saved_out);

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

    /* Renderer stress test above the supported operator-speed range. */
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

    /* Level control scales the PCM amplitude. */
    wpm = 12;
    freq_hz = 800;
    volume_percent = 50;
    audio_state_t half = {0};
    int peak = 0;
    half.gap_frames = 4800;
    queue_mark(start_ns + 2000000000LL, 100000000LL);
    render_blocked(samples, 4800, &half, AUDIO_RATE);
    for (size_t i = 0; i < 4800; i++) {
        int v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > peak)
            peak = v;
    }
    if (peak < 16000 || peak > 16500) {
        fprintf(stderr, "level control did not scale the PCM peak (%d)\n",
                peak);
        failures++;
    }

    /* Full level must stay inside the 16-bit range. */
    volume_percent = 100;
    audio_state_t full = {0};
    int peak_full = 0;
    full.gap_frames = 4800;
    queue_mark(start_ns + 3000000000LL, 100000000LL);
    render_blocked(samples, 4800, &full, AUDIO_RATE);
    for (size_t i = 0; i < 4800; i++) {
        int v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > peak_full)
            peak_full = v;
    }
    if (peak_full < 32000 || peak_full > 32767) {
        fprintf(stderr, "full level left the 16-bit range (%d)\n",
                peak_full);
        failures++;
    }
    volume_percent = 70;

    /*
     * Straight-key passthrough: the tone follows the paddle gate.  While the
     * gate is up the output is silent; while it is down the tone ramps up to
     * the full level and holds; releasing ramps it back down.
     */
    straight_mode = 1;
    ASTORE(straight_down, 0);
    audio_state_t straight = {0};
    render_blocked(samples, 4800, &straight, AUDIO_RATE);
    for (size_t i = 0; i < 4800; i++) {
        if (samples[i]) {
            fprintf(stderr, "straight key leaked audio while up\n");
            failures++;
            break;
        }
    }

    ASTORE(straight_down, 1);
    render_blocked(samples, 4800, &straight, AUDIO_RATE);
    int straight_peak = 0;
    for (size_t i = 0; i < 4800; i++) {
        int v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > straight_peak)
            straight_peak = v;
    }
    if (straight_peak < 22000 || straight_peak > 23500) {
        fprintf(stderr, "straight key level is wrong (%d)\n", straight_peak);
        failures++;
    }

    ASTORE(straight_down, 0);
    render_blocked(samples, 4800, &straight, AUDIO_RATE);
    int straight_tail = 0;
    for (size_t i = 2400; i < 4800; i++) {
        int v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > straight_tail)
            straight_tail = v;
    }
    if (straight_tail > 100) {
        fprintf(stderr, "straight key did not release (%d)\n", straight_tail);
        failures++;
    }
    straight_mode = 0;

    /*
     * Weighting: a constant offset added to the mark and taken from the
     * following space, so the element period is unchanged.  0 is strict
     * ITU timing.
     */
    wpm = 12;                       /* dit = 100 ms = 4800 frames */
    weight_ms = 0.0;
    if (keyer_space_ns() != 100000000LL) {
        fprintf(stderr, "unweighted space is not one dit\n");
        failures++;
    }
    weight_ms = 20.0;
    if (keyer_space_ns() != 80000000LL) {
        fprintf(stderr, "positive weight did not shorten the space\n");
        failures++;
    }
    weight_ms = -20.0;
    if (keyer_space_ns() != 120000000LL) {
        fprintf(stderr, "negative weight did not lengthen the space\n");
        failures++;
    }
    wpm = MAX_WPM;                 /* dit = 30 ms */
    weight_ms = 25.0;
    if (keyer_space_ns() != 5000000LL) {
        fprintf(stderr, "max-speed positive weight did not leave a 5 ms space\n");
        failures++;
    }
    weight_ms = -25.0;
    if (keyer_space_ns() != 55000000LL) {
        fprintf(stderr, "max-speed negative weight did not leave a 5 ms dit\n");
        failures++;
    }
    wpm = 12;

    /* +20 ms: mark 120 ms, space 80 ms, period still 200 ms. */
    weight_ms = 20.0;
    audio_state_t weighted = {0};
    queue_mark(start_ns, 120000000LL);
    queue_mark(start_ns + 200000000LL, 120000000LL);
    render_blocked(samples, 256, &weighted, AUDIO_RATE);
    if (weighted.mark_end != 5760 || weighted.next_start != 9600) {
        fprintf(stderr, "weighted mark/space timing is wrong (%llu/%llu)\n",
                (unsigned long long)weighted.mark_end,
                (unsigned long long)weighted.next_start);
        failures++;
    }
    weight_ms = 0.0;

    failures += run_iambic_tests();
    failures += run_alphabet_tests();
    failures += run_debounce_tests();
    failures += run_evdev_tests();
    failures += run_press_order_tests();
    failures += run_decode_tests();
    failures += run_arg_tests();
    failures += run_control_tests();
    failures += run_control_pipe_test();
    failures += run_driver_tests();
    failures += run_main_tests();

    if (failures)
        fprintf(stderr, "%d test(s) failed\n", failures);
    return failures ? 1 : 0;
}
