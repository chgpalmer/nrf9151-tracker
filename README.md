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
make setup-host                # server tools: Python deps (aiocoap, protobuf)
make build                     # build the default app (tracker), non-secure
make windows-usb-passthrough   # attach the J-Link into WSL (usbipd)
make flash
make uart                      # stream the serial console
make demo                      # run local CoAP server + web map + sim
./smoke.sh                     # check both flows: make build + make demo
make setup-webtest             # dev only: playwright + headless Chromium
make webtest                   # execute the web UI headlessly; fail on any JS error
```

`setup.sh` installs the bootstrap apt packages, creates `nrf9151-tracker-ws/`, sets up a
venv, runs `west init -m <this repo>`, then `make setup-zephyr` (system build deps +
west update + minimal Zephyr SDK). The workspace is created **under the current
directory** — override with `WS=~/my-path bash <(curl ...)`. Idempotent.

**Supported host: Ubuntu 24.04 LTS (Python 3.12).** The NCS modules pin exact
Python package versions (nanopb pins `grpcio-tools==1.68.0`, nrf pins `hidapi`)
that ship prebuilt wheels only up to Python 3.13 — on a newer Python (e.g. 3.14
on Ubuntu 26.04) pip falls back to building them from source and `make
setup-zephyr` dies mid-pip. Stick to an LTS release.

## Configuration (`.env`)

Host-specific values are **not** committed. Copy the template and edit:

```sh
cp env.template .env
```

| Variable | Used by | Purpose |
|---|---|---|
| `TRACKER_SERVER_HOST` | `make build APP=tracker` | CoAP server the physical device sends to over LTE (`coap://HOST:PORT/obs`, UDP) — set to your server's hostname/IP. |
| `TRACKER_SERVER_PORT` | `make build APP=tracker` | CoAP UDP port (default `5683`). |
| `CADDY_DOMAIN` | `make serve` | Domain for auto-HTTPS; empty serves plain HTTP by IP. |

Precedence is **CLI > `.env` > built-in default**, e.g. `make build APP=tracker
TRACKER_SERVER_HOST=noil.uk` overrides `.env` for one build. `.env` is
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
(80/443 TCP + 5683 UDP) automatically.

```sh
make serve                      # public HTTP by IP (works before DNS is set up)
make serve CADDY_DOMAIN=noil.uk # public HTTPS: Caddy gets a Let's Encrypt cert
```

- **HTTP by IP** (default, `CADDY_DOMAIN` unset): reach the map at
  `http://<public-ip>/`. Use this while DNS is still propagating.
- **HTTPS by domain**: set `CADDY_DOMAIN` once the domain's A record points at
  this host. Caddy fetches + auto-renews the cert; no manual certificate steps.

Caddy runs as a persistent service, so HTTPS stays up across `Ctrl-C` and reboots
(`Ctrl-C` on `make serve` only stops the app + CoAP ingest). Requirements:
the domain must resolve to this host, and OCI/cloud security lists must allow
**80** (HTTP + ACME), **443** (HTTPS), and **UDP 5683** (CoAP for real devices).

## Wire protocol

Reports go as **protobuf in non-confirmable CoAP POSTs over UDP**
(`coap://server:5683/obs`). The schema lives in one place —
`proto/tracker.proto` (+ `tracker.options` for nanopb) — and both sides consume
generated code from it: the firmware encodes with
[nanopb](https://github.com/nanopb/nanopb) (a west module of this workspace),
the server decodes with the committed `server/tracker_pb2.py`, so the two cannot
drift apart silently.

```sh
vi proto/tracker.proto         # edit the schema (never the generated code)
make proto                     # regen apps/tracker/src/proto/ + server/tracker_pb2.py
```

The unit of GPS data is a **track segment**: one absolute anchor fix plus packed
delta arrays (`dlat`/`dlon`/`spd_dms` as zigzag varints) at a fixed cadence, so
per-point time is implicit (anchor age + i×dt) and a moving point costs ~5–7 B
instead of ~33 B. Speed is the GNSS Doppler measurement, sent per point — NOT
derived from position deltas on the server, which carries ~8 km/h of jitter at
1 Hz and is unusable at higher rates. A stationary run is zero-deltas (~3 B per
point). The firmware caps a segment at 50 points *before* encoding (packed
arrays are written field-by-field, so a mid-encode cap would desync them) and
that keeps every datagram under the ~512 B safe cellular MTU; longer flushes
just emit several datagrams. Adding a per-point field later (e.g. lean angle) is
a new packed array — old decoders skip it, no version bump.

### Data budget (against a 10 MB/month IoT plan)

Measured wire sizes (payload + 47 B UDP/IP + CoAP per datagram, IPv4):

| Scenario | Per datagram | 30 days |
|---|---|---|
| 1 Hz driving, flushed every 60 s | ~395 B (60 pts, ~6.6 B/pt) | — |
| Cell keepalive | ~86 B | — |
| Mixed month: 2 h/day 1 Hz driving + keepalives /15 min | — | **~1.7 MB** |
| 10 Hz race-telemetry burst | ~4.8 B/pt | ~150 KB per hour |

The levers, in measured order of impact: flush interval (2.5×), delta-vs-full
representation (4×), per-datagram overhead, and only then the encoding container
(<5% between protobuf / CBOR / hand-packed — protobuf was chosen for hot-path
extensibility, not bytes). Carrier accounting may round per-session; treat these
as floors. Reports carry the RFC 7967 No-Response option, and the firmware sets
`SO_RAI` (`RAI_LAST`) before each send — with nothing coming back, the modem
releases the radio connection immediately after the datagram, which is also what
keeps GNSS acquisition windows intact. Note the ingest is **anonymous and
unauthenticated** (as the MQTT broker was); DTLS-PSK via the modem's offloaded
sockets is the upgrade path when that starts to matter.

## LEDs (state at a glance)

| LED | Meaning | Off | Blink | Solid |
|---|---|---|---|---|
| 1 | LTE | radio off on purpose (exclusive) | attaching | registered |
| 2 | GPS | GNSS not running | acquiring | **fix current** |
| 3 | TX | — | — | ~½ s pulse per successful send |
| 4 | help | normal | CELL_LOOP (retrying on sight) | GNSS_EXCLUSIVE (radio dark) |

Glance rules: **LED2 solid = safe to start moving** (hot re-fix from here is
seconds). LED4 lit = GPS is struggling, and explains a dark LED1. LED3
flickering every ~50 s while moving = track segments are flowing.

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
    env.template        copy to .env for host-specific config (server, domain)
    proto/
      tracker.proto     wire schema — single source for fw + server codecs
      tracker.options   nanopb field options (callbacks, not fixed arrays)
    mk/
      fw.mk             firmware/Zephyr targets (build, flash, debug, sim, proto)
      server.mk         server/host services (CoAP ingest, web, Caddy)
    apps/<name>/        CMakeLists.txt, prj.conf, src/main.c
    server/             Python CoAP ingest (aiocoap + protobuf), FastAPI web map
    scripts/
      setup-system.sh   installs the Zephyr apt build deps
      setup-tools.sh    installs nrfutil / usbip / tio
      windows/
        passthrough.ps1 usbipd bind + attach (the only Windows-side script)
        uart.ps1        PowerShell serial fallback (not needed in the native flow)
```
