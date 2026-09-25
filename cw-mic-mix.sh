#!/usr/bin/env bash
# Sets up a virtual "CW_Mic_Mix" device that combines your real microphone
# with the CW sidetone, so apps like Discord/Mumble can use it as their mic.
set -e

MIC_SOURCE=${1:-$(pactl get-default-source)}
OUT_SINK=${2:-$(pactl get-default-sink)}

echo "Using mic source:      $MIC_SOURCE"
echo "Using headphone sink:  $OUT_SINK"

# 1. A null sink that becomes the mixed mic feed. Its monitor is what
#    Discord/Mumble will see as an input device.
pactl load-module module-null-sink sink_name=cw_mic_mix \
    sink_properties=device.description=CW_Mic_Mix

# 2. Loop your real mic into it.
pactl load-module module-loopback source="$MIC_SOURCE" sink=cw_mic_mix \
    latency_msec=1

# 3. A combine-sink so the CW tone plays to both the mix (so it reaches
#    Discord/Mumble) and your headphones (so you can hear yourself keying).
pactl load-module module-combine-sink sink_name=cw_out \
    slaves=cw_mic_mix,"$OUT_SINK"

echo
echo "Done. Now:"
echo "  1. In Discord/Mumble, set the input device to: Monitor of CW_Mic_Mix"
echo "  2. Run the keyer with:  PULSE_SINK=cw_out ./cwkeyer /dev/input/eventX"
echo
echo "To undo: pactl list modules short   (find the three module ids you just loaded, then pactl unload-module <id>)"
