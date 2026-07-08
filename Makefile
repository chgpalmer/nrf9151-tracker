# nRF9151-DK tracker — top-level Makefile.
#
# Split across:
#   mk/fw.mk      firmware: build/flash/debug on the DK + host toolchain setup
#   mk/server.mk  server + native_sim: broker, ingest, web map, sim
#
# Main targets:
#   make setup        one-time firmware setup (SDK + host build tools)
#   make build        build APP for BOARD  (default APP=hello)
#   make flash        flash APP to the DK
#   make setup-host   one-time server setup (mosquitto + Python deps)
#   make demo         localhost end-to-end: broker + ingest + web + sim
#   make serve        real server: broker + ingest + web (no sim)

# Use bash everywhere: recipes rely on /dev/tcp, `trap`, and other bashisms.
SHELL := /bin/bash
.RECIPEPREFIX := >
.DEFAULT_GOAL := help

# ── shared paths (used by both included makefiles) ────────────────────────────
REPO := $(CURDIR)
WS   := $(abspath $(CURDIR)/..)
VENV := $(WS)/.venv
WEST := $(VENV)/bin/west

# nrfutil lives in ~/.local/bin; add it to PATH for all targets
export PATH := $(HOME)/.local/bin:$(PATH)

# Trust the system CA bundle for TLS-inspecting proxies
CA := /etc/ssl/certs/ca-certificates.crt
export REQUESTS_CA_BUNDLE := $(CA)
export SSL_CERT_FILE := $(CA)
export CURL_CA_BUNDLE := $(CA)

include mk/fw.mk
include mk/server.mk

.PHONY: help
help:
> @echo "nRF9151-DK tracker (Linux-native, run inside WSL)"
> @echo ""
> @echo "MAIN"
> @echo "  make setup                    one-time firmware setup (SDK + host tools)"
> @echo "  make build [APP=] [BOARD=]    build firmware app"
> @echo "  make flash [APP=]             flash firmware to the DK"
> @echo "  make setup-host               one-time server setup (mosquitto + py deps)"
> @echo "  make demo                     localhost end-to-end: broker+ingest+web+sim"
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
> @echo ""
> @echo "SERVER + SIM (mk/server.mk)"
> @echo "  make sim                      build tracker as native Linux process (no DK)"
> @echo "  make run-sim [SIM_DEVICE_ID=] run the sim binary (foreground)"
> @echo "  make broker                   start mosquitto (background)"
> @echo "  make ingest                   start MQTT→SQLite ingest (server/tracker.db)"
> @echo "  make server                   start FastAPI map (127.0.0.1:8080)"
> @echo "  make setup-caddy              install Caddy (systemd HTTPS reverse proxy)"
> @echo "  make open-ports               open firewall for 80/443/1883 (persisted)"
> @echo "  make caddy [CADDY_DOMAIN=]    write Caddyfile + reload caddy"
> @echo "  make stop                     stop broker + server + sims (free ports)"
> @echo ""
> @echo "Current: APP=$(APP)  BOARD=$(BOARD)  PORT=$(PORT)  RUNNER=$(RUNNER)"
> @echo "Apps available: $$(ls apps 2>/dev/null | tr '\n' ' ')"
> @echo ""
> @echo "Typical use:"
> @echo "  make setup setup-host         # once"
> @echo "  make demo                     # local end-to-end, open http://localhost:8080"
> @echo "  make build flash              # to the real DK"
