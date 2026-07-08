#!/usr/bin/env bash
# nrf9151-tracker bootstrap. Creates the workspace directory, clones the repo, and
# pulls all SDK deps. Run from anywhere -- no need to mkdir first:
#
#   curl -fsSL https://raw.githubusercontent.com/chgpalmer/nrf9151-tracker/main/setup.sh | bash
#   cd nrf9151-tracker-ws/nrf9151-tracker && make setup setup-host
#
# Or override the workspace location:
#   WS=~/my-ws bash <(curl -fsSL ...)
#
# Idempotent: re-running in an existing workspace just brings it up to date.
set -eu

REPO_URL="${REPO_URL:-https://github.com/chgpalmer/nrf9151-tracker}"
MANIFEST="${MANIFEST:-west.yml}"
WS="${WS:-$HOME/nrf9151-tracker-ws}"
REPO_DIR="$WS/nrf9151-tracker"

echo "== nrf9151-tracker bootstrap -> $WS =="
mkdir -p "$WS"

# 1. west init (clones nrf9151-tracker as the manifest repo) if not already done
if [ ! -d "$WS/.west" ]; then
  echo "-- creating venv for west"
  python3 -m venv "$WS/.venv"
  "$WS/.venv/bin/pip" install --quiet --upgrade pip west

  echo "-- west init -m $REPO_URL"
  "$WS/.venv/bin/west" init -m "$REPO_URL" --mf "$MANIFEST" "$WS"
else
  echo "-- workspace already initialized"
fi

# 2. Delegate all setup to the Makefile (venv creation, west update, SDK, etc.)
echo "-- make setup-zephyr (SDK + firmware toolchain)"
make -C "$REPO_DIR" setup-zephyr

cat <<EOF

== bootstrap complete ==
  cd $REPO_DIR
  make setup-tools               # host tools: nrfutil, usbip, tio
  make setup-host                # server tools: mosquitto, Python deps
  make build                     # build firmware
  make demo                      # run local server + sim
EOF
