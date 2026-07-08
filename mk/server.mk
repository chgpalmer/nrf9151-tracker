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

SIM_BUILD     := build/tracker-sim
BROKER_HOST   ?= localhost
MQTT_PORT     ?= 1883
MQTT_TOPIC    ?= trackers/\#
SIM_DEVICE_ID ?= sim-dev-1
HTTP_PORT     ?= 8080

INGEST := $(VENV)/bin/python3 server/ingest.py --host $(BROKER_HOST) --port $(MQTT_PORT) --topic '$(MQTT_TOPIC)'
WEB    := $(VENV)/bin/uvicorn app:app --host 0.0.0.0 --port $(HTTP_PORT) --app-dir server
SIM    := TRACKER_DEVICE_ID=$(SIM_DEVICE_ID) $(SIM_BUILD)/tracker/zephyr/zephyr.exe

.PHONY: setup-venv setup-west setup-host sim run-sim broker ingest server serve demo stop

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

# Real-server stack: broker + ingest (background) + web (foreground).
# Ctrl-C stops the web server; the EXIT trap tears down broker + ingest.
serve: broker
> @trap '$(MAKE) --no-print-directory stop' EXIT; \
>  $(INGEST) & echo "ingest started (pid $$!)"; \
>  echo "map at http://localhost:$(HTTP_PORT)  —  Ctrl-C to stop"; \
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
