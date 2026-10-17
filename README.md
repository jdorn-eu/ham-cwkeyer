# ham-cwkeyer

Low-latency little C utility to be used with HRS (Ham Radio Solution)'s VBand adapter working as an iambic keyer in software.
## Design description
- Reads evdev (/dev/input/eventX) input from adapter or alternatively keyboard input
- Runs an iambic mode B keyer (Curtis-8044 style, with paddle memory)
- Renders each mark and its raised-cosine attack/release at PCM sample boundaries; late input/audio scheduling may still lengthen gaps
- Sends the sine wave to ALSA's `pulse` device by default, with `PULSE_SINK=cw_out` for the mic-mixing setup

### Keying and audio timing

The evdev thread tracks paddle state, including short presses and recovery from dropped input events. The keyer thread makes iambic Mode B decisions and watches the paddles through each mark and its following one-dit space, using monotonic deadlines for the nominal 1:3 dit/dah timing. With `decode` off, it does no Morse decoding or terminal output.

For each element, the keyer queues its start time and duration. The audio thread converts that duration to frames at the **sample rate ALSA actually selected**, then generates the entire mark from sample counts rather than polling a key-down flag. Its raised-cosine attack and release fit *inside* the mark: normally 5 ms each, reduced to at most half a dit at high speeds. The tone defaults to 70% of the 16-bit PCM range. The audio renderer enforces at least a one-dit silent gap; late delivery cannot shorten a mark, but scheduling delays or ALSA underruns can make a gap longer. ALSA buffering also adds audible latency, which is separate from the generated mark length.

The mark queue is bounded. If audio falls far enough behind to fill it, the program stops with an error instead of silently losing elements. `make test` checks decoding, sample-level envelope/spacing across PCM buffer boundaries, late marks, and the high-speed ramp; it does not test the evdev device or your audio routing. A live paddle/listening check is still useful after changing timing or ALSA settings.

## Build
```
sudo dnf install gcc alsa-lib-devel
make
make test   # hardware-independent decoder and PCM timing checks
```

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

While running, terminal echo is disabled so keystrokes don't print in the window (the keyer reads evdev, not the tty); the previous terminal state is restored on exit. With `decode`, each keyed symbol is printed as text as you key, without newlines; unknown symbols are ignored. Pausing for more than seven dits between characters (the ITU word gap) prints a single space, so word breaks you key by hand appear as spaces.

## Environment
- `CW_ALSA_DEVICE` ALSA PCM to open. Default: `pulse`.
- `CW_ALSA_LATENCY_US` Requested ALSA latency in microseconds. Default: `20000` (20 ms). ALSA may negotiate a different buffer/period; the achieved values are printed at startup. Smaller buffers may reduce latency but increase underrun risk.
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
- The script is idempotent: re-running it is safe, leftover modules from a previous (or crashed) run are unloaded first.
- Works on both PulseAudio and PipeWire; it handles the different `pactl` module list formats of the two.
