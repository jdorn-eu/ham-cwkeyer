#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Wrap the binary built by "make" in a minimal cwkeyer.app.  The bundle gives
# macOS a stable identity for Input Monitoring and Microphone permissions;
# launched straight from a terminal, the terminal is the responsible process.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
bin=${1:-"$root/cwkeyer"}
app=$root/cwkeyer.app
plist=$root/contrib/macos/Info.plist.in

[ -x "$bin" ] || {
    echo "error: $bin is not built; run make first" >&2
    exit 1
}
[ -f "$plist" ] || {
    echo "error: $plist is missing" >&2
    exit 1
}

version=$("$bin" --version 2>/dev/null | sed -n 's/^cwkeyer //p')
[ -n "$version" ] || version=0.0.0
# CFBundleVersion is period-separated integers; strip a git suffix.
short=$(printf '%s\n' "$version" | sed 's/[^0-9.].*$//')
[ -n "$short" ] || short=0.0.0

rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp "$bin" "$app/Contents/MacOS/cwkeyer"
sed "s/@VERSION@/$short/g" "$plist" > "$app/Contents/Info.plist"

if command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - "$app"
else
    echo "warning: codesign not found; the bundle is unsigned" >&2
fi

echo "wrote $app"
echo "run it with: open '$app' --args <device> [wpm] [freq] ..."
