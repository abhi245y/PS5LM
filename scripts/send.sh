#!/usr/bin/env bash
# Sends a payload ELF to the console's ELF loader (elfldr, port 9021) and
# streams the payload's stdout back until it exits.
#
#   scripts/send.sh build/memprobe.elf            (uses $PS5_HOST)
#   PS5_HOST=192.168.1.50 scripts/send.sh x.elf
set -euo pipefail

elf=${1:?usage: send.sh PAYLOAD.elf}
host=${PS5_HOST:?set PS5_HOST to the console IP}
port=${PS5_PORT:-9021}

if ! command -v socat >/dev/null; then
    echo "send.sh: socat is required (brew install socat / apt install socat)" >&2
    exit 1
fi

socat -t 9999999 - "TCP:$host:$port" < "$elf"
