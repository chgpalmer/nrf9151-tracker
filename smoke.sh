#!/usr/bin/env bash
# Smoke test: the two flows that must never break.
#
#   1. make build   firmware for the DK (default APP/BOARD)
#   2. make demo    localhost end-to-end: broker + ingest + web + sim
#
# `make demo` runs in the foreground forever, so we background it, wait for the
# web map to answer, check the sim's data actually reached the API, then tear
# the whole stack down. Exit 0 = both flows healthy.
set -euo pipefail

cd "$(dirname "$0")"

HTTP_PORT=${HTTP_PORT:-8080}
DEMO_LOG=$(mktemp -t smoke-demo-XXXXXX.log)
TIMEOUT=${TIMEOUT:-60}

step() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }
fail() { printf '\n\033[31mFAIL: %s\033[0m\n' "$*"; [ -s "$DEMO_LOG" ] && tail -30 "$DEMO_LOG"; exit 1; }

cleanup() { make --no-print-directory stop >/dev/null 2>&1 || true; rm -f "$DEMO_LOG"; }
trap cleanup EXIT

step "make build"
make build || fail "make build"

step "make demo (background, log: $DEMO_LOG)"
make demo >"$DEMO_LOG" 2>&1 &
demo_pid=$!

step "waiting for the web map on 127.0.0.1:$HTTP_PORT"
for _ in $(seq "$TIMEOUT"); do
  curl -fsS "http://127.0.0.1:$HTTP_PORT/" >/dev/null 2>&1 && break
  kill -0 "$demo_pid" 2>/dev/null || fail "make demo exited early"
  sleep 1
done
curl -fsS "http://127.0.0.1:$HTTP_PORT/" | grep -q '<title>' \
  || fail "web map did not serve a page within ${TIMEOUT}s"
echo "web map OK"

step "waiting for the sim's observations to reach the API"
# The sim publishes over MQTT -> ingest -> SQLite -> /api/devices.
for _ in $(seq "$TIMEOUT"); do
  if curl -fsS "http://127.0.0.1:$HTTP_PORT/api/devices" | grep -q '"device_id"'; then
    echo "device data OK"
    step "PASS: build + demo both healthy"
    exit 0
  fi
  kill -0 "$demo_pid" 2>/dev/null || fail "make demo exited early"
  sleep 1
done
fail "no device appeared in /api/devices within ${TIMEOUT}s"
