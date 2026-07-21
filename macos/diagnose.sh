#!/bin/bash
# Read-only diagnostic snapshot for display and ScreenFlip lifecycle bugs.
set -u
cd "$(dirname "$0")"

section() {
  printf '\n## %s\n' "$1"
}

section "Timestamp"
date -u '+%Y-%m-%dT%H:%M:%SZ'

section "macOS"
sw_vers
uname -m

section "ScreenFlip processes"
pgrep -fl ScreenFlip || printf 'No ScreenFlip process found.\n'

section "Active display summary"
system_profiler SPDisplaysDataType

section "CoreGraphics display probe"
PROBE_BINARY="${TMPDIR:-/tmp}/screenflip-display-probe"
SF_MODULE_CACHE="${TMPDIR:-/tmp}/screenflip-swift-module-cache"
mkdir -p "$SF_MODULE_CACHE"
if xcrun swiftc probe/probe.swift \
  -module-cache-path "$SF_MODULE_CACHE" \
  -framework AppKit \
  -framework Metal \
  -framework CoreGraphics \
  -framework ScreenCaptureKit \
  -o "$PROBE_BINARY"; then
  "$PROBE_BINARY"
else
  printf 'Display probe could not be compiled.\n'
fi

section "Saved preferences"
defaults read io.vbar.screenflip 2>&1 || true

section "Screen Recording permission entry"
if command -v sqlite3 >/dev/null 2>&1; then
  printf 'Permission state is intentionally not read directly from the protected TCC database.\n'
fi

section "Recent ScreenFlip log"
if [[ -f /tmp/screenflip.log ]]; then
  tail -n 240 /tmp/screenflip.log
else
  printf 'No /tmp/screenflip.log found.\n'
fi
