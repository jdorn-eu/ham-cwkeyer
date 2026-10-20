# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Changed

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

### Added

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

### Fixed

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

### Changed

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
