#!/usr/bin/env bash
# Writes site/payloads.json, the Payload Manager source for PS5LM, for a
# release. Users add https://ps5lm.cobanov.dev/payloads.json under Sources and
# install or update PS5LM from Payload Manager. The format is the one
# itsPLK/ps5-payloads-mirror uses; the checksum is the SHA-256 of the asset.
#
#   scripts/update-source.sh v0.1.0      (after the release has ps5lm.elf)
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version=${1:?usage: update-source.sh VERSION}
url="https://github.com/cobanov/PS5LM/releases/download/$version/ps5lm.elf"

# The checksum of the asset as GitHub records it. Downloading it to hash
# can return a cached older file when a release was recreated.
sum=$(gh api "repos/cobanov/PS5LM/releases/tags/$version" \
    --jq '.assets[] | select(.name == "ps5lm.elf") | .digest' | sed 's/^sha256://')
if [ -z "$sum" ]; then
    echo "update-source: no ps5lm.elf in release $version" >&2
    exit 1
fi

python3 - "$version" "$url" "$sum" "$root/site/payloads.json" <<'EOF'
import datetime, json, sys
version, url, checksum, out = sys.argv[1:]
entry = {
    "name": "PS5LM",
    "filename": f"PS5LM_{version}.elf",
    "url": url,
    "source": "https://github.com/cobanov/PS5LM/releases",
    "source_direct": url,
    "description": "Run local LLMs (llama.cpp) on your PS5: download a model on the console and chat in the browser",
    "last_update": datetime.date.today().isoformat(),
    "version": version,
    "category": "Utilities & Tools",
    "checksum": checksum,
}
with open(out, "w") as f:
    json.dump([entry], f, indent=2)
    f.write("\n")
print(f"update-source: {out} -> {version} ({checksum[:12]})")
EOF
