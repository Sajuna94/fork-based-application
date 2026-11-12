#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-9000}"
OUT="${2:-captures/loopback.pcap}"
mkdir -p "$(dirname "$OUT")"
echo "Capturing TCP loopback traffic on port $PORT to $OUT"
echo "Start the client in another terminal, then press Ctrl-C here."
sudo tcpdump -i lo -nn -s0 -w "$OUT" "tcp port $PORT"
