#!/usr/bin/env bash
# Sends the helper payloads PS5LM needs to a freshly jailbroken console:
# ftpsrv (FTP on 2121), klogsrv (kernel log on 3232) and shsrv (shell on 2323).
# Run it after the jailbreak, once elfldr listens on 9021. They stay up until
# the console reboots.
#
#   PS5_HOST=192.168.1.50 scripts/console-setup.sh
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
host=${PS5_HOST:?set PS5_HOST to the console IP}
port=${PS5_PORT:-9021}
dir="$root/.deps/payloads"
mkdir -p "$dir"

payloads=(
    "ftpsrv/v0.21.1/ftpsrv-ps5.elf"
    "klogsrv/v0.9/klogsrv-ps5.elf"
    "shsrv/v0.20/shsrv-ps5.elf"
)

for p in "${payloads[@]}"; do
    repo=${p%%/*}
    rest=${p#*/}
    tag=${rest%%/*}
    name=${rest#*/}
    if [ ! -f "$dir/$name" ]; then
        curl -fsSL -o "$dir/$name" "https://github.com/ps5-payload-dev/$repo/releases/download/$tag/$name"
    fi
    # -u: send the file and hang up; the payload keeps running on the console.
    socat -u "FILE:$dir/$name" "TCP:$host:$port"
    echo "console-setup: sent $name"
    sleep 1
done

echo "console-setup: FTP ftp://$host:2121, kernel log: nc $host 3232, shell: nc $host 2323"
