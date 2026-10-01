#!/usr/bin/env bash
# Uploads a llama.cpp tool to the console over FTP and starts it through elfldr
# with arguments, streaming its output back.
#
#   scripts/run.sh llama-cli -m /data/PS5LM/models/model.gguf -p "Hello" -n 64
#   scripts/run.sh llama-server -m /data/PS5LM/models/model.gguf --host 0.0.0.0 --port 8081
#
# Needs elfldr (port 9021) and ftpsrv (port 2121) running on the console.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
tool=${1:?usage: run.sh TOOL [ARGS...]}
shift
host=${PS5_HOST:?set PS5_HOST to the console IP}
port=${PS5_PORT:-9021}
ftp_port=${PS5_FTP_PORT:-2121}
remote_dir=/data/PS5LM/bin

elf="$root/build/llama-ps5/bin/$tool"
[ -f "$elf" ] || elf="$tool"
[ -f "$elf" ] || { echo "run.sh: no $tool, build it first" >&2; exit 1; }
name=$(basename "$elf")

curl -sS --ftp-create-dirs -T "$elf" "ftp://$host:$ftp_port$remote_dir/$name"

# elfldr splits args on spaces; a space inside one argument is escaped with a
# backslash, and the whole string is URL-encoded.
args=$(python3 -c '
import sys, urllib.parse
parts = [a.replace("\\", "\\\\").replace(" ", "\\ ") for a in sys.argv[1:]]
print(urllib.parse.quote(" ".join(parts), safe=""))
' "$@")

echo "file:$remote_dir/$name?args=$args" | socat -t 9999999 - "TCP:$host:$port"
