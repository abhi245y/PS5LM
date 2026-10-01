#!/usr/bin/env bash
# Starts llama-server on the console and opens its chat UI in the PS5's own
# browser. Needs ftpsrv (2121) and shsrv (2323) running on the console, and the
# model already in /data/PS5LM/models.
#
#   PS5_HOST=192.168.8.52 scripts/ps5-chat.sh [model.gguf]
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
host=${PS5_HOST:?set PS5_HOST to the console IP}
model=${1:-granite3b.gguf}
port=${PS5LM_PORT:-8081}
logdir="$root/build/console"
mkdir -p "$logdir"

# Upload the current build and the chat page. shsrv needs absolute paths and
# short command lines (keep model file names short).
curl -sS --disable-epsv --ftp-create-dirs -T "$root/build/llama-ps5/bin/llama-server" \
    "ftp://$host:2121/data/PS5LM/bin/llama-server"
curl -sS --disable-epsv --ftp-create-dirs -T "$root/ps5/webui/index.html" \
    "ftp://$host:2121/data/PS5LM/www/index.html"

# Keep the shell session that runs the server open in the background; its output
# goes to build/console/server.log. The context is capped: the model's own
# default (131K for Granite) would not fit in a payload's ~6 GiB. The model is
# read into memory (-lm none): mapped from the file, its pages get evicted once
# the browser opens, and generation drops to a fraction of a token a second.
# --path www serves ps5/webui instead of llama.cpp's UI, which the PS5 browser
# renders blank.
nohup bash -c "
    { sleep 1
      printf 'chmod 755 /data/PS5LM/bin/llama-server\n'; sleep 1
      printf 'cd /data/PS5LM\n'; sleep 1
      printf '/data/PS5LM/bin/llama-server -m models/$model -c 4096 -np 1 -lm none --path www --host 0.0.0.0 --port $port\n'
      sleep 86400
    } | nc $host 2323
" > "$logdir/server.log" 2>&1 &
echo "ps5-chat: llama-server starting (log: $logdir/server.log)"

for _ in $(seq 1 90); do
    sleep 2
    code=$(curl -s -m 3 -o /dev/null -w "%{http_code}" "http://$host:$port/health" || true)
    [ "$code" = "200" ] && break
done
if [ "$code" != "200" ]; then
    echo "ps5-chat: server not healthy (last status $code), see $logdir/server.log" >&2
    exit 1
fi
echo "ps5-chat: server ready"

{ sleep 1; printf 'browse http://127.0.0.1:%s/\n' "$port"; sleep 3; printf 'exit\n'; sleep 1; } \
    | nc -w 5 "$host" 2323 > /dev/null 2>&1 || true
echo "ps5-chat: chat UI opened in the PS5 browser; from another device: http://$host:$port/"
