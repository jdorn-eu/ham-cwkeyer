# ham-cwkeyer

[![CI](https://github.com/jdorn-eu/ham-cwkeyer/actions/workflows/ci.yml/badge.svg)](https://github.com/jdorn-eu/ham-cwkeyer/actions/workflows/ci.yml)

Low-latency little C utility to be used with HRS (Ham Radio Solution)'s VBand adapter working as an iambic keyer in software.
## Design description
- Reads a paddle adapter (or a keyboard) through the platform input backend:
  evdev on Linux, IOKit HID on macOS
- Runs a Curtis 8044 type B iambic keyer (the Accu-keyer behaviour) with dot/dash paddle memory
- Runtime speed, pitch, and sidetone level controls from stdin while keying
- Renders each mark and its raised-cosine attack/release at PCM sample boundaries; late input/audio scheduling may still lengthen gaps
- Plays the sine wave through the platform audio backend: ALSA (PulseAudio/
  PipeWire) on Linux, CoreAudio on macOS. On Linux the default PCM is `pulse`
  with `PULSE_SINK=cw_out` for the mic-mixing setup; on macOS the sidetone
  follows the system output.

### Keying and audio timing

The input thread tracks paddle state (including presses that land between keyer samples) and recovers from dropped input events. The keyer thread runs a small state machine and watches the paddles through each mark and its following one-dit space, using monotonic deadlines for the nominal 1:3 dit/dah timing. With `decode` off, it does no Morse decoding or terminal output.

Paddle inputs are filtered with a digital approximation of the 8044's external RC network: the recommended 1 MΩ/0.01 µF network has a time constant of about 10 ms. The filter runs in the keyer's own time base, so contact bounce is smoothed and normally cannot inject extra elements. `CW_KEYER_DEBOUNCE_MS` sets the RC time constant (default 10 ms; 0 disables the filter). Lower it, for example to 5 ms, for a snappier response at high speed. As with the real RC network, a legitimate press shorter than the filter's threshold time may not be recognized; at high speeds use a shorter setting or 0.

The keyer implements the type B (Accu-keyer) behaviour that most transceivers mean by "Curtis mode B". The dot and dash latches are set whenever their paddle closes and cleared at the end of that element if the paddle is open. Because the latches drive element selection, releasing a squeeze leaves the opposite latch set, so one extra element is keyed: squeeze and release during the second dash and you get `-.-.` (C). A held paddle repeats its element, a held squeeze alternates, and a tap of the opposite paddle during an element is remembered (the "N"/"A" tests). When both levers close within the same sample, the keyer starts with whichever was closed first (the input backend preserves the press order). Type A is deliberately out of scope for this project.

The `straight` startup option turns the iambic core off and passes either paddle (or both, squeezed) through as a manual key: the sidetone follows the contact with the same raised-cosine ramp, and the RC paddle filter still applies. This is the software equivalent of the 8044ABM's separate `MAN. KEY` input. `decode` is ignored in this mode.

Optional weighting matches the 8044ABM's weight control, which the application note describes as stretching each element "by a constant amount regardless of what speed is being used". `CW_KEYER_WEIGHT_MS` (default `0`, range `-25`–`25`) adds that constant to every mark and removes it from the following space, so the element period and the character speed are unchanged: a `+10` setting makes marks heavier and spaces lighter, `-10` does the reverse. Because it is a fixed time, not a ratio, its relative effect grows with speed — exactly as the chip's RC network behaves — so a setting that sounds right at one speed may not at another. `0` is strict ITU timing. The supported speed range is 1–40 WPM: even at 40 WPM a dit lasts 30 ms, leaving at least 5 ms for both the mark and space at the maximum ±25 ms weighting. Faster internal renderer stress tests are not a promise that the 10 ms input filter can capture very short high-speed taps.

For each element, the keyer queues its start time and duration. The audio thread converts that duration to frames at the **sample rate the audio backend actually selected**, then generates the entire mark from sample counts rather than polling a key-down flag. Its raised-cosine attack and release fit *inside* the mark: normally 5 ms each, reduced to at most half a dit at high speeds. The tone defaults to 70% of the 16-bit PCM range and can be changed at startup with `CW_KEYER_LEVEL` or while running (see below). The audio renderer enforces the intended inter-element gap (one dit, reduced or extended by the weighting setting); late delivery cannot shorten a mark, but scheduling delays or audio interruptions can make a gap longer. The backend's buffering also adds audible latency, which is separate from the generated mark length.

The mark queue is bounded. If audio falls far enough behind to fill it, the program stops with an error instead of silently losing elements.

Audio health is reported at exit: audio interruptions are counted (ALSA underruns, or a jump in CoreAudio's sample timeline, with the first few logged as they happen), and the keyer counts elements that started more than 1 ms later than their nominal mark + space. If either is non-zero, a summary line with the count and the worst delay is printed when the keyer stops; a clean run stays silent, except that the macOS backend prints its measured output latency once at exit.

## Source layout

The program is one translation unit, split into a portable core and a pair
of platform backends:

```
cwkeyer.c          portable core: iambic keyer, RC filter, weighting,
                   decoder, PCM renderer, device listing, setup wizard,
                   terminal handling, arguments, main
platform.h         the backend contract and platform selection
platform_linux.h   CWKEY_* key codes for Linux (evdev codes)
platform_macos.h   CWKEY_* key codes for macOS (HID usages)
input_evdev.c      Linux input: /dev/input discovery, grab, primitive waits
audio_alsa.c       Linux output: ALSA, default PCM "pulse"
input_hid.c        macOS input: IOHID keyboards, seize, primitive waits
audio_coreaudio.c  macOS output: CoreAudio HALOutput render callback
```

`cwkeyer.c` ends with an `#include` of the backend pair for the target, so a
backend sees every core static and needs no exported API of its own. The
whole contract is declared in `platform.h`: `audio_backend_open/run/close`,
`input_backend_thread/suspend/resume/grab_description`, and the primitives
(`discover`, `close_devices`, `drain`, `wait_press`, `wait_release`) that the
shared `--list-devices` listing and `--setup` wizard are built on, so neither
has to exist twice. The keyer core, the RC filter, the weighting, the
decoder, and the renderer never name a platform API; keys are named through
the portable `CWKEY_*` namespace, which each platform supplies.

The backends are included rather than linked so the test suites can keep
including `cwkeyer.c` and reaching into internals directly. Porting means
adding `platform_<os>.h`, `input_<os>.c`, and `audio_<os>.c`, plus one branch
at the end of `cwkeyer.c` and one in `platform.h`.

## Tests

The build is deliberately strict by default: `-Werror` plus a broad warning set (including `-Wconversion`, `-Wsign-conversion`, `-Wshadow`, `-Wcast-qual`, and `-Wdouble-promotion`). Both GCC and clang are supported; the Makefile adapts the warning flags to the compiler (`make CC=clang`). Pass `make WERROR=` to build despite warnings on an unknown toolchain. `make analyze` runs GCC's `-fanalyzer`.

`make test` builds and runs the hardware-independent suites below; it does not touch your input device, audio routing, or sound server:

`make test` picks the per-platform suites: the portable two plus `test_audio`
and `test_evdev` on Linux, or `test_coreaudio` and `test_hid` on macOS.

- `tests/test_keyer.c` — the portable core: the type B iambic core (held-paddle repeats, alternating squeezes, the N/A memory cases, and the `C` extra-element rule), the RC paddle filter (glitches rejected, bounce smoothed, threshold delay), the decode display (character and word gaps, unknown symbols dropped), the PCM renderer (envelope, spacing across buffer boundaries, late marks, high-speed ramp, level scaling), a short run of the real `keyer_thread` to check mark sequencing, the device listing and prompt formatting, the runtime controls, the `main()` startup paths, and the command line itself (options in any order, `--setup` with settings, the wording of error messages, `--help` on stdout). On Linux it also covers evdev event parsing (press/release, autorepeat ignored, `SYN_DROPPED` resync), the capability bitmap and classification, and the pipe-driven tap waits.
- `tests/test_signals.c` — `SIGPIPE` ignored, a stop signal stops the loops, terminal disable/restore is idempotent on a pty, queued tty input is discarded on restore, a spurious `SIGCONT` does not re-disable echo, and `SIGTSTP`/`SIGCONT` genuinely stop and resume through the signal-waiting thread.
- `tests/test_audio.c` — the audio layer against ALSA's built-in `null` PCM (no sound server or hardware): `open_audio()` opens and negotiates a rate, `pcm_write_all()` writes generated periods, `audio_loop()` runs in a thread and stops cleanly, and a missing device and an at-Nyquist tone are both rejected.
- `tests/test_evdev.c` — `input_backend_thread()` fed through a FIFO standing in for the input device: press/release update the paddle state, the press edge is latched, and state is cleared on exit.
- `tests/test_mic_mix.sh` — `cw-mic-mix.sh` against a mocked `pactl`: setup, idempotent reruns, `--unload`, rollback on a failed module load, and argument validation. Linux only.
- `tests/test_coreaudio.c` — `ca_render()` driven with a synthetic buffer list, which is what CoreAudio does on its realtime thread: silence is exactly zero, a queued mark has the right level and stops at its length, a jump in the sample timeline is counted as a dropout, an implausible host time (from a suspend) is kept out of the latency figures, an unexpected buffer list is refused without being written, the microphone is summed with clipping, and the capture ring drops the oldest and holds the last sample.
- `tests/test_hid.c` — the backend's decision logic through `hid_dispatch()`: run-mode press and release with press-order stamping, waiting for a press or a release including the device filter, and ignoring other keys, other usage pages, value 2, and callbacks that arrive after their phase ended.

The same suite also covers argument parsing (`parse_double_arg`, `parse_uint_env`, `lookup_key`), input-device classification for `--list-devices`, paddle tap detection, setup prompts, runtime control keys, and a real `main()` startup/failure/shutdown path.

There is also an optional differential test against the real UnixCW libcw keyer: `make test-libcw` drives libcw's iambic graph and our mode B core with identical random paddle schedules and compares the element sequences (200k trials, no mismatches). It needs a built unixcw checkout; set `LIBCW_DIR` if it is not at `../unixcw-code`. It is not part of `make test` or CI because of that external dependency.

`make test-differential` is a self-contained differential test against eight third-party iambic keyer variants (seven projects: recri/keyer's vk6ph, ad5dz, dttsp, k1el and nd7pa, plus cw-dojo and morse-flipper; dttsp is tested in two configurations). Each algorithm is transcribed into the test, so it needs no external checkout; it checks the published `N`/`A`/`C` reference cases, 20,000 random paddle schedules, and a realistic operator gesture for every character in the decode table (with and without human timing jitter). It is kept out of `make test` because it takes a few seconds. See `docs/validation.md` for the results and for the argument that the type B core matches the 8044ABM specification.

A live paddle/listening check is still useful after changing timing, debouncing, or ALSA settings.

`make coverage` (requires `gcovr`) runs the C suites with `--coverage` and writes an HTML report to `coverage/index.html` plus a Cobertura `coverage/coverage.xml`. Coverage is reported for `cwkeyer.c` and the Linux backends and sits around 70%; the remainder is mostly error paths that are hard to trigger (thread-creation failure, ALSA recovery, evdev read errors) and the usage text, so treat it as a map of what to test next rather than a target.

CI (`.github/workflows/ci.yml`) runs, on every push and pull request: the strict GCC build, `make test`, `make analyze`, and `shellcheck`; a clang build and test; a sanitizer job that rebuilds and runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer; and a coverage job that uploads the report as an artifact. A macOS job builds and runs the portable and Darwin suites, the differential test, and the sanitizers; it cannot ask for AddressSanitizer's leak checker, which Darwin does not provide.

## License

GPL-3.0-or-later; see `LICENSE`.

Mode behaviour follows the primary sources: the Curtis Electro Devices *8044 Series Keyer-on-a-Chip Application Note* and Karl Fischer (DJ5IL), *All About Squeeze-Keying*. The latter distinguishes the true type A edge-memory rule from the Accu-keyer type B latch rule; this project implements and verifies only the Accu-keyer/type B behaviour, against UnixCW libcw on 200k random paddle schedules and eight third-party keyer variants (see `docs/validation.md`).

## Build (Linux)
```
sudo dnf install gcc alsa-lib-devel
make        # strict build (-Werror plus a broad warning set)
make test    # iambic, RC filter, evdev, decode, driver, signal, and script checks
make analyze # GCC -fanalyzer
make coverage # gcovr HTML/XML report in coverage/
```

## macOS

The same source builds on macOS with the IOKit and CoreAudio backends:

```
make                 # the Makefile picks the Darwin backends and frameworks
./cwkeyer --list-devices
./cwkeyer --setup
```

No driver is needed. A paddle adapter is a plain USB HID keyboard on macOS,
which is exactly what the HRS PaddleAdapter reports: its report descriptor
offers nothing but the eight modifier usages 0xE0..0xE7, so the default
LEFTCTRL/RIGHTCTRL paddle mapping works unchanged. Devices are named
`vendor:product:location` (for example `04d8:01a6:00110000`), which is what
`--list-devices` prints and what the keyer takes as its device argument. The
last field is the USB port, so it changes when the adapter is moved; the
argument may therefore stop at a field boundary, and `04d8:01a6` selects the
adapter wherever it is plugged in as long as that is unambiguous. The built-in
keyboard reports zeros for vendor and product, so it needs the full id.

Three things differ from Linux, all of them forced by the platform:

- **Input Monitoring.** macOS only delivers keyboard HID values to a process
  holding the Input Monitoring permission, and attributes it to whatever
  launched the keyer (your terminal). If the permission has been denied, the
  keyer says so at startup instead of waiting for a tap that never arrives;
  grant it under System Settings → Privacy & Security → Input Monitoring and
  restart the terminal.
- **Exclusive access needs root.** `kIOHIDOptionsTypeSeizeDevice` stands in
  for `EVIOCGRAB`, and is refused for keyboards unless the process is root,
  so the keyer defaults to `nograb` when exclusive access is not possible (a
  normal user); `--setup` offers the same default. An explicit attempt that
  fails falls back with a warning. Without it the levers also reach the
  foreground application, which is harmless for a dedicated adapter and noisy
  on a normal keyboard.
- **Launchers, app bundles, and `sudo`.** macOS attributes Input Monitoring
  and Microphone permission to the process that launches the keyer, usually
  your terminal. If you start it from a script, a launcher, or an app bundle,
  grant the permission to that program instead. `sudo` changes the environment
  and the TCC identity too, so prefer `--setup` (which defaults to `nograb`
  where a seize is impossible) over running the keyer as root.
- **Latency is the device buffer.** CoreAudio asks for one device buffer per
  render callback (512 frames, 10.7 ms on the built-in speakers) and the
  measured output latency is about 12 ms. The client format uses the output
  device's nominal rate, so the renderer counts frames at the rate the device
  runs rather than through a hidden conversion; 48 kHz is the fallback.
  `CW_AUDIO_DEVICE` names an output device or UID and pins it; the default
  follows the system output and keeps following it, so plugging in headphones
  moves the sidetone with everything else. `./cwkeyer --list-audio-devices`
  prints the names, UIDs, rates, and
  defaults. Mark lengths are unaffected either way, because every mark is
  still rendered at sample boundaries.

The capture ring is measured rather than merely reasoned about: a minute of
`mix-mic` into BlackHole reported no dropped frames and one callback's worth
held (512 frames, before the microphone had produced its first buffer), so the
two units share a clock closely enough that no resampling is needed.

A dropout is reported the way an ALSA underrun is, but without an error code:
CoreAudio's own sample timeline jumps when audio is lost, and that jump is
counted and shown in the exit summary. A suspend/resume or a switch of the
system output device re-anchors the timeline instead, so neither is mistaken
for a dropout.

`make test` runs the portable suites plus the Darwin ones on macOS and the
Linux ones elsewhere; `make install` installs `cw-mic-mix` only on Linux,
because it drives `pactl`. `make analyze` uses `-fanalyzer` with GCC and
clang's own analyzer otherwise. `make coverage` is Linux-only.

## Install

```
make install                        # /usr/local by default
make install PREFIX=/usr            # or another prefix
make install DESTDIR=/tmp/stage PREFIX=/usr   # for packagers
```

This installs `cwkeyer` and the `cwkeyer(1)` man page everywhere, plus the
`cw-mic-mix` helper and its man page on Linux. `make uninstall` removes them. `make dist` produces a `ham-cwkeyer-<version>.tar.gz` from the current commit; the version comes from `git describe` (a `-dirty` suffix is stripped for the tarball), or override it with `make VERSION=1.2.3`. Run `cwkeyer --version` to see the version a binary was built with.

On Linux, in order for your user session to be able to access the adapter input, set proper udev rules:
```
lsusb   # find the adapter's vendor:product ID
```
```
# /etc/udev/rules.d/99-cwkey.rules
SUBSYSTEM=="input", ATTRS{idVendor}=="XXXX", ATTRS{idProduct}=="YYYY", MODE="0660", GROUP="input"
```
```
sudo usermod -aG input $USER   # re-login after this
```

## Run (Linux)

For macOS, use the examples in the macOS section above: the device is
`vendor:product:location` and there is no udev rule.

First check for the correct device: `sudo libinput list-devices` or `sudo evtest` are your friend. If you want to use your keyboard, that's usually `/dev/input/event3`. In that case, add the `nograb` parameter so you can still send SIGINT to close the program.
```
./cwkeyer                                      # on a terminal: tap-to-select setup
./cwkeyer --setup                              # tap your paddles to pick the device
./cwkeyer --setup 25 700                       # ... and start at 25 wpm, 700 Hz
./cwkeyer --setup mix-mic                      # ... with mic mixing (macOS)
./cwkeyer /dev/input/eventX                    # 12 wpm, 800 Hz
./cwkeyer /dev/input/eventX 25 700             # 25 wpm, 700 Hz
./cwkeyer /dev/input/eventX 25 700 COMMA DOT   # remap dit/dah keys
./cwkeyer /dev/input/event3 nograb             # keyboard instead of HRS vband adapter
./cwkeyer /dev/input/event3 nograb decode      # also print keyed morse as text
./cwkeyer /dev/input/eventX straight            # paddles act as a straight key
```
Options and keywords may appear anywhere, and `--setup` can be combined with
the settings it should start with, as in the second and third lines above.
A bad argument gets one line and a pointer to `--help`, rather than the whole
usage text; `--help` itself is written to standard output, so it can be piped
or paged.

If you do not know which device is your adapter, `./cwkeyer --list-devices`
groups the readable `/dev/input/event*` devices into `unknown`, `keyboard`,
and `paddle` categories in that order, so paddles appear last and stay near
the shell prompt. Each group is sorted by device number, and devices that
report the default LEFTCTRL/RIGHTCTRL paddle keys are marked. The category
names are colored when stdout is a terminal; set `NO_COLOR` to turn that
off. Known adapters, currently the HRS PaddleAdapter, are also recognized by
USB vendor/product ID and marked in the listing; the table in `cwkeyer.c`
stores their default keys and is a one-line addition per device. A single
known adapter skips the setup tap test.
`./cwkeyer --setup` is the guided version: it asks you to tap the dot lever
and then the dash lever, learns the device and both key codes, and starts
with the defaults. It does not need the `/dev/input/eventX` name. If exactly
one known paddle adapter is present, the tap test is skipped and the
adapter's known default keys are used. Otherwise, a device that looks like a
normal keyboard defaults to `nograb`; a dedicated paddle defaults to
exclusive grab. If both taps use the same key, it offers straight-key mode.
Speed, pitch, level, and weighting stay at their defaults and remain
adjustable while running.

`./cwkeyer --list-audio-devices` (or `-A`) prints the audio devices the
platform accepts with the names its audio settings use: ALSA PCM names and a
reminder about `PULSE_SINK` on Linux, and device names or UIDs with their
rates, buffers, and defaults on macOS. It is the quickest way to fill in
`CW_ALSA_DEVICE`, `CW_AUDIO_DEVICE`, `CW_MIX_MIC_DEVICE`, or
`CW_MIX_MONITOR`.
With no arguments on an interactive terminal, `./cwkeyer` runs the same
setup; without a terminal it prints the usage text.
The default speed, pitch, PCM peak level, and 5 ms raised-cosine slopes match UnixCW's `cw` defaults: 12 WPM, 800 Hz, and 70% of full scale. Paddle defaults are left ctrl (dit) and right ctrl (dah). `./cwkeyer --help` lists the known key names; raw evdev key codes work too.

While running, terminal echo is disabled so keystrokes don't print in the window (the keyer reads evdev, not the tty). Terminal input queued while the keyer runs is discarded on exit, so keystrokes do not reappear at the shell prompt. Ctrl+C, SIGTERM, SIGHUP, and SIGQUIT stop it cleanly and restore the terminal and the evdev grab. Ctrl+Z suspends it properly: a dedicated thread releases the terminal and grab before stopping and re-establishes them on resume, so the shell stays usable while it is suspended. Ordinary stop/suspend operations run outside asynchronous signal handlers and are serialized with device and terminal startup/shutdown. A fatal crash (for example SIGSEGV) still takes the kernel's default action; terminal restoration cannot be guaranteed after a crash. If needed, run `stty sane` to repair the tty.

With `decode`, each keyed symbol is printed as text as you key, without newlines; unknown symbols are ignored. Pausing for more than seven dits between characters (the ITU word gap) prints a single space, so word breaks you key by hand appear as spaces. Decoder output is written synchronously by the keyer thread, so a blocked stdout consumer can delay paddle processing; keep the output destination draining when using `decode`.

## Runtime controls

If stdin is a readable terminal or pipe, the keyer accepts single-key controls while it is running:

- `[` / `]` — slower / faster (1 WPM per step, limited to 1–40 WPM)
- `-` / `=` (or `+`) — lower / higher pitch (10 Hz per step)
- `9` / `0` — quieter / louder (5% per step)
- `,` / `.` — lighter / heavier weighting (1 ms per step)

A pipe's final controls are read even if its writer has already closed. Speed, pitch, level, and weighting changes take effect on the next element. On a terminal, the current settings are shown on a single status line that is rewritten in place rather than appended line by line. The startup values come from the command line, `CW_KEYER_LEVEL`, and the usual environment variables. These control keys are read from stdin, not from the evdev paddle device, so avoid remapping a paddle to one of them.

## Environment
- `CW_ALSA_DEVICE` Linux only. ALSA PCM to open. Default: `pulse`. With this default, an unset or empty `PULSE_SINK` is tried as `cw_out`, falling back to the system default sink if that one is unavailable. `./cwkeyer --list-audio-devices` prints the PCM names.
- `CW_ALSA_LATENCY_US` Linux only. Requested ALSA latency in microseconds. Default: `20000` (20 ms). ALSA may negotiate a different buffer/period; the achieved values are printed at startup. Smaller buffers may reduce latency but increase underrun risk.
- `CW_KEYER_LEVEL` Sidetone level, percent of full scale, `0`–`100`. Default: `70`. The level can also be changed while running with `9` and `0`.
- `CW_KEYER_WEIGHT_MS` Weighting, `-25`–`25` ms. Default: `0` (strict ITU timing). A constant time added to each mark and removed from the following space. The weighting can also be changed while running with `,` and `.`.
- `CW_KEYER_DEBOUNCE_MS` Paddle RC filter time constant in milliseconds, `0`–`50`. Default: `10`; `0` disables the filter. The 8044's recommended 1 MΩ/0.01 µF network is about 10 ms. A press shorter than the filter's threshold time may not be recognized; lower the value for high-speed sending.
- `PULSE_SINK` Linux only. Pulse sink the keyer audio goes to. If the device is `pulse` and this is unset or empty, the keyer tries `cw_out`, the combined sink created by `cw-mic-mix.sh`. If that sink is unavailable, it falls back to the system default sink with a warning, so a bare run still plays; set this explicitly to choose a different sink.
- `CW_AUDIO_DEVICE` macOS only. Output device name or UID. Default: follow the system output, so the sidetone moves with everything else. `./cwkeyer --list-audio-devices` prints the names.
- `CW_MIX_MIC_DEVICE` macOS only. Microphone captured by `mix-mic`. Default: the system input.
- `CW_MIX_MONITOR` macOS only. Output device that plays the sidetone alone while the mixed stream goes to a voice application.
- `NO_COLOR` If set, `--list-devices` and the setup wizard do not use colors.

## Combine with mic input (Linux)

```
./cw-mic-mix.sh                                # auto-detects default mic + output
./cwkeyer /dev/input/eventX                    # PULSE_SINK defaults to cw_out
```
Then in an app, set the microphone/input device to **Monitor of CW_Mic_Mix**.

What the script sets up:
- `cw_mic_mix` is a null sink; its monitor is what the app sees as the input.
- Your real mic is looped into `cw_mic_mix` only, so you don't hear your own mic.
- `cw_out` is a combine sink: the keyer tone plays to both `cw_mic_mix` and your headphones.

If you want different mic/output devices than your system defaults: `./cw-mic-mix.sh alsa_input.usb-... alsa_output.pci-...` (list names with `pactl list short sources` / `pactl list short sinks`).

Tear it down again with:
```
./cw-mic-mix.sh --unload
```

Notes:
- The script is idempotent: re-running it is safe, leftover modules from a previous (or crashed) run are unloaded first. If a module load fails or setup catches HUP, INT, or TERM, modules created by that attempt are rolled back; after SIGKILL or power loss, rerun the script or use `--unload` to remove leftovers.
- `RATE` must be 8000–192000 Hz, `CHANNELS` 1–8, and `LOOP_LATENCY_MS` 0–1000; invalid settings are rejected before changing the audio setup.
- Works on both PulseAudio and PipeWire; it handles common `pactl` module-list formats used by the two.

### macOS: let the keyer do the mixing

macOS has no equivalent of a combine sink, so the keyer mixes instead:

```
brew install blackhole-2ch
CW_AUDIO_DEVICE="BlackHole 2ch" ./cwkeyer <device> mix-mic
```

Then choose **BlackHole 2ch** as the microphone in the voice application.
`CW_AUDIO_DEVICE` names the device that receives the mix and
`CW_MIX_MIC_DEVICE` names the microphone to capture (default: the system
input). Capturing needs the Microphone permission, which macOS attributes to
the terminal that launched the keyer.

`CW_MIX_MONITOR` names a device that plays the **sidetone alone**, so you can
hear what you are sending while the mixed stream goes to the application:

```
CW_AUDIO_DEVICE="BlackHole 2ch" CW_MIX_MONITOR="MacBook Air Speakers" \
  ./cwkeyer --setup mix-mic
```

It works without `mix-mic` too, which is the simple answer to "send the tone to
BlackHole but let me hear it". The monitor runs about one device buffer behind
the output, and you do not hear your own microphone.

Two things about levels. The sidetone is the same 70% of full scale that a
local sidetone uses, which is loud as a microphone feed and can make small
speakers rasp when you listen back; `CW_KEYER_LEVEL=40` is a better starting
point when mixing. And the microphone is ducked by the sidetone's
instantaneous level rather than the sum being clipped, so the tone stays a
clean sine and the mix cannot overload; between elements the microphone is
untouched.
