#!/usr/bin/env bash
# Install the system packages Zephyr needs to build (the list from the official
# Zephyr getting-started docs, plus build-essential).
#
# Idempotent per-package: we ask dpkg what's actually missing rather than using
# a single canary binary. A canary (e.g. "is cmake on PATH?") wrongly skips the
# whole install when the canary happens to be present but a sibling package
# isn't -- which is exactly how a box ends up with cmake but no build-essential.
set -eu

PKGS=(
  build-essential
  git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget
  python3-dev python3-venv python3-pip python3-tk
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1
)

if ! command -v apt-get >/dev/null 2>&1; then
  echo "setup-system: no apt-get -- install these yourself: ${PKGS[*]}"
  exit 0
fi

missing=()
for p in "${PKGS[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done

if [ "${#missing[@]}" -eq 0 ]; then
  echo "System packages already installed."
  exit 0
fi

echo "Installing system build dependencies: ${missing[*]}"
sudo apt-get update -qq
sudo apt-get install --no-install-recommends -y "${missing[@]}"
echo "System packages installed."
