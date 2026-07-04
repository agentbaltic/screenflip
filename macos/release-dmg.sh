#!/usr/bin/env bash
# Build a signed + notarized, drag-to-install DMG of ScreenFlip for distribution.
#
#   ./release-dmg.sh
#
# Steps (all proven end-to-end):
#   1. Compile the app bundle via build.sh (ad-hoc; re-signed below).
#   2. Re-sign the .app with Developer ID + Hardened Runtime + secure timestamp
#      (both required by Apple's notary service). ScreenFlip links the private
#      SkyLight framework and resolves private CGVirtualDisplay symbols via
#      -undefined dynamic_lookup; those are Apple-signed so Hardened Runtime's
#      library validation passes with no extra entitlements. Notarization does
#      NOT reject private-API use (that's only an App Store review concern).
#   3. Notarize + staple the .app (a DMG staple alone does NOT cover the app
#      inside, so a copied-out app would fail Gatekeeper offline without this).
#   4. Build the DMG, sign it, notarize + staple it.
#
# Requires:
#   - Signing identity in the login keychain: DEVID_IDENTITY (default below).
#   - notarytool keychain profile: NOTARY_PROFILE (default "notary").
set -euo pipefail
cd "$(dirname "$0")"

APP="ScreenFlip"
BUNDLE_ID="io.vbar.screenflip"
APP_DIR="build/${APP}.app"
DEVID_IDENTITY="${DEVID_IDENTITY:-Developer ID Application: TipaSoft Inc. (X5U9NZN249)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-notary}"

# ── 1. Build (ad-hoc) ──
echo "==> Building app bundle"
SF_USE_CERT=0 ./build.sh

# ── 2. Re-sign with Developer ID + Hardened Runtime ──
echo "==> Signing with Developer ID + Hardened Runtime: $DEVID_IDENTITY"
codesign --force --options runtime --timestamp \
  --sign "$DEVID_IDENTITY" --identifier "$BUNDLE_ID" "$APP_DIR"
codesign --verify --strict --verbose=2 "$APP_DIR"

VERSION="$(plutil -extract CFBundleShortVersionString raw Info.plist)"
DMG="build/${APP}-${VERSION}.dmg"

# ── 3. Notarize + staple the .app ──
echo "==> Notarizing the app bundle (can take a few minutes)…"
APP_ZIP="build/${APP}-app.zip"
rm -f "$APP_ZIP"
ditto -c -k --keepParent "$APP_DIR" "$APP_ZIP"   # notarytool needs a zip/pkg/dmg
xcrun notarytool submit "$APP_ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
rm -f "$APP_ZIP"
xcrun stapler staple "$APP_DIR"
xcrun stapler validate "$APP_DIR"

# ── 4. Build, sign, notarize + staple the DMG ──
echo "==> Building $DMG"
STAGE="build/dmg-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
ditto "$APP_DIR" "$STAGE/${APP}.app"             # ditto preserves the stapled ticket
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "$APP" -srcfolder "$STAGE" -ov -format UDZO -fs HFS+ "$DMG" >/dev/null
rm -rf "$STAGE"

echo "==> Signing DMG"
codesign --force --timestamp --sign "$DEVID_IDENTITY" "$DMG"

echo "==> Notarizing the DMG (can take a few minutes)…"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"

echo "==> Gatekeeper assessment"
spctl --assess --type open --context context:primary-signature -v "$DMG"

echo
echo "Built: $DMG"
ls -lh "$DMG"
shasum -a 256 "$DMG"
