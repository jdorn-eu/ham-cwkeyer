// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * audio_coreaudio.c - macOS audio backend.
 *
 * Included at the end of cwkeyer.c; see platform.h for the contract.
 *
 * CoreAudio pulls where ALSA pushes, so this backend is the mirror image of
 * audio_alsa.c: instead of writing generated periods at the device, the
 * device asks for frames and we render them with the same generate_audio()
 * the Linux backend feeds.
 *
 * Everything here was measured before it was written (see the notes in the
 * README): a HALOutput unit accepts a mono S16 client format at the device's
 * nominal rate, asks for exactly one device buffer per callback (512 frames,
 * 10.67 ms, on the built-in speakers), and keeps the AU's own sample timeline
 * contiguous.  Two consequences shape this file:
 *
 *   - The slice size is the device's, not ours, so the renderer must accept
 *     any inNumberFrames.  It does: generate_audio() is size-agnostic.
 *   - A dropout does not return an error the way an ALSA underrun does.  It
 *     shows up as a jump in the AU's mSampleTime, which is what we count.
 *
 * The presentation delay (mHostTime - now) is measured and reported, because
 * that is the latency the operator actually hears.
 */

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <math.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform.h"

/* Device selection: CW_AUDIO_DEVICE names a device or UID, and "default"
 * follows the system output.  The only latency control CoreAudio offers is the
 * device buffer, which is shared with every other application, so it is left
 * alone; the buffer size and the measured output delay are reported instead. */

static AudioUnit ca_unit;
static unsigned int ca_rate;
static audio_state_t ca_state;

/* Diagnostics, reported through the core's exit summary. */
static uint64_t ca_last_sample_time;
static uint32_t ca_last_frames;
static unsigned int ca_callbacks;
static int ca_have_last;
static unsigned int ca_gaps;
static double ca_delay_min = 1e9, ca_delay_max, ca_delay_sum;
static unsigned int ca_delay_n;
static uint32_t ca_slice_frames;
static UInt32 ca_device_buffer;
static _Atomic unsigned int ca_bad_buffers;
static unsigned int ca_oversized_slices;
/* Set when the audio timeline must be re-anchored rather than measured: a
 * resume or a change of the system output device. */
static _Atomic int ca_reanchor;
static int ca_listener_installed;

#define CA_MAX_GAP_NOTES 4
static double ca_gap_expect[CA_MAX_GAP_NOTES];
static double ca_gap_got[CA_MAX_GAP_NOTES];

static double host_seconds(uint64_t ticks)
{
    static mach_timebase_info_data_t tb;
    static int have_tb;

    if (!have_tb) {
        (void)mach_timebase_info(&tb);
        have_tb = 1;
    }

    return (double)ticks * (double)tb.numer / (double)tb.denom / 1e9;
}

/* ---- mic mixing (--mix-mic) and monitoring ----
 *
 * Two audio units can run alongside the sidetone unit, and each runs on its
 * own clock:
 *
 *   - the microphone feeds the mix, so a voice application can take the
 *     operator and the sidetone from one input;
 *   - an optional monitor output plays the sidetone alone, so the operator
 *     can hear what they are sending while the mixed stream goes elsewhere.
 *
 * Both cross a ring.  The rates are nominally the same; when the clocks
 * disagree the ring absorbs it: if it overflows the oldest samples are
 * dropped, and if it runs dry the last sample is held, so neither a gap nor a
 * step appears in the audio.
 */
#define RING_FRAMES 48000u              /* about one second at 48 kHz */
#define MIX_SLICE_FRAMES 4096u          /* scratch for one callback */

typedef struct {
    /* Slots are atomic so a writer that laps the reader overwrites an old
     * sample instead of racing the read; the indices preserve the order. */
    _Atomic int16_t buf[RING_FRAMES];
    _Atomic uint64_t written;           /* monotonic; index by modulo */
    _Atomic uint64_t read;
    unsigned int dropped;               /* frames lost to overruns (reader) */
    unsigned int held;                  /* frames filled in on a dry ring */
} ring_t;

static ring_t mic_ring;
static ring_t monitor_ring;
static int16_t mix_capture[MIX_SLICE_FRAMES];   /* mic callback only */
static int16_t mix_mix[MIX_SLICE_FRAMES];       /* sidetone callback only */
static int16_t monitor_buf[MIX_SLICE_FRAMES];   /* monitor callback only */
static AudioUnit mic_unit;
static AudioUnit monitor_unit;
static int16_t mix_last_mic;
static int16_t monitor_last;

static void ring_put(ring_t *ring, const int16_t *src, size_t n)
{
    uint64_t w = atomic_load_explicit(&ring->written, memory_order_relaxed);

    for (size_t i = 0; i < n; i++)
        atomic_store_explicit(&ring->buf[(size_t)((w + i) % RING_FRAMES)],
                              src[i], memory_order_relaxed);

    /* Publish the new frames.  The reader owns `read`: if it falls a whole
     * ring behind, it skips forward itself, so the writer never touches the
     * consumer's cursor. */
    atomic_store_explicit(&ring->written, w + n, memory_order_release);
}

/* Copy up to n samples out; returns how many were available. */
static size_t ring_take(ring_t *ring, int16_t *dst, size_t n)
{
    uint64_t r = atomic_load_explicit(&ring->read, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&ring->written, memory_order_acquire);
    size_t got = 0;

    /* The writer lapped us: keep the newest RING_FRAMES and count the rest.
     * Only the consumer advances its own cursor. */
    if (w - r > RING_FRAMES) {
        ring->dropped += (unsigned int)(w - r - RING_FRAMES);
        r = w - RING_FRAMES;
    }

    while (got < n && r < w) {
        dst[got++] = atomic_load_explicit(&ring->buf[(size_t)(r % RING_FRAMES)],
                                          memory_order_relaxed);
        r++;
    }
    atomic_store_explicit(&ring->read, r, memory_order_release);

    return got;
}

/* Drain a ring into dst, holding the last sample when it runs dry. */
static void ring_fill(ring_t *ring, int16_t *dst, size_t n, int16_t *last)
{
    size_t got = ring_take(ring, dst, n);

    if (got > 0)
        *last = dst[got - 1];
    for (size_t i = got; i < n; i++)
        dst[i] = *last;
    ring->held += (unsigned int)(n - got);
}

/*
 * Add the microphone to the rendered sidetone.
 *
 * The microphone is ducked by the sidetone's instantaneous level rather than
 * the sum being clipped.  A clipped sine is a buzz, and the sidetone is the
 * thing the operator is listening to; with this the tone stays exactly as
 * generated and the sum cannot leave the sample range at all.  Between
 * elements, where the tone is silent, the microphone is untouched.
 *
 * Kept separate from the callback so it can be tested without a device.
 */
static void mix_block(int16_t *tone, const int16_t *mic, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int t = tone[i];
        int m = mic[i];
        int mag = t < 0 ? -t : t;

        /* -32768 has no positive counterpart, and negating it here would make
         * the headroom negative. */
        if (mag > 32767)
            mag = 32767;

        /* (32767 - mag) / 32767 is the headroom the tone is not using. */
        tone[i] = (int16_t)(t + (int)(((long)m * (32767 - mag)) / 32767));
    }
}

/* Bound a unit's render slices so the fixed capture scratch is enough. */
static void ca_set_max_slice(AudioUnit unit)
{
    UInt32 max_frames = MIX_SLICE_FRAMES;

    (void)AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global, 0, &max_frames,
                               sizeof(max_frames));
}

static OSStatus mic_render(void *refcon, AudioUnitRenderActionFlags *flags,
                           const AudioTimeStamp *ts, UInt32 bus,
                           UInt32 nframes, AudioBufferList *io)
{
    AudioBufferList buf;

    (void)refcon;
    (void)bus;
    (void)io;

    if (nframes > MIX_SLICE_FRAMES) {
        ca_oversized_slices++;
        return noErr;
    }

    /* The unit does not own a buffer for us: we supply one and pull. */
    buf.mNumberBuffers = 1;
    buf.mBuffers[0].mNumberChannels = 1;
    buf.mBuffers[0].mDataByteSize = nframes * 2;
    buf.mBuffers[0].mData = mix_capture;

    if (AudioUnitRender(mic_unit, flags, ts, 1, nframes, &buf) == noErr)
        ring_put(&mic_ring, mix_capture, nframes);

    return noErr;
}

static OSStatus monitor_render(void *refcon, AudioUnitRenderActionFlags *flags,
                               const AudioTimeStamp *ts, UInt32 bus,
                               UInt32 nframes, AudioBufferList *io)
{
    int16_t *out;
    UInt32 chans;

    (void)refcon;
    (void)flags;
    (void)ts;
    (void)bus;

    if (!io || io->mNumberBuffers != 1 || !io->mBuffers[0].mData) {
        (void)atomic_fetch_add_explicit(&ca_bad_buffers, 1,
                                        memory_order_relaxed);
        return noErr;
    }

    out = (int16_t *)io->mBuffers[0].mData;
    chans = io->mBuffers[0].mNumberChannels;
    if (chans == 0 ||
        (size_t)io->mBuffers[0].mDataByteSize <
            (size_t)nframes * (size_t)chans * sizeof(int16_t)) {
        (void)atomic_fetch_add_explicit(&ca_bad_buffers, 1,
                                        memory_order_relaxed);
        return noErr;
    }

    /* Cover every frame: a device buffer can be larger than the scratch. */
    for (size_t off = 0; off < nframes; off += MIX_SLICE_FRAMES) {
        size_t n = (size_t)nframes - off;

        if (n > MIX_SLICE_FRAMES)
            n = MIX_SLICE_FRAMES;

        ring_fill(&monitor_ring, monitor_buf, n, &monitor_last);

        for (size_t i = 0; i < n; i++)
            for (UInt32 c = 0; c < chans; c++)
                out[(off + i) * chans + c] = monitor_buf[i];
    }

    return noErr;
}

static OSStatus ca_render(void *refcon, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 frames, AudioBufferList *io)
{
    (void)refcon;
    (void)flags;
    (void)bus;

    if (!io || io->mNumberBuffers != 1 || !io->mBuffers[0].mData) {
        (void)atomic_fetch_add_explicit(&ca_bad_buffers, 1,
                                        memory_order_relaxed);
        return noErr;
    }
    if (io->mBuffers[0].mNumberChannels != 1 ||
        (size_t)io->mBuffers[0].mDataByteSize <
            (size_t)frames * sizeof(int16_t)) {
        (void)atomic_fetch_add_explicit(&ca_bad_buffers, 1,
                                        memory_order_relaxed);
        return noErr;
    }

    if (atomic_exchange_explicit(&ca_reanchor, 0, memory_order_relaxed))
        ca_have_last = 0;

    /*
     * The AU's own timeline is the exact record of whether audio was lost:
     * contiguous mSampleTime means the callback ran on time, a jump means the
     * host skipped audio and the operator heard a gap.  Skip the first
     * transition: a unit that has just started can re-anchor its timeline
     * while it settles, which is not audio anyone could have heard.
     */
    if (ca_have_last && ca_callbacks >= 2) {
        double expect = (double)ca_last_sample_time + (double)ca_last_frames;

        if (ts->mSampleTime < expect - 0.5 || ts->mSampleTime > expect + 0.5) {
            /* No stdio here: this runs on CoreAudio's realtime thread.  Record
             * the first few and report them once the unit has stopped. */
            if (ca_gaps < CA_MAX_GAP_NOTES) {
                ca_gap_expect[ca_gaps] = expect;
                ca_gap_got[ca_gaps] = ts->mSampleTime;
            }
            ca_gaps++;
        }
    }
    ca_last_sample_time = (uint64_t)ts->mSampleTime;
    ca_last_frames = frames;
    ca_have_last = 1;
    ca_callbacks++;

    if (ts->mFlags & kAudioTimeStampHostTimeValid) {
        double lead = host_seconds(ts->mHostTime) -
                      host_seconds(mach_absolute_time());

        /*
         * The first callback after the process was suspended carries a host
         * time from before the stop, which measures the suspend rather than
         * the audio path.  Ignore leads that no output device could justify.
         */
        if (lead >= -0.5) {
            if (lead < ca_delay_min)
                ca_delay_min = lead;
            if (lead > ca_delay_max)
                ca_delay_max = lead;
            ca_delay_sum += lead;
            ca_delay_n++;
        }
    }

    if (frames > ca_slice_frames)
        ca_slice_frames = frames;

    generate_audio((int16_t *)io->mBuffers[0].mData, (size_t)frames,
                   &ca_state, ca_rate);

    if (!mix_mic_mode && !monitor_unit)
        return noErr;

    {
        int16_t *out = (int16_t *)io->mBuffers[0].mData;

        /* The device buffer can exceed the scratch size; process it in
         * chunks so the microphone and the monitor cover every frame. */
        for (size_t off = 0; off < frames; off += MIX_SLICE_FRAMES) {
            size_t n = (size_t)frames - off;

            if (n > MIX_SLICE_FRAMES)
                n = MIX_SLICE_FRAMES;

            /* The monitor wants the tone as generated, before anything is
             * mixed into it, so it gets a copy on the way past. */
            if (monitor_unit)
                ring_put(&monitor_ring, out + off, n);

            if (mix_mic_mode) {
                ring_fill(&mic_ring, mix_mix, n, &mix_last_mic);
                mix_block(out + off, mix_mix, n);
            }
        }
    }

    return noErr;
}

/* ---- device selection ---- */

static int ca_device_name(AudioDeviceID id, char *out, size_t size)
{
    CFStringRef name = NULL;
    UInt32 sz = sizeof(name);
    AudioObjectPropertyAddress a = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };

    out[0] = '\0';
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &name) != noErr || !name)
        return -1;
    (void)CFStringGetCString(name, out, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(name);
    return 0;
}

static int ca_device_uid(AudioDeviceID id, char *out, size_t size)
{
    CFStringRef uid = NULL;
    UInt32 sz = sizeof(uid);
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };

    out[0] = '\0';
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &uid) != noErr || !uid)
        return -1;
    (void)CFStringGetCString(uid, out, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(uid);
    return 0;
}

static int ca_has_streams(AudioDeviceID id, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;

    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) != noErr)
        return 0;
    return sz > 0;
}

static int ca_has_output(AudioDeviceID id)
{
    return ca_has_streams(id, kAudioDevicePropertyScopeOutput);
}

static int ca_has_input(AudioDeviceID id)
{
    return ca_has_streams(id, kAudioDevicePropertyScopeInput);
}

static AudioDeviceID ca_default_device(AudioObjectPropertySelector selector)
{
    AudioDeviceID def = 0;
    UInt32 sz = sizeof(def);
    AudioObjectPropertyAddress a = {
        selector, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz,
                                   &def) != noErr)
        return 0;
    return def;
}

static AudioDeviceID ca_default_output(void)
{
    return ca_default_device(kAudioHardwarePropertyDefaultOutputDevice);
}

static AudioDeviceID ca_default_input(void)
{
    return ca_default_device(kAudioHardwarePropertyDefaultInputDevice);
}

/* Following the system output means the timeline re-anchors when the user
 * switches devices; that is not a dropout of the keyer's audio. */
static OSStatus default_device_changed(AudioObjectID id, UInt32 n,
                                       const AudioObjectPropertyAddress *addrs,
                                       void *client)
{
    (void)id;
    (void)n;
    (void)addrs;
    (void)client;

    atomic_store_explicit(&ca_reanchor, 1, memory_order_relaxed);
    return noErr;
}

static void ca_watch_default_device(int on)
{
    AudioObjectPropertyAddress a = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

    if (on) {
        if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a,
                                           default_device_changed,
                                           NULL) == noErr)
            ca_listener_installed = 1;
    } else if (ca_listener_installed) {
        (void)AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &a,
                                                default_device_changed, NULL);
        ca_listener_installed = 0;
    }
}

static AudioDeviceID ca_find_device(const char *want, int for_output)
{
    AudioObjectPropertyAddress a = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioDeviceID *ids;
    UInt32 n;
    AudioDeviceID found = 0;

    if (!want || !*want || strcmp(want, "default") == 0)
        return for_output ? ca_default_output() : ca_default_input();

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL,
                                      &sz) != noErr || sz == 0)
        return 0;

    n = sz / (UInt32)sizeof(AudioDeviceID);
    ids = calloc(n, sizeof(*ids));
    if (!ids)
        return 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz,
                                   ids) != noErr) {
        free(ids);
        return 0;
    }

    for (UInt32 i = 0; i < n && !found; i++) {
        char name[256], uid[256];

        if (!(for_output ? ca_has_output(ids[i]) : ca_has_input(ids[i])))
            continue;
        ca_device_name(ids[i], name, sizeof(name));
        ca_device_uid(ids[i], uid, sizeof(uid));
        if (strcmp(name, want) == 0 || strcmp(uid, want) == 0)
            found = ids[i];
    }

    free(ids);
    return found;
}

static double ca_nominal_rate(AudioDeviceID id)
{
    Float64 rate = 0.0;
    UInt32 sz = sizeof(rate);
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };

    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &rate) != noErr)
        return 0.0;
    return rate;
}

static UInt32 ca_buffer_frames(AudioDeviceID id)
{
    UInt32 frames = 0;
    UInt32 sz = sizeof(frames);
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };

    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &frames) != noErr)
        return 0;
    return frames;
}

static void ca_list_devices(FILE *out)
{
    AudioObjectPropertyAddress a = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioDeviceID *ids;
    AudioDeviceID def_out = ca_default_output();
    AudioDeviceID def_in = ca_default_input();
    UInt32 n;

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL,
                                      &sz) != noErr || sz == 0) {
        fprintf(out, "no audio output devices found\n");
        return;
    }

    n = sz / (UInt32)sizeof(AudioDeviceID);
    ids = calloc(n, sizeof(*ids));
    if (!ids)
        return;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz,
                                   ids) == noErr) {
        for (UInt32 i = 0; i < n; i++) {
            char name[256], uid[256];
            int has_out = ca_has_output(ids[i]);
            int has_in = ca_has_input(ids[i]);
            double rate = ca_nominal_rate(ids[i]);
            UInt32 buffer = has_out ? ca_buffer_frames(ids[i]) : 0;

            ca_device_name(ids[i], name, sizeof(name));
            ca_device_uid(ids[i], uid, sizeof(uid));
            fprintf(out, "  %s%s%s  %s  [%s]",
                    has_out ? "out" : "   ",
                    has_in ? " in" : "   ",
                    has_out || has_in ? "" : " --",
                    name, uid);
            if (rate > 0.0)
                fprintf(out, "  %.0f Hz", rate);
            if (buffer)
                fprintf(out, ", %u frames", (unsigned)buffer);
            if (ids[i] == def_out || ids[i] == def_in) {
                fputs("  (default", out);
                if (ids[i] == def_out)
                    fputs(" output", out);
                if (ids[i] == def_in)
                    fputs(" input", out);
                fputc(')', out);
            }
            fputc('\n', out);
        }
    }
    free(ids);
}

/*
 * Open the microphone.  This is the other half of --mix-mic: the same audio
 * ends up in one output device, so a voice application can take both the
 * operator and the sidetone from a single input.
 */
static int ca_open_mic(AudioDeviceID output_dev)
{
    const char *want = getenv("CW_MIX_MIC_DEVICE");
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp;
    AudioDeviceID dev;
    char name[256], uid[256];
    UInt32 one = 1, zero = 0;
    AudioStreamBasicDescription fmt;
    AURenderCallbackStruct cb = {
        .inputProc = mic_render, .inputProcRefCon = NULL };
    OSStatus err;

    dev = ca_find_device(want, 0);
    if (!dev) {
        fprintf(stderr, "mix-mic: no audio input device matching '%s'\n",
                want && *want ? want : "default");
        fprintf(stderr, "available audio devices:\n");
        ca_list_devices(stderr);
        return -1;
    }
    if (!ca_has_input(dev)) {
        fprintf(stderr, "mix-mic: '%s' has no input streams\n",
                want && *want ? want : "default");
        return -1;
    }

    ca_device_name(dev, name, sizeof(name));
    ca_device_uid(dev, uid, sizeof(uid));

    if (dev == output_dev)
        fprintf(stderr,
                "warning: mix-mic: the microphone and the output are the same "
                "device; this will feed back\n");

    comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        fprintf(stderr, "mix-mic: cannot find the CoreAudio HAL output unit\n");
        return -1;
    }

    err = AudioComponentInstanceNew(comp, &mic_unit);
    if (err != noErr) {
        fprintf(stderr, "mix-mic: AudioComponentInstanceNew: %d\n", (int)err);
        mic_unit = NULL;
        return -1;
    }

    ca_set_max_slice(mic_unit);

    (void)AudioUnitSetProperty(mic_unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1, &one, sizeof(one));
    (void)AudioUnitSetProperty(mic_unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0, &zero, sizeof(zero));

    err = AudioUnitSetProperty(mic_unit, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0, &dev, sizeof(dev));
    if (err != noErr) {
        fprintf(stderr, "mix-mic: cannot select input device '%s': %d\n",
                name, (int)err);
        goto fail;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = (Float64)ca_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket = 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 2;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 16;

    /* Audio flows device -> application on input element 1, so our side of
     * that element is its output scope. */
    err = AudioUnitSetProperty(mic_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));
    if (err != noErr) {
        fprintf(stderr, "mix-mic: cannot set the capture format: %d\n",
                (int)err);
        goto fail;
    }

    err = AudioUnitSetProperty(mic_unit,
                               kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0, &cb, sizeof(cb));
    if (err != noErr) {
        fprintf(stderr, "mix-mic: cannot set the capture callback: %d\n",
                (int)err);
        goto fail;
    }

    err = AudioUnitInitialize(mic_unit);
    if (err != noErr) {
        fprintf(stderr, "mix-mic: AudioUnitInitialize: %d\n", (int)err);
        fprintf(stderr,
                "         If Microphone access has not been granted, allow "
                "it for the terminal in\n"
                "         System Settings > Privacy & Security > Microphone, "
                "then restart it.\n");
        goto fail;
    }

    fprintf(stderr, "mix-mic: capturing %s (%s) into the sidetone output\n",
            name, uid);
    return 0;

fail:
    /* Every jump here happens before the unit was initialised, so disposing
     * is the whole cleanup. */
    AudioComponentInstanceDispose(mic_unit);
    mic_unit = NULL;
    return -1;
}

/*
 * Open the monitor output: the sidetone alone, so the operator can hear what
 * they are sending while the mixed stream goes to a voice application.
 */
static int ca_open_monitor(AudioDeviceID primary)
{
    const char *want = getenv("CW_MIX_MONITOR");
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp;
    AudioDeviceID dev;
    char name[256], uid[256];
    AudioStreamBasicDescription fmt;
    AURenderCallbackStruct cb = {
        .inputProc = monitor_render, .inputProcRefCon = NULL };
    OSStatus err;

    if (!want || !*want)
        return 0;

    dev = ca_find_device(want, 1);
    if (!dev) {
        fprintf(stderr, "monitor: no audio output device matching '%s'\n", want);
        fprintf(stderr, "available audio devices:\n");
        ca_list_devices(stderr);
        return -1;
    }
    if (!ca_has_output(dev)) {
        fprintf(stderr, "monitor: '%s' has no output streams\n", want);
        return -1;
    }
    if (dev == primary)
        fprintf(stderr,
                "warning: monitor: that is the same device the sidetone "
                "already goes to; you will hear it twice\n");

    ca_device_name(dev, name, sizeof(name));
    ca_device_uid(dev, uid, sizeof(uid));

    comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        fprintf(stderr, "monitor: cannot find the CoreAudio HAL output unit\n");
        return -1;
    }

    err = AudioComponentInstanceNew(comp, &monitor_unit);
    if (err != noErr) {
        fprintf(stderr, "monitor: AudioComponentInstanceNew: %d\n", (int)err);
        monitor_unit = NULL;
        return -1;
    }

    err = AudioUnitSetProperty(monitor_unit,
                               kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0, &dev, sizeof(dev));
    if (err != noErr) {
        fprintf(stderr, "monitor: cannot select '%s': %d\n", name, (int)err);
        goto fail;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = (Float64)ca_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket = 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 2;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 16;

    err = AudioUnitSetProperty(monitor_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    if (err != noErr) {
        fprintf(stderr, "monitor: cannot set the client format: %d\n", (int)err);
        goto fail;
    }

    err = AudioUnitSetProperty(monitor_unit,
                               kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (err != noErr) {
        fprintf(stderr, "monitor: cannot set the render callback: %d\n",
                (int)err);
        goto fail;
    }

    err = AudioUnitInitialize(monitor_unit);
    if (err != noErr) {
        fprintf(stderr, "monitor: AudioUnitInitialize: %d\n", (int)err);
        goto fail;
    }

    fprintf(stderr, "monitor: playing the sidetone on %s (%s)\n", name, uid);
    return 0;

fail:
    AudioComponentInstanceDispose(monitor_unit);
    monitor_unit = NULL;
    return -1;
}

int audio_backend_supports_mix(void)
{
    return 1;
}

int audio_backend_list(FILE *out)
{
    ca_list_devices(out);
    fputs("\nSet CW_AUDIO_DEVICE to an output name or UID; the default "
          "follows the system output.\n"
          "CW_MIX_MIC_DEVICE selects the microphone and CW_MIX_MONITOR the "
          "monitor output.\n", out);
    return 0;
}

/* ---- the backend API ---- */

int audio_backend_open(unsigned int *actual_rate)
{
    const char *want = getenv("CW_AUDIO_DEVICE");
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp;
    AudioDeviceID dev;
    int follows_default;
    char name[256], uid[256];
    OSStatus err;
    AudioStreamBasicDescription fmt;
    AudioStreamBasicDescription got;
    UInt32 got_size = sizeof(got);
    double wanted_rate;
    AURenderCallbackStruct cb = { .inputProc = ca_render, .inputProcRefCon = NULL };

    /*
     * With no device asked for, use DefaultOutput: a specialisation of the
     * same unit that follows the Sound Prefs default, so plugging in
     * headphones moves the sidetone with everything else.  Naming a device
     * pins it instead, which is what the mic-mix setup wants.
     */
    follows_default = !want || !*want || strcmp(want, "default") == 0;

    if (follows_default) {
        dev = ca_default_output();
        if (!dev) {
            fprintf(stderr, "no default audio output device\n");
            return -1;
        }
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    } else {
        dev = ca_find_device(want, 1);
        if (!dev) {
            fprintf(stderr, "no audio output device matching '%s'\n", want);
            fprintf(stderr, "available output devices:\n");
            ca_list_devices(stderr);
            return -1;
        }
        desc.componentSubType = kAudioUnitSubType_HALOutput;
    }

    if (!ca_has_output(dev)) {
        fprintf(stderr, "audio device '%s' has no output streams\n",
                follows_default ? "default" : want);
        return -1;
    }

    ca_device_name(dev, name, sizeof(name));
    ca_device_uid(dev, uid, sizeof(uid));

    comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        fprintf(stderr, "cannot find the CoreAudio HAL output unit\n");
        return -1;
    }

    /*
     * Use the device's own rate as the client format, so the renderer counts
     * frames at the rate the device runs instead of hiding a sample-rate
     * conversion in the HAL.  A device that cannot report one gets the same
     * 48 kHz the Linux backend asks ALSA for.
     */
    wanted_rate = ca_nominal_rate(dev);
    if (wanted_rate < 1.0)
        wanted_rate = (double)AUDIO_RATE;

    err = AudioComponentInstanceNew(comp, &ca_unit);
    if (err != noErr) {
        fprintf(stderr, "AudioComponentInstanceNew: %d\n", (int)err);
        ca_unit = NULL;
        return -1;
    }

    /*
     * No EnableIO here: an output unit is output-only by default, which is
     * exactly what this one is for, and DefaultOutput does not even have the
     * property (it would return kAudioUnitErr_InvalidProperty).  Only the
     * capture unit below has to turn anything on.
     */

    /* DefaultOutput finds the device itself and keeps following it. */
    if (!follows_default) {
        err = AudioUnitSetProperty(ca_unit, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global, 0, &dev, sizeof(dev));
        if (err != noErr) {
            fprintf(stderr, "cannot select audio device '%s': %d\n", name,
                    (int)err);
            goto fail;
        }
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = wanted_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket = 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 2;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 16;

    err = AudioUnitSetProperty(ca_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    if (err != noErr) {
        fprintf(stderr,
                "warning: cannot use a %.0f Hz client format (%d); "
                "retrying %d Hz\n",
                wanted_rate, (int)err, AUDIO_RATE);
        wanted_rate = (double)AUDIO_RATE;
        fmt.mSampleRate = wanted_rate;
        err = AudioUnitSetProperty(ca_unit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input, 0, &fmt,
                                   sizeof(fmt));
    }
    if (err != noErr) {
        fprintf(stderr, "cannot set the mono 16-bit %.0f Hz client format: "
                "%d\n", wanted_rate, (int)err);
        goto fail;
    }

    err = AudioUnitSetProperty(ca_unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (err != noErr) {
        fprintf(stderr, "cannot set the audio render callback: %d\n", (int)err);
        goto fail;
    }

    err = AudioUnitInitialize(ca_unit);
    if (err != noErr) {
        fprintf(stderr, "AudioUnitInitialize: %d\n", (int)err);
        goto fail;
    }

    /* Accept the rate the AU reports: it is what the renderer counts in. */
    if (AudioUnitGetProperty(ca_unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &got, &got_size) != noErr) {
        fprintf(stderr, "cannot read back the audio stream format\n");
        goto fail;
    }
    ca_rate = (unsigned int)(got.mSampleRate + 0.5);
    if (ca_rate == 0) {
        fprintf(stderr, "the audio unit reported a zero sample rate\n");
        goto fail;
    }
    *actual_rate = ca_rate;

    memset(&ca_state, 0, sizeof(ca_state));
    ca_state.gap_frames = ns_to_frames(keyer_space_ns(), ca_rate);

    if (freq_hz >= (double)ca_rate / 2.0) {
        fprintf(stderr, "tone %.2f Hz is at or above the Nyquist frequency "
                "(%u Hz sample rate)\n", freq_hz, ca_rate);
        goto fail;
    }

    {
        UInt32 bf = 0, bfs = sizeof(bf);
        AudioObjectPropertyAddress ba = {
            kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };

        if (AudioObjectGetPropertyData(dev, &ba, 0, NULL, &bfs, &bf) == noErr)
            ca_device_buffer = bf;
    }

    if (follows_default)
        ca_watch_default_device(1);

    if (mix_mic_mode && ca_open_mic(dev) < 0) {
        fprintf(stderr, "mix-mic: continuing without the microphone\n");
        mix_mic_mode = 0;
    }

    (void)ca_open_monitor(dev);

    fprintf(stderr,
            "CoreAudio device: %s (%s)%s, rate: %u Hz, mono 16-bit, "
            "device buffer %u frames (%.1f ms)\n",
            name, uid, follows_default ? " [system default]" : "",
            ca_rate, (unsigned)ca_device_buffer,
            1000.0 * (double)ca_device_buffer / (double)ca_rate);

    return 0;

fail:
    if (ca_unit) {
        AudioUnitUninitialize(ca_unit);
        AudioComponentInstanceDispose(ca_unit);
        ca_unit = NULL;
    }
    return -1;
}

int audio_backend_run(void)
{
    OSStatus err;

    if (mix_mic_mode && mic_unit) {
        err = AudioOutputUnitStart(mic_unit);
        if (err != noErr) {
            fprintf(stderr, "mix-mic: cannot start the microphone: %d; "
                    "continuing without it\n", (int)err);
            mix_mic_mode = 0;
        }
    }

    if (monitor_unit) {
        err = AudioOutputUnitStart(monitor_unit);
        if (err != noErr) {
            fprintf(stderr, "monitor: cannot start: %d; continuing without it\n",
                    (int)err);
            AudioComponentInstanceDispose(monitor_unit);
            monitor_unit = NULL;
        }
    }

    err = AudioOutputUnitStart(ca_unit);
    if (err != noErr) {
        fprintf(stderr, "AudioOutputUnitStart: %d\n", (int)err);
        return -1;
    }

    while (keep_running()) {
        struct timespec ts = {0, 20000000L}; /* 20 ms */

        (void)nanosleep(&ts, NULL);
    }

    AudioOutputUnitStop(ca_unit);
    if (mic_unit)
        AudioOutputUnitStop(mic_unit);
    if (monitor_unit)
        AudioOutputUnitStop(monitor_unit);

    if (mic_ring.dropped || mic_ring.held)
        fprintf(stderr, "mix-mic: %u captured frame(s) dropped, %u held\n",
                mic_ring.dropped, mic_ring.held);
    if (monitor_ring.dropped || monitor_ring.held)
        fprintf(stderr, "monitor: %u frame(s) dropped, %u held\n",
                monitor_ring.dropped, monitor_ring.held);

    if (ca_oversized_slices)
        fprintf(stderr,
                "mix-mic: %u capture slice(s) exceeded %u frames and were "
                "dropped\n",
                ca_oversized_slices, (unsigned int)MIX_SLICE_FRAMES);

    {
        unsigned int bad = atomic_load_explicit(&ca_bad_buffers,
                                                memory_order_relaxed);

        if (bad)
            fprintf(stderr,
                    "warning: %u render callback(s) received an unexpected "
                    "buffer list and were left alone\n", bad);
    }

    /* Hand the backend's own diagnostics to the core's exit summary. */
    audio_underruns += ca_gaps;

    for (unsigned int i = 0; i < ca_gaps && i < CA_MAX_GAP_NOTES; i++)
        fprintf(stderr,
                "warning: audio dropout #%u: expected sample time %.0f, "
                "got %.0f\n", i + 1, ca_gap_expect[i], ca_gap_got[i]);

    if (ca_delay_n) {
        fprintf(stderr, "audio: output latency %.1f ms (min %.1f, max %.1f), "
                "slice %u frames\n",
                ca_delay_sum / (double)ca_delay_n * 1000.0,
                ca_delay_min * 1000.0, ca_delay_max * 1000.0,
                ca_slice_frames);
    }

    return 0;
}

void audio_backend_suspend(void)
{
}

void audio_backend_resume(void)
{
    /* The unit stopped rendering while the process was stopped; the first
     * callback after SIGCONT has a new timeline, not a lost buffer. */
    atomic_store_explicit(&ca_reanchor, 1, memory_order_relaxed);
}

void audio_backend_close(void)
{
    ca_watch_default_device(0);

    if (monitor_unit) {
        AudioUnitUninitialize(monitor_unit);
        AudioComponentInstanceDispose(monitor_unit);
        monitor_unit = NULL;
    }

    if (mic_unit) {
        AudioUnitUninitialize(mic_unit);
        AudioComponentInstanceDispose(mic_unit);
        mic_unit = NULL;
    }

    if (!ca_unit)
        return;

    AudioUnitUninitialize(ca_unit);
    AudioComponentInstanceDispose(ca_unit);
    ca_unit = NULL;
}

void audio_backend_usage(FILE *out)
{
    fputs("  CW_AUDIO_DEVICE=default   Output device name or UID (default)\n"
          "  CW_MIX_MIC_DEVICE=default Input device name or UID for mix-mic\n"
          "  CW_MIX_MONITOR=none       Play the sidetone alone on this device\n",
          out);
}
