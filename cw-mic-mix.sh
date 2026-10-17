#!/usr/bin/env bash
# Sets up a virtual "CW_Mic_Mix" device that combines your real microphone
# with the CW sidetone, so apps like Discord/Mumble can use it as a mic.
#
# Safe to re-run: stale modules with this script's fixed sink names are
# unloaded first. Use ./cw-mic-mix.sh --unload to tear everything down.

set -euo pipefail

usage() {
    echo "Usage: $0 [--unload | [MIC_SOURCE [OUT_SINK]]]" >&2
    echo "Environment: RATE=48000 CHANNELS=2 LOOP_LATENCY_MS=15" >&2
}

command -v pactl >/dev/null || {
    echo "error: pactl not found (need PulseAudio or PipeWire)" >&2
    exit 1
}

# Unload all modules of $1 whose arguments contain $2. pactl's short module
# listing differs between PulseAudio and PipeWire versions, so locate the
# module-name token among the first few columns and treat the rest as args.
unload_modules_matching() {
    local wanted_name=$1
    local wanted_arg=$2
    local listing line module_name module_args module_id failed=0 pos
    local -a fields

    if ! listing=$(pactl list modules short); then
        echo "error: cannot list PulseAudio/PipeWire modules" >&2
        return 1
    fi

    while IFS= read -r line || [[ -n $line ]]; do
        fields=()
        read -r -a fields <<< "$line"
        ((${#fields[@]} >= 2)) || continue

        module_id=${fields[0]}
        module_name=
        module_args=
        for pos in 1 2 3; do
            if [[ ${fields[$pos]-} == module-* ]]; then
                module_name=${fields[$pos]}
                module_args=${fields[*]:$((pos + 1))}
                break
            fi
        done
        [[ -n $module_name ]] || continue

        if [[ $module_name == "$wanted_name" &&
              $module_args == *"$wanted_arg"* ]]; then
            echo "Unloading stale module $module_id ($module_name)"
            if ! pactl unload-module "$module_id"; then
                echo "warning: failed to unload module $module_id" >&2
                failed=1
            fi
        fi
    done <<< "$listing"

    return "$failed"
}

unload_cw_modules() {
    # Order matters: stop if a dependent module cannot be unloaded, rather
    # than removing a sink that a still-loaded module may be using.
    unload_modules_matching module-loopback "sink=cw_mic_mix" || return 1
    unload_modules_matching module-combine-sink "sink_name=cw_out" || return 1
    unload_modules_matching module-null-sink "sink_name=cw_mic_mix"
}

valid_integer() {
    local value=$1 min=$2 max=$3
    [[ $value =~ ^[0-9]{1,6}$ ]] || return 1
    value=$((10#$value))
    ((value >= min && value <= max))
}

audio_object_exists() {
    local kind=$1 wanted=$2 listing line
    local -a fields

    if ! listing=$(pactl list short "$kind"); then
        echo "error: cannot list PulseAudio/PipeWire $kind" >&2
        return 1
    fi
    while IFS= read -r line || [[ -n $line ]]; do
        fields=()
        read -r -a fields <<< "$line"
        if [[ ${fields[1]-} == "$wanted" ]]; then
            return 0
        fi
    done <<< "$listing"
    return 1
}

# If setup fails or is interrupted after module creation starts, remove the
# modules using their reserved CW sink names in dependency order.
setup_started=0
setup_complete=0

cleanup_on_exit() {
    local status=$?

    trap - EXIT INT TERM HUP
    if ((status != 0 && !setup_complete && setup_started)); then
        echo "Setup did not complete; rolling back loaded CW modules" >&2
        if ! unload_cw_modules; then
            echo "warning: could not remove every partially loaded CW module" >&2
        fi
    fi

    exit "$status"
}

trap cleanup_on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

load_module() {
    local module_id

    if ! module_id=$(pactl load-module "$@"); then
        echo "error: failed to load ${1}" >&2
        return 1
    fi
    if [[ ! $module_id =~ ^[0-9]+$ ]]; then
        echo "error: pactl returned an invalid module id for ${1}: $module_id" >&2
        return 1
    fi

    echo "Loaded ${1} (module $module_id)"
}

if [[ ${1:-} == "--unload" ]]; then
    if (($# != 1)); then
        usage
        exit 2
    fi
    echo "Unloading CW mix modules"
    unload_cw_modules
    setup_complete=1
    exit 0
fi

if (($# > 2)); then
    usage
    exit 2
fi

RATE=${RATE:-48000}
CHANNELS=${CHANNELS:-2}
LOOP_LATENCY_MS=${LOOP_LATENCY_MS:-15}

if ! valid_integer "$RATE" 8000 192000; then
    echo "error: RATE must be an integer from 8000 to 192000 (got '$RATE')" >&2
    exit 2
fi
if ! valid_integer "$CHANNELS" 1 8; then
    echo "error: CHANNELS must be an integer from 1 to 8 (got '$CHANNELS')" >&2
    exit 2
fi
if ! valid_integer "$LOOP_LATENCY_MS" 0 1000; then
    echo "error: LOOP_LATENCY_MS must be an integer from 0 to 1000 (got '$LOOP_LATENCY_MS')" >&2
    exit 2
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

# These names are torn down during idempotent setup, so they cannot also be
# used as inputs to the new modules.
if [[ $MIC_SOURCE == cw_mic_mix.monitor || $MIC_SOURCE == cw_out.monitor ]]; then
    echo "error: '$MIC_SOURCE' is created by this script and cannot be its mic source" >&2
    exit 2
fi
if [[ $OUT_SINK == cw_mic_mix || $OUT_SINK == cw_out ]]; then
    echo "error: '$OUT_SINK' is created by this script and cannot be its output sink" >&2
    exit 2
fi
if ! audio_object_exists sources "$MIC_SOURCE"; then
    echo "error: source '$MIC_SOURCE' does not exist" >&2
    pactl list short sources >&2
    exit 1
fi
if ! audio_object_exists sinks "$OUT_SINK"; then
    echo "error: sink '$OUT_SINK' does not exist" >&2
    pactl list short sinks >&2
    exit 1
fi

echo "Using mic source:      $MIC_SOURCE"
echo "Using headphone sink:  $OUT_SINK"
echo "Rate:                  $RATE"
echo "Channels:              $CHANNELS"
echo "Loopback latency:      ${LOOP_LATENCY_MS} ms"

# 0. Clean up anything left over from a previous run.
unload_cw_modules

# 1. A null sink that becomes the mixed mic feed. Its monitor is what
#    Discord/Mumble/Audacity will see as an input device.
setup_started=1
load_module module-null-sink \
  sink_name=cw_mic_mix \
  rate="$RATE" \
  channels="$CHANNELS" \
  sink_properties=device.description=CW_Mic_Mix

# 2. Loop your real mic into it.
load_module module-loopback \
  source="$MIC_SOURCE" \
  sink=cw_mic_mix \
  latency_msec="$LOOP_LATENCY_MS"

# 3. A combine-sink so the CW tone plays to both the mix and your headphones.
load_module module-combine-sink \
  sink_name=cw_out \
  slaves=cw_mic_mix,"$OUT_SINK" \
  rate="$RATE" \
  channels="$CHANNELS" \
  sink_properties=device.description=CW_Output_To_Mix_And_Headphones

setup_complete=1

echo
echo "Done. Now:"
echo "  1. In Discord/Mumble/Audacity, set input device to: Monitor of CW_Mic_Mix"
echo "  2. Run:"
echo "       PULSE_SINK=cw_out CW_ALSA_DEVICE=pulse CW_ALSA_LATENCY_US=20000 ./cwkeyer /dev/input/eventX"
echo
echo "To undo:"
echo "  ./cw-mic-mix.sh --unload"
