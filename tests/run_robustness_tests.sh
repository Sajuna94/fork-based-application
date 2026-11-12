#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-19091}"
SERVER_LOG="$(mktemp)"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -f "$SERVER_LOG"
}
trap cleanup EXIT

"$ROOT/build/bin/server" --bind "$HOST" --port "$PORT" --max-clients 32 --log-level debug >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

ready=0
for _ in $(seq 1 50); do
  if python3 - "$HOST" "$PORT" <<'PY' >/dev/null 2>&1
import socket, sys
s = socket.socket()
s.settimeout(0.2)
try:
    s.connect((sys.argv[1], int(sys.argv[2])))
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

python3 "$ROOT/tests/fragmented_frame.py" "$HOST" "$PORT"
python3 "$ROOT/tests/oversized_frame.py" "$HOST" "$PORT"
python3 "$ROOT/tests/partial_frame.py" "$HOST" "$PORT"

echo "All robustness tests passed. Server log excerpt:"
grep -E 'invalid/incomplete|worker finished|child reaped' "$SERVER_LOG" | tail -12 || true
