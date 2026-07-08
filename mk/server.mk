# Server + native_sim: MQTT broker, SQLite ingest, web map, and the sim.
#
# native_sim uses NSOS (offloaded sockets) — no TAP, no root, no net-setup;
# the sim binary connects straight to the host network stack via localhost.
#
# Override on the command line:
#   BROKER_HOST=localhost     broker the sim/ingest connect to
#   MQTT_PORT=1883            broker port
#   HTTP_PORT=8080            web map port
#   SIM_DEVICE_ID=sim-dev-1   device id the sim reports (env at runtime)
#   CADDY_DOMAIN=noil.uk      empty -> HTTP on :80 by IP; set -> HTTPS for domain
#
# Public access goes through Caddy (systemd service) as a reverse proxy to the
# app on 127.0.0.1. With CADDY_DOMAIN unset, Caddy serves plain HTTP on :80 so
# you can reach it by public IP while DNS is still pending. Set CADDY_DOMAIN to
# your domain once it resolves to this host and Caddy gets a Let's Encrypt cert
# automatically (HTTPS + www + auto-renew).

SIM_BUILD     := build/tracker-sim
BROKER_HOST   ?= localhost
MQTT_PORT     ?= 1883
MQTT_TOPIC    ?= trackers/\#
SIM_DEVICE_ID ?= sim-dev-1
HTTP_PORT     ?= 8080
CADDY_DOMAIN  ?=
CADDYFILE     := /etc/caddy/Caddyfile

INGEST := $(VENV)/bin/python3 server/ingest.py --host $(BROKER_HOST) --port $(MQTT_PORT) --topic '$(MQTT_TOPIC)'
# Bind the app to localhost only — Caddy is the sole public face.
WEB    := $(VENV)/bin/uvicorn app:app --host 127.0.0.1 --port $(HTTP_PORT) --app-dir server
SIM    := TRACKER_DEVICE_ID=$(SIM_DEVICE_ID) $(SIM_BUILD)/tracker/zephyr/zephyr.exe

.PHONY: setup-venv setup-west setup-host setup-caddy open-ports caddy \
        sim run-sim broker ingest server serve demo stop

# Create the Python venv if it doesn't exist. Just the venv, nothing else.
setup-venv:
> @if [ ! -d "$(VENV)" ]; then \
>   echo "Creating venv at $(VENV)"; \
>   python3 -m venv $(VENV); \
>   $(VENV)/bin/pip install --quiet --upgrade pip; \
> else \
>   echo "venv exists at $(VENV)"; \
> fi


setup-host: setup-venv
> sudo apt-get install -y mosquitto mosquitto-clients
> $(VENV)/bin/pip install --quiet -r server/requirements.txt
> @echo "Host tools ready."

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

sim: setup-zephyr
> $(WEST) build -p auto -b native_sim/native/64 apps/tracker -d $(SIM_BUILD) \
>   -DCONFIG_TRACKER_MQTT_BROKER_HOST=\"$(BROKER_HOST)\"
> @echo "Sim built: $(SIM_BUILD)/tracker/zephyr/zephyr.exe"
> @echo "Run: make run-sim  (override device: make run-sim SIM_DEVICE_ID=sim-dev-2)"

run-sim:
> @test -f $(SIM_BUILD)/tracker/zephyr/zephyr.exe || { echo "Run 'make sim' first"; exit 1; }
> $(SIM)

# Start mosquitto in the background and wait until the port accepts a TCP
# connection before returning. Any distro mosquitto.service auto-started by
# apt would squat 1883, so stop it and reclaim the port first.
broker: setup-host
> @which mosquitto >/dev/null 2>&1 || { echo "Run: make setup-host"; exit 1; }
> @sudo systemctl stop mosquitto 2>/dev/null || true
> @fuser -k $(MQTT_PORT)/tcp 2>/dev/null || true
> @sleep 0.3
> @nohup mosquitto -p $(MQTT_PORT) > /tmp/mosquitto-$(MQTT_PORT).log 2>&1 &
> @for i in {1..50}; do \
>    (exec 3<>/dev/tcp/127.0.0.1/$(MQTT_PORT)) 2>/dev/null && { exec 3>&-; break; }; \
>    sleep 0.1; \
>  done; \
>  if (exec 3<>/dev/tcp/127.0.0.1/$(MQTT_PORT)) 2>/dev/null; then \
>    exec 3>&-; echo "mosquitto started on port $(MQTT_PORT)"; \
>  else echo "mosquitto failed — see /tmp/mosquitto-$(MQTT_PORT).log"; exit 1; fi

ingest: setup-host
> $(INGEST)

server: setup-host
> $(WEB)

# Real-server stack: firewall + broker + Caddy front + ingest (bg) + web (fg).
# Caddy runs as a persistent systemd service (stays up across Ctrl-C/disconnect);
# Ctrl-C here stops the app + broker + ingest via the EXIT trap.
#   make serve                 public HTTP by IP  (default, works without DNS)
#   make serve CADDY_DOMAIN=noil.uk   public HTTPS once DNS points here
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

# Full localhost end-to-end: broker + ingest + web + one sim, one command.
# Builds the sim first if needed. Ctrl-C tears the whole stack down.
demo: broker
> @test -f $(SIM_BUILD)/tracker/zephyr/zephyr.exe || $(MAKE) --no-print-directory sim
> @trap '$(MAKE) --no-print-directory stop' EXIT; \
>  $(INGEST) & echo "ingest started (pid $$!)"; \
>  $(SIM) > /dev/null 2>&1 & echo "sim '$(SIM_DEVICE_ID)' started (pid $$!)"; \
>  echo "map at http://localhost:$(HTTP_PORT)  —  Ctrl-C to stop"; \
>  $(WEB)

# Stop the listeners by port (server 8080, broker 1883) and kill any
# backgrounded sim binaries. The '[z]ephyr.exe' pattern matches real
# zephyr.exe processes but not pkill's own argv, so it never self-kills.
stop:
> @fuser -k $(HTTP_PORT)/tcp 2>/dev/null && echo "stopped server (port $(HTTP_PORT))" || true
> @fuser -k $(MQTT_PORT)/tcp  2>/dev/null && echo "stopped broker (port $(MQTT_PORT))" || true
> @pkill -f '[z]ephyr.exe' 2>/dev/null && echo "stopped sim(s)" || true
> @echo "cleanup done"
