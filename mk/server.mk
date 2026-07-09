# Server/host services: MQTT broker, SQLite ingest, web map, Caddy front.
# (The native_sim build lives in mk/fw.mk; the demo entry point in Makefile.)
#
# Override on the command line:
#   BROKER_HOST=localhost     broker the ingest connects to
#   MQTT_PORT=1883            broker port
#   HTTP_PORT=8080            web map port
#   CADDY_DOMAIN=noil.uk      empty -> HTTP on :80 by IP; set -> HTTPS for domain
#
# Public access goes through Caddy (systemd service) as a reverse proxy to the
# app on 127.0.0.1. With CADDY_DOMAIN unset, Caddy serves plain HTTP on :80 so
# you can reach it by public IP while DNS is still pending. Set CADDY_DOMAIN to
# your domain once it resolves to this host and Caddy gets a Let's Encrypt cert
# automatically (HTTPS + www + auto-renew).

MQTT_PORT  ?= 1883
MQTT_TOPIC ?= trackers/\#
HTTP_PORT  ?= 8080
CADDYFILE  := /etc/caddy/Caddyfile
# Address mosquitto listens on. Loopback by default so `make demo` (sim on the
# same host) never exposes a broker; `serve` overrides it to 0.0.0.0 because a
# real device dials in from the internet. Generated config, git-ignored.
MQTT_BIND  ?= 127.0.0.1
MOSQ_CONF  := server/mosquitto.conf

# OpenCelliD cell-tower DB for GPS-less location fallback.
# CELL_MCC: MCC files to download (space-separated). Default = UK (234,730,748).
# OPENCELLID_TOKEN: your opencellid.org API token (put in .env), needed only for
# `make update-cells`. `make cells` just builds from CSVs already in CELL_CSV_DIR.
CELL_CSV_DIR     ?= server/opencellid
CELL_MCC         ?= 234 730 748
OPENCELLID_TOKEN ?=

INGEST := $(VENV)/bin/python3 server/ingest.py --host $(BROKER_HOST) --port $(MQTT_PORT) --topic '$(MQTT_TOPIC)'
# Bind the app to localhost only — Caddy is the sole public face.
WEB    := $(VENV)/bin/uvicorn app:app --host 127.0.0.1 --port $(HTTP_PORT) --app-dir server

.PHONY: setup-host setup-caddy open-ports caddy \
        broker ingest server serve stop cells update-cells

# One-time server setup. Idempotent: skips apt when mosquitto is already on
# PATH (like setup-system does), so re-running is cheap and prompts no sudo.
setup-host: setup-venv
> @if ! command -v mosquitto >/dev/null 2>&1; then \
>    echo "Installing mosquitto..."; \
>    sudo apt-get install -y mosquitto mosquitto-clients; \
>  else echo "mosquitto already installed"; fi
> $(VENV)/bin/pip install --quiet -r server/requirements.txt
> @echo "Host tools ready."

# Build the local cell-tower DB (server/cells.db) from OpenCelliD CSVs already
# present in CELL_CSV_DIR. No token needed — offline. Drop *.csv.gz there (or
# run `make update-cells` to fetch them) first.
cells: setup-venv
> @shopt -s nullglob; files=($(CELL_CSV_DIR)/*.csv.gz $(CELL_CSV_DIR)/*.csv); \
>  if [ $${#files[@]} -eq 0 ]; then \
>    echo "No CSVs in $(CELL_CSV_DIR)/. Add *.csv.gz or run: make update-cells"; \
>    exit 1; \
>  fi; \
>  $(VENV)/bin/python3 server/build_cells.py --db server/cells.db "$${files[@]}"

# Download the configured MCC files from OpenCelliD (needs OPENCELLID_TOKEN in
# your environment/.env), then build cells.db. Separated from `cells` so the
# token is only ever needed for the fetch, not the build.
update-cells: setup-venv
> @test -n "$(OPENCELLID_TOKEN)" || { echo "Set OPENCELLID_TOKEN (get one at opencellid.org)"; exit 1; }
> @mkdir -p $(CELL_CSV_DIR)
> @for mcc in $(CELL_MCC); do \
>    echo "Downloading MCC $$mcc..."; \
>    curl -fsSL -o $(CELL_CSV_DIR)/$$mcc.csv.gz \
>      "https://opencellid.org/ocid/downloads?token=$(OPENCELLID_TOKEN)&type=mcc&file=$$mcc.csv.gz" \
>      || { echo "download failed for MCC $$mcc"; exit 1; }; \
>  done
> @$(MAKE) --no-print-directory cells

# Install Caddy from its official apt repo. The package ships a systemd unit
# (runs as user 'caddy', reads $(CADDYFILE), has CAP_NET_BIND_SERVICE for
# ports 80/443). Idempotent: skips if caddy is already on PATH.
setup-caddy:
> @if command -v caddy >/dev/null 2>&1; then \
>    echo "caddy already installed"; \
>  else \
>    echo "Installing Caddy from official repo..."; \
>    sudo apt-get install -y debian-keyring debian-archive-keyring apt-transport-https curl; \
>    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
>      | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg; \
>    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
>      | sudo tee /etc/apt/sources.list.d/caddy-stable.list >/dev/null; \
>    sudo apt-get update && sudo apt-get install -y caddy; \
>  fi

# Open the public ports (80 HTTP, 443 HTTPS, 1883 MQTT) in the host firewall.
# The default INPUT chain REJECTs anything but SSH, so these must be inserted
# before the REJECT rule. Idempotent (-C guards duplicates) and persisted.
open-ports:
> @for p in 80 443 1883; do \
>    sudo iptables -C INPUT -p tcp --dport $$p -j ACCEPT 2>/dev/null \
>      || sudo iptables -I INPUT 4 -p tcp --dport $$p -j ACCEPT; \
>  done
> @sudo netfilter-persistent save >/dev/null 2>&1 \
>    || sudo sh -c 'iptables-save > /etc/iptables/rules.v4'
> @echo "ports 80/443/1883 open + persisted"

# Write the Caddyfile for the current mode and (re)load the caddy service.
#   CADDY_DOMAIN unset -> HTTP on :80, reachable by public IP
#   CADDY_DOMAIN=host  -> auto-HTTPS for host + www (needs DNS -> this host)
caddy: setup-caddy
> @if [ -n "$(CADDY_DOMAIN)" ]; then \
>    printf '%s, www.%s {\n    reverse_proxy 127.0.0.1:%s\n}\n' \
>      '$(CADDY_DOMAIN)' '$(CADDY_DOMAIN)' '$(HTTP_PORT)' | sudo tee $(CADDYFILE) >/dev/null; \
>    echo "Caddy: HTTPS for $(CADDY_DOMAIN) (+www) -> 127.0.0.1:$(HTTP_PORT)"; \
>  else \
>    printf ':80 {\n    reverse_proxy 127.0.0.1:%s\n}\n' \
>      '$(HTTP_PORT)' | sudo tee $(CADDYFILE) >/dev/null; \
>    echo "Caddy: HTTP on :80 (reach by IP) -> 127.0.0.1:$(HTTP_PORT)"; \
>  fi
> @sudo systemctl enable caddy >/dev/null 2>&1 || true
> @sudo systemctl reload caddy 2>/dev/null || sudo systemctl restart caddy
> @echo "caddy (re)loaded"

# Start mosquitto in the background and wait until the port accepts a TCP
# connection before returning. Any distro mosquitto.service auto-started by
# apt would squat 1883, so stop it and reclaim the port first.
#
# mosquitto 2.x run without a config file starts in "local only mode": it binds
# 127.0.0.1 and no remote client can ever reach it. `mosquitto -p N` does NOT
# change that -- the port is right, the bind address is not. So we always write
# an explicit listener. MQTT_BIND stays on loopback for `make demo` (the sim is
# a local process) and `serve` widens it to 0.0.0.0 for real devices.
#
# The 127.0.0.1 readiness probe below passes in either mode, which is exactly why
# a loopback-only broker looked healthy while the DK timed out against it.
broker:
> @which mosquitto >/dev/null 2>&1 || { echo "Run: make setup-host"; exit 1; }
> @sudo systemctl stop mosquitto 2>/dev/null || true
> @fuser -k $(MQTT_PORT)/tcp 2>/dev/null || true
> @sleep 0.3
> @printf 'listener %s %s\nallow_anonymous true\n' '$(MQTT_PORT)' '$(MQTT_BIND)' > $(MOSQ_CONF)
> @nohup mosquitto -c $(MOSQ_CONF) > /tmp/mosquitto-$(MQTT_PORT).log 2>&1 &
> @for i in {1..50}; do \
>    (exec 3<>/dev/tcp/127.0.0.1/$(MQTT_PORT)) 2>/dev/null && { exec 3>&-; break; }; \
>    sleep 0.1; \
>  done; \
>  if (exec 3<>/dev/tcp/127.0.0.1/$(MQTT_PORT)) 2>/dev/null; then \
>    exec 3>&-; echo "mosquitto started on $(MQTT_BIND):$(MQTT_PORT)"; \
>  else echo "mosquitto failed — see /tmp/mosquitto-$(MQTT_PORT).log"; exit 1; fi

ingest:
> @test -x $(VENV)/bin/python3 || { echo "Run: make setup-host"; exit 1; }
> $(INGEST)

server:
> @test -x $(VENV)/bin/uvicorn || { echo "Run: make setup-host"; exit 1; }
> $(WEB)

# Real-server stack: firewall + broker + Caddy front + ingest (bg) + web (fg).
# Caddy runs as a persistent systemd service (stays up across Ctrl-C/disconnect);
# Ctrl-C here stops the app + broker + ingest via the EXIT trap.
#   make serve                 public HTTP by IP  (default, works without DNS)
#   make serve CADDY_DOMAIN=noil.uk   public HTTPS once DNS points here
# Target-specific, and GNU make passes it down to the `broker` prerequisite.
# This is the one place the broker is meant to face the internet.
serve: MQTT_BIND := 0.0.0.0
serve: open-ports broker caddy
> @trap '$(MAKE) --no-print-directory stop' EXIT; \
>  $(INGEST) & echo "ingest started (pid $$!)"; \
>  if [ -n "$(CADDY_DOMAIN)" ]; then \
>    echo "public: https://$(CADDY_DOMAIN)"; \
>  else \
>    echo "public: http://$$(curl -s --max-time 3 https://api.ipify.org 2>/dev/null || echo YOUR_PUBLIC_IP)"; \
>  fi; \
>  echo "app on 127.0.0.1:$(HTTP_PORT)  —  Ctrl-C to stop app (Caddy stays up)"; \
>  $(WEB)

# Stop the listeners by port (server 8080, broker 1883) and kill the
# backgrounded sim + ingest processes. ingest is an MQTT *client* with no
# listening socket, so fuser can't find it — match it by argv instead. The
# bracket trick ('[z]ephyr.exe') stops pkill matching its own argv.
stop:
> @fuser -k $(HTTP_PORT)/tcp 2>/dev/null && echo "stopped server (port $(HTTP_PORT))" || true
> @fuser -k $(MQTT_PORT)/tcp  2>/dev/null && echo "stopped broker (port $(MQTT_PORT))" || true
> @pkill -f '[z]ephyr.exe' 2>/dev/null && echo "stopped sim(s)" || true
> @pkill -f '[s]erver/ingest.py' 2>/dev/null && echo "stopped ingest" || true
> @echo "cleanup done"
