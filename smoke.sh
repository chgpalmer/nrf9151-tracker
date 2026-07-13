#!/usr/bin/env bash
# Smoke test: the two flows that must never break.
#
#   1. make build   firmware for the DK (default APP/BOARD)
#   2. make demo    localhost end-to-end: CoAP ingest + web + sim
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
# A-GNSS from the committed fixture: no network dependency in tests. (A stale
# fixture serves time-only assistance — the exchange still runs and asserts.)
export TRACKER_AGNSS_FILE=server/tests/brdc_fixture.rnx
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
# The sim publishes over CoAP/protobuf -> ingest -> SQLite -> /api/devices.
for _ in $(seq "$TIMEOUT"); do
  if curl -fsS "http://127.0.0.1:$HTTP_PORT/api/devices" | grep -q '"device_id"'; then
    echo "device data OK"
    step "waiting for the A-GNSS exchange"
    # The sim's mock reports a thin inventory, so a fetch must follow.
    for _ in $(seq "$TIMEOUT"); do
      if grep -q '\[agnss\].*served' "$DEMO_LOG"; then
        echo "A-GNSS exchange OK"

        # Phase 2 — cold-boot resilience (the 2026-07-13 commute failure):
        # a power-cycled modem (empty inventory) whose first fetches fail
        # must keep LTE up and retry on the 15 s cold cadence until the
        # supply delivers. The server drops the first 2 responses.
        step "cold-boot retry: server drops 2 A-GNSS responses"
        make --no-print-directory stop >/dev/null 2>&1 || true
        sleep 2
        : >"$DEMO_LOG"
        # The sim's console is discarded (Makefile: $(SIM) > /dev/null), so
        # assert on the SERVER's view: two dropped requests plus a third,
        # served one inside the window is only reachable on the 15 s cold
        # retry cadence — the old 60 s pacing would need ~130 s+.
        export TRACKER_SIM_COLD_EPHE=1 TRACKER_AGNSS_DROP_FIRST=2
        make demo >"$DEMO_LOG" 2>&1 &
        demo_pid=$!
        for _ in $(seq 90); do
          grep -q '\[agnss\].*served' "$DEMO_LOG" && break
          kill -0 "$demo_pid" 2>/dev/null || fail "make demo exited early (cold phase)"
          sleep 1
        done
        [ "$(grep -c 'dropping response' "$DEMO_LOG")" -eq 2 ] \
          || fail "drop knob did not bite twice (device stopped retrying?)"
        grep -q '\[agnss\].*served' "$DEMO_LOG" \
          || fail "no served exchange after drops within 90 s (cold retry cadence broken?)"
        echo "cold-retry recovery OK"
        step "PASS: build + demo + cold-boot retry all healthy"
        exit 0
      fi
      sleep 1
    done
    fail "server never served an A-GNSS request (see $DEMO_LOG)"
  fi
  kill -0 "$demo_pid" 2>/dev/null || fail "make demo exited early"
  sleep 1
done
fail "no device appeared in /api/devices within ${TIMEOUT}s"
