#!/bin/bash
# One-time setup: authorize `codesign` to use the self-signed "ScreenFlip Dev" key
# WITHOUT a GUI prompt, so builds with SF_USE_CERT=1 are stable and non-interactive.
#
# Run it yourself (your keychain password stays on your machine):
#     ! ./scripts/authorize-signing.sh
set -euo pipefail
KC="$HOME/Library/Keychains/login.keychain-db"

if ! security find-identity -v -p codesigning 2>/dev/null | grep -q "ScreenFlip Dev"; then
  echo "No 'ScreenFlip Dev' identity found in the keychain. Nothing to authorize."
  exit 1
fi

printf "Login (keychain) password: "
read -rs PW
echo

security unlock-keychain -p "$PW" "$KC"
# Add codesign/apple tools to the key's partition list so they can use it silently.
security set-key-partition-list -S apple-tool:,apple:,unsigned: -s -l "ScreenFlip Dev" -k "$PW" "$KC" >/dev/null
unset PW

echo "✓ Authorized. Builds with SF_USE_CERT=1 will now sign silently with a stable identity."
