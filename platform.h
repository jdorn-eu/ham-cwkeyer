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
 * Not yet behind the boundary: usage() and the startup banner still print
 * Linux device and environment text (the /dev/input path form, CW_ALSA_*,
 * PULSE_SINK).  That moves behind a backend hook when the second backend
 * lands, because until then there is nothing to say differently.
 */
#ifndef CWKEYER_PLATFORM_H
#define CWKEYER_PLATFORM_H

#include <stddef.h>
#include <stdio.h>

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
 * audio_backend_open() negotiates a playback stream and reports the sample
 * rate the device actually uses; the core renders every mark at that rate.
 * audio_backend_run() then blocks, pushing or pulling generated audio, until
 * keep_running() turns false.  audio_backend_close() releases the stream and
 * is safe after a failed open and safe to call twice.
 */
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
 * setting.
 *
 * list() and setup() back --list-devices and --setup. setup() fills in the
 * device path plus the dit/dah codes, the straight flag, and the grab
 * policy.
 */
void *input_backend_thread(void *arg);
void  input_backend_suspend(void);
void  input_backend_resume(void);
const char *input_backend_grab_description(void);
int   input_backend_list(FILE *out);
int   input_backend_setup(char *device_path, size_t path_size);

#endif /* CWKEYER_PLATFORM_H */
