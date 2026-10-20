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
    ca_last_sample_time = 0;
    ca_last_frames = 0;
    ca_gaps = 0;
    ca_slice_frames = 0;
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

    /* The host skipped 1024 frames: audio was lost. */
    make_time(&ts, 2048);
    (void)ca_render(NULL, NULL, &ts, 0, 512, &list);
    expect(ca_gaps == 1, "a gap in the sample timeline was not counted");
}

static void test_odd_buffer_lists(void)
{
    int16_t a[64];
    int16_t b[64];
    AudioBufferList list;
    AudioTimeStamp ts;

    reset_audio();

    /* More than one buffer is unexpected; refuse it without writing. */
    memset(&list, 0, sizeof(list));
    list.mNumberBuffers = 2;
    list.mBuffers[0].mNumberChannels = 1;
    list.mBuffers[0].mDataByteSize = sizeof(a);
    list.mBuffers[0].mData = a;
    list.mBuffers[1].mNumberChannels = 1;
    list.mBuffers[1].mDataByteSize = sizeof(b);
    list.mBuffers[1].mData = b;

    memset(a, 0x5a, sizeof(a));
    memset(b, 0x5a, sizeof(b));
    make_time(&ts, 0);
    expect(ca_render(NULL, NULL, &ts, 0, 64, &list) == noErr,
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

int main(void)
{
    test_silence_and_level();
    test_mark_ends();
    test_dropout_detection();
    test_odd_buffer_lists();

    if (failures == 0)
        printf("coreaudio tests passed\n");
    else
        fprintf(stderr, "coreaudio: %d test(s) failed\n", failures);

    return failures ? 1 : 0;
}
