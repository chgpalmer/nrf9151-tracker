#!/usr/bin/env bash
# nrf9151-tracker bootstrap. Creates the workspace directory, clones the repo, and
# pulls all SDK deps. Run from anywhere -- no need to mkdir first:
#
#   curl -fsSL https://raw.githubusercontent.com/chgpalmer/nrf9151-tracker/main/setup.sh | bash
#   cd nrf9151-tracker-ws/nrf9151-tracker && make setup-tools setup-host
#
# The workspace is created under the CURRENT directory. Override the location:
#   WS=~/my-ws bash <(curl -fsSL ...)
#
# Idempotent: re-running in an existing workspace just brings it up to date.
set -eu

REPO_URL="${REPO_URL:-https://github.com/chgpalmer/nrf9151-tracker}"
MANIFEST="${MANIFEST:-west.yml}"
WS="${WS:-$PWD/nrf9151-tracker-ws}"
# Must match `manifest.self.path` in west.yml -- that is where west clones the
# manifest repo, and it is the directory the Makefile is invoked from below.
REPO_DIR="$WS/nrf9151-tracker"

echo "== nrf9151-tracker bootstrap -> $WS =="

# 0. Bootstrap packages. Enough to create a venv, run git, and call make; the
#    full Zephyr build-dep list is installed later by `make setup-system`, which
#    we cannot reach until the repo is cloned. Keep this list minimal.
if command -v apt-get >/dev/null 2>&1; then
  missing=()
  for p in git make python3-venv python3-pip; do
    dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
  done
  if [ "${#missing[@]}" -gt 0 ]; then
    echo "-- installing bootstrap packages: ${missing[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y "${missing[@]}"
  else
    echo "-- bootstrap packages present"
  fi
fi

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

if [ ! -d "$REPO_DIR" ]; then
  echo "ERROR: expected the manifest repo at $REPO_DIR but it is not there."
  echo "       Contents of $WS: $(ls "$WS" 2>/dev/null | tr '\n' ' ')"
  echo "       If this workspace predates the nrf9151-fw -> nrf9151-tracker rename,"
  echo "       delete $WS and re-run this script."
  exit 1
fi

# 2. Delegate all setup to the Makefile (system pkgs, west update, SDK, etc.)
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
