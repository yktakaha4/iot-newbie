ARDUINO_CLI := arduino-cli
ARDUINO_CONFIG := arduino-cli.yaml
PROFILE ?= default
APPS_DIR := apps
MAKE_TARGETS := compile secrets list-apps require-app
APP_FROM_GOALS := $(filter-out $(MAKE_TARGETS),$(MAKECMDGOALS))
APP ?= $(firstword $(APP_FROM_GOALS))
APP_DIR := $(APPS_DIR)/$(APP)

.PHONY: compile secrets list-apps require-app

compile: require-app secrets
	$(ARDUINO_CLI) --config-file $(ARDUINO_CONFIG) compile --profile $(PROFILE) $(if $(PORT),-p $(PORT) -u,) $(APP_DIR)

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
%:
	@:
