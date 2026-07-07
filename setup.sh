#!/usr/bin/env bash
# nrf9151-fw bootstrap. Creates the workspace directory, clones the repo, and
# pulls all SDK deps. Run from anywhere -- no need to mkdir first:
#
#   curl -fsSL https://raw.githubusercontent.com/chgpalmer/nrf9151-fw/main/setup.sh | bash
#   cd nrf9151-fw && make build
#
# Or clone an existing workspace name:
#   WS=~/my-ws bash <(curl -fsSL ...)
#
# Idempotent: re-running in an existing workspace just brings it up to date.
set -eu

REPO_URL="${REPO_URL:-https://github.com/chgpalmer/nrf9151-fw}"
MANIFEST="${MANIFEST:-west.yml}"
WS="${WS:-$HOME/nrf9151-ws}"
VENV="$WS/.venv"

echo "== nrf9151-fw bootstrap -> $WS =="
mkdir -p "$WS"

# 1. Python venv + west
if [ ! -d "$VENV" ]; then
  echo "-- creating venv"
  python3 -m venv "$VENV"
fi
"$VENV/bin/pip" install --quiet --upgrade pip west

WEST="$VENV/bin/west"

# 2. west init (clones nrf9151-fw as the manifest repo) + pull deps
if [ ! -d "$WS/.west" ]; then
  echo "-- west init -m $REPO_URL"
  "$WEST" init -m "$REPO_URL" --mf "$MANIFEST" "$WS"
fi

# 3. Delegate the heavy lifting (west update + SDK) to the in-repo target.
echo "-- make setup-zephyr"
make -C "$WS/nrf9151-fw" setup-zephyr

cat <<EOF

== bootstrap complete ==
  cd $WS/nrf9151-fw
  make setup-tools               # host tools: nrfutil, usbip, tio
  make build                     # build the default app (hello)
  make windows-usb-passthrough   # attach the J-Link into WSL (usbipd)
  make flash
  make uart
EOF
