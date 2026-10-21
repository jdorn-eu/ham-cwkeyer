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

/* One chunk past the mixing scratch, to exercise the chunked path. */
#define LARGE_SLICE_FRAMES (MIX_SLICE_FRAMES + 16)

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
    atomic_store(&ca_bad_buffers, 0);
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

    /* A buffer that is too small, or not mono, must be left alone too. */
    memset(a, 0x5a, sizeof(a));
    make_list(&list, a, 64);
    list.mBuffers[0].mDataByteSize = 64 * 2 - 2;
    expect(ca_render(NULL, NULL, &ts, 0, 64, &list) == noErr,
           "a short buffer returned an error");
    touched = 0;
    for (size_t i = 0; i < 64; i++)
        touched += a[i] != 0x5a5a;
    expect(touched == 0, "a short buffer was written to");

    memset(a, 0x5a, sizeof(a));
    make_list(&list, a, 64);
    list.mBuffers[0].mNumberChannels = 2;
    expect(ca_render(NULL, NULL, &ts, 0, 64, &list) == noErr,
           "a non-mono buffer returned an error");
    touched = 0;
    for (size_t i = 0; i < 64; i++)
        touched += a[i] != 0x5a5a;
    expect(touched == 0, "a non-mono buffer was written to");
}

/* ---- mic mixing ---- */

static void reset_ring(ring_t *ring)
{
    atomic_store(&ring->written, 0);
    atomic_store(&ring->read, 0);
    for (size_t i = 0; i < RING_FRAMES; i++)
        atomic_store_explicit(&ring->buf[i], 0, memory_order_relaxed);
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

static void test_ring(void)
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
    expect(ring_take(&mic_ring, out, 1) == 1,
           "the ring was empty after an overrun");
    expect(mic_ring.dropped == 10, "the ring did not drop the oldest samples");
    expect(out[0] == 10, "the ring kept the wrong end after an overrun");
}

typedef struct {
    ring_t *ring;
    unsigned int total;
    int timed_out;
} ring_probe_t;

static void *ring_producer(void *arg)
{
    ring_probe_t *p = arg;
    int16_t block[64];

    for (unsigned int i = 0; i < p->total; ) {
        unsigned int n = p->total - i < 64 ? p->total - i : 64;

        for (unsigned int j = 0; j < n; j++)
            block[j] = (int16_t)((i + j) % 1000);
        ring_put(p->ring, block, (size_t)n);
        i += n;
        if ((i % 4096) == 0)
            sleep_ns(100000);
    }
    return NULL;
}

static void *ring_consumer(void *arg)
{
    ring_probe_t *p = arg;
    int16_t block[128];
    uint64_t got = 0;
    const int64_t deadline = mono_ns() + 5000000000LL;

    while (got + p->ring->dropped < p->total) {
        size_t n = ring_take(p->ring, block, sizeof(block) / sizeof(block[0]));

        got += n;
        if (n == 0) {
            if (mono_ns() >= deadline) {
                p->timed_out = 1;
                break;
            }
            sleep_ns(100000);
        }
    }
    return NULL;
}

/* A producer and a consumer on the ring at once: the indices must stay
 * consistent and the slots must not race. */
static void test_ring_threaded(void)
{
    enum { TOTAL = 200000 };
    ring_probe_t p = { &mic_ring, TOTAL, 0 };
    pthread_t producer, consumer;
    int has_consumer, has_producer;

    reset_mix();
    has_consumer = pthread_create(&consumer, NULL, ring_consumer, &p) == 0;
    has_producer = pthread_create(&producer, NULL, ring_producer, &p) == 0;
    expect(has_consumer && has_producer,
           "could not create the ring test threads");
    if (has_producer)
        pthread_join(producer, NULL);
    if (has_consumer)
        pthread_join(consumer, NULL);

    expect(!p.timed_out, "the threaded ring test timed out");
    expect(atomic_load(&mic_ring.read) == (uint64_t)TOTAL,
           "the threaded ring did not consume every frame");
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

/* A slice larger than the scratch must still mix the microphone to the end. */
static void test_large_mix_slice(void)
{
    int16_t buf[LARGE_SLICE_FRAMES];
    AudioBufferList list;
    AudioTimeStamp ts;

    reset_audio();
    reset_mix();

    for (size_t i = 0; i < LARGE_SLICE_FRAMES; i++) {
        int16_t v = 1000;

        ring_put(&mic_ring, &v, 1);
    }

    mix_mic_mode = 1;
    make_list(&list, buf, LARGE_SLICE_FRAMES);
    make_time(&ts, 0);
    expect(ca_render(NULL, NULL, &ts, 0, LARGE_SLICE_FRAMES, &list) == noErr,
           "render returned an error for a large slice");
    expect(buf[0] == 1000, "the microphone did not reach the first frame");
    expect(buf[LARGE_SLICE_FRAMES - 1] == 1000,
           "the microphone did not reach the tail of a large slice");
    expect(mic_ring.dropped == 0 && mic_ring.held == 0,
           "a large slice lost or invented capture frames");
    mix_mic_mode = 0;
}

/* The monitor output plays the sidetone copy that ca_render left behind. */
static void test_monitor_render(void)
{
    int16_t out[16];
    int16_t in[8] = {11, 22, 33, 44, 55, 66, 77, 88};
    AudioBufferList list;
    AudioTimeStamp ts;

    reset_audio();
    reset_mix();
    make_time(&ts, 0);

    ring_put(&monitor_ring, in, 8);
    make_list(&list, out, 8);
    expect(monitor_render(NULL, NULL, &ts, 0, 8, &list) == noErr,
           "the monitor returned an error");
    for (int i = 0; i < 8; i++)
        expect(out[i] == in[i], "the monitor did not copy the sidetone");

    /* A dry ring holds the last sample rather than going silent. */
    memset(out, 0, sizeof(out));
    make_list(&list, out, 4);
    expect(monitor_render(NULL, NULL, &ts, 0, 4, &list) == noErr,
           "the monitor returned an error on a dry ring");
    expect(out[0] == 88 && out[3] == 88,
           "a dry monitor ring did not hold its last sample");

    /* Both channels of a device get the same mono signal. */
    {
        int16_t stereo[16];

        reset_mix();
        memset(&list, 0, sizeof(list));
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 2;
        list.mBuffers[0].mDataByteSize = 8 * 2 * 2;
        list.mBuffers[0].mData = stereo;

        ring_put(&monitor_ring, in, 8);
        expect(monitor_render(NULL, NULL, &ts, 0, 8, &list) == noErr,
               "the monitor returned an error for a stereo device");
        for (int i = 0; i < 8; i++)
            expect(stereo[2 * i] == in[i] && stereo[2 * i + 1] == in[i],
                   "the monitor did not fill both channels");
    }

    /* An unexpected buffer list must not be written to. */
    reset_mix();
    ring_put(&monitor_ring, in, 8);
    memset(&list, 0, sizeof(list));
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = 1;
    list.mBuffers[0].mDataByteSize = 16;
    list.mBuffers[0].mData = NULL;
    expect(monitor_render(NULL, NULL, &ts, 0, 8, &list) == noErr,
           "the monitor returned an error for a null buffer");

    /* A slice larger than the scratch must be filled to the last frame. */
    {
        int16_t large[LARGE_SLICE_FRAMES];

        reset_mix();
        for (size_t i = 0; i < LARGE_SLICE_FRAMES; i++) {
            int16_t v = (int16_t)(i % 1000);

            ring_put(&monitor_ring, &v, 1);
        }
        make_list(&list, large, LARGE_SLICE_FRAMES);
        expect(monitor_render(NULL, NULL, &ts, 0, LARGE_SLICE_FRAMES,
                              &list) == noErr,
               "the monitor returned an error for a large slice");
        for (size_t i = 0; i < LARGE_SLICE_FRAMES; i++)
            expect(large[i] == (int16_t)(i % 1000),
                   "the monitor did not fill the tail of a large slice");
    }
}

int main(void)
{
    test_mix_block();
    test_ring();
    test_ring_threaded();
    test_mix_in_render();
    test_large_mix_slice();
    test_silence_and_level();
    test_mark_ends();
    test_dropout_detection();
    test_latency_filter();
    test_monitor_render();
    test_odd_buffer_lists();

    if (failures == 0)
        printf("coreaudio tests passed\n");
    else
        fprintf(stderr, "coreaudio: %d test(s) failed\n", failures);

    return failures ? 1 : 0;
}
