// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * platform.h - the boundary between the portable keyer core and a platform
 * backend.
 *
 * cwkeyer.c is a single translation unit.  It holds the portable core (the
 * iambic keyer, the RC paddle filter, the decoder, the PCM renderer, the
 * terminal handling, and main) and ends with
 *
 *     #include "input_<platform>.c"
 *     #include "audio_<platform>.c"
 *
 * so a backend sees every core static and needs no exported API of its own.
 * This header fixes what a backend must provide; it is the whole contract.
 *
 * The backends are included rather than linked so that the test suites can
 * keep including cwkeyer.c and reaching into internals directly.
 *
 * Adding a platform:
 *   1. platform_<platform>.h: the CWKEY_* key codes and CWKEY_MAX.
 *   2. input_<platform>.c:   the input_backend_* functions below.
 *   3. audio_<platform>.c:   the audio_backend_* functions below.
 *   4. One branch in the platform selection here, and one at the end of
 *      cwkeyer.c.
 *
 * input_device_t, the device listing, the --setup wizard, and the known
 * paddle table live in the core; a backend only supplies the primitives in
 * the input section below.
 */
#ifndef CWKEYER_PLATFORM_H
#define CWKEYER_PLATFORM_H

#include <stddef.h>
#include <stdio.h>

/* The device view lives in the core; backends fill it in and read it. */
typedef struct input_device_t input_device_t;

#if defined(__APPLE__)
#include "platform_macos.h"
#elif defined(__linux__)
#include "platform_linux.h"
#else
#error "no platform backend for this target"
#endif

/*
 * Audio output.
 *
 * audio_backend_supports_mix() says whether the backend can mix the
 * microphone into the sidetone itself (mix_mic_mode); where it cannot, the
 * PulseAudio helper script does the job on the system side.
 *
 * audio_backend_open() negotiates a playback stream and reports the sample
 * rate the device actually uses; the core renders every mark at that rate.
 * audio_backend_run() then blocks, pushing or pulling generated audio, until
 * keep_running() turns false.  audio_backend_close() releases the stream and
 * is safe after a failed open and safe to call twice.
 */
int  audio_backend_supports_mix(void);
int  audio_backend_open(unsigned int *actual_rate);
int  audio_backend_run(void);
void audio_backend_close(void);

/*
 * Input.
 *
 * input_backend_thread() is the thread body. It watches the paddles and
 * keeps dit_paddle/dah_paddle, the dit_edge/dah_edge latches, and the
 * press-order stamps current through set_key_state(). It returns once
 * keep_running() turns false and leaves the paddle state cleared.
 *
 * suspend() and resume() drop and re-take whatever exclusive access the
 * backend may hold, so SIGTSTP/SIGCONT leave the shell usable.
 * grab_description() supplies the startup-banner text for the current grab
 * setting, which describes what will be attempted: whether it then works is
 * reported when the device opens. grab_possible() says whether exclusive
 * access can work at all for this user, which is what the setup wizard uses
 * to pick the default answer.
 *
 * The rest is the primitive set that the shared --list-devices listing and
 * --setup wizard are built on, so neither has to exist twice:
 *
 *   discover()/close_devices()  fill in and release an input_device_t array
 *   drain()                     drop events queued before a prompt
 *   wait_press()                next non-pointer key press on one/all device
 *   wait_release()              release of one key on one device
 *
 * wait_press() and wait_release() return 1 when the event happened, 0 on
 * timeout, and -1 when the device cannot be polled. `only` is a device index
 * or SIZE_MAX for "any device".
 *
 * devices_noun() and unreadable_hint() are the words the listing uses for
 * the platform's device nodes and for the advice it gives about them.
 */
void *input_backend_thread(void *arg);
void  input_backend_suspend(void);
void  input_backend_resume(void);
const char *input_backend_grab_description(void);
int  input_backend_grab_possible(void);
const char *input_backend_devices_noun(void);
const char *input_backend_unreadable_hint(void);

int  input_backend_discover(input_device_t **out, size_t *out_count);
void input_backend_close_devices(input_device_t *devices, size_t count);
void input_backend_drain(const input_device_t *devices, size_t count);
int  input_backend_wait_press(const input_device_t *devices, size_t count,
                              size_t only, int timeout_ms,
                              size_t *which, int *code);
int  input_backend_wait_release(const input_device_t *device, int code,
                                int timeout_ms);

int   input_backend_setup(char *device_path, size_t path_size);
int   input_backend_list(FILE *out);

/*
 * Help text.
 *
 * usage() keeps the portable half; a backend prints the parts that describe
 * its own device argument and environment.  Splitting it this way keeps
 * --help honest on a platform whose devices and settings differ.
 */
typedef enum {
    USAGE_DEVICE_ARG,   /* the device placeholder on the usage line */
    USAGE_EXAMPLES,     /* example command lines */
    USAGE_KEYNOTE,      /* where raw key codes come from */
    USAGE_DEVICE_OPTS,  /* the --list-devices and --setup paragraphs */
} usage_part_t;

void input_backend_usage(FILE *out, usage_part_t part, const char *prog);
void audio_backend_usage(FILE *out);

#endif /* CWKEYER_PLATFORM_H */
