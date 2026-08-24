# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - Remote Build Module
# Build on a fast remote Linux host and retrieve binaries
#
# Usage:
#   make remote-pi              # Build Pi target on remote, retrieve binaries
#   make remote-ad5m            # Build AD5M target on remote, retrieve binaries
#   make remote-native          # Build native Linux on remote, retrieve binaries
#   make remote-sync            # Just sync source to remote (no build)
#   make remote-fetch           # Just fetch binaries from remote
#
# Configuration:
#   REMOTE_HOST=thelio.local    # Remote build host
#   REMOTE_USER=                # Remote user (empty = use SSH config)
#   REMOTE_DIR=~/Code/Printing/helixscreen  # Source location on remote
#
# The remote host needs:
#   - Docker (for pi-docker, ad5m-docker targets)
#   - GCC/G++ and SDL2-dev (for native Linux builds)
#   - rsync and SSH access

# =============================================================================
# Remote Build Configuration
# =============================================================================

# Remote build host settings (override via environment or command line)
# Example: make remote-pi REMOTE_HOST=buildbox.local REMOTE_USER=builder
REMOTE_HOST ?= thelio.local
REMOTE_USER ?=
REMOTE_DIR ?= ~/Code/Printing/helixscreen

# Build SSH target string
ifdef REMOTE_USER
    REMOTE_SSH_TARGET := $(REMOTE_USER)@$(REMOTE_HOST)
else
    REMOTE_SSH_TARGET := $(REMOTE_HOST)
endif

# =============================================================================
# Remote Sync Targets
# =============================================================================

.PHONY: remote-sync remote-fetch remote-fetch-full remote-clean remote-fetch-pi remote-fetch-ad5m remote-fetch-native remote-fetch-pi-full remote-fetch-ad5m-full

# Sync source code to remote host
# Note: We explicitly exclude build artifacts rather than using .gitignore filtering
# because submodule .gitignore files exclude files we need for cross-compilation
# (e.g., lib/wpa_supplicant/wpa_supplicant/.config)
remote-sync:
	@echo "$(CYAN)$(BOLD)Syncing source to $(REMOTE_SSH_TARGET):$(REMOTE_DIR)...$(RESET)"
	@ssh $(REMOTE_SSH_TARGET) "mkdir -p $(REMOTE_DIR)"
	rsync -avz --copy-unsafe-links --delete \
		--exclude='build/' \
		--exclude='build[0-9]*/' \
		--exclude='_deps/' \
		--exclude='.git/' \
		--exclude='*.o' \
		--exclude='*.a' \
		--exclude='*.d' \
		--exclude='*.so' \
		--exclude='*.dylib' \
		--exclude='.venv/' \
		--exclude='node_modules/' \
		--exclude='.DS_Store' \
		--exclude='*.pyc' \
		--exclude='__pycache__/' \
		--exclude='*.log' \
		--exclude='.claude/plans/' \
		--exclude='lib/libhv/hconfig.h' \
		--exclude='lib/libhv/lib/' \
		--exclude='lib/libnl/Makefile' \
		--exclude='lib/libnl/config.*' \
		--exclude='lib/libnl/lib/.libs/' \
		./ $(REMOTE_SSH_TARGET):$(REMOTE_DIR)/
	@echo "$(GREEN)✓ Source synced to $(REMOTE_HOST)$(RESET)"

# Fetch build artifacts from remote host
# Retrieves only final binaries (not object files) - use remote-fetch-full for everything
remote-fetch:
	@echo "$(CYAN)$(BOLD)Fetching binaries from $(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/...$(RESET)"
	@mkdir -p build/pi/bin build/ad5m/bin build/bin
	rsync -avz --progress \
		--exclude='*.o' \
		--exclude='*.d' \
		--exclude='*.a' \
		--exclude='obj/' \
		$(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/ \
		./build/
	@echo "$(GREEN)✓ Binaries retrieved from $(REMOTE_HOST)$(RESET)"

# Fetch everything including object files (for incremental local rebuilds)
remote-fetch-full:
	@echo "$(CYAN)$(BOLD)Fetching FULL build from $(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/...$(RESET)"
	@mkdir -p build
	rsync -avz --progress \
		$(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/ \
		./build/
	@echo "$(GREEN)✓ Full build retrieved from $(REMOTE_HOST)$(RESET)"

# Fetch only specific target binaries (fast - only final executables)
# For full build cache (including .o files), use remote-fetch-pi-full
remote-fetch-pi:
	@echo "$(CYAN)Fetching Pi binaries from $(REMOTE_HOST)...$(RESET)"
	@mkdir -p build/pi/bin
	rsync -az \
		$(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/pi/bin/ \
		./build/pi/bin/
	@echo "$(GREEN)✓ Pi binaries retrieved$(RESET)"

remote-fetch-ad5m:
	@echo "$(CYAN)Fetching AD5M binaries from $(REMOTE_HOST)...$(RESET)"
	@mkdir -p build/ad5m/bin
	rsync -az \
		$(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/ad5m/bin/ \
		./build/ad5m/bin/
	@echo "$(GREEN)✓ AD5M binaries retrieved$(RESET)"

remote-fetch-native:
	@echo "$(CYAN)Fetching native Linux binaries from $(REMOTE_HOST)...$(RESET)"
	@mkdir -p build/bin
	rsync -az \
		$(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/bin/ \
		./build/bin/
	@echo "$(GREEN)✓ Native Linux binaries retrieved$(RESET)"

# Full fetch targets (include .o files for local incremental rebuilds - much slower)
remote-fetch-pi-full:
	@echo "$(CYAN)Fetching full Pi build from $(REMOTE_HOST)...$(RESET)"
	@mkdir -p build/pi
	rsync -az --info=progress2 \
		$(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/pi/ \
		./build/pi/
	@echo "$(GREEN)✓ Full Pi build retrieved$(RESET)"

remote-fetch-ad5m-full:
	@echo "$(CYAN)Fetching full AD5M build from $(REMOTE_HOST)...$(RESET)"
	@mkdir -p build/ad5m
	rsync -az --info=progress2 \
		$(REMOTE_SSH_TARGET):$(REMOTE_DIR)/build/ad5m/ \
		./build/ad5m/
	@echo "$(GREEN)✓ Full AD5M build retrieved$(RESET)"

# Clean remote build directory
remote-clean:
	@echo "$(YELLOW)Cleaning remote build directory...$(RESET)"
	ssh $(REMOTE_SSH_TARGET) "cd $(REMOTE_DIR) && make clean"
	@echo "$(GREEN)✓ Remote build cleaned$(RESET)"

# =============================================================================
# Remote Build Targets
# =============================================================================

# remote-sync deliberately excludes .git (submodule .gitignore files would drop
# files the cross build needs), so NOTHING on the build host can resolve HEAD and
# every remote build stamped HELIX_GIT_HASH "unknown" — the binary you then flash
# cannot say which commit produced it. Resolve it here, where the checkout is,
# and carry it in the environment: scripts/gen-git-hash.sh prefers it over its own
# lookup, and mk/cross.mk forwards it into the toolchain container.
REMOTE_GIT_HASH = $(shell git -c safe.directory='*' rev-parse --short HEAD 2>/dev/null)
REMOTE_MAKE_ENV = $(if $(REMOTE_GIT_HASH),HELIX_GIT_HASH=$(REMOTE_GIT_HASH) )

.PHONY: remote-pi remote-ad5m remote-native remote-all

# Build for Raspberry Pi on remote host
# Full cycle: sync → build → fetch
remote-pi: remote-sync
	@echo "$(CYAN)$(BOLD)Building Pi target on $(REMOTE_HOST)...$(RESET)"
	@START_TIME=$$(date +%s); \
	ssh $(REMOTE_SSH_TARGET) "cd $(REMOTE_DIR) && $(REMOTE_MAKE_ENV)make pi-docker" || exit $$?; \
	END_TIME=$$(date +%s); \
	ELAPSED=$$((END_TIME - START_TIME)); \
	echo "$(GREEN)✓ Remote build completed in $${ELAPSED}s$(RESET)"
	@$(MAKE) --no-print-directory remote-fetch-pi
	@echo "$(GREEN)$(BOLD)✓ Pi build complete - binaries in build/pi/$(RESET)"

# Build for Adventurer 5M on remote host
remote-ad5m: remote-sync
	@echo "$(CYAN)$(BOLD)Building AD5M target on $(REMOTE_HOST)...$(RESET)"
	@START_TIME=$$(date +%s); \
	ssh $(REMOTE_SSH_TARGET) "cd $(REMOTE_DIR) && $(REMOTE_MAKE_ENV)make ad5m-docker" || exit $$?; \
	END_TIME=$$(date +%s); \
	ELAPSED=$$((END_TIME - START_TIME)); \
	echo "$(GREEN)✓ Remote build completed in $${ELAPSED}s$(RESET)"
	@$(MAKE) --no-print-directory remote-fetch-ad5m
	@echo "$(GREEN)$(BOLD)✓ AD5M build complete - binaries in build/ad5m/$(RESET)"

# Build native Linux on remote host (no Docker needed)
remote-native: remote-sync
	@echo "$(CYAN)$(BOLD)Building native Linux on $(REMOTE_HOST)...$(RESET)"
	@START_TIME=$$(date +%s); \
	ssh $(REMOTE_SSH_TARGET) "cd $(REMOTE_DIR) && $(REMOTE_MAKE_ENV)make -j" || exit $$?; \
	END_TIME=$$(date +%s); \
	ELAPSED=$$((END_TIME - START_TIME)); \
	echo "$(GREEN)✓ Remote build completed in $${ELAPSED}s$(RESET)"
	@$(MAKE) --no-print-directory remote-fetch-native
	@echo "$(GREEN)$(BOLD)✓ Native Linux build complete - binaries in build/$(RESET)"

# Build all targets on remote (parallel Docker builds)
remote-all: remote-sync
	@echo "$(CYAN)$(BOLD)Building ALL targets on $(REMOTE_HOST)...$(RESET)"
	@START_TIME=$$(date +%s); \
	ssh $(REMOTE_SSH_TARGET) "cd $(REMOTE_DIR) && $(REMOTE_MAKE_ENV)make pi-docker ad5m-docker -j2" || exit $$?; \
	END_TIME=$$(date +%s); \
	ELAPSED=$$((END_TIME - START_TIME)); \
	echo "$(GREEN)✓ Remote builds completed in $${ELAPSED}s$(RESET)"
	@$(MAKE) --no-print-directory remote-fetch
	@echo "$(GREEN)$(BOLD)✓ All builds complete$(RESET)"

# =============================================================================
# Remote Status & Diagnostics
# =============================================================================

.PHONY: remote-status remote-ssh remote-linux-run

# Check remote host status and Docker availability
remote-status:
	@echo "$(CYAN)Checking remote host $(REMOTE_HOST)...$(RESET)"
	@echo ""
	@echo "Connection:"
	@ssh -o ConnectTimeout=5 $(REMOTE_SSH_TARGET) "echo '  ✓ SSH connection OK'" || \
		{ echo "  $(RED)✗ Cannot connect to $(REMOTE_HOST)$(RESET)"; exit 1; }
	@echo ""
	@echo "System info:"
	@ssh $(REMOTE_SSH_TARGET) "echo '  Hostname: '\$$(hostname); \
		echo '  OS: '\$$(uname -s -r); \
		echo '  CPUs: '\$$(nproc); \
		echo '  Memory: '\$$(free -h 2>/dev/null | awk '/^Mem:/{print \$$2}' || echo 'N/A')"
	@echo ""
	@echo "Docker status:"
	@ssh $(REMOTE_SSH_TARGET) "docker info >/dev/null 2>&1 && echo '  ✓ Docker is running' || echo '  $(YELLOW)⚠ Docker not available$(RESET)'"
	@ssh $(REMOTE_SSH_TARGET) "docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1 && echo '  ✓ Pi toolchain image exists' || echo '  ○ Pi toolchain not built yet'"
	@ssh $(REMOTE_SSH_TARGET) "docker image inspect helixscreen/toolchain-ad5m >/dev/null 2>&1 && echo '  ✓ AD5M toolchain image exists' || echo '  ○ AD5M toolchain not built yet'"
	@echo ""
	@echo "Project directory:"
	@ssh $(REMOTE_SSH_TARGET) "test -d $(REMOTE_DIR) && echo '  ✓ $(REMOTE_DIR) exists' || echo '  ○ $(REMOTE_DIR) not created yet'"
	@ssh $(REMOTE_SSH_TARGET) "test -d $(REMOTE_DIR)/build/pi/bin && ls -la $(REMOTE_DIR)/build/pi/bin/*.screen 2>/dev/null | head -1 | awk '{print \"  Pi binary: \" \$$9 \" (\" \$$5 \" bytes)\"}' || echo '  ○ No Pi binaries'"
	@ssh $(REMOTE_SSH_TARGET) "test -d $(REMOTE_DIR)/build/ad5m/bin && ls -la $(REMOTE_DIR)/build/ad5m/bin/*.screen 2>/dev/null | head -1 | awk '{print \"  AD5M binary: \" \$$9 \" (\" \$$5 \" bytes)\"}' || echo '  ○ No AD5M binaries'"

# SSH into remote build host
remote-ssh:
	@echo "$(CYAN)Connecting to $(REMOTE_HOST)...$(RESET)"
	ssh -t $(REMOTE_SSH_TARGET) "cd $(REMOTE_DIR) 2>/dev/null || cd ~; exec \$$SHELL -l"

# Run native Linux build on remote with X11 forwarding
# Requires: XQuartz running locally, X11Forwarding enabled on remote
# Note: Clears SSH control socket to ensure X11 forwarding is established
# Uses wmctrl to reposition window (XQuartz multi-monitor workaround)
# Use REMOTE_RUN_ARGS to pass additional arguments (e.g., --test, -p motion)
remote-linux-run:
	@echo "$(CYAN)Running helix-screen on $(REMOTE_HOST) with X11 forwarding...$(RESET)"
	@echo "$(YELLOW)Tip: Make sure XQuartz is running locally$(RESET)"
	@rm -f ~/.ssh/ctl/*$(REMOTE_HOST)* 2>/dev/null || true
	@ssh -Y $(REMOTE_SSH_TARGET) "cd $(REMOTE_DIR) && ./build/bin/helix-screen $(REMOTE_RUN_ARGS)" & \
		SSH_PID=$$!; \
		sleep 2; \
		wmctrl -r "LVGL" -e 0,100,100,-1,-1 2>/dev/null || true; \
		wait $$SSH_PID

# =============================================================================
# Help
# =============================================================================

.PHONY: help-remote

help-remote:
	@if [ -t 1 ] && [ -n "$(TERM)" ] && [ "$(TERM)" != "dumb" ]; then \
		B='$(BOLD)'; G='$(GREEN)'; Y='$(YELLOW)'; C='$(CYAN)'; R='$(RED)'; X='$(RESET)'; \
	else \
		B=''; G=''; Y=''; C=''; R=''; X=''; \
	fi; \
	echo "$${B}Remote Build System$${X}"; \
	echo "Build on a fast remote Linux host and retrieve binaries locally."; \
	echo ""; \
	echo "$${C}Build Targets:$${X}"; \
	echo "  $${G}remote-pi$${X}             - Sync, build Pi target, fetch binaries"; \
	echo "  $${G}remote-ad5m$${X}           - Sync, build AD5M target, fetch binaries"; \
	echo "  $${G}remote-native$${X}         - Sync, build native Linux, fetch binaries"; \
	echo "  $${G}remote-all$${X}            - Build all cross-compile targets"; \
	echo ""; \
	echo "$${C}Sync & Fetch:$${X}"; \
	echo "  $${G}remote-sync$${X}           - Sync source code to remote (no build)"; \
	echo "  $${G}remote-fetch$${X}          - Fetch all binaries from remote"; \
	echo "  $${G}remote-fetch-pi$${X}       - Fetch Pi binaries only (fast)"; \
	echo "  $${G}remote-fetch-ad5m$${X}     - Fetch AD5M binaries only (fast)"; \
	echo "  $${G}remote-fetch-native$${X}   - Fetch native Linux binaries only (fast)"; \
	echo "  $${G}remote-fetch-pi-full$${X}  - Fetch Pi build + object files (slow)"; \
	echo "  $${G}remote-fetch-ad5m-full$${X} - Fetch AD5M build + object files (slow)"; \
	echo ""; \
	echo "$${C}Utilities:$${X}"; \
	echo "  $${G}remote-status$${X}         - Check remote host status and Docker"; \
	echo "  $${G}remote-ssh$${X}            - SSH into remote build host"; \
	echo "  $${G}remote-linux-run$${X}      - Run native Linux build with X11 forwarding"; \
	echo "  $${G}remote-clean$${X}          - Clean remote build directory"; \
	echo ""; \
	echo "$${C}Configuration (env vars or make args):$${X}"; \
	echo "  $${Y}REMOTE_HOST$${X}=$(REMOTE_HOST)"; \
	echo "  $${Y}REMOTE_USER$${X}=$(if $(REMOTE_USER),$(REMOTE_USER),(from SSH config))"; \
	echo "  $${Y}REMOTE_DIR$${X}=$(REMOTE_DIR)"; \
	echo "  $${Y}REMOTE_RUN_ARGS$${X}=$(if $(REMOTE_RUN_ARGS),$(REMOTE_RUN_ARGS),(extra args for remote-linux-run))"; \
	echo ""; \
	echo "$${C}Examples:$${X}"; \
	echo "  make remote-pi                          # Build Pi on default remote"; \
	echo "  make remote-pi REMOTE_HOST=fast.local   # Use different host"; \
	echo "  make remote-status                      # Check remote is ready"; \
	echo "  make remote-all                         # Build all cross targets"; \
	echo "  make remote-linux-run                   # Run with X11 forwarding"; \
	echo "  make remote-linux-run REMOTE_RUN_ARGS=\"-p motion -s large\""
