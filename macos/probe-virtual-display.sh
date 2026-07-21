#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

SDK="$(xcrun --show-sdk-path)"
SF_MODULE_CACHE="${TMPDIR:-/tmp}/screenflip-swift-module-cache"
mkdir -p build/probe
mkdir -p "$SF_MODULE_CACHE"

xcrun swiftc \
  -sdk "$SDK" \
  -module-cache-path "$SF_MODULE_CACHE" \
  -target arm64-apple-macos13.0 \
  -framework AppKit \
  -framework CoreGraphics \
  -F /System/Library/PrivateFrameworks \
  -framework SkyLight \
  -import-objc-header Sources/Bridging.h \
  -Xlinker -undefined -Xlinker dynamic_lookup \
  Sources/Log.swift \
  Sources/Displays.swift \
  Sources/VirtualDisplay.swift \
  probe/virtual-display/main.swift \
  -o build/probe/VirtualDisplayProbe

build/probe/VirtualDisplayProbe
