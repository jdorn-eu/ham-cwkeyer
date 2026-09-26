#!/usr/bin/env bash
# Sets up a virtual "CW_Mic_Mix" device that combines your real microphone
# with the CW sidetone, so apps like Discord/Mumble can use it as their mic.
#
# Safe to re-run: leftover modules from a previous run are unloaded first.
# Use ./cw-mic-mix.sh --unload to tear everything down.

set -euo pipefail

command -v pactl >/dev/null || {
    echo "error: pactl not found (need PulseAudio or PipeWire)" >&2
    exit 1
}

RATE=${RATE:-48000}
CHANNELS=${CHANNELS:-2}
LOOP_LATENCY_MS=${LOOP_LATENCY_MS:-15}

# Unload all modules of $1 whose args match *$2*.
unload_modules_matching() {
    local line name rest
    local -a f
    while IFS= read -r line; do
        read -r -a f <<< "$line"
        # PulseAudio pactl prints: index numbering refcount name args...
        # PipeWire pw-pactl prints:  index name args...
        # (pw-pactl can also emit multi-line args; skip any line that has
        # no module name in one of those positions.)
        [[ ${f[1]-} == module-* || ${f[3]-} == module-* ]] || continue

        if [[ ${f[1]} == module-* ]]; then
            name=${f[1]}
            rest=${f[*]:2}
        else
            name=${f[3]}
            rest=${f[*]:4}
        fi

        if [[ $name == "$1" && $rest == *"$2"* ]]; then
            echo "Unloading stale module ${f[0]} ($name)"
            pactl unload-module "${f[0]}"
        fi
    done < <(pactl list modules short)
    return 0
}

unload_cw_modules() {
    # Order matters: loopback first, then the combine sink, then the
    # null sink they feed.
    unload_modules_matching module-loopback "sink=cw_mic_mix"
    unload_modules_matching module-combine-sink "sink_name=cw_out"
    unload_modules_matching module-null-sink "sink_name=cw_mic_mix"
}

if [[ ${1:-} == "--unload" ]]; then
    echo "Unloading CW mix modules"
    unload_cw_modules
    exit 0
fi

MIC_SOURCE=${1:-$(pactl get-default-source)}
OUT_SINK=${2:-$(pactl get-default-sink)}

[[ -n $MIC_SOURCE ]] || {
    echo "error: no default source; pass the mic as the first argument:" >&2
    pactl list short sources >&2
    exit 1
}
[[ -n $OUT_SINK ]] || {
    echo "error: no default sink; pass the output as the second argument:" >&2
    pactl list short sinks >&2
    exit 1
}

echo "Using mic source:      $MIC_SOURCE"
echo "Using headphone sink:  $OUT_SINK"
echo "Rate:                  $RATE"
echo "Channels:              $CHANNELS"
echo "Loopback latency:      ${LOOP_LATENCY_MS} ms"

# 0. Clean up anything left over from a previous run.
unload_cw_modules

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
echo "  ./cw-mic-mix.sh --unload"
