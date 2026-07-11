#!/usr/bin/env bash
# Self-contained web UI test: seed a temp DB, serve the app on a test port,
# drive it in headless Chromium (scripts/web-check.py), tear everything down.
# Never touches the real tracker.db. Invoked as `make webtest`.
set -euo pipefail

cd "$(dirname "$0")/.."

VENV="${VENV:-../.venv}"
PORT="${WEBTEST_PORT:-8891}"
DB=$(mktemp -t webtest-XXXXXX.db)
SHOT="${WEBTEST_SCREENSHOT:-}"

cleanup() {
  [ -n "${UV_PID:-}" ] && kill "$UV_PID" 2>/dev/null || true
  rm -f "$DB"
}
trap cleanup EXIT

"$VENV/bin/python3" scripts/seed-testday.py "$DB"

TRACKER_DB="$DB" "$VENV/bin/uvicorn" app:app --app-dir server \
  --port "$PORT" --log-level warning &
UV_PID=$!

for _ in $(seq 40); do
  curl -fsS -o /dev/null "http://127.0.0.1:$PORT/api/devices" 2>/dev/null && break
  sleep 0.25
done

"$VENV/bin/python3" scripts/web-check.py --url "http://127.0.0.1:$PORT" \
  ${SHOT:+--screenshot "$SHOT"}
