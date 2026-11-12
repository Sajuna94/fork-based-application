#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-19090}"
SERVER_LOG="$(mktemp)"
CLIENT_PIDS=()

cleanup() {
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -f "$SERVER_LOG"
}
trap cleanup EXIT

"$ROOT/build/bin/server" --bind 127.0.0.1 --port "$PORT" --max-clients 32 --log-level info >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

# Wait until the listening socket is ready.
ready=0
for _ in $(seq 1 50); do
  if python3 - "$PORT" <<'PY' >/dev/null 2>&1
import socket, sys
s = socket.socket()
s.settimeout(0.2)
try:
    s.connect(("127.0.0.1", int(sys.argv[1])))
    raise SystemExit(0)
except OSError:
    raise SystemExit(1)
finally:
    s.close()
PY
  then
    ready=1
    break
  fi
  sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
  echo "FAIL: server did not become ready" >&2
  cat "$SERVER_LOG" >&2
  exit 1
fi

# All clients hold their server-side workers for 4 seconds.
for i in $(seq 1 10); do
  "$ROOT/build/bin/client" 127.0.0.1 "$PORT" HOLD 4 >/dev/null &
  CLIENT_PIDS+=("$!")
done
for pid in "${CLIENT_PIDS[@]}"; do
  wait "$pid"
done
CLIENT_PIDS=()

peak="$(sed -n 's/.*peak_children=\([0-9][0-9]*\).*/\1/p' "$SERVER_LOG" | sort -n | tail -1)"
peak="${peak:-0}"
echo "Observed peak concurrent child processes: $peak"
if (( peak < 10 )); then
  echo "FAIL: expected at least 10 concurrent worker processes" >&2
  cat "$SERVER_LOG" >&2
  exit 1
fi
echo "PASS: runtime proof confirms at least 10 clients were served concurrently."
