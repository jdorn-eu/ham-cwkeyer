# ham-cwkeyer

Low-latency little C utility to be used with HRS (Ham Radio Solution)'s VBand adapter working as a iambic keyer in software.
## Design description
- Reads evdev (/dev/input/eventX) input from adapter or alternatively keyboard input
- Runs an iambic mode B keyer (Curtis-8044 style, with paddle memory)
- Generates a sine wave pattern
- Ultimately feeds the sine wave to ALSA

## Build
```
sudo dnf install gcc alsa-lib-devel
gcc -O2 -Wall -Wextra -o cwkeyer cwkeyer.c -lasound -lpthread -lm
```

In order for your user session to able to access the adapter input, set proper udev rules:
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
./cwkeyer /dev/input/eventX                    # 20 wpm, 600 Hz
./cwkeyer /dev/input/eventX 25 700             # 25 wpm, 700 Hz
./cwkeyer /dev/input/eventX 25 700 COMMA DOT   # remap dit/dah keys
./cwkeyer /dev/input/event3 nograb             # keyboard instead of HRS vband adapter
```
Paddle defaults are left ctrl (dit) and right ctrl (dah). Running `./cwkeyer` with no arguments lists the known key names; raw evdev key codes work too.

## Environment
- `CW_ALSA_DEVICE` ALSA PCM to open. Default: `pulse`.
- `CW_ALSA_LATENCY_US` Requested ALSA latency in microseconds. Default: `20000`.
- `PULSE_SINK` Pulse sink the keyer audio goes to. If the device is `pulse` and this is unset, it defaults to `cw_out`, the combined sink created by `cw-mic-mix.sh`. If you haven't run that script, set it explicitly to a real sink, e.g. `PULSE_SINK=$(pactl get-default-sink)`.

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
