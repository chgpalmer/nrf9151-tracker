# Firmware: build/flash/debug on the nRF9151-DK + host toolchain setup.
# USB reaches WSL via usbipd (see windows-usb-passthrough). Flash/debug go
# through west -> nrfutil; serial console uses tio.
#
# Override on the command line:
#   APP=tracker                  app under apps/  (hello, gnss, tracker)
#   BOARD=nrf9151dk/nrf9151/ns   board; the secure nrf9151dk/nrf9151 has no modem
#   PORT=/dev/ttyACM0         serial port for `make uart`
#   BAUD=115200               serial baud
#   RUNNER=nrfutil            west flash/debug runner (nrfutil|nrfjprog|openocd)

APP    ?= tracker
BOARD  ?= nrf9151dk/nrf9151/ns
PORT   ?= /dev/ttyACM0
BAUD   ?= 115200
RUNNER ?= nrfutil

APP_DIR := apps/$(APP)
BUILD   := build/$(APP)

SDK := $(HOME)/zephyr-sdk-1.0.1
GDB := $(SDK)/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb

# native_sim build of the tracker app (runs as a host Linux process, no DK).
SIM_BUILD     := build/tracker-sim
SIM_DEVICE_ID ?= sim-dev-1
SIM           := TRACKER_DEVICE_ID=$(SIM_DEVICE_ID) $(SIM_BUILD)/tracker/zephyr/zephyr.exe

# Point the physical tracker firmware at the configured CoAP server. Only
# applied when building the tracker app; empty (harmless) for every other app.
ifeq ($(APP),tracker)
BUILD_DEFINES := -DCONFIG_TRACKER_SERVER_HOST=\"$(TRACKER_SERVER_HOST)\" \
                 -DCONFIG_TRACKER_SERVER_PORT=$(TRACKER_SERVER_PORT)
endif

.PHONY: setup setup-system setup-zephyr setup-tools windows-usb-passthrough \
        build flash recover uart gdb clean sim run-sim check-workspace proto \
        fsmtest

# Regenerate both ends of the wire protocol from proto/tracker.proto:
#   firmware -> apps/tracker/src/proto/tracker.pb.{c,h}  (nanopb)
#   server   -> server/tracker_pb2.py                    (protoc)
# Both outputs are committed, so neither a firmware build nor a server run needs
# the generators; run this after editing the .proto or .options. The nanopb
# generator comes from the workspace's own module (modules/lib/nanopb), keeping
# the generator and the on-target nanopb runtime the same version.
NANOPB_DIR := $(WS)/modules/lib/nanopb
proto:
> @test -f $(NANOPB_DIR)/generator/nanopb_generator.py || { \
>   echo "nanopb missing — run: west update"; exit 1; }
> @test -x $(VENV)/bin/python3 || { echo "Run: make setup-host"; exit 1; }
> $(VENV)/bin/python3 $(NANOPB_DIR)/generator/nanopb_generator.py \
>   -I proto -D apps/tracker/src/proto proto/tracker.proto
> $(VENV)/bin/python3 -m grpc_tools.protoc -Iproto --python_out=server \
>   proto/tracker.proto
> @echo "regenerated apps/tracker/src/proto/ + server/tracker_pb2.py from proto/tracker.proto"

# One-time firmware setup: pull SDK/deps + install host build tools.
setup: setup-zephyr setup-tools
> @echo "Firmware setup complete."

# Fail early and legibly if the workspace isn't fully populated. Checking only
# for .west is not enough: `west init` creates it, but `west build` is an
# extension command that ships inside the zephyr tree, so a workspace that was
# init'd but never `west update`d passes a .west check and then dies with an
# opaque `unknown command "build"`.
check-workspace:
> @test -x $(WEST)     || { echo "No west at $(WEST) — run: make setup-zephyr"; exit 1; }
> @test -d $(WS)/.west || { echo "No west workspace at $(WS) — run: make setup-zephyr"; exit 1; }
> @test -d $(WS)/zephyr || { echo "Zephyr not cloned into $(WS) — run: make setup-zephyr"; exit 1; }

# Install system packages needed for Zephyr (from official docs)
setup-system:
> @bash scripts/setup-system.sh

# setup-system MUST precede setup-venv: the venv needs python3-venv, which
# setup-system is what installs. (Prerequisites run left to right.)
setup-zephyr: setup-system setup-venv
> @mkdir -p $(HOME)/.cache/ztmp
> @if [ ! -d "$(WS)/.west" ]; then \
>   $(VENV)/bin/pip install --quiet west; \
>   $(WEST) init -l $(REPO); \
> fi
> cd $(WS) && $(WEST) update --narrow -o=--depth=1
> @echo "Installing Zephyr Python dependencies..."
> cd $(WS) && $(WEST) zephyr-export
> cd $(WS) && $(WEST) packages pip --install
> @test -d $(SDK) || (cd $(WS) && TMPDIR=$(HOME)/.cache/ztmp $(WEST) sdk install -t arm-zephyr-eabi --install-base $(HOME))
> @echo "Zephyr SDK setup complete."

setup-tools:
> @bash scripts/setup-tools.sh

windows-usb-passthrough:
> @powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$$(wslpath -w scripts/windows/passthrough.ps1)"
> @echo "If /dev/ttyACM* still missing, install usbipd on Windows (admin): winget install usbipd"

build: | check-workspace
> @test -d $(APP_DIR) || { echo "No app '$(APP)' in apps/ ($$(ls apps 2>/dev/null | tr '\n' ' '))"; exit 1; }
> $(WEST) build -p auto -b $(BOARD) $(APP_DIR) -d $(BUILD) $(BUILD_DEFINES)
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

# Build the tracker for native_sim (host Linux process). The sim always talks
# to the local CoAP server on $(SIM_SERVER_HOST) (localhost) — independent of the
# hardware TRACKER_SERVER_HOST used by `make build APP=tracker`.
sim: | check-workspace
> $(WEST) build -p auto -b native_sim/native/64 apps/tracker -d $(SIM_BUILD) \
>   -DCONFIG_TRACKER_SERVER_HOST=\"$(SIM_SERVER_HOST)\"
> @echo "Sim built: $(SIM_BUILD)/tracker/zephyr/zephyr.exe"
> @echo "Run: make run-sim  (override device: make run-sim SIM_DEVICE_ID=sim-dev-2)"

run-sim:
> @test -f $(SIM_BUILD)/tracker/zephyr/zephyr.exe || { echo "Run 'make sim' first"; exit 1; }
> $(SIM)

# Unit-test the location FSM against docs/loc-fsm-decision-table.md (ztest on
# native_sim; the FSM's clock is a parameter, so the 300 s timeout is instant).
FSMTEST_BUILD := build/fsmtest
fsmtest: | check-workspace
> $(WEST) build -p auto -b native_sim/native/64 tests/fsm -d $(FSMTEST_BUILD)
> @exe=$$(find $(FSMTEST_BUILD) -name zephyr.exe | head -1); \
>  test -n "$$exe" || { echo "no test binary built"; exit 1; }; \
>  out=$$(timeout 60 "$$exe"); status=$$?; echo "$$out"; \
>  test $$status -eq 0 || { echo "fsmtest: binary exited $$status"; exit 1; }; \
>  echo "$$out" | grep -q "PROJECT EXECUTION SUCCESSFUL" || { echo "fsmtest FAILED"; exit 1; }
