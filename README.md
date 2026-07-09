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
cd nrf9151-tracker-ws/nrf9151-tracker
make setup-tools               # host tools: nrfutil, usbip, tio
make setup-host                # server tools: mosquitto, Python deps
make build                     # build the default app (tracker), non-secure
make windows-usb-passthrough   # attach the J-Link into WSL (usbipd)
make flash
make uart                      # stream the serial console
make demo                      # run local MQTT broker + server + sim
./smoke.sh                     # check both flows: make build + make demo
```

`setup.sh` installs the bootstrap apt packages, creates `nrf9151-tracker-ws/`, sets up a
venv, runs `west init -m <this repo>`, then `make setup-zephyr` (system build deps +
west update + minimal Zephyr SDK). The workspace is created **under the current
directory** — override with `WS=~/my-path bash <(curl ...)`. Idempotent.

## Configuration (`.env`)

Host-specific values are **not** committed. Copy the template and edit:

```sh
cp env.template .env
```

| Variable | Used by | Purpose |
|---|---|---|
| `TRACKER_BROKER_HOST` | `make build APP=tracker` | MQTT broker the physical device connects to over LTE — set to your server's hostname/IP. Default `test.mosquitto.org`. |
| `TRACKER_BROKER_PORT` | `make build APP=tracker` | Broker port (default `1883`). |
| `CADDY_DOMAIN` | `make serve` | Domain for auto-HTTPS; empty serves plain HTTP by IP. |

Precedence is **CLI > `.env` > built-in default**, e.g. `make build APP=tracker
TRACKER_BROKER_HOST=noil.uk` overrides `.env` for one build. `.env` is
git-ignored; `env.template` is the committed reference.

## USB passthrough (one-time)

`make windows-usb-passthrough` runs `scripts/windows/passthrough.ps1`, which uses
`usbipd` to `bind` + `attach` the SEGGER J-Link (VID 1366) into WSL. The first `bind`
needs an **admin** PowerShell on Windows; if usbipd isn't installed, run once:

```powershell
winget install usbipd
```

After attaching, the DK appears in WSL as `/dev/ttyACM0` (console) and `/dev/ttyACM1`.
Confirm with `nrfutil device list` — it should report `Product J-Link`, `Board version PCA10171`.

**If `bind` succeeds but `attach` fails** with `The VBoxUsbMon driver is not correctly
installed`, usbipd's kernel driver never registered — usually because an unofficial CI
build got installed instead of a tagged release. Check with `sc.exe query VBoxUsbMon`;
if the service is absent, reinstall from the release:

```powershell
winget uninstall dorssel.usbipd-win
winget install --id dorssel.usbipd-win -e
```

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
| `APP` | `tracker` | app under `apps/` (`tracker`, `hello`, `gnss`) |
| `BOARD` | `nrf9151dk/nrf9151/ns` | non-secure; needed for modem/GNSS. `nrf9151dk/nrf9151` is the secure target (`hello` only) |
| `PORT` | `/dev/ttyACM0` | serial port for `make uart` |
| `BAUD` | `115200` | serial baud |
| `RUNNER` | `nrfutil` | west flash/debug runner (`nrfutil`\|`nrfjprog`\|`openocd`) |

Example:

```sh
make build flash APP=gnss BOARD=nrf9151dk/nrf9151/ns
```

## Serving the web map publicly

`make serve` runs the full stack behind [Caddy](https://caddyserver.com/) as a
reverse proxy. The FastAPI app binds `127.0.0.1` only; Caddy (a systemd service)
is the sole public face. First run installs Caddy and opens the firewall
(80/443/1883) automatically.

```sh
make serve                      # public HTTP by IP (works before DNS is set up)
make serve CADDY_DOMAIN=noil.uk # public HTTPS: Caddy gets a Let's Encrypt cert
```

- **HTTP by IP** (default, `CADDY_DOMAIN` unset): reach the map at
  `http://<public-ip>/`. Use this while DNS is still propagating.
- **HTTPS by domain**: set `CADDY_DOMAIN` once the domain's A record points at
  this host. Caddy fetches + auto-renews the cert; no manual certificate steps.

Caddy runs as a persistent service, so HTTPS stays up across `Ctrl-C` and reboots
(`Ctrl-C` on `make serve` only stops the app + broker + ingest). Requirements:
the domain must resolve to this host, and OCI/cloud security lists must allow
**80** (HTTP + ACME), **443** (HTTPS), and **1883** (MQTT for real devices).

## Apps

- `apps/hello` — secure `printk` loop. Verified working.
- `apps/gnss` — GNSS sample; needs the `/ns` board target.

## Layout

```
nrf9151-tracker-ws/          workspace root (created by setup.sh)
  .venv/                   python venv holding west
  .west/                   west workspace marker
  zephyr/  nrf/  ...       SDK trees, cloned by `west update`
  nrf9151-tracker/         this repo (== manifest.self.path in west.yml)
    west.yml            self-manifest; imports sdk-nrf v3.4.0 (trimmed allowlist)
    Makefile            shared config + cross-cutting entry points (demo, setup-venv)
    setup.sh            one-line bootstrap (curl | bash)
    smoke.sh            smoke test: make build + make demo end-to-end
    env.template        copy to .env for host-specific config (broker, domain)
    mk/
      fw.mk             firmware/Zephyr targets (build, flash, debug, sim)
      server.mk         server/host services (broker, ingest, web, Caddy)
    apps/<name>/        CMakeLists.txt, prj.conf, src/main.c
    server/             Python MQTT ingest, FastAPI web map
    scripts/
      setup-system.sh   installs the Zephyr apt build deps
      setup-tools.sh    installs nrfutil / usbip / tio
      windows/
        passthrough.ps1 usbipd bind + attach (the only Windows-side script)
        uart.ps1        PowerShell serial fallback (not needed in the native flow)
```
