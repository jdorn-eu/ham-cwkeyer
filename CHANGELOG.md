# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[1.0.0]: https://github.com/jdorn-eu/ham-cwkeyer/releases/tag/v1.0.0
