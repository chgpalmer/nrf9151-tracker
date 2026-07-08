# nRF9151-DK application workflow -- Linux-native (run INSIDE WSL).
# Build + flash + debug all use native Linux tools. USB reaches WSL via usbipd
# (see `make windows-usb-passthrough`). Flash/debug go through `west`, which
# dispatches to Nordic's nrfutil runner; serial console uses `tio`.
#
#   make                        show this help
#   make setup-zephyr           create venv, west update (pull SDK deps), install toolchain
#   make setup-tools            install native host tools (nrfutil, usbip, tio)
#   make windows-usb-passthrough  bind+attach the J-Link into WSL via usbipd (Windows)
#   make build                  build APP for BOARD
#   make flash                  program the board (west -> nrfutil)
#   make recover                unlock a readback-protected chip
#   make uart                   stream the serial console (tio)
#   make gdb                    west debugserver + arm-zephyr-eabi-gdb
#   make clean                  remove this app's build dir
#
# Variables (override on the command line):
#   APP=hello                 app under apps/  (hello, gnss, ...)
#   BOARD=nrf9151dk/nrf9151   board target; use nrf9151dk/nrf9151/ns for modem/GNSS
#   PORT=/dev/ttyACM0         serial port for `make uart`
#   BAUD=115200               serial baud
#   RUNNER=nrfutil            west flash/debug runner (nrfutil|nrfjprog|openocd)

.RECIPEPREFIX := >
.DEFAULT_GOAL := help
.PHONY: help setup-zephyr setup-tools setup-host windows-usb-passthrough build flash recover uart gdb clean sim run-sim broker

APP    ?= hello
BOARD  ?= nrf9151dk/nrf9151
PORT   ?= /dev/ttyACM0
BAUD   ?= 115200
RUNNER ?= nrfutil

APP_DIR := apps/$(APP)
BUILD   := build/$(APP)

REPO := $(CURDIR)
WS   := $(abspath $(CURDIR)/..)
VENV := $(WS)/.venv
WEST := $(VENV)/bin/west
SDK  := $(HOME)/zephyr-sdk-1.0.1
GDB  := $(SDK)/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb

# nrfutil lives in ~/.local/bin; add it to PATH for all targets
export PATH := $(HOME)/.local/bin:$(PATH)

# Trust the system CA bundle for TLS-inspecting proxies
CA := /etc/ssl/certs/ca-certificates.crt
export REQUESTS_CA_BUNDLE := $(CA)
export SSL_CERT_FILE := $(CA)
export CURL_CA_BUNDLE := $(CA)

help:
> @echo "nRF9151-DK workflow (Linux-native, run inside WSL)"
> @echo ""
> @echo "  make setup-zephyr             venv + west update + install toolchain"
> @echo "  make setup-tools              install host tools (nrfutil, usbip, tio)"
> @echo "  make windows-usb-passthrough  attach the J-Link into WSL (usbipd)"
> @echo "  make build                    build APP for BOARD"
> @echo "  make flash                    flash via west ($(RUNNER))"
> @echo "  make recover                  unlock a readback-protected chip"
> @echo "  make uart                     stream serial console (tio)"
> @echo "  make gdb                      west debugserver + arm-gdb"
> @echo "  make clean                    remove build dir for APP"
> @echo ""
> @echo "  make setup-host               install mosquitto + paho-mqtt (for sim workflow)"
> @echo "  make sim                      build tracker as native Linux process (no DK needed)"
> @echo "  make run-sim                  run the sim binary (connects to localhost mosquitto)"
> @echo "  make broker                   start mosquitto + python subscriber"
> @echo ""
> @echo "Current: APP=$(APP)  BOARD=$(BOARD)  PORT=$(PORT)  BAUD=$(BAUD)  RUNNER=$(RUNNER)"
> @echo "Apps available: $$(ls apps 2>/dev/null | tr '\n' ' ')"
> @echo ""
> @echo "Examples:"
> @echo "  make build APP=gnss BOARD=nrf9151dk/nrf9151/ns"
> @echo "  make flash APP=gnss BOARD=nrf9151dk/nrf9151/ns"
> @echo "  make uart PORT=/dev/ttyACM0"
> @echo "  make sim && make run-sim      hardware-free test loop"

setup-zephyr:
> @mkdir -p $(HOME)/.cache/ztmp
> @test -d $(VENV) || python3 -m venv $(VENV)
> $(VENV)/bin/pip install --quiet --upgrade pip west
> @test -d $(WS)/.west || $(WEST) init -l $(REPO)
> cd $(WS) && $(WEST) update --narrow -o=--depth=1
> @test -d $(SDK) || (cd $(WS) && TMPDIR=$(HOME)/.cache/ztmp $(WEST) sdk install -t arm-zephyr-eabi --install-base $(HOME))
> @echo "setup complete."

setup-tools:
> @bash scripts/setup-tools.sh

setup-host:
> sudo apt-get install -y mosquitto mosquitto-clients
> $(VENV)/bin/pip install --quiet paho-mqtt
> @echo "Host tools ready: mosquitto broker + paho-mqtt subscriber"

windows-usb-passthrough:
> @powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$$(wslpath -w scripts/windows/passthrough.ps1)"
> @echo "If /dev/ttyACM* still missing, install usbipd on Windows (admin): winget install usbipd"

build:
> @test -d $(APP_DIR) || { echo "No app '$(APP)' in apps/ ($$(ls apps 2>/dev/null | tr '\n' ' '))"; exit 1; }
> $(WEST) build -p auto -b $(BOARD) $(APP_DIR) -d $(BUILD)
> @echo "built $(APP) for $(BOARD)"

flash:
> $(WEST) flash -d $(BUILD) --runner $(RUNNER)

recover:
> $(WEST) flash -d $(BUILD) --runner $(RUNNER) --recover

uart:
> stty -F $(PORT) $(BAUD) raw -echo -echoe -echok -echoctl -echoke
> cat $(PORT)

gdb:
> @elf=$$(find $(BUILD) -name zephyr.elf 2>/dev/null | head -1); \
>  test -n "$$elf" || { echo "build first"; exit 1; }; \
>  echo "starting west debugserver ($(RUNNER)) on :3333..."; \
>  $(WEST) debugserver -d $(BUILD) --runner $(RUNNER) & \
>  sleep 2; \
>  $(GDB) "$$elf" -ex "target extended-remote :3333" -ex "monitor reset halt"

clean:
> rm -rf $(BUILD)

# ── native_sim targets ────────────────────────────────────────────────────────
# Uses NSOS (offloaded sockets) — no TAP, no root, no net-setup.
# The sim binary connects directly to the host's network stack via localhost.
# Nordic modem/GNSS stubs in src/lib/nrf_modem_mock/ fill all missing symbols.
#
# Workflow:
#   make broker      # start mosquitto + python subscriber (background)
#   make sim         # build (once)
#   make run-sim     # run — publishes to localhost:1883 every 10s
SIM_BUILD   := build/tracker-sim
BROKER_HOST ?= localhost
MQTT_PORT   ?= 1883
MQTT_TOPIC  ?= tracker/#
SERVER_VENV := $(WS)/.venv

sim:
> $(WEST) build -p auto -b native_sim/native/64 apps/tracker -d $(SIM_BUILD) \
>   -DCONFIG_TRACKER_MQTT_BROKER_HOST=\"$(BROKER_HOST)\"
> @echo "Sim built: $(SIM_BUILD)/tracker/zephyr/zephyr.exe"
> @echo "Run: make run-sim"

run-sim:
> @test -f $(SIM_BUILD)/tracker/zephyr/zephyr.exe || { echo "Run 'make sim' first"; exit 1; }
> $(SIM_BUILD)/tracker/zephyr/zephyr.exe

broker:
> @which mosquitto >/dev/null 2>&1 || { echo "Install mosquitto: sudo apt install mosquitto"; exit 1; }
> @pip install --quiet paho-mqtt 2>/dev/null || $(SERVER_VENV)/bin/pip install --quiet paho-mqtt
> @echo "Starting mosquitto on port $(MQTT_PORT)..."
> @mosquitto -p $(MQTT_PORT) &
> @sleep 1
> @echo "Starting subscriber on topic '$(MQTT_TOPIC)'..."
> python3 server/broker.py --host $(BROKER_HOST) --port $(MQTT_PORT) --topic "$(MQTT_TOPIC)"
