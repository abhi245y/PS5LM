#!/usr/bin/env bash
# Serves the Relapse exploit page (firmware 7.00 to 13.60) from this machine,
# for opening in the PS5's browser on the same network. Relapse's own serve.py
# finds the local IP with Windows' ipconfig, so this does the same on macOS
# and Linux. After a successful run, elfldr listens on the console's port 9021.
#
#   scripts/host-relapse.sh            (port 8000)
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dir="$root/.deps/Relapse-Exploit"
port=${RELAPSE_PORT:-8000}

if [ ! -d "$dir/.git" ]; then
    git clone --depth 1 https://github.com/ntfargo/Relapse-Exploit "$dir"
else
    git -C "$dir" pull -q --ff-only
fi

ip=""
if [ "$(uname -s)" = "Darwin" ]; then
    for ifc in en0 en1 en2; do
        ip=$(ipconfig getifaddr "$ifc" 2>/dev/null || true)
        [ -n "$ip" ] && break
    done
else
    ip=$(hostname -I 2>/dev/null | awk '{print $1}')
fi

echo "host-relapse: open http://${ip:-<this machine>}:$port/ in the PS5's browser"
echo "host-relapse: if the browser stalls, reload; if the console hangs, reboot and retry"
cd "$dir"
exec python3 -m http.server "$port" --bind 0.0.0.0
