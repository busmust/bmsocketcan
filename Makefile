obj-m += bmcan.o
bmcan-objs := src/bmcan_usb.o src/bmcan_netdev.o src/bmcan_proto.o
ccflags-y += -I$(PWD)/inc

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
OUT_DIR ?= $(PWD)/out

# Userspace tool
API_SRC := $(PWD)/src/bmcan_api.c
API_BIN := $(OUT_DIR)/bmcan_api
CC ?= gcc
API_CFLAGS ?= -O2 -Wall -Wextra

ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG
endif

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@mkdir -p $(OUT_DIR)
	@cp bmcan.ko $(OUT_DIR)/
	@$(CC) -I$(PWD)/inc $(API_CFLAGS) -o $(API_BIN) $(API_SRC)

install:
	@echo "=================================="
	@echo "BMCAN Driver Installation"
	@echo "=================================="
	@# Check if module is loaded and unload it
	@if lsmod | grep -q "^bmcan "; then \
		echo "Unloading old bmcan module..."; \
		rmmod bmcan 2>/dev/null || true; \
		sleep 1; \
	fi
	@# Install the module
	@echo "Installing bmcan.ko..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	@# Update module dependencies
	@echo "Updating module dependencies..."
	depmod -a $(shell uname -r)
	@# Install udev rules for auto-loading
	@echo "Installing udev rules..."
	install -d $(DESTDIR)/etc/udev/rules.d
	install -m 644 udev/99-bmcan.rules $(DESTDIR)/etc/udev/rules.d/
	@udevadm control --reload-rules 2>/dev/null || true
	@# Verify installation
	@if modinfo bmcan >/dev/null 2>&1; then \
		echo "[OK] Module installed successfully"; \
	else \
		echo "[FAIL] Installation failed"; \
		exit 1; \
	fi
	@# Auto-load if BMCAN device is already connected
	@if lsusb 2>/dev/null | grep -q '0810:'; then \
		echo "BMCAN device detected, loading module..."; \
		modprobe bmcan 2>/dev/null || true; \
		if lsmod | grep -q '^bmcan '; then \
			echo "[OK] Module loaded (device was already connected)"; \
		fi; \
	else \
		echo "Module will auto-load when a BMCAN USB device is plugged in."; \
	fi

load:
	@echo "Loading bmcan module (requires root: sudo make load)..."
	@modprobe bmcan
	@if lsmod | grep -q "^bmcan "; then \
		echo "[OK] Module loaded successfully"; \
	else \
		echo "[FAIL] Failed to load module"; \
		exit 1; \
	fi

uninstall:
	@echo "=================================="
	@echo "BMCAN Driver Uninstallation"
	@echo "=================================="
	@# Check if module is loaded and unload it
	@if lsmod | grep -q "^bmcan "; then \
		echo "Unloading bmcan module..."; \
		rmmod bmcan 2>/dev/null || true; \
		sleep 1; \
	else \
		echo "bmcan module is not loaded"; \
	fi
	@# Remove the module file
	@echo "Removing bmcan.ko..."
	@rm -f $$(modinfo -n bmcan 2>/dev/null)
	@# Update module dependencies
	@echo "Updating module dependencies..."
	depmod -a $(shell uname -r)
	@# Remove auto-load config if exists
	@if [ -f /etc/modules-load.d/bmcan.conf ]; then \
		echo "Removing auto-load configuration..."; \
		rm -f /etc/modules-load.d/bmcan.conf; \
	fi
	@# Remove udev rules
	@if [ -f /etc/udev/rules.d/99-bmcan.rules ]; then \
		echo "Removing udev rules..."; \
		rm -f /etc/udev/rules.d/99-bmcan.rules; \
		udevadm control --reload-rules 2>/dev/null || true; \
	fi
	@echo "[OK] Driver uninstalled successfully"

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	@rm -rf $(OUT_DIR)

# Quick reload for development: build, unload old module, load new module
reload:
	@echo "=================================="
	@echo "BMCAN Quick Reload"
	@echo "=================================="
	@# Build
	@echo "Building module..."
	@$(MAKE) -C $(KDIR) M=$(PWD) modules
	@mkdir -p $(OUT_DIR)
	@cp bmcan.ko $(OUT_DIR)/
	@$(CC) -I$(PWD)/inc $(API_CFLAGS) -o $(API_BIN) $(API_SRC)
	@# Unload old module
	@if lsmod | grep -q "^bmcan "; then \
		echo "Unloading old bmcan module..."; \
		rmmod bmcan 2>/dev/null || true; \
		sleep 1; \
	fi
	@# Load new module
	@echo "Loading new bmcan module..."
	@insmod $(OUT_DIR)/bmcan.ko
	@# Verify
	@if lsmod | grep -q "^bmcan "; then \
		echo "[OK] Module loaded successfully"; \
		echo ""; \
		lsmod | grep "^bmcan "; \
		echo ""; \
		dmesg | tail -3 | grep -E "bmcan|can"; \
	else \
		echo "[FAIL] Failed to load module"; \
		exit 1; \
	fi
