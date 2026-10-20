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
 * README): a HALOutput unit accepts a mono S16 48 kHz client format, asks for
 * exactly one device buffer per callback (512 frames, 10.67 ms, on the
 * built-in speakers), and keeps the AU's own sample timeline contiguous.  Two
 * consequences shape this file:
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

/* Device selection: CW_AUDIO_DEVICE names a device or UID ("default" for the
 * system output).  CW_AUDIO_LATENCY_FRAMES caps nothing in CoreAudio, so the
 * only latency control is the device buffer, which is shared; we leave it
 * alone and report what we got. */

static AudioUnit ca_unit;
static unsigned int ca_rate;
static audio_state_t ca_state;

/* Diagnostics, reported through the core's exit summary. */
static uint64_t ca_last_sample_time;
static uint32_t ca_last_frames;
static int ca_have_last;
static unsigned int ca_gaps;
static double ca_delay_min = 1e9, ca_delay_max, ca_delay_sum;
static unsigned int ca_delay_n;
static uint32_t ca_slice_frames;
static UInt32 ca_device_buffer;

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

static OSStatus ca_render(void *refcon, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 frames, AudioBufferList *io)
{
    (void)refcon;
    (void)flags;
    (void)bus;

    if (!io || io->mNumberBuffers != 1 || !io->mBuffers[0].mData)
        return noErr;

    /*
     * The AU's own timeline is the exact record of whether audio was lost:
     * contiguous mSampleTime means the callback ran on time, a jump means the
     * host skipped audio (a dropout) and the operator heard a gap.
     */
    if (ca_have_last) {
        double expect = (double)ca_last_sample_time + (double)ca_last_frames;

        if (ts->mSampleTime < expect - 0.5 || ts->mSampleTime > expect + 0.5) {
            ca_gaps++;
            if (ca_gaps <= 3)
                fprintf(stderr,
                        "warning: audio dropout #%u: expected sample time %.0f, "
                        "got %.0f\n", ca_gaps, expect, ts->mSampleTime);
        }
    }
    ca_last_sample_time = (uint64_t)ts->mSampleTime;
    ca_last_frames = frames;
    ca_have_last = 1;

    if (ts->mFlags & kAudioTimeStampHostTimeValid) {
        double lead = host_seconds(ts->mHostTime) -
                      host_seconds(mach_absolute_time());

        if (lead < ca_delay_min)
            ca_delay_min = lead;
        if (lead > ca_delay_max)
            ca_delay_max = lead;
        ca_delay_sum += lead;
        ca_delay_n++;
    }

    if (frames > ca_slice_frames)
        ca_slice_frames = frames;

    generate_audio((int16_t *)io->mBuffers[0].mData, (size_t)frames,
                   &ca_state, ca_rate);

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

static int ca_has_output(AudioDeviceID id)
{
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0;

    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) != noErr)
        return 0;
    return sz > 0;
}

static AudioDeviceID ca_default_output(void)
{
    AudioDeviceID def = 0;
    UInt32 sz = sizeof(def);
    AudioObjectPropertyAddress a = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz,
                                   &def) != noErr)
        return 0;
    return def;
}

static AudioDeviceID ca_find_device(const char *want)
{
    AudioObjectPropertyAddress a = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioDeviceID *ids;
    UInt32 n;
    AudioDeviceID found = 0;

    if (!want || !*want || strcmp(want, "default") == 0)
        return ca_default_output();

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

        if (!ca_has_output(ids[i]))
            continue;
        ca_device_name(ids[i], name, sizeof(name));
        ca_device_uid(ids[i], uid, sizeof(uid));
        if (strcmp(name, want) == 0 || strcmp(uid, want) == 0)
            found = ids[i];
    }

    free(ids);
    return found;
}

static void ca_list_devices(void)
{
    AudioObjectPropertyAddress a = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioDeviceID *ids;
    UInt32 n;

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL,
                                      &sz) != noErr || sz == 0) {
        fprintf(stderr, "no audio output devices found\n");
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

            if (!ca_has_output(ids[i]))
                continue;
            ca_device_name(ids[i], name, sizeof(name));
            ca_device_uid(ids[i], uid, sizeof(uid));
            fprintf(stderr, "  %s  [%s]\n", name, uid);
        }
    }
    free(ids);
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
    char name[256], uid[256];
    UInt32 one = 1, zero = 0;
    OSStatus err;
    AudioStreamBasicDescription fmt;
    AudioStreamBasicDescription got;
    UInt32 got_size = sizeof(got);
    AURenderCallbackStruct cb = { .inputProc = ca_render, .inputProcRefCon = NULL };

    dev = ca_find_device(want);
    if (!dev) {
        fprintf(stderr, "no audio output device matching '%s'\n",
                want && *want ? want : "default");
        fprintf(stderr, "available output devices:\n");
        ca_list_devices();
        return -1;
    }
    if (!ca_has_output(dev)) {
        fprintf(stderr, "audio device '%s' has no output streams\n",
                want && *want ? want : "default");
        return -1;
    }

    ca_device_name(dev, name, sizeof(name));
    ca_device_uid(dev, uid, sizeof(uid));

    comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        fprintf(stderr, "cannot find the CoreAudio HAL output unit\n");
        return -1;
    }

    err = AudioComponentInstanceNew(comp, &ca_unit);
    if (err != noErr) {
        fprintf(stderr, "AudioComponentInstanceNew: %d\n", (int)err);
        ca_unit = NULL;
        return -1;
    }

    err = AudioUnitSetProperty(ca_unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0, &one, sizeof(one));
    if (err != noErr)
        fprintf(stderr, "warning: cannot enable audio output: %d\n", (int)err);

    (void)AudioUnitSetProperty(ca_unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1, &zero, sizeof(zero));

    err = AudioUnitSetProperty(ca_unit, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0, &dev, sizeof(dev));
    if (err != noErr) {
        fprintf(stderr, "cannot select audio device '%s': %d\n", name, (int)err);
        goto fail;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = (Float64)AUDIO_RATE;
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
        fprintf(stderr, "cannot set the mono 16-bit %d Hz client format: %d\n",
                AUDIO_RATE, (int)err);
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

    /* Confirm the AU took the format we asked for; the renderer counts
     * frames at this rate. */
    if (AudioUnitGetProperty(ca_unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &got, &got_size) != noErr) {
        fprintf(stderr, "cannot read back the audio stream format\n");
        goto fail;
    }
    if ((unsigned int)got.mSampleRate != AUDIO_RATE) {
        fprintf(stderr, "CoreAudio negotiated %u Hz, expected %d\n",
                (unsigned int)got.mSampleRate, AUDIO_RATE);
        goto fail;
    }

    ca_rate = (unsigned int)got.mSampleRate;
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

    fprintf(stderr,
            "CoreAudio device: %s (%s), rate: %u Hz, mono 16-bit, "
            "device buffer %u frames (%.1f ms)\n",
            name, uid, ca_rate, (unsigned)ca_device_buffer,
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
    OSStatus err = AudioOutputUnitStart(ca_unit);

    if (err != noErr) {
        fprintf(stderr, "AudioOutputUnitStart: %d\n", (int)err);
        return -1;
    }

    while (keep_running()) {
        struct timespec ts = {0, 20000000L}; /* 20 ms */

        (void)nanosleep(&ts, NULL);
    }

    AudioOutputUnitStop(ca_unit);

    /* Hand the backend's own diagnostics to the core's exit summary. */
    audio_underruns += ca_gaps;

    if (ca_delay_n) {
        fprintf(stderr, "audio: output latency %.1f ms (min %.1f, max %.1f), "
                "slice %u frames\n",
                ca_delay_sum / (double)ca_delay_n * 1000.0,
                ca_delay_min * 1000.0, ca_delay_max * 1000.0,
                ca_slice_frames);
    }

    return 0;
}

void audio_backend_close(void)
{
    if (!ca_unit)
        return;

    AudioUnitUninitialize(ca_unit);
    AudioComponentInstanceDispose(ca_unit);
    ca_unit = NULL;
}

void audio_backend_usage(FILE *out)
{
    fputs("  CW_AUDIO_DEVICE=default   Output device name or UID (default)\n",
          out);
}
