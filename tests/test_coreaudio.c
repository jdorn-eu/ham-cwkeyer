// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * macOS audio backend tests.
 *
 * CoreAudio has no equivalent of ALSA's "null" PCM, so instead of opening a
 * device these call the render callback directly with a synthetic buffer list,
 * which is what the backend does on CoreAudio's realtime thread anyway.  That
 * covers the parts that actually hold the timing: the frames handed in, what
 * comes back out, and how a dropout is detected.
 */
int cwkeyer_program_main(int argc, char **argv);
#define main cwkeyer_program_main
#include "../cwkeyer.c"
#undef main

static int failures;

static void expect(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "coreaudio: %s\n", what);
        failures++;
    }
}

/* One interleaved mono S16 buffer. */
static void make_list(AudioBufferList *list, void *data, UInt32 frames)
{
    memset(list, 0, sizeof(*list));
    list->mNumberBuffers = 1;
    list->mBuffers[0].mNumberChannels = 1;
    list->mBuffers[0].mDataByteSize = frames * 2;
    list->mBuffers[0].mData = data;
}

static void make_time(AudioTimeStamp *ts, double sample_time)
{
    memset(ts, 0, sizeof(*ts));
    ts->mSampleTime = sample_time;
    /* Deliberately no host time: the latency measurement is optional. */
}

static void reset_audio(void)
{
    memset(&ca_state, 0, sizeof(ca_state));
    ca_rate = AUDIO_RATE;
    ca_state.gap_frames = ns_to_frames(keyer_space_ns(), ca_rate);
    ca_have_last = 0;
    ca_callbacks = 0;
    ca_last_sample_time = 0;
    ca_last_frames = 0;
    ca_gaps = 0;
    ca_slice_frames = 0;
    ca_delay_min = 1e9;
    ca_delay_max = 0;
    ca_delay_sum = 0;
    ca_delay_n = 0;
}

static void test_silence_and_level(void)
{
    int16_t buf[480];
    AudioBufferList list;
    AudioTimeStamp ts;

    reset_audio();
    volume_percent = 70;
    wpm = 12.0;
    freq_hz = 800.0;

    make_list(&list, buf, 480);
    make_time(&ts, 0);
    expect(ca_render(NULL, NULL, &ts, 0, 480, &list) == noErr,
           "render returned an error");

    /* Nothing queued: exactly zero, not merely quiet. */
    int nonzero = 0;
    for (size_t i = 0; i < 480; i++)
        nonzero += buf[i] != 0;
    expect(nonzero == 0, "silence was not silent");
    expect(ca_slice_frames == 480, "slice size was not recorded");

    /* Queue a mark longer than the block we are about to render. */
    queue_mark(0, 1440000000LL);
    reset_audio();
    memset(&ca_state, 0, sizeof(ca_state));
    ca_state.gap_frames = 4800;

    static int16_t big[5760];
    memset(big, 0, sizeof(big));
    make_list(&list, big, 5760);
    make_time(&ts, 0);
    expect(ca_render(NULL, NULL, &ts, 0, 5760, &list) == noErr,
           "render returned an error for a long block");

    /* The mark starts at frame 0 and its envelope starts at zero. */
    expect(big[0] == 0, "the mark did not start at zero");
    int peak = 0;
    int first_sound = -1;
    int last_sound = -1;
    for (int i = 0; i < 5760; i++) {
        int a = big[i] < 0 ? -big[i] : big[i];
        if (a > peak)
            peak = a;
        if (a > 100) {
            if (first_sound < 0)
                first_sound = i;
            last_sound = i;
        }
    }
    /* 70% of full scale, within the raised-cosine ramps. */
    expect(peak > 32767 * 60 / 100 && peak <= 32767 * 70 / 100,
           "tone level is not 70% of full scale");
    /* 1.44 s = 69120 frames, longer than the block, so it runs to the end. */
    expect(first_sound >= 0 && last_sound == 5759,
           "the mark did not fill the block");
}

static void test_mark_ends(void)
{
    int16_t buf[5760];
    AudioBufferList list;
    AudioTimeStamp ts;

    reset_audio();
    volume_percent = 70;
    wpm = 12.0;
    freq_hz = 800.0;

    /* 100 ms mark (one dit at 12 WPM) rendered in a single 120 ms block. */
    queue_mark(0, 100000000LL);
    make_list(&list, buf, 5760);
    make_time(&ts, 0);
    expect(ca_render(NULL, NULL, &ts, 0, 5760, &list) == noErr,
           "render returned an error");

    int tail_nonzero = 0;
    for (int i = 4800; i < 5760; i++)
        tail_nonzero += buf[i] != 0;
    expect(tail_nonzero == 0, "the mark did not stop at its length");
    expect(buf[4799] == 0, "the mark did not release to zero");
}

static void test_dropout_detection(void)
{
    int16_t buf[512];
    AudioBufferList list;
    AudioTimeStamp ts;

    reset_audio();
    wpm = 12.0;
    freq_hz = 800.0;
    volume_percent = 70;

    make_list(&list, buf, 512);

    make_time(&ts, 0);
    (void)ca_render(NULL, NULL, &ts, 0, 512, &list);
    make_time(&ts, 512);
    (void)ca_render(NULL, NULL, &ts, 0, 512, &list);
    expect(ca_gaps == 0, "a contiguous timeline was reported as a dropout");

    /* The first transition is a settling unit, not a dropout. */
    reset_audio();
    make_time(&ts, 0);
    (void)ca_render(NULL, NULL, &ts, 0, 512, &list);
    make_time(&ts, 1376);
    (void)ca_render(NULL, NULL, &ts, 0, 512, &list);
    expect(ca_gaps == 0, "a settling unit was reported as a dropout");

    /* The host skipped 1024 frames: audio was lost. */
    make_time(&ts, 2048);
    (void)ca_render(NULL, NULL, &ts, 0, 512, &list);
    expect(ca_gaps == 1, "a gap in the sample timeline was not counted");
}

/* AudioBufferList declares room for one buffer; a two-buffer list needs a
 * struct that actually has the space, not an index past the end. */
typedef struct {
    AudioBufferList list;
    AudioBuffer extra;
} two_buffer_list_t;

/* Monotonic ticks for a duration, for synthesising host times. */
static uint64_t ticks_for(double seconds)
{
    mach_timebase_info_data_t tb;

    mach_timebase_info(&tb);
    return (uint64_t)(seconds * 1e9 * (double)tb.denom / (double)tb.numer);
}

static void test_latency_filter(void)
{
    int16_t buf[64];
    AudioBufferList list;
    AudioTimeStamp ts;

    reset_audio();
    wpm = 12.0;
    freq_hz = 800.0;
    volume_percent = 70;
    make_list(&list, buf, 64);

    /* A host time of now means no presentation delay, which is plausible. */
    memset(&ts, 0, sizeof(ts));
    ts.mSampleTime = 0;
    ts.mHostTime = mach_absolute_time();
    ts.mFlags = kAudioTimeStampHostTimeValid;
    (void)ca_render(NULL, NULL, &ts, 0, 64, &list);
    expect(ca_delay_n == 1, "a plausible presentation delay was not counted");

    /* A host time from before a suspend measures the suspend, not the audio
     * path, and must not drag the average down. */
    memset(&ts, 0, sizeof(ts));
    ts.mSampleTime = 64;
    ts.mHostTime = mach_absolute_time() - ticks_for(1.0);
    ts.mFlags = kAudioTimeStampHostTimeValid;
    (void)ca_render(NULL, NULL, &ts, 0, 64, &list);
    expect(ca_delay_n == 1, "a suspended gap was counted as presentation delay");
    expect(ca_delay_min > -0.001,
           "the latency minimum was dragged negative by a suspend");
}

static void test_odd_buffer_lists(void)
{
    int16_t a[64];
    int16_t b[64];
    two_buffer_list_t two;
    AudioBufferList list;
    AudioTimeStamp ts;

    reset_audio();

    /* More than one buffer is unexpected; refuse it without writing. */
    memset(&two, 0, sizeof(two));
    two.list.mNumberBuffers = 2;
    two.list.mBuffers[0].mNumberChannels = 1;
    two.list.mBuffers[0].mDataByteSize = sizeof(a);
    two.list.mBuffers[0].mData = a;
    two.extra.mNumberChannels = 1;
    two.extra.mDataByteSize = sizeof(b);
    two.extra.mData = b;

    memset(a, 0x5a, sizeof(a));
    memset(b, 0x5a, sizeof(b));
    make_time(&ts, 0);
    expect(ca_render(NULL, NULL, &ts, 0, 64, &two.list) == noErr,
           "an odd buffer list returned an error");

    int touched = 0;
    for (size_t i = 0; i < 64; i++)
        touched += a[i] != 0x5a5a || b[i] != 0x5a5a;
    expect(touched == 0, "an odd buffer list was written to");

    /* A null data pointer must not be dereferenced. */
    make_list(&list, NULL, 64);
    expect(ca_render(NULL, NULL, &ts, 0, 64, &list) == noErr,
           "a null buffer returned an error");
}

/* ---- mic mixing ---- */

static void reset_ring(ring_t *ring)
{
    atomic_store(&ring->written, 0);
    atomic_store(&ring->read, 0);
    memset(ring->buf, 0, sizeof(ring->buf));
    ring->dropped = 0;
    ring->held = 0;
}

static void reset_mix(void)
{
    reset_ring(&mic_ring);
    reset_ring(&monitor_ring);
    mix_last_mic = 0;
    monitor_last = 0;
}

static void test_mix_block(void)
{
    int16_t quiet[3] = {0, 0, 0};
    int16_t sig[3] = {5, -5, 32767};
    int16_t full[2] = {32767, -32767};
    int16_t loud[2] = {32767, -32768};
    int16_t half[1] = {16384};
    int16_t voice[1] = {20000};

    /* A silent sidetone passes the microphone through untouched. */
    mix_block(quiet, sig, 3);
    expect(quiet[0] == 5 && quiet[1] == -5 && quiet[2] == 32767,
           "the mixer altered a signal it should have passed through");

    /* A full-scale sidetone leaves no headroom, so the microphone is ducked
     * away entirely and the tone is preserved exactly. */
    mix_block(full, loud, 2);
    expect(full[0] == 32767, "the tone was altered at full scale");
    expect(full[1] == -32767, "the tone was altered at negative full scale");

    /* Half scale leaves about half the headroom:
     * 16384 + 20000 * (32767 - 16384) / 32767 = 16384 + 9999. */
    mix_block(half, voice, 1);
    expect(half[0] == 26383, "the microphone was not ducked by the tone level");

    /*
     * The point of ducking: no pair of inputs can wrap.  A sum that left the
     * range would come back on the wrong side of zero.
     */
    {
        int wrapped = 0;

        for (int t = -32768; t <= 32767; t += 61) {
            for (int m = -32768; m <= 32767; m += 397) {
                int16_t tt = (int16_t)t;
                int16_t mm = (int16_t)m;

                mix_block(&tt, &mm, 1);
                if (m > 0 && tt < t)
                    wrapped++;
                if (m < 0 && tt > t)
                    wrapped++;
            }
        }
        expect(wrapped == 0, "the mixer wrapped instead of ducking");
    }

    mix_block(quiet, sig, 0); /* no-op must not fault */
}

static void test_mix_ring(void)
{
    int16_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int16_t out[8];

    reset_mix();
    ring_put(&mic_ring, in, 8);
    expect(ring_take(&mic_ring, out, 8) == 8,
           "the ring did not return what went in");
    for (int i = 0; i < 8; i++)
        expect(out[i] == in[i], "ring order was not preserved");

    reset_mix();
    expect(ring_take(&mic_ring, out, 4) == 0, "a dry ring returned samples");

    /* A dry ring holds its last sample rather than stepping to zero. */
    {
        int16_t last = 321;
        int16_t block[4];

        reset_mix();
        ring_fill(&mic_ring, block, 4, &last);
        expect(block[0] == 321 && block[3] == 321,
               "an empty ring did not hold the last sample");
        expect(mic_ring.held == 4,
               "an empty ring did not report what it filled in");

        ring_put(&mic_ring, in, 2);
        ring_fill(&mic_ring, block, 4, &last);
        expect(block[0] == 1 && block[1] == 2, "the ring lost its data");
        expect(block[2] == 2 && block[3] == 2,
               "a partly dry ring did not hold the last sample");
    }

    /* Filling past capacity keeps the newest and drops the oldest. */
    reset_mix();
    for (size_t i = 0; i < RING_FRAMES + 10; i++) {
        int16_t v = (int16_t)(i % 1000);

        ring_put(&mic_ring, &v, 1);
    }
    expect(mic_ring.dropped == 10, "the ring did not drop the oldest samples");
    expect(ring_take(&mic_ring, out, 1) == 1,
           "the ring was empty after an overrun");
    expect(out[0] == 10, "the ring kept the wrong end after an overrun");
}

/* The whole path: captured samples in, sidetone plus microphone out. */
static void test_mix_in_render(void)
{
    int16_t buf[480];
    AudioBufferList list;
    AudioTimeStamp ts;
    int16_t mic = 1000;

    reset_audio();
    reset_mix();
    volume_percent = 70;
    wpm = 12.0;
    freq_hz = 800.0;

    /* 100 samples are waiting for a 480-frame block; the last one is
     * distinguishable so the held tail can be told from the real samples. */
    for (int i = 0; i < 100; i++) {
        int16_t v = (i == 99) ? 777 : mic;

        ring_put(&mic_ring, &v, 1);
    }

    mix_mic_mode = 1;
    make_list(&list, buf, 480);
    make_time(&ts, 0);
    expect(ca_render(NULL, NULL, &ts, 0, 480, &list) == noErr,
           "render returned an error with mic mixing on");

    /* No mark queued, so the sidetone is silence and the mic shows through. */
    expect(buf[0] == 1000, "the microphone did not reach the output");
    expect(buf[98] == 1000, "the microphone did not reach the end of its data");
    expect(buf[99] == 777, "the last captured sample was not used");

    /* Past the captured samples the last one is held, so the sum cannot
     * click; the drop to zero would be a step discontinuity. */
    expect(buf[100] == 777 && buf[479] == 777,
           "a dry mic ring did not hold its last sample");
    expect(mic_ring.held == 380, "the held-sample count is wrong");

    mix_mic_mode = 0;
}

int main(void)
{
    test_mix_block();
    test_mix_ring();
    test_mix_in_render();
    test_silence_and_level();
    test_mark_ends();
    test_dropout_detection();
    test_latency_filter();
    test_odd_buffer_lists();

    if (failures == 0)
        printf("coreaudio tests passed\n");
    else
        fprintf(stderr, "coreaudio: %d test(s) failed\n", failures);

    return failures ? 1 : 0;
}
