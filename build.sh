#!/bin/bash
# Build ScreenFlip.app from Sources/*.swift with command-line tools only (no Xcode).
# Assembles a proper .app bundle and ad-hoc signs it so TCC (Screen Recording /
# Accessibility) prompts work. Usage: ./build.sh [run]
set -euo pipefail
cd "$(dirname "$0")"

APP="ScreenFlip"
BUNDLE_ID="io.vbar.screenflip"
APP_DIR="build/${APP}.app"
DEPLOY_TARGET="13.0"
SDK="$(xcrun --show-sdk-path)"

FRAMEWORKS=(AppKit Metal MetalKit CoreGraphics CoreVideo QuartzCore Carbon IOKit ScreenCaptureKit CoreMedia)
FW_FLAGS=()
for f in "${FRAMEWORKS[@]}"; do FW_FLAGS+=(-framework "$f"); done

# Link the private SkyLight framework (for display-transform SPIs) if we end up using it.
LINK_SKYLIGHT="${LINK_SKYLIGHT:-1}"
if [[ "$LINK_SKYLIGHT" == "1" ]]; then
  FW_FLAGS+=(-F /System/Library/PrivateFrameworks -framework SkyLight)
fi

# Optional Objective-C bridging header for private C SPI declarations.
BRIDGE_FLAGS=()
if [[ -f Sources/Bridging.h ]]; then
  BRIDGE_FLAGS+=(-import-objc-header Sources/Bridging.h)
fi

SWIFT_FILES=(Sources/*.swift)

echo ">> Compiling ${#SWIFT_FILES[@]} swift file(s) for arm64-apple-macos${DEPLOY_TARGET}"
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"

swiftc \
  -sdk "$SDK" \
  -target "arm64-apple-macos${DEPLOY_TARGET}" \
  -O \
  "${FW_FLAGS[@]}" \
  ${BRIDGE_FLAGS[@]+"${BRIDGE_FLAGS[@]}"} \
  -Xlinker -undefined -Xlinker dynamic_lookup \
  "${SWIFT_FILES[@]}" \
  -o "$APP_DIR/Contents/MacOS/${APP}"

cp Info.plist "$APP_DIR/Contents/Info.plist"
if compgen -G "Resources/*" >/dev/null; then cp -R Resources/* "$APP_DIR/Contents/Resources/" 2>/dev/null || true; fi

# Signing. Prefer the stable self-signed "ScreenFlip Dev" identity when present so
# macOS Screen Recording grants survive rebuilds. Set SF_USE_CERT=0 to force ad-hoc
# signing for one-off builds.
SIGN_ID="-"
if [[ "${SF_USE_CERT:-1}" == "1" ]] && security find-identity -v -p codesigning 2>/dev/null | grep -q "ScreenFlip Dev"; then
  SIGN_ID="ScreenFlip Dev"
fi
echo ">> Signing with identity: ${SIGN_ID}  (bundle id ${BUNDLE_ID})"
codesign --force --sign "$SIGN_ID" --identifier "$BUNDLE_ID" "$APP_DIR"

echo ">> Built $APP_DIR"
codesign -dv "$APP_DIR" 2>&1 | grep -E "Identifier|Signature|TeamIdentifier" || true

if [[ "${1:-}" == "run" ]]; then
  echo ">> Launching"
  "$APP_DIR/Contents/MacOS/${APP}"
fi
