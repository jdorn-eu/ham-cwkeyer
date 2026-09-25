#!/usr/bin/env bash
# Sets up a virtual "CW_Mic_Mix" device that combines your real microphone
# with the CW sidetone, so apps like Discord/Mumble can use it as their mic.

set -euo pipefail

MIC_SOURCE=${1:-$(pactl get-default-source)}
OUT_SINK=${2:-$(pactl get-default-sink)}

RATE=${RATE:-48000}
CHANNELS=${CHANNELS:-2}
LOOP_LATENCY_MS=${LOOP_LATENCY_MS:-15}

echo "Using mic source:      $MIC_SOURCE"
echo "Using headphone sink:  $OUT_SINK"
echo "Rate:                  $RATE"
echo "Channels:              $CHANNELS"
echo "Loopback latency:      ${LOOP_LATENCY_MS} ms"

# 1. A null sink that becomes the mixed mic feed. Its monitor is what
#    Discord/Mumble/Audacity will see as an input device.
pactl load-module module-null-sink \
  sink_name=cw_mic_mix \
  rate="$RATE" \
  channels="$CHANNELS" \
  sink_properties=device.description=CW_Mic_Mix

# 2. Loop your real mic into it.
pactl load-module module-loopback \
  source="$MIC_SOURCE" \
  sink=cw_mic_mix \
  latency_msec="$LOOP_LATENCY_MS"

# 3. A combine-sink so the CW tone plays to both the mix and your headphones.
pactl load-module module-combine-sink \
  sink_name=cw_out \
  slaves=cw_mic_mix,"$OUT_SINK" \
  rate="$RATE" \
  channels="$CHANNELS" \
  sink_properties=device.description=CW_Output_To_Mix_And_Headphones

echo
echo "Done. Now:"
echo "  1. In Discord/Mumble/Audacity, set input device to: Monitor of CW_Mic_Mix"
echo "  2. Run:"
echo "       PULSE_SINK=cw_out CW_ALSA_DEVICE=pulse CW_ALSA_LATENCY_US=20000 ./cwkeyer /dev/input/eventX"
echo
echo "To undo:"
echo "  pactl list modules short"
echo "  pactl unload-module <id>"
