# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Added

- `CW_MIX_MONITOR` on macOS: play the sidetone alone on another device while
  the mixed stream goes to a voice application, which is what makes `mix-mic`
  usable - without somewhere to hear it, sending everything to BlackHole means
  sending blind. It works without `mix-mic` as well, for the simpler case of
  routing the tone to a virtual device and monitoring locally. The sidetone
  crosses a ring like the microphone does, so the two units can run on their
  own clocks.
- macOS support: `input_hid.c` and `audio_coreaudio.c` implement the backend
  contract with IOKit HID and CoreAudio. A paddle adapter is a plain USB HID
  keyboard there, so no driver is needed and the default
  LEFTCTRL/RIGHTCTRL paddle mapping is unchanged. `--list-devices` and
  `--setup` work as on Linux; devices are named `vendor:product:location`.
  Exclusive access uses `kIOHIDOptionsTypeSeizeDevice` and needs root, so
  losing it is a warning rather than an error; reading keyboard HID values
  needs the Input Monitoring permission. Audio goes through a HALOutput
  AudioUnit whose render callback feeds the same `generate_audio()`, at the
  device buffer size (512 frames, 10.7 ms on the built-in speakers) with
  about 12 ms measured output latency. A dropout is detected from a jump in
  the unit's sample timeline and counted like an ALSA underrun.
- Known paddle adapters are recognized by USB vendor/product ID, starting
  with the HRS PaddleAdapter. The table in `cwkeyer.c` stores the default
  dot/dash key codes and is a one-line addition per device. A single known
  adapter skips the tap test and uses those codes; `--list-devices` shows
  the known label.
- `--setup`: interactively choose the paddle device by tapping the dot and
  dash levers, which also learns both key codes. Starts with the default
  speed, pitch, level, and weighting. A keyboard-like device defaults to
  `nograb`; a paddle defaults to exclusive grab. If both taps use the same
  key, the wizard offers straight-key mode.
- `--list-devices` (`-L`): list readable `/dev/input/event*` devices with a
  keyboard/paddle guess and a note when the default LEFTCTRL/RIGHTCTRL
  paddle keys are reported. Devices are grouped (unknown, keyboard, paddle)
  and sorted by event number within each group; the group names are colored
  on a terminal unless `NO_COLOR` is set. The guess is a hint for humans,
  not a device selection policy.
- `mix-mic` on macOS: the microphone is captured by a second audio unit and
  summed with the sidetone in the render callback, so one output device (for
  example BlackHole) carries both and a voice application can take the
  operator and the sidetone from a single input. `CW_AUDIO_DEVICE` says where
  the mix goes and `CW_MIX_MIC_DEVICE` which microphone to capture. The two
  units run on their own clocks, so captured samples cross a ring buffer:
  overflow drops the oldest, and a dry ring holds the last sample rather than
  stepping to zero. Missing microphone, missing permission or a refused seek
  all fall back to an ordinary sidetone with a warning. On Linux `mix-mic` is
  refused with a pointer to `cw-mic-mix.sh`.

### Changed

- Paddle polling now backs off while nothing is happening: 200 us at first,
  doubling to 2 ms (1 ms with the RC filter disabled) and resetting when a
  lever is seen. A measured idle run fell from about 5.4% to 0.4% of one core.
  Presses are latched by the input backend and held by the RC filter, so the
  backoff delays noticing a tap rather than losing it.
- Exclusive access now defaults to what the backend can actually take:
  `nograb` is still honoured, `--setup` still asks, but a direct run on macOS
  as a normal user no longer attempts a seize that is bound to fail (and warn)
  on every start. Linux keeps grabbing by default.
- The macOS device argument may stop at a field boundary: the last field is the
  USB port, so a command written against one port stopped working when the
  adapter was moved. `04d8:01a6` now selects the adapter in any port, and an
  ambiguous selector lists the devices it matched.
- The first render-callback transition is no longer reported as a dropout. A
  unit that has just started can re-anchor its sample timeline while it
  settles, which is not audio anyone could have heard.
- The command line is more forgiving about order and more direct about
  mistakes: options and keywords may appear anywhere, and `--setup` can be
  combined with the settings it should start with (`--setup 25 700 mix-mic`
  reads the speed, pitch, and key names instead of a device path). A bad
  argument now gets one line naming the problem plus a pointer to `--help`
  instead of the whole usage text, an argument starting with `-` is reported
  as an unknown option rather than being taken for a device path, and
  `--help` is written to standard output so it can be piped or paged.
- `--setup` and `--list-devices` now live in the portable core and are built
  on five backend primitives (`discover`, `close_devices`, `drain`,
  `wait_press`, `wait_release`), so the two platforms share one wizard and
  one listing instead of two copies. Device codes and the device view
  (`input_device_t`) moved with them; a backend now fills in `id`, `order`,
  `readable`, and an opaque `handle`.
- `--help` takes its device argument, examples, key-code note, and
  `--list-devices`/`--setup` paragraphs from the backend, so the macOS help
  no longer talks about evdev. The portable environment variables are now
  listed before the platform's, which moves the two `CW_ALSA_*` lines.
- `sleep_ns()` uses `nanosleep()` instead of the Linux-only
  `clock_nanosleep()`.
- The Makefile detects clang by asking the compiler rather than by looking at
  `$(CC)`, because `cc` is clang on macOS, and picks the Darwin frameworks.
- CI builds and tests macOS as well as Linux.
- The startup banner says "grab requested" rather than claiming the grab
  succeeded, since the attempt happens when the device opens; a backend that
  cannot seize as this user now says so through the new
  `input_backend_grab_possible()`, which makes `--setup` default to `nograb`
  on macOS unless it is running as root.
- `make analyze` uses clang's analyzer when the compiler is clang; `make
  install` installs `cw-mic-mix` and its man page only on Linux, since it
  drives `pactl`; the man page marks the `CW_ALSA_*` and `PULSE_SINK`
  variables as Linux-only and documents `CW_AUDIO_DEVICE`.
- Split the single source file into a portable core (`cwkeyer.c`) and a pair
  of Linux backends (`input_evdev.c`, `audio_alsa.c`), joined at the end of
  `cwkeyer.c` and described by `platform.h`. The core no longer includes
  `linux/input.h` or `alsa/asoundlib.h`, and names keys through the
  portable `CWKEY_*` namespace. Behaviour, messages, and timing are
  unchanged; this is groundwork for a macOS backend. Internal entry points
  were renamed accordingly: `evdev_thread` is now `input_backend_thread`,
  `run_setup_wizard` is `input_backend_setup`, `list_input_devices` is
  `input_backend_list`, and the audio loop is reached through
  `audio_backend_open/run/close`.
- Runtime control feedback is rewritten on a single status line on a
  terminal instead of appending a new line for every change.
- `cwkeyer` with no arguments on an interactive terminal now runs the same
  tap-to-select setup as `--setup`. Without a terminal it still prints the
  usage text, so scripts and CI are unaffected.
- The `--setup` wizard reports both taps in the same format and uses spacing
  and colors on a terminal; `NO_COLOR` disables the colors.
- Limit user-selectable keying speed to 1–40 WPM, including runtime controls.
  At 40 WPM the full ±25 ms weighting range retains positive mark and space
  durations; higher speeds previously broke the promised element period.

### Fixed

- The CoreAudio capture and monitor rings now have one owner per cursor: the
  writer never advances the reader's index, and the slots are atomic, so a
  ring that is lapped cannot race the reader or resurrect stale frames.  The
  render path also covers every frame of a buffer larger than the 4096-frame
  scratch, and refuses (and counts) an unexpected buffer list instead of
  writing into it.  The microphone unit is bounded to the scratch size so an
  oversized capture slice is counted rather than silently skipped.
- The shared setup wizard no longer names Linux devices in its failure messages.
  It uses the backend's own device noun and permission hint, so macOS says
  "HID keyboard" and points at Input Monitoring instead of udev.
- A bare Linux run with `CW_ALSA_DEVICE=pulse` and no `PULSE_SINK` now falls back
  to the system default sink when the `cw_out` sink from `cw-mic-mix.sh` is not
  available, instead of refusing to start. An explicitly set `PULSE_SINK` is
  still never replaced.
- Two defects on the macOS default-output path, found by running it: the
  playback unit asked for `kAudioOutputUnitProperty_EnableIO`, which
  `DefaultOutput` does not have, so every run without `CW_AUDIO_DEVICE`
  printed `warning: cannot enable audio output: -10879`. An output unit is
  output-only by default, so the call was both unnecessary and wrong for that
  subtype; only the capture unit enables anything. And the presentation-delay
  statistics counted the first callback after a `SIGTSTP`/`SIGCONT`, whose
  host time predates the stop, which reported a minimum latency of -1006 ms
  for a session of ordinary 12 ms latency. Implausible leads are now ignored.
- macOS crash (SIGBUS/SIGSEGV) on some runs: the setup wizard's HID input
  callback was registered with a pointer to a stack frame that had already
  returned, and `IOHIDDeviceRegisterInputValueCallback(NULL)` is not a
  documented unregister, so IOKit kept dispatching to it. The keyer's own
  thread shares the device, so the next key press called a callback whose
  context was freed. The backend now uses one callback for both phases with
  all of its state in a static session and a mode switch, and every IOKit
  callback is registered with a NULL context, so no registration can outlive
  what it points at. The crash report that identified it is in
  `cwkeyer-2026-10-20-205052.ips`: `IOHIDDeviceInputElementValueCallback` ->
  `CFSetApplyFunction` -> `watch_cb`, faulting on a freed context.
- `--setup` now recognizes the HRS PaddleAdapter by name even though it
  advertises a full keyboard keyset, so it is treated as a paddle and
  defaults to exclusive grab.
- `--setup` waits for the DOT lever to be released before accepting the DASH
  tap, so a held lever or its repeat events cannot be mistaken for the
  second lever.
- `--setup` no longer reports the tap timeout immediately because another
  input device (for example a mouse) had a pending non-key event. The wait
  now keeps polling until the deadline or a real key press.
- `--setup` ignores pointer and gamepad buttons (`BTN_*` codes), so a
  stray mouse click cannot be mistaken for a paddle tap.
- Consume pending runtime controls from a pipe before reacting to a
  simultaneous `POLLHUP` when its writer closes.
- Route ordinary stop, suspend, and resume signals through a `sigwait()`
  thread, synchronizing terminal and evdev grab operations with startup and
  shutdown. Non-async-signal-safe terminal ioctls no longer run inside
  handlers. Fatal crashes now use default signal disposition; restoring the
  terminal after a crash cannot be guaranteed.

## [1.0.1] - 2026-10-18

### Fixed

- Build the differential test with `-Wno-strict-overflow` on GCC.  Its
  level-4 `-Wstrict-overflow` reports false positives inside the verbatim
  third-party state-machine transcriptions, and the diagnostics move
  between GCC versions.  This only affected building the optional
  `make test-differential` target; the keyer and `make test` were
  unaffected.

## [1.0.0] - 2026-10-18

Initial release: a low-latency software CW sidetone keyer for HRS VBand
adapters (or a keyboard), implementing the Curtis 8044 type B
(Accu-keyer) iambic keyer.

### Keying

- Curtis 8044 type B (Accu-keyer) iambic core: dot/dash level memory,
  held-paddle repeat, held-squeeze alternation, the N/A memory cases, and
  the extra element on squeeze release (squeeze and release during the
  second dash gives `-.-.`).
- First-lever-first: when both levers close within the same sample, the
  element that was closed first goes out (the evdev layer preserves the
  press order).
- Digital model of the 8044's external RC paddle network (1 MΩ / 0.01 µF,
  about 10 ms by default; `CW_KEYER_DEBOUNCE_MS`).
- Optional 8044ABM weighting (`CW_KEYER_WEIGHT_MS`): a constant time added
  to every mark and removed from the following space, `-25`–`25` ms.
  `0` is strict ITU timing.
- Straight-key passthrough (`straight`): either paddle keys the sidetone.
- Type A is intentionally out of scope.

### Audio and input

- evdev input from an HRS VBand adapter or a keyboard, with `EVIOCGRAB`,
  edge latches for presses that land between samples, and `SYN_DROPPED`
  resync.
- ALSA sidetone with a raised-cosine attack/release rendered inside each
  mark at the sample rate ALSA actually negotiated, with a PulseAudio /
  PipeWire default sink (`cw-mic-mix.sh`).
- Runtime speed, pitch, level, and weighting controls read from stdin.
- Optional `decode` display of the keyed text.

### Robustness

- Async-signal-safe stop and suspend handling, terminal restore on fatal
  signals, and reporting of ALSA underruns and keying overruns.

### Testing and tooling

- Strict build (`-Werror` plus a broad warning set) for GCC and clang,
  `-fanalyzer`, Address/UndefinedBehavior sanitizers, and gcovr coverage in
  CI.
- Hardware-independent suites for the core, RC filter, evdev parsing,
  decode, PCM rendering, signals, and the mic-mix script.
- Optional differential test against UnixCW libcw (200 000 random
  schedules, no mismatches).
- Differential test against eight third-party iambic keyers: the published
  reference cases, random schedules, and realistic per-character gestures
  (including the K7QO hold-the-squeeze technique).
- `docs/validation.md` records why the core is believed to match the
  8044ABM type B behaviour.

[1.0.1]: https://github.com/jdorn-eu/ham-cwkeyer/releases/tag/v1.0.1
[1.0.0]: https://github.com/jdorn-eu/ham-cwkeyer/releases/tag/v1.0.0
