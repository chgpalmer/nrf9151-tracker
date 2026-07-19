#!/usr/bin/env bash
# flog (flash flight recorder) end-to-end on native_sim: init, persistence
# across reboot, ring rotation under overfill, erase. Uses a 64 KB ring
# (tests/flog/tiny-ring.overlay) so rotation happens in seconds; the flash
# file lives in a temp dir. Invoked as `make flogtest`.
set -euo pipefail
cd "$(dirname "$0")/.."

WEST="${WEST:-../.venv/bin/west}"
BUILD=build/flogtest
OVL="$PWD/tests/flog/tiny-ring.overlay"

"$WEST" build -p auto -b native_sim/native/64 apps/tracker -d "$BUILD" -- \
  -DCONFIG_TRACKER_FLOG_SECTORS=2 \
  '-DCONFIG_TRACKER_SERVER_HOST="127.0.0.1"' \
  -DEXTRA_DTC_OVERLAY_FILE="$OVL" >/dev/null

EXE=$BUILD/tracker/zephyr/zephyr.exe
TMP=$(mktemp -d -t flogtest-XXXXXX)
trap 'rm -rf "$TMP"' EXIT
FLASH=$TMP/flash.bin

fail() { echo "flogtest FAIL: $1"; exit 1; }

run() { # run <stop_at_s>  (stdin = shell script, stdout = full console)
  timeout 90 env TRACKER_DEVICE_ID=flog-test "$EXE" \
    -flash="$FLASH" -uart_stdinout -stop_at="$1" 2>&1 || true
}

# 1: fresh flash — recorder formats the ring and records.
out=$({ sleep 2; printf 'flog mark persistence probe\nflog stats\n'; sleep 2; } | run 6)
grep -q "flight recorder up" <<<"$out" || fail "recorder did not init on fresh flash"
grep -q "state: recording"   <<<"$out" || fail "recorder not recording"

# 2: same flash file — a reboot must not lose records, and each boot
#    leaves a marker (the post-mortem boundary).
out=$({ sleep 2; printf 'flog dump 200\n'; sleep 2; } | run 6)
grep -q "mark: persistence probe" <<<"$out" || fail "records lost across reboot"
[ "$(grep -c '=== boot ===' <<<"$out")" -ge 2 ] || fail "boot markers missing"

# 3: overfill the 64 KB ring — rotation must reclaim the oldest sector
#    while the newest records and the recorder itself survive.
out=$({ sleep 2
        for i in $(seq 1 800); do
          printf 'flog mark filler %04d aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n' "$i"
        done
        sleep 4; printf 'flog stats\nflog dump 20\n'; sleep 2; } | run 45)
grep -q "ring: 2 sectors"  <<<"$out" || fail "stats missing after overfill"
grep -qE "filler 07[0-9][0-9]" <<<"$out" || fail "newest records missing after rotate"
grep -q "state: recording" <<<"$out" || fail "recorder not recording after rotate"
if grep -q "OFFLINE" <<<"$out"; then fail "recorder went OFFLINE during rotate"; fi
total=$(grep -oE -- '-- [0-9]+ of [0-9]+ lines' <<<"$out" | grep -oE '[0-9]+ lines' | grep -oE '[0-9]+')
[ -n "$total" ] && [ "$total" -lt 800 ] || fail "ring never rotated (holds $total lines)"

# 4: erase empties the ring and the recorder keeps going.
out=$({ sleep 2; printf 'flog erase\nflog dump 5\nflog stats\n'; sleep 2; } | run 6)
grep -q "erase: 0" <<<"$out" || fail "erase errored"
grep -q -- "-- 0 of 0 lines" <<<"$out" || fail "ring not empty after erase"
grep -q "state: recording" <<<"$out" || fail "recorder dead after erase"

echo "flogtest OK: init, persistence-across-reboot, rotation, erase"
