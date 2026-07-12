# Server/host services: CoAP ingest, SQLite store, web map, Caddy front.
# (The native_sim build lives in mk/fw.mk; the demo entry point in Makefile.)
#
# Override on the command line:
#   COAP_PORT=5683            CoAP ingest UDP port
#   HTTP_PORT=8080            web map port
#   CADDY_DOMAIN=tracker.example.com      empty -> HTTP on :80 by IP; set -> HTTPS for domain
#
# Public access goes through Caddy (systemd service) as a reverse proxy to the
# app on 127.0.0.1. With CADDY_DOMAIN unset, Caddy serves plain HTTP on :80 so
# you can reach it by public IP while DNS is still pending. Set CADDY_DOMAIN to
# your domain once it resolves to this host and Caddy gets a Let's Encrypt cert
# automatically (HTTPS + www + auto-renew).
#
# The CoAP ingest is NOT proxied by Caddy (it proxies HTTP, this is UDP): the
# ingest binds all interfaces itself and open-ports admits UDP 5683.

COAP_PORT  ?= 5683
HTTP_PORT  ?= 8080
CADDYFILE  := /etc/caddy/Caddyfile

# OpenCelliD cell-tower DB for GPS-less location fallback.
# CELL_MCC: MCC files to download (space-separated). Default = UK (234,730,748).
# OPENCELLID_TOKEN: your opencellid.org API token (put in .env), needed only for
# `make update-cells`. `make cells` just builds from CSVs already in CELL_CSV_DIR.
CELL_CSV_DIR     ?= server/opencellid
CELL_MCC         ?= 234 730 748
OPENCELLID_TOKEN ?=

COAP := $(VENV)/bin/python3 -u server/coap_server.py --port $(COAP_PORT)
# Bind the app to localhost only — Caddy is the sole public face for HTTP.
WEB  := $(VENV)/bin/uvicorn app:app --host 127.0.0.1 --port $(HTTP_PORT) --app-dir server

.PHONY: setup-host setup-caddy open-ports caddy \
        coap-server server serve stop cells update-cells \
        setup-webtest webtest

# One-time server setup: Python deps only (the MQTT-era mosquitto is gone —
# the CoAP ingest is a plain Python process on UDP).
setup-host: setup-venv
> $(VENV)/bin/pip install --quiet -r server/requirements.txt
> @echo "Host tools ready."

# One-time web-test setup (dev machines only, not the VM): playwright + a
# headless Chromium for `make webtest`.
setup-webtest: setup-venv
> $(VENV)/bin/pip install --quiet -r server/requirements-dev.txt
> $(VENV)/bin/playwright install --with-deps chromium
> @echo "Web test tools ready."

# Automated web UI test: seeds a TEMP db, serves the app on a test port,
# EXECUTES the page in headless Chromium and fails on any console error,
# broken import, missing UI element, or interaction that throws. This is what
# actually catches a page that serves fine but dies at runtime — parse checks
# and curl cannot. Screenshot: make webtest WEBTEST_SCREENSHOT=/tmp/ui.png
webtest:
> @test -x $(VENV)/bin/playwright || { echo "Run: make setup-webtest"; exit 1; }
> @VENV=$(VENV) bash scripts/webtest.sh

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

# Open the public ports in the host firewall: 80/443 TCP (web via Caddy) and
# the CoAP ingest's UDP port. The default INPUT chain REJECTs anything but
# SSH, so these must be inserted before the REJECT rule. Idempotent (-C guards
# duplicates) and persisted. Remember the cloud provider's security list must
# admit UDP $(COAP_PORT) too — an iptables rule can't help with that.
open-ports:
> @for p in 80 443; do \
>    sudo iptables -C INPUT -p tcp --dport $$p -j ACCEPT 2>/dev/null \
>      || sudo iptables -I INPUT 4 -p tcp --dport $$p -j ACCEPT; \
>  done
> @sudo iptables -C INPUT -p udp --dport $(COAP_PORT) -j ACCEPT 2>/dev/null \
>    || sudo iptables -I INPUT 4 -p udp --dport $(COAP_PORT) -j ACCEPT
> @sudo netfilter-persistent save >/dev/null 2>&1 \
>    || sudo sh -c 'iptables-save > /etc/iptables/rules.v4'
> @echo "ports 80/443 tcp + $(COAP_PORT) udp open + persisted"

# Write the Caddyfile for the current mode and (re)load the caddy service.
#   CADDY_DOMAIN unset -> HTTP on :80, reachable by public IP
#   CADDY_DOMAIN=host  -> auto-HTTPS for host + www (needs DNS -> this host)
caddy: setup-caddy
> HASH=$$(caddy hash-password --plaintext "$(CADDY_PASSWORD)"); \
> CADDY_CREDENTIALS="$(CADDY_USERNAME) $$HASH"; \
> if [ -n "$(CADDY_DOMAIN)" ]; then \
>     printf '%s {\n    basicauth * {\n        %s\n    }\n    reverse_proxy 127.0.0.1:%s\n}\n' \
>         "$(CADDY_DOMAIN), www.$(CADDY_DOMAIN)" "$$CADDY_CREDENTIALS" "$(HTTP_PORT)" \
>         | sudo tee $(CADDYFILE) >/dev/null; \
>     echo "Caddy: HTTPS for $(CADDY_DOMAIN) (+www) -> 127.0.0.1:$(HTTP_PORT)"; \
> else \
>     printf ':80 {\n    basicauth * {\n        %s\n    }\n    reverse_proxy 127.0.0.1:%s\n}\n' \
>         "$$CADDY_CREDENTIALS" "$(HTTP_PORT)" \
>         | sudo tee $(CADDYFILE) >/dev/null; \
>     echo "Caddy: HTTP on :80 (reach by IP) -> 127.0.0.1:$(HTTP_PORT)"; \
> fi
> sudo systemctl enable caddy >/dev/null 2>&1 || true
> sudo systemctl reload caddy 2>/dev/null || sudo systemctl restart caddy
> echo "caddy (re)loaded"

# Run the CoAP ingest in the foreground (demo/serve background it themselves).
coap-server:
> @test -x $(VENV)/bin/python3 || { echo "Run: make setup-host"; exit 1; }
> $(COAP)

server:
> @test -x $(VENV)/bin/uvicorn || { echo "Run: make setup-host"; exit 1; }
> $(WEB)

# Real-server stack: firewall + Caddy front + CoAP ingest (bg) + web (fg).
# Caddy runs as a persistent systemd service (stays up across Ctrl-C/disconnect);
# Ctrl-C here stops the app + ingest via the EXIT trap.
#   make serve                 public HTTP by IP  (default, works without DNS)
#   make serve CADDY_DOMAIN=tracker.example.com   public HTTPS once DNS points here
serve: open-ports caddy
> @trap '$(MAKE) --no-print-directory stop' EXIT; \
>  $(COAP) & echo "coap ingest started (pid $$!)"; \
>  if [ -n "$(CADDY_DOMAIN)" ]; then \
>    echo "public: https://$(CADDY_DOMAIN)  +  coap://$(CADDY_DOMAIN):$(COAP_PORT)"; \
>  else \
>    echo "public: http://$$(curl -s --max-time 3 https://api.ipify.org 2>/dev/null || echo YOUR_PUBLIC_IP)"; \
>  fi; \
>  echo "app on 127.0.0.1:$(HTTP_PORT)  —  Ctrl-C to stop app (Caddy stays up)"; \
>  $(WEB)

# Stop the listeners (web by TCP port, CoAP ingest by UDP port) and kill the
# backgrounded sim. The bracket trick ('[z]ephyr.exe') stops pkill matching
# its own argv.
stop:
> @fuser -k $(HTTP_PORT)/tcp 2>/dev/null && echo "stopped server (port $(HTTP_PORT))" || true
> @fuser -k $(COAP_PORT)/udp 2>/dev/null && echo "stopped coap ingest (udp $(COAP_PORT))" || true
> @pkill -f '[z]ephyr.exe' 2>/dev/null && echo "stopped sim(s)" || true
> @pkill -f '[c]oap_server.py' 2>/dev/null && echo "stopped coap ingest" || true
> @echo "cleanup done"

# Server-side unit tests: A-GNSS RINEX parsing, ICD scaling, orbit math,
# payload assembly — pure Python against a committed real BRDC fixture.
.PHONY: servertest
servertest:
> @test -x $(VENV)/bin/pytest || $(VENV)/bin/pip install --quiet pytest
> cd server && $(VENV)/bin/python3 -m pytest tests/ -q
