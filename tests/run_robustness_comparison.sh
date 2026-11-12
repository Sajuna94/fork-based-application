#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="127.0.0.1"
ROBUST_PORT="${ROBUST_PORT:-19092}"
BASELINE_PORT="${BASELINE_PORT:-19093}"
ROBUST_LOG="$(mktemp)"
BASELINE_LOG="$(mktemp)"
ROBUST_PID=""
BASELINE_PID=""

cleanup() {
  for pid in "$ROBUST_PID" "$BASELINE_PID"; do
    if [[ -n "$pid" ]]; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -f "$ROBUST_LOG" "$BASELINE_LOG"
}
trap cleanup EXIT

wait_ready() {
  local port="$1"
  for _ in $(seq 1 50); do
    if python3 - "$HOST" "$port" <<'PY' >/dev/null 2>&1
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
      return 0
    fi
    sleep 0.1
  done
  return 1
}

"$ROOT/build/bin/server" --bind "$HOST" --port "$ROBUST_PORT" \
  --max-clients 1 --log-level info --robustness robust --io-timeout 2 \
  >"$ROBUST_LOG" 2>&1 &
ROBUST_PID=$!

"$ROOT/build/bin/server" --bind "$HOST" --port "$BASELINE_PORT" \
  --max-clients 1 --log-level info --robustness baseline --io-timeout 2 \
  >"$BASELINE_LOG" 2>&1 &
BASELINE_PID=$!

if ! wait_ready "$ROBUST_PORT" || ! wait_ready "$BASELINE_PORT"; then
  echo "FAIL: test servers did not become ready" >&2
  cat "$ROBUST_LOG" "$BASELINE_LOG" >&2
  exit 1
fi
# Let the short-lived readiness-probe workers be reaped before testing the
# one-worker resource-exhaustion scenario.
sleep 0.2

python3 "$ROOT/tests/robustness_comparison.py" "$ROBUST_PORT" "$BASELINE_PORT"
