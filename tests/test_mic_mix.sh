#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for cw-mic-mix.sh using a mocked pactl.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
script="$here/../cw-mic-mix.sh"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/pactl" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
state=${MOCK_PACTL_STATE:?}
case "${1-} ${2-} ${3-}" in
  'get-default-source  ') echo mic0 ;;
  'get-default-sink  ') echo headphones0 ;;
  'list modules short') [[ -f $state ]] && cat "$state" || : ;;
  'list short sources') printf '1\tmic0\tdriver\n' ;;
  'list short sinks') printf '2\theadphones0\tdriver\n' ;;
  'load-module '* )
    name=$2
    if [[ ${MOCK_PACTL_FAIL:-} == "$name" ]]; then
        echo "mock failure: $name" >&2
        exit 1
    fi
    id=$(($(tail -n 1 "$state" 2>/dev/null | cut -f1 || echo 0) + 1))
    printf '%s\t%s\t%s\n' "$id" "$name" "${*:3}" >>"$state"
    echo "$id"
    ;;
  'unload-module '* )
    id=$2
    [[ -f $state ]] || exit 0
    awk -F '\t' -v id="$id" '$1 != id' "$state" >"$state.tmp"
    mv "$state.tmp" "$state"
    ;;
  *) echo "unexpected pactl args: $*" >&2; exit 2 ;;
esac
MOCK
chmod +x "$tmp/pactl"

export PATH="$tmp:$PATH" MOCK_PACTL_STATE="$tmp/modules"

fail=0
check() { if ! "$@"; then echo "FAIL: $*" >&2; fail=1; fi; }

# Setup creates the null sink, loopback, and combine sink.
"$script" >/dev/null
check test "$(wc -l < "$MOCK_PACTL_STATE")" -eq 3

# Re-running is idempotent: still exactly those three modules.
"$script" >/dev/null
check test "$(wc -l < "$MOCK_PACTL_STATE")" -eq 3

# --unload removes them all.
"$script" --unload >/dev/null
check test ! -s "$MOCK_PACTL_STATE"

# A failed module load rolls the whole attempt back.
if MOCK_PACTL_FAIL=module-loopback "$script" >/dev/null 2>&1; then
    echo "FAIL: expected the mocked load failure" >&2
    fail=1
fi
check test ! -s "$MOCK_PACTL_STATE"

# Invalid numeric settings are rejected before touching the audio server.
if RATE=invalid "$script" >/dev/null 2>&1; then
    echo "FAIL: expected RATE to be rejected" >&2
    fail=1
fi
check test ! -s "$MOCK_PACTL_STATE"

# A missing source is rejected.
if "$script" missing-mic headphones0 >/dev/null 2>&1; then
    echo "FAIL: expected a missing source to be rejected" >&2
    fail=1
fi
check test ! -s "$MOCK_PACTL_STATE"

if ((fail)); then
    echo "mic-mix tests failed" >&2
    exit 1
fi

echo "mic-mix tests passed"
