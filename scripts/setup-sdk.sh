#!/usr/bin/env bash
# Downloads the pinned ps5-payload-dev SDK release into .deps/ps5-payload-sdk.
#
#   macOS:  brew install llvm@21 lld socat cmake ninja
#   Debian: apt install clang-18 lld-18 socat cmake ninja-build
set -euo pipefail

SDK_VERSION=v0.43
SDK_SHA256=a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
deps="$root/.deps"
zip="$deps/ps5-payload-sdk-$SDK_VERSION.zip"
mkdir -p "$deps"

if [ ! -f "$zip" ]; then
    curl -fL -o "$zip.part" \
        "https://github.com/ps5-payload-dev/sdk/releases/download/$SDK_VERSION/ps5-payload-sdk.zip"
    mv "$zip.part" "$zip"
fi

if command -v sha256sum >/dev/null; then
    actual=$(sha256sum "$zip" | cut -d' ' -f1)
else
    actual=$(shasum -a 256 "$zip" | cut -d' ' -f1)
fi
if [ "$actual" != "$SDK_SHA256" ]; then
    echo "setup-sdk: checksum mismatch for $zip ($actual)" >&2
    exit 1
fi

rm -rf "$deps/ps5-payload-sdk"
unzip -q "$zip" -d "$deps"
echo "setup-sdk: ps5-payload-sdk $SDK_VERSION in $deps/ps5-payload-sdk"
