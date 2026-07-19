# nRF9151-DK tracker — top-level Makefile.
#
# Split across:
#   mk/fw.mk      firmware: build/flash/debug on the DK + native_sim (Zephyr)
#   mk/server.mk  server/host services: CoAP ingest, web map, Caddy
# This file owns shared config and the cross-cutting entry points (demo).
#
# Main targets:
#   make setup        one-time firmware setup (SDK + host build tools)
#   make build        build APP for BOARD  (default APP=tracker)
#   make flash        flash APP to the DK
#   make setup-host   one-time server setup (mosquitto + Python deps)
#   make demo         localhost end-to-end: CoAP ingest + web + sim
#   make serve        real server: CoAP ingest + web (no sim)
#
# Host-specific config (server host, HTTPS domain) lives in a git-ignored .env;
# copy env.template to .env and edit. Precedence: CLI > .env > built-in default.

# Use bash everywhere: recipes rely on /dev/tcp, `trap`, and other bashisms.
SHELL := /bin/bash
.RECIPEPREFIX := >
.DEFAULT_GOAL := help

# ── shared paths (used by both included makefiles) ────────────────────────────
REPO := $(CURDIR)
WS   := $(abspath $(CURDIR)/..)
VENV := $(WS)/.venv
WEST := $(VENV)/bin/west

# Optional host-specific overrides. Must precede the mk/*.mk includes so the
# values exist when fw.mk parses its APP=tracker conditional.
-include .env

# ── shared config (CLI > .env > these defaults) ───────────────────────────────
# Keep comments on their own lines: make preserves the space before a trailing
# `#`, so `VAR ?= value # note` yields "value " and breaks quoted -D defines.

# Local plumbing: where the sim sends its CoAP reports (the demo server).
SIM_SERVER_HOST     ?= 127.0.0.1
# Where the physical device sends over LTE (coap://HOST:PORT/obs).
TRACKER_SERVER_HOST ?= 127.0.0.1
TRACKER_SERVER_PORT ?= 5683
# Empty -> Caddy serves HTTP by IP; set -> HTTPS for the domain.
CADDY_DOMAIN        ?=

# nrfutil lives in ~/.local/bin; add it to PATH for all targets
export PATH := $(HOME)/.local/bin:$(PATH)

# Motion-alert config (from .env) — export so the server subprocesses see it.
TRACKER_NTFY_URL ?=
TRACKER_WEB_URL  ?=
export TRACKER_NTFY_URL
export TRACKER_WEB_URL

# Trust the system CA bundle for TLS-inspecting proxies
CA := /etc/ssl/certs/ca-certificates.crt
export REQUESTS_CA_BUNDLE := $(CA)
export SSL_CERT_FILE := $(CA)
export CURL_CA_BUNDLE := $(CA)

include mk/fw.mk
include mk/server.mk

.PHONY: setup-venv demo

# Create the Python venv if it doesn't exist. Shared by setup-zephyr (fw.mk)
# and setup-host (server.mk). Just the venv, nothing else.
setup-venv:
> @if [ ! -d "$(VENV)" ]; then \
>   echo "Creating venv at $(VENV)"; \
>   python3 -m venv $(VENV); \
>   $(VENV)/bin/pip install --quiet --upgrade pip; \
> else \
>   echo "venv exists at $(VENV)"; \
> fi

# Full localhost end-to-end: CoAP ingest + web + one sim, one command.
# Cross-cutting: builds the sim (fw.mk) if needed, then runs the server stack
# (server.mk). Ctrl-C tears the whole stack down.
demo:
> @test -f $(SIM_BUILD)/tracker/zephyr/zephyr.exe || $(MAKE) --no-print-directory sim
> @trap '$(MAKE) --no-print-directory stop' EXIT; \
>  $(COAP) & echo "coap ingest started (pid $$!)"; \
>  sleep 0.5; \
>  $(SIM) > /dev/null 2>&1 & echo "sim '$(SIM_DEVICE_ID)' started (pid $$!)"; \
>  echo "map at http://localhost:$(HTTP_PORT)  —  Ctrl-C to stop"; \
>  $(WEB)

.PHONY: help
help:
> @echo "nRF9151-DK tracker (Linux-native, run inside WSL)"
> @echo ""
> @echo "MAIN"
> @echo "  make setup                    one-time firmware setup (SDK + host tools)"
> @echo "  make build [APP=] [BOARD=]    build firmware app"
> @echo "  make flash [APP=]             flash firmware to the DK"
> @echo "  make setup-host               one-time server setup (Python deps)"
> @echo "  make demo                     localhost end-to-end: coap+web+sim"
> @echo "  make serve                    public server via Caddy: HTTP by IP (no sim)"
> @echo "  make serve CADDY_DOMAIN=x.uk  public server via Caddy: HTTPS for domain"
> @echo ""
> @echo "FIRMWARE (mk/fw.mk)"
> @echo "  make setup-zephyr             venv + west update + install toolchain"
> @echo "  make setup-tools              install host tools (nrfutil, usbip, tio)"
> @echo "  make windows-usb-passthrough  attach the J-Link into WSL (usbipd)"
> @echo "  make recover                  unlock a readback-protected chip"
> @echo "  make uart [PORT=]             stream serial console (tio)"
> @echo "  make gdb                      west debugserver + arm-gdb"
> @echo "  make clean                    remove build dir for APP"
> @echo "  make sim                      build tracker as native Linux process (no DK)"
> @echo "  make run-sim [SIM_DEVICE_ID=] run the sim binary (foreground)"
> @echo "  make fsmtest                  unit-test loc_fsm vs its decision table"
> @echo "  make flogtest                 e2e the flash flight recorder on native_sim"
> @echo ""
> @echo "SERVER (mk/server.mk)"
> @echo "  make coap-server              start CoAP→SQLite ingest (UDP 5683, fg)"
> @echo "  make server                   start FastAPI map (127.0.0.1:8080)"
> @echo "  make setup-caddy              install Caddy (systemd HTTPS reverse proxy)"
> @echo "  make open-ports               open firewall 80/443 tcp + 5683 udp (persisted)"
> @echo "  make caddy [CADDY_DOMAIN=]    write Caddyfile + reload caddy"
> @echo "  make stop                     stop coap + server + sims (free ports)"
> @echo ""
> @echo "PROTOCOL"
> @echo "  make proto                    regen protobuf codecs from proto/tracker.proto"
> @echo ""
> @echo "Current: APP=$(APP)  BOARD=$(BOARD)  PORT=$(PORT)  RUNNER=$(RUNNER)"
> @echo "Apps available: $$(ls apps 2>/dev/null | tr '\n' ' ')"
> @echo ""
> @echo "Typical use:"
> @echo "  make setup setup-host         # once"
> @echo "  make demo                     # local end-to-end, open http://localhost:8080"
> @echo "  make build flash              # to the real DK"
> @echo "  ./smoke.sh                    # check build + demo still work"
