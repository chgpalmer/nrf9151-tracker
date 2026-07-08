# nrf9151-tracker

Firmware and server for the [Nordic nRF9151-DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)
(nRF9151 SiP, Arm Cortex-M33, LTE-M / NB-IoT / GNSS). A freestanding, multi-app
Zephyr/[NCS](https://github.com/nrfconnect/sdk-nrf) **west manifest repo** — the repo
*is* the manifest, so contributors clone one thing and pull a trimmed subset of the SDK
(see `west.yml` `name-allowlist`), not the whole world.

Linux-native workflow: build, flash, and debug all run inside WSL with native tools. The
board's USB is brought across the WSL2 boundary once with
[usbipd-win](https://github.com/dorssel/usbipd-win); after that everything is
`west` + `nrfutil` + `tio`.

## Quick start

```sh
curl -fsSL https://raw.githubusercontent.com/chgpalmer/nrf9151-tracker/main/setup.sh | bash
cd ~/nrf9151-tracker-ws/nrf9151-tracker
make setup-tools               # host tools: nrfutil, usbip, tio
make setup-host                # server tools: mosquitto, Python deps
make build                     # build the default app (hello), secure
make windows-usb-passthrough   # attach the J-Link into WSL (usbipd)
make flash
make uart                      # -> "Hello from nRF9151-DK!"
make demo                      # run local MQTT broker + server + sim
```

`setup.sh` creates `~/nrf9151-tracker-ws/`, sets up a venv, runs `west init -m <this repo>`, then
`make setup-zephyr` (west update + minimal Zephyr SDK). The workspace dir is
`$WS` (default `~/nrf9151-tracker-ws`) — override with `WS=~/my-path bash <(curl ...)`. Idempotent.

## USB passthrough (one-time)

`make windows-usb-passthrough` runs `scripts/windows/passthrough.ps1`, which uses
`usbipd` to `bind` + `attach` the SEGGER J-Link (VID 1366) into WSL. The first `bind`
needs an **admin** PowerShell on Windows; if usbipd isn't installed, run once:

```powershell
winget install usbipd
```

After attaching, the DK appears in WSL as `/dev/ttyACM0` (console) and `/dev/ttyACM1`.

## Make targets

| Target | What it does |
|---|---|
| `make setup-zephyr` | venv + `west update` + minimal Zephyr SDK (arm only) |
| `make setup-tools` | install host tools: `nrfutil`, `usbip`, `tio` |
| `make windows-usb-passthrough` | bind + attach the J-Link into WSL via usbipd |
| `make build` | build `APP` for `BOARD` |
| `make flash` | program the board (`west flash --runner nrfutil`) |
| `make recover` | unlock a readback-protected chip (some /ns builds lock it) |
| `make uart` | stream the serial console with `tio` |
| `make gdb` | `west debugserver` + `arm-zephyr-eabi-gdb` |
| `make clean` | remove the build dir for `APP` |

## Variables

| Var | Default | Notes |
|---|---|---|
| `APP` | `hello` | app under `apps/` (`hello`, `gnss`, …) |
| `BOARD` | `nrf9151dk/nrf9151` | secure; use `nrf9151dk/nrf9151/ns` for modem/GNSS |
| `PORT` | `/dev/ttyACM0` | serial port for `make uart` |
| `BAUD` | `115200` | serial baud |
| `RUNNER` | `nrfutil` | west flash/debug runner (`nrfutil`\|`nrfjprog`\|`openocd`) |

Example:

```sh
make build flash APP=gnss BOARD=nrf9151dk/nrf9151/ns
```

## Apps

- `apps/hello` — secure `printk` loop. Verified working.
- `apps/gnss` — GNSS sample; needs the `/ns` board target.

## Layout

```
nrf9151-tracker/
  west.yml            self-manifest; imports sdk-nrf v3.4.0 (trimmed allowlist)
  Makefile            the workflow (run inside WSL)
  setup.sh            one-line bootstrap (curl | bash)
  mk/
    fw.mk             firmware targets (build, flash, debug)
    server.mk         server targets (broker, ingest, web, sim)
  apps/<name>/        CMakeLists.txt, prj.conf, src/main.c
  server/             Python MQTT ingest, FastAPI web map
  scripts/
    setup-tools.sh    installs nrfutil / usbip / tio
    windows/
      passthrough.ps1 usbipd bind + attach (the only Windows-side script)
      uart.ps1        PowerShell serial fallback (not needed in the native flow)
```
