# Firmware: build/flash/debug on the nRF9151-DK + host toolchain setup.
# USB reaches WSL via usbipd (see windows-usb-passthrough). Flash/debug go
# through west -> nrfutil; serial console uses tio.
#
# Override on the command line:
#   APP=hello                 app under apps/  (hello, gnss, tracker)
#   BOARD=nrf9151dk/nrf9151   board; use nrf9151dk/nrf9151/ns for modem/GNSS
#   PORT=/dev/ttyACM0         serial port for `make uart`
#   BAUD=115200               serial baud
#   RUNNER=nrfutil            west flash/debug runner (nrfutil|nrfjprog|openocd)

APP    ?= hello
BOARD  ?= nrf9151dk/nrf9151
PORT   ?= /dev/ttyACM0
BAUD   ?= 115200
RUNNER ?= nrfutil

APP_DIR := apps/$(APP)
BUILD   := build/$(APP)

SDK := $(HOME)/zephyr-sdk-1.0.1
GDB := $(SDK)/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb

.PHONY: setup setup-system setup-zephyr setup-tools windows-usb-passthrough \
        build flash recover uart gdb clean

# One-time firmware setup: pull SDK/deps + install host build tools.
setup: setup-zephyr setup-tools
> @echo "Firmware setup complete."

# Install system packages needed for Zephyr (from official docs)
# Idempotent: checks for cmake as a canary before running apt-get
setup-system:
> @if ! command -v cmake >/dev/null 2>&1; then \
>   echo "Installing system build dependencies..."; \
>   sudo apt-get update -qq && \
>   sudo apt-get install --no-install-recommends -y git cmake ninja-build gperf \
>     ccache dfu-util device-tree-compiler wget python3-dev python3-venv python3-tk \
>     xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1; \
>   echo "System packages installed."; \
> else \
>   echo "System packages already installed."; \
> fi

setup-zephyr: setup-venv setup-system
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

build: setup-zephyr
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
