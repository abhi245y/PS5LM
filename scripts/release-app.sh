#!/usr/bin/env bash
# Packages the built app for a release: build/release/ps5lm-app-v<version>.zip
# holding the PPSA99581 title folder, plus its sha256. The version comes from
# ps5/app/version.hpp; the git tag is v<version>.
#
#   scripts/build-app.sh && scripts/release-app.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version=$(sed -n 's/^#define PS5LM_VERSION "\(.*\)"$/\1/p' "$root/ps5/app/version.hpp")
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/app/sce_sys/param.json")
app="$root/build/app-ps5/title/$title"
out="$root/build/release"
zip="$out/ps5lm-app-v$version.zip"

[ -f "$app/eboot.bin" ] || { echo "release-app: build it first (scripts/build-app.sh)" >&2; exit 1; }
grep -q "^## \[$version\]" "$root/CHANGELOG.md" || { echo "release-app: CHANGELOG.md has no [$version] entry" >&2; exit 1; }
mkdir -p "$out"
rm -f "$zip"
(cd "$(dirname "$app")" && zip -qr -X "$zip" "$title")
(cd "$out" && sha256sum "$(basename "$zip")" > "$(basename "$zip").sha256")
echo "release-app: $zip ($(du -h "$zip" | cut -f1))"
cat "$zip.sha256"
