# ham-cwkeyer

[![CI](https://github.com/jdorn-eu/ham-cwkeyer/actions/workflows/ci.yml/badge.svg)](https://github.com/jdorn-eu/ham-cwkeyer/actions/workflows/ci.yml)

Low-latency little C utility to be used with HRS (Ham Radio Solution)'s VBand adapter working as an iambic keyer in software.
## Design description
- Reads evdev (/dev/input/eventX) input from adapter or alternatively keyboard input
- Runs a Curtis 8044 iambic keyer, selectable mode A or B, with dot/dash paddle memory
- Renders each mark and its raised-cosine attack/release at PCM sample boundaries; late input/audio scheduling may still lengthen gaps
- Sends the sine wave to ALSA's `pulse` device by default, with `PULSE_SINK=cw_out` for the mic-mixing setup

### Keying and audio timing

The evdev thread tracks paddle state, including short presses and recovery from dropped input events. The keyer thread runs a small state machine and watches the paddles through each mark and its following one-dit space, using monotonic deadlines for the nominal 1:3 dit/dah timing. With `decode` off, it does no Morse decoding or terminal output.

Paddle inputs are debounced the way the 8044's RC input filter does it: a level change is accepted only after it has been stable for the hold time (`CW_KEYER_DEBOUNCE_MS`, default 5 ms). Contact bounce and glitches shorter than that are discarded, so they cannot inject extra elements. A real press shorter than the hold time is discarded too; at the slowest intended speeds a real tap is far longer than 5 ms. Set it to 0 to disable debouncing, or raise it for a noisy paddle (the chip's 1M/0.01µF filter is roughly 10 ms).

Both original Curtis keyer behaviours are implemented; `CW_KEYER_MODE` selects one:

- **Mode B** (default; the Accu-keyer logic that the 8044ABM labelled "B"): the dot and dash memories are set whenever their paddle closes and cleared at the end of that element if the paddle is open. Because the memories drive element selection, releasing a squeeze leaves the opposite memory set, so one extra element is keyed. Squeeze and release during the second dash and you get `-.-.` (C).
- **Mode A** (the original Curtis/8044 logic): plain iambic selection from the paddle *levels*, plus a memory set only by a fresh press of the opposite paddle during an element. The same squeeze released during the second dash stops at `-.-` (K).

In both modes a held paddle repeats its element and a held squeeze alternates, and a tap of the opposite paddle during an element is remembered (the "N"/"A" tests). Note that this is *not* the same as UnixCW's libcw keyer: libcw's default is effectively type B, but it labels it mode A and its `curtis_mode_b` option adds a spurious extra element.

For each element, the keyer queues its start time and duration. The audio thread converts that duration to frames at the **sample rate ALSA actually selected**, then generates the entire mark from sample counts rather than polling a key-down flag. Its raised-cosine attack and release fit *inside* the mark: normally 5 ms each, reduced to at most half a dit at high speeds. The tone defaults to 70% of the 16-bit PCM range. The audio renderer enforces at least a one-dit silent gap; late delivery cannot shorten a mark, but scheduling delays or ALSA underruns can make a gap longer. ALSA buffering also adds audible latency, which is separate from the generated mark length.

The mark queue is bounded. If audio falls far enough behind to fill it, the program stops with an error instead of silently losing elements.

## Tests

The build is deliberately strict by default: `-Werror` plus a broad warning set (including `-Wconversion`, `-Wsign-conversion`, `-Wshadow`, `-Wcast-qual`, and `-Wdouble-promotion`). Pass `make WERROR=` to build despite warnings on an unknown toolchain. `make analyze` runs GCC's `-fanalyzer`.

`make test` builds and runs three hardware-independent suites; it does not touch your input device, audio routing, or sound server:

- `tests/test_keyer.c` — the iambic core (the N and A memory tests, the mode A/mode B `K`/`C` difference, held-paddle repeats, alternating squeezes), paddle debouncing (glitches rejected, bounce collapsed to one transition), evdev event parsing (press/release, autorepeat ignored, `SYN_DROPPED` resync), the decode display (character and word gaps, unknown symbols dropped), the PCM renderer (envelope, spacing across buffer boundaries, late marks, high-speed ramp), and a short run of the real `keyer_thread` to check mark sequencing.
- `tests/test_signals.c` — `SIGPIPE` ignored, a stop signal stops the loops, terminal disable/restore is idempotent on a pty, and `SIGTSTP`/`SIGCONT` genuinely stop and resume.
- `tests/test_mic_mix.sh` — `cw-mic-mix.sh` against a mocked `pactl`: setup, idempotent reruns, `--unload`, rollback on a failed module load, and argument validation.

A live paddle/listening check is still useful after changing timing, debouncing, or ALSA settings.

CI (`.github/workflows/ci.yml`) runs the strict build, `make test`, `make analyze`, and `shellcheck`, plus a second job that rebuilds and runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer, on every push and pull request.

## License

GPL-3.0-or-later; see `LICENSE`.

Mode behaviour follows the primary sources: the Curtis Electro Devices *8044 Series Keyer-on-a-Chip Application Note* and Karl Fischer (DJ5IL), *All About Squeeze-Keying*. The latter distinguishes the true type A edge-memory rule from the Accu-keyer type B latch rule in detail; only the Accu-keyer/type B behaviour is verified here against a second implementation (UnixCW libcw), which matches on 200k random paddle schedules.

## Build
```
sudo dnf install gcc alsa-lib-devel
make        # strict build (-Werror plus a broad warning set)
make test    # iambic, debounce, evdev, decode, driver, signal, and script checks
make analyze # GCC -fanalyzer
make coverage # gcovr HTML/XML report in coverage/
```

## Install

```
make install                        # /usr/local by default
make install PREFIX=/usr            # or another prefix
make install DESTDIR=/tmp/stage PREFIX=/usr   # for packagers
```

This installs `cwkeyer`, the `cw-mic-mix` helper, and the `cwkeyer(1)` man page. `make uninstall` removes them. `make dist` produces a `ham-cwkeyer-<version>.tar.gz` from the current commit; the version comes from `git describe`, or override it with `make VERSION=1.2.3`. Run `cwkeyer --version` to see the version a binary was built with.

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
./cwkeyer /dev/input/eventX                    # 12 wpm, 800 Hz
./cwkeyer /dev/input/eventX 25 700             # 25 wpm, 700 Hz
./cwkeyer /dev/input/eventX 25 700 COMMA DOT   # remap dit/dah keys
./cwkeyer /dev/input/event3 nograb             # keyboard instead of HRS vband adapter
./cwkeyer /dev/input/event3 nograb decode      # also print keyed morse as text
```
The default speed, pitch, PCM peak level, and 5 ms raised-cosine slopes match UnixCW's `cw` defaults: 12 WPM, 800 Hz, and 70% of full scale. Paddle defaults are left ctrl (dit) and right ctrl (dah). Running `./cwkeyer` with no arguments or `./cwkeyer --help` lists the known key names; raw evdev key codes work too.

While running, terminal echo is disabled so keystrokes don't print in the window (the keyer reads evdev, not the tty). Ctrl+C, SIGTERM, SIGHUP, and SIGQUIT stop it cleanly and restore the terminal and the evdev grab. Ctrl+Z suspends it properly: the terminal and grab are released before stopping and re-established on resume, so the shell stays usable while it is suspended. Fatal crashes also restore the terminal on a best-effort basis before re-raising the signal.

With `decode`, each keyed symbol is printed as text as you key, without newlines; unknown symbols are ignored. Pausing for more than seven dits between characters (the ITU word gap) prints a single space, so word breaks you key by hand appear as spaces. Decoder output is written synchronously by the keyer thread, so a blocked stdout consumer can delay paddle processing; keep the output destination draining when using `decode`.

## Environment
- `CW_ALSA_DEVICE` ALSA PCM to open. Default: `pulse`. With this default, an unset or empty `PULSE_SINK` is set to `cw_out` by the keyer.
- `CW_ALSA_LATENCY_US` Requested ALSA latency in microseconds. Default: `20000` (20 ms). ALSA may negotiate a different buffer/period; the achieved values are printed at startup. Smaller buffers may reduce latency but increase underrun risk.
- `CW_KEYER_MODE` Curtis iambic mode, `a` or `b` (case-insensitive; also accepts `mode_a`/`mode_b`). Default: `b`. The mode is printed at startup.
- `CW_KEYER_DEBOUNCE_MS` Paddle debounce hold time in milliseconds, `0`–`50`. Default: `5`; `0` disables debouncing. Any level change shorter than the hold time is treated as contact bounce and discarded.
- `PULSE_SINK` Pulse sink the keyer audio goes to. If the device is `pulse` and this is unset or empty, it defaults to `cw_out`, the combined sink created by `cw-mic-mix.sh`. If you haven't run that script, set it explicitly to a real sink, e.g. `PULSE_SINK=$(pactl get-default-sink)`.

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
