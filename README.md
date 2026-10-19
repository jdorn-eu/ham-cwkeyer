# ham-cwkeyer

[![CI](https://github.com/jdorn-eu/ham-cwkeyer/actions/workflows/ci.yml/badge.svg)](https://github.com/jdorn-eu/ham-cwkeyer/actions/workflows/ci.yml)

Low-latency little C utility to be used with HRS (Ham Radio Solution)'s VBand adapter working as an iambic keyer in software.
## Design description
- Reads evdev (/dev/input/eventX) input from adapter or alternatively keyboard input
- Runs a Curtis 8044 type B iambic keyer (the Accu-keyer behaviour) with dot/dash paddle memory
- Runtime speed, pitch, and sidetone level controls from stdin while keying
- Renders each mark and its raised-cosine attack/release at PCM sample boundaries; late input/audio scheduling may still lengthen gaps
- Sends the sine wave to ALSA's `pulse` device by default, with `PULSE_SINK=cw_out` for the mic-mixing setup

### Keying and audio timing

The evdev thread tracks paddle state (including presses that land between keyer samples) and recovers from dropped input events. The keyer thread runs a small state machine and watches the paddles through each mark and its following one-dit space, using monotonic deadlines for the nominal 1:3 dit/dah timing. With `decode` off, it does no Morse decoding or terminal output.

Paddle inputs are filtered with a digital approximation of the 8044's external RC network: the recommended 1 MΩ/0.01 µF network has a time constant of about 10 ms. The filter runs in the keyer's own time base, so contact bounce is smoothed and normally cannot inject extra elements. `CW_KEYER_DEBOUNCE_MS` sets the RC time constant (default 10 ms; 0 disables the filter). Lower it, for example to 5 ms, for a snappier response at high speed. As with the real RC network, a legitimate press shorter than the filter's threshold time may not be recognized; at high speeds use a shorter setting or 0.

The keyer implements the type B (Accu-keyer) behaviour that most transceivers mean by "Curtis mode B". The dot and dash latches are set whenever their paddle closes and cleared at the end of that element if the paddle is open. Because the latches drive element selection, releasing a squeeze leaves the opposite latch set, so one extra element is keyed: squeeze and release during the second dash and you get `-.-.` (C). A held paddle repeats its element, a held squeeze alternates, and a tap of the opposite paddle during an element is remembered (the "N"/"A" tests). When both levers close within the same sample, the keyer starts with whichever was closed first (the evdev layer preserves the press order). Type A is deliberately out of scope for this project.

The `straight` startup option turns the iambic core off and passes either paddle (or both, squeezed) through as a manual key: the sidetone follows the contact with the same raised-cosine ramp, and the RC paddle filter still applies. This is the software equivalent of the 8044ABM's separate `MAN. KEY` input. `decode` is ignored in this mode.

Optional weighting matches the 8044ABM's weight control, which the application note describes as stretching each element "by a constant amount regardless of what speed is being used". `CW_KEYER_WEIGHT_MS` (default `0`, range `-25`–`25`) adds that constant to every mark and removes it from the following space, so the element period and the character speed are unchanged: a `+10` setting makes marks heavier and spaces lighter, `-10` does the reverse. Because it is a fixed time, not a ratio, its relative effect grows with speed — exactly as the chip's RC network behaves — so a setting that sounds right at one speed may not at another. `0` is strict ITU timing. The supported speed range is 1–40 WPM: even at 40 WPM a dit lasts 30 ms, leaving at least 5 ms for both the mark and space at the maximum ±25 ms weighting. Faster internal renderer stress tests are not a promise that the 10 ms input filter can capture very short high-speed taps.

For each element, the keyer queues its start time and duration. The audio thread converts that duration to frames at the **sample rate ALSA actually selected**, then generates the entire mark from sample counts rather than polling a key-down flag. Its raised-cosine attack and release fit *inside* the mark: normally 5 ms each, reduced to at most half a dit at high speeds. The tone defaults to 70% of the 16-bit PCM range and can be changed at startup with `CW_KEYER_LEVEL` or while running (see below). The audio renderer enforces the intended inter-element gap (one dit, reduced or extended by the weighting setting); late delivery cannot shorten a mark, but scheduling delays or ALSA underruns can make a gap longer. ALSA buffering also adds audible latency, which is separate from the generated mark length.

The mark queue is bounded. If audio falls far enough behind to fill it, the program stops with an error instead of silently losing elements.

Audio health is reported at exit: ALSA underruns are counted (and the first few logged as they happen), and the keyer counts elements that started more than 1 ms later than their nominal mark + space. If either is non-zero, a summary line with the count and the worst delay is printed when the keyer stops; a clean run stays silent.

## Tests

The build is deliberately strict by default: `-Werror` plus a broad warning set (including `-Wconversion`, `-Wsign-conversion`, `-Wshadow`, `-Wcast-qual`, and `-Wdouble-promotion`). Both GCC and clang are supported; the Makefile adapts the warning flags to the compiler (`make CC=clang`). Pass `make WERROR=` to build despite warnings on an unknown toolchain. `make analyze` runs GCC's `-fanalyzer`.

`make test` builds and runs the hardware-independent suites below; it does not touch your input device, audio routing, or sound server:

- `tests/test_keyer.c` — the type B iambic core (held-paddle repeats, alternating squeezes, the N/A memory cases, and the `C` extra-element rule), the RC paddle filter (glitches rejected, bounce smoothed, threshold delay), evdev event parsing (press/release, autorepeat ignored, `SYN_DROPPED` resync), the decode display (character and word gaps, unknown symbols dropped), the PCM renderer (envelope, spacing across buffer boundaries, late marks, high-speed ramp, level scaling), and a short run of the real `keyer_thread` to check mark sequencing.
- `tests/test_signals.c` — `SIGPIPE` ignored, a stop signal stops the loops, terminal disable/restore is idempotent on a pty, queued tty input is discarded on restore, a spurious `SIGCONT` does not re-disable echo, and `SIGTSTP`/`SIGCONT` genuinely stop and resume through the signal-waiting thread.
- `tests/test_audio.c` — the audio layer against ALSA's built-in `null` PCM (no sound server or hardware): `open_audio()` opens and negotiates a rate, `pcm_write_all()` writes generated periods, `audio_loop()` runs in a thread and stops cleanly, and a missing device and an at-Nyquist tone are both rejected.
- `tests/test_evdev.c` — `evdev_thread()` fed through a FIFO standing in for the input device: press/release update the paddle state, the press edge is latched, and state is cleared on exit.
- `tests/test_mic_mix.sh` — `cw-mic-mix.sh` against a mocked `pactl`: setup, idempotent reruns, `--unload`, rollback on a failed module load, and argument validation.

The same suite also covers argument parsing (`parse_double_arg`, `parse_uint_env`, `lookup_key`), input-device classification for `--list-devices`, paddle tap detection, setup prompts, runtime control keys, and a real `main()` startup/failure/shutdown path.

There is also an optional differential test against the real UnixCW libcw keyer: `make test-libcw` drives libcw's iambic graph and our mode B core with identical random paddle schedules and compares the element sequences (200k trials, no mismatches). It needs a built unixcw checkout; set `LIBCW_DIR` if it is not at `../unixcw-code`. It is not part of `make test` or CI because of that external dependency.

`make test-differential` is a self-contained differential test against eight third-party iambic keyer variants (seven projects: recri/keyer's vk6ph, ad5dz, dttsp, k1el and nd7pa, plus cw-dojo and morse-flipper; dttsp is tested in two configurations). Each algorithm is transcribed into the test, so it needs no external checkout; it checks the published `N`/`A`/`C` reference cases, 20,000 random paddle schedules, and a realistic operator gesture for every character in the decode table (with and without human timing jitter). It is kept out of `make test` because it takes a few seconds. See `docs/validation.md` for the results and for the argument that the type B core matches the 8044ABM specification.

A live paddle/listening check is still useful after changing timing, debouncing, or ALSA settings.

`make coverage` (requires `gcovr`) runs the C suites with `--coverage` and writes an HTML report to `coverage/index.html` plus a Cobertura `coverage/coverage.xml`. Coverage is reported for `cwkeyer.c` only and sits around 78%; the remainder is mostly error paths that are hard to trigger (thread-creation failure, ALSA recovery, evdev read errors) and the usage text, so treat it as a map of what to test next rather than a target.

CI (`.github/workflows/ci.yml`) runs, on every push and pull request: the strict GCC build, `make test`, `make analyze`, and `shellcheck`; a clang build and test; a sanitizer job that rebuilds and runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer; and a coverage job that uploads the report as an artifact.

## License

GPL-3.0-or-later; see `LICENSE`.

Mode behaviour follows the primary sources: the Curtis Electro Devices *8044 Series Keyer-on-a-Chip Application Note* and Karl Fischer (DJ5IL), *All About Squeeze-Keying*. The latter distinguishes the true type A edge-memory rule from the Accu-keyer type B latch rule; this project implements and verifies only the Accu-keyer/type B behaviour, against UnixCW libcw on 200k random paddle schedules and eight third-party keyer variants (see `docs/validation.md`).

## Build
```
sudo dnf install gcc alsa-lib-devel
make        # strict build (-Werror plus a broad warning set)
make test    # iambic, RC filter, evdev, decode, driver, signal, and script checks
make analyze # GCC -fanalyzer
make coverage # gcovr HTML/XML report in coverage/
```

## Install

```
make install                        # /usr/local by default
make install PREFIX=/usr            # or another prefix
make install DESTDIR=/tmp/stage PREFIX=/usr   # for packagers
```

This installs `cwkeyer`, the `cw-mic-mix` helper, and the `cwkeyer(1)` and `cw-mic-mix(1)` man pages. `make uninstall` removes them. `make dist` produces a `ham-cwkeyer-<version>.tar.gz` from the current commit; the version comes from `git describe` (a `-dirty` suffix is stripped for the tarball), or override it with `make VERSION=1.2.3`. Run `cwkeyer --version` to see the version a binary was built with.

In order for your user session to be able to access the adapter input, set proper udev rules:
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

## Run
First check for the correct device: `sudo libinput list-devices` or `sudo evtest` are your friend. If you want to use your keyboard, that's usually `/dev/input/event3`. In that case, add the `nograb` parameter so you can still send SIGINT to close the program.
```
./cwkeyer --setup                              # tap your paddles to pick the device
./cwkeyer /dev/input/eventX                    # 12 wpm, 800 Hz
./cwkeyer /dev/input/eventX 25 700             # 25 wpm, 700 Hz
./cwkeyer /dev/input/eventX 25 700 COMMA DOT   # remap dit/dah keys
./cwkeyer /dev/input/event3 nograb             # keyboard instead of HRS vband adapter
./cwkeyer /dev/input/event3 nograb decode      # also print keyed morse as text
./cwkeyer /dev/input/eventX straight            # paddles act as a straight key
```
If you do not know which device is your adapter, `./cwkeyer --list-devices`
groups the readable `/dev/input/event*` devices into `unknown`, `keyboard`,
and `paddle` categories in that order, so paddles appear last and stay near
the shell prompt. Each group is sorted by device number, and devices that
report the default LEFTCTRL/RIGHTCTRL paddle keys are marked. The category
names are colored when stdout is a terminal; set `NO_COLOR` to turn that
off.
`./cwkeyer --setup` is the guided version: it asks you to tap the dot lever
and then the dash lever, learns the device and both key codes, and starts
with the defaults. It does not need the `/dev/input/eventX` name. A device
that looks like a normal keyboard defaults to `nograb`; a dedicated paddle
defaults to exclusive grab. If both taps use the same key, it offers
straight-key mode. Speed, pitch, level, and weighting stay at their defaults
and remain adjustable while running.
The default speed, pitch, PCM peak level, and 5 ms raised-cosine slopes match UnixCW's `cw` defaults: 12 WPM, 800 Hz, and 70% of full scale. Paddle defaults are left ctrl (dit) and right ctrl (dah). Running `./cwkeyer` with no arguments or `./cwkeyer --help` lists the known key names; raw evdev key codes work too.

While running, terminal echo is disabled so keystrokes don't print in the window (the keyer reads evdev, not the tty). Terminal input queued while the keyer runs is discarded on exit, so keystrokes do not reappear at the shell prompt. Ctrl+C, SIGTERM, SIGHUP, and SIGQUIT stop it cleanly and restore the terminal and the evdev grab. Ctrl+Z suspends it properly: a dedicated thread releases the terminal and grab before stopping and re-establishes them on resume, so the shell stays usable while it is suspended. Ordinary stop/suspend operations run outside asynchronous signal handlers and are serialized with device and terminal startup/shutdown. A fatal crash (for example SIGSEGV) still takes the kernel's default action; terminal restoration cannot be guaranteed after a crash. If needed, run `stty sane` to repair the tty.

With `decode`, each keyed symbol is printed as text as you key, without newlines; unknown symbols are ignored. Pausing for more than seven dits between characters (the ITU word gap) prints a single space, so word breaks you key by hand appear as spaces. Decoder output is written synchronously by the keyer thread, so a blocked stdout consumer can delay paddle processing; keep the output destination draining when using `decode`.

## Runtime controls

If stdin is a readable terminal or pipe, the keyer accepts single-key controls while it is running:

- `[` / `]` — slower / faster (1 WPM per step, limited to 1–40 WPM)
- `-` / `=` (or `+`) — lower / higher pitch (10 Hz per step)
- `9` / `0` — quieter / louder (5% per step)
- `,` / `.` — lighter / heavier weighting (1 ms per step)

A pipe's final controls are read even if its writer has already closed. Speed, pitch, level, and weighting changes take effect on the next element. The startup values come from the command line, `CW_KEYER_LEVEL`, and the usual environment variables. These control keys are read from stdin, not from the evdev paddle device, so avoid remapping a paddle to one of them.

## Environment
- `CW_ALSA_DEVICE` ALSA PCM to open. Default: `pulse`. With this default, an unset or empty `PULSE_SINK` is set to `cw_out` by the keyer.
- `CW_ALSA_LATENCY_US` Requested ALSA latency in microseconds. Default: `20000` (20 ms). ALSA may negotiate a different buffer/period; the achieved values are printed at startup. Smaller buffers may reduce latency but increase underrun risk.
- `CW_KEYER_LEVEL` Sidetone level, percent of full scale, `0`–`100`. Default: `70`. The level can also be changed while running with `9` and `0`.
- `CW_KEYER_WEIGHT_MS` Weighting, `-25`–`25` ms. Default: `0` (strict ITU timing). A constant time added to each mark and removed from the following space. The weighting can also be changed while running with `,` and `.`.
- `CW_KEYER_DEBOUNCE_MS` Paddle RC filter time constant in milliseconds, `0`–`50`. Default: `10`; `0` disables the filter. The 8044's recommended 1 MΩ/0.01 µF network is about 10 ms. A press shorter than the filter's threshold time may not be recognized; lower the value for high-speed sending.
- `PULSE_SINK` Pulse sink the keyer audio goes to. If the device is `pulse` and this is unset or empty, it defaults to `cw_out`, the combined sink created by `cw-mic-mix.sh`. If you haven't run that script, set it explicitly to a real sink, e.g. `PULSE_SINK=$(pactl get-default-sink)`.
- `NO_COLOR` If set, `--list-devices` does not color the category names.

## Combine with mic input

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
