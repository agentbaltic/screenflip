#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p build/tests
SF_MODULE_CACHE="${TMPDIR:-/tmp}/screenflip-swift-module-cache"
mkdir -p "$SF_MODULE_CACHE"
xcrun swiftc \
  -module-cache-path "$SF_MODULE_CACHE" \
  Sources/Geometry.swift \
  Tests/main.swift \
  -framework CoreGraphics \
  -o build/tests/GeometryTests

build/tests/GeometryTests
