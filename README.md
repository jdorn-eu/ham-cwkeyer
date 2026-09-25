# ham-cwkeyer

Low-latency little C utility to be used with HRS (Ham Radio Solution)'s VBand adapter working as a iambic keyer in software.
## Design description
- Reads evdev (/dev/input/eventX) input from adapter
- Generates a sine wave pattern
- Ultimately feeds the sine wave to ALSA

## Build
```
sudo dnf install gcc alsa-lib-devel
gcc -O2 -o cwkeyer cwkeyer.c -lasound -lpthread -lm
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
First check for the correct device: `sudo libinput list-devices` or `sudo evtest` are your friend.
```
./cwkeyer /dev/input/eventX          # 20 wpm, 600 Hz
./cwkeyer /dev/input/eventX 25 700   # 25 wpm, 700 Hz
```

## Combine with mic input

```
./cw-mic-mix.sh                                   # auto-detects default mic + output
PULSE_SINK=cw_out ./cwkeyer /dev/input/eventX
```
Then in an app, set the microphone/input device to **Monitor of CW_Mic_Mix**.

If you want different mic/output devices than your system defaults: `./cw-mic-mix.sh alsa_input.usb-... alsa_output.pci-...` (list names with `pactl list short sources` / `sinks`).

