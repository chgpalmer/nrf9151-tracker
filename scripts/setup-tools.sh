#!/usr/bin/env bash
# Install native Linux host tools for the nRF9151-DK workflow (run inside WSL).
#   - tio          serial console
#   - usbip        USB/IP client, to receive the J-Link attached from Windows
#   - nrfutil      Nordic CLI + `device` command (west's flash/debug runner)
#   - J-Link       SEGGER software; provides libjlinkarm.so that nrfutil needs
#                  to actually drive SWD (flash/debug). Without it flashing fails.
# Idempotent: skips anything already present.
set -eu

echo "== setup-tools: native host tools =="

# On WSL2 the `usbip` client isn't the stock `usbip` package -- it ships in
# linux-tools-virtual (+ hwdata for VID/PID names). Note: the /usr/bin/usbip that
# comes with it is a kernel-version wrapper that fails on the WSL kernel
# ("usbip not found for kernel ..."); the real binary lives under
# /usr/lib/linux-tools/<ver>/usbip, which we expose via update-alternatives.
usbip_works() { usbip version >/dev/null 2>&1; }

need_apt=()
command -v tio >/dev/null 2>&1 || need_apt+=(tio)
# usbutils: `lsusb`, which passthrough.ps1 tells you to run to confirm the attach.
command -v lsusb >/dev/null 2>&1 || need_apt+=(usbutils)
usbip_works || need_apt+=(linux-tools-virtual hwdata)

if [ "${#need_apt[@]}" -gt 0 ]; then
  echo "-- apt install: ${need_apt[*]}"
  sudo apt-get update -qq
  sudo apt-get install -y "${need_apt[@]}"
else
  echo "-- tio, usbip already present"
fi

if ! usbip_works; then
  ubin="$(ls /usr/lib/linux-tools/*/usbip 2>/dev/null | tail -1 || true)"
  if [ -n "$ubin" ]; then
    echo "-- registering usbip alternative -> $ubin"
    sudo update-alternatives --install /usr/local/bin/usbip usbip "$ubin" 20
    hash -r
  else
    echo "   WARNING: usbip client not found under /usr/lib/linux-tools/*/ -- attach may still work with usbipd-win 4.x"
  fi
fi

# nrfutil: single static binary from Nordic. Install to ~/.local/bin.
BIN="$HOME/.local/bin"
mkdir -p "$BIN"
if ! command -v nrfutil >/dev/null 2>&1 && [ ! -x "$BIN/nrfutil" ]; then
  echo "-- downloading nrfutil"
  curl -fsSL -o "$BIN/nrfutil" \
    https://developer.nordicsemi.com/.pc-tools/nrfutil/x64-linux/nrfutil
  chmod +x "$BIN/nrfutil"
else
  echo "-- nrfutil already present"
fi

NRFUTIL="$(command -v nrfutil || echo "$BIN/nrfutil")"
echo "-- installing nrfutil device command"
"$NRFUTIL" install device || echo "   (nrfutil install device failed; re-run after PATH includes $BIN)"

# SEGGER J-Link: nrfutil's flash/debug path loads libjlinkarm.so via the
# JLinkARM DLL. Install the Linux .deb if that library isn't present.
if ! ls /opt/SEGGER/JLink*/libjlinkarm.so* >/dev/null 2>&1 \
   && ! ls /usr/lib*/libjlinkarm.so* >/dev/null 2>&1; then
  echo "-- installing SEGGER J-Link (for libjlinkarm.so)"
  JL_DEB="$HOME/.cache/JLink_Linux_x86_64.deb"
  mkdir -p "$HOME/.cache"
  # SEGGER gates the download behind a license-accept POST form.
  if curl -fsSL -o "$JL_DEB" \
       -d "accept_license_agreement=accepted" \
       "https://www.segger.com/downloads/jlink/JLink_Linux_x86_64.deb"; then
    sudo apt-get install -y "$JL_DEB" || sudo dpkg -i "$JL_DEB" || true
  else
    echo "   WARNING: could not download J-Link automatically."
    echo "   Get the 64-bit DEB from https://www.segger.com/downloads/jlink/ and:"
    echo "     sudo apt-get install -y ~/Downloads/JLink_Linux_x86_64.deb"
  fi
else
  echo "-- SEGGER J-Link (libjlinkarm.so) already present"
fi

# Serial ports enumerate as root:dialout (0660). Add the user to `dialout` so
# `make uart` (tio) can open /dev/ttyACM* without sudo.
if ! id -nG "$USER" | tr ' ' '\n' | grep -qx dialout; then
  echo "-- adding $USER to dialout group"
  sudo usermod -aG dialout "$USER"
  echo "   NOTE: log out/in (or run 'newgrp dialout') for the new group to take effect."
else
  echo "-- $USER already in dialout"
fi

case ":$PATH:" in
  *":$BIN:"*) ;;
  *) echo "NOTE: add $BIN to PATH  (e.g. echo 'export PATH=\$HOME/.local/bin:\$PATH' >> ~/.bashrc)";;
esac

echo "== setup-tools done =="
