ARDUINO_CLI := arduino-cli
ARDUINO_CONFIG := arduino-cli.yaml
PROFILE ?= default
MONITOR ?= NO
MONITOR_BAUDRATE ?= 115200
APPS_DIR := apps
MAKE_TARGETS := compile secrets monitor list-apps require-app require-port require-monitor-port
APP_FROM_GOALS := $(filter-out $(MAKE_TARGETS),$(MAKECMDGOALS))
APP ?= $(firstword $(APP_FROM_GOALS))
APP_DIR := $(APPS_DIR)/$(APP)

.PHONY: compile secrets monitor list-apps require-app require-port require-monitor-port

compile: require-app require-monitor-port secrets
	$(ARDUINO_CLI) --config-file $(ARDUINO_CONFIG) compile --profile $(PROFILE) $(if $(PORT),-p $(PORT) -u,) $(APP_DIR)
	@if [ "$(MONITOR)" = "YES" ]; then \
		$(MAKE) --no-print-directory monitor APP="$(APP)" PORT="$(PORT)" MONITOR_BAUDRATE="$(MONITOR_BAUDRATE)"; \
	fi

monitor: require-port
	$(ARDUINO_CLI) --config-file $(ARDUINO_CONFIG) monitor -p $(PORT) -c baudrate=$(MONITOR_BAUDRATE)

secrets: require-app
	@if [ -f "$(APP_DIR)/.env.example" ]; then \
		./scripts/generate_secrets.sh "$(APP_DIR)"; \
	else \
		echo "No secrets template for $(APP_DIR); skipping secrets generation."; \
	fi

list-apps:
	@find $(APPS_DIR) -mindepth 1 -maxdepth 1 -type d -exec sh -c 'test -f "$$1/sketch.yaml" && basename "$$1"' _ {} \;

require-app:
	@if [ -z "$(APP)" ]; then \
		echo "APP is required. Example: make compile TRHCheckerM5Paper"; \
		exit 1; \
	fi
	@if [ ! -f "$(APP_DIR)/sketch.yaml" ]; then \
		echo "Application not found: $(APP_DIR)"; \
		echo "Available applications:"; \
		$(MAKE) --no-print-directory list-apps; \
		exit 1; \
	fi

require-port:
	@if [ -z "$(PORT)" ]; then \
		echo "PORT is required. Example: make compile WIFICheckerM5Paper PORT=/dev/cu.usbserial-02142314 MONITOR=YES"; \
		exit 1; \
	fi

require-monitor-port:
	@if [ "$(MONITOR)" = "YES" ] && [ -z "$(PORT)" ]; then \
		echo "PORT is required when MONITOR=YES. Example: make compile WIFICheckerM5Paper PORT=/dev/cu.usbserial-02142314 MONITOR=YES"; \
		exit 1; \
	fi

%:
	@:
