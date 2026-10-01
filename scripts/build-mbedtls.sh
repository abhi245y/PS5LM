#!/usr/bin/env bash
# Builds mbedTLS for the PS5 into .deps/mbedtls-ps5: the TLS stack PS5LM uses
# to download models over HTTPS (Sony's own HTTP library fails on the console).
set -euo pipefail

VERSION=3.6.7
SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=env.sh
source "$root/scripts/env.sh"
deps="$root/.deps"
tarball="$deps/mbedtls-$VERSION.tar.bz2"
src="$deps/mbedtls-$VERSION"
out="$deps/mbedtls-ps5"

if [ ! -f "$tarball" ]; then
    curl -fsSL -o "$tarball" \
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VERSION/mbedtls-$VERSION.tar.bz2"
fi
if command -v sha256sum >/dev/null; then
    actual=$(sha256sum "$tarball" | cut -d' ' -f1)
else
    actual=$(shasum -a 256 "$tarball" | cut -d' ' -f1)
fi
[ "$actual" = "$SHA256" ] || { echo "build-mbedtls: checksum mismatch" >&2; exit 1; }
[ -d "$src" ] || tar xjf "$tarball" -C "$deps"

"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S "$src" -B "$deps/mbedtls-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$out" \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DMBEDTLS_FATAL_WARNINGS=OFF \
    -DCMAKE_VERBOSE_MAKEFILE=OFF
cmake --build "$deps/mbedtls-build"
cmake --install "$deps/mbedtls-build" > /dev/null
echo "build-mbedtls: mbedTLS $VERSION in $out"
