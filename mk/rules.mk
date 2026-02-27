# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Compilation Rules Module
# Handles all compilation rules, linking, and main build targets

# ============================================================================
# BUILD UP-TO-DATE CHECK (for pre-commit hook)
# ============================================================================
# Problem: 'make -q' can't evaluate shell commands in the 'all' target,
# so it always returns "needs rebuild" even when the build is current.
#
# Solution: A fast target that uses 'find -newer' to compare source timestamps
# against the binary. Used by scripts/quality-checks.sh to skip unnecessary builds.
#
# Exit codes: 0 = up to date, 1 = needs rebuild
# ============================================================================
.PHONY: check-uptodate
check-uptodate:
	@if [ ! -f "$(TARGET)" ]; then \
		exit 1; \
	fi
	@if find $(SRC_DIR) $(INC_DIR) -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.mm' \) -newer "$(TARGET)" 2>/dev/null | grep -q .; then \
		exit 1; \
	fi

# ============================================================================
# UNLIMITED -j DETECTION AND AUTO-FIX
# ============================================================================
# Problem: 'make -j' (no number) means UNLIMITED parallelism in GNU Make.
# This spawns hundreds of compiler processes, crushing the system (load 100+).
#
# Solution: We use a two-phase build. The default 'all' target checks for
# unlimited -j and re-invokes make with bounded parallelism if needed.
# The _PARALLEL_CHECKED variable prevents infinite recursion.
#
# MAKEFLAGS format: "j" = unlimited, " --jobserver-fds=X,Y -j" = bounded
# ============================================================================

# ============================================================================
# ARCHITECTURE CHANGE DETECTION
# ============================================================================
# Problem: Switching between native and cross-compilation targets leaves
# stale object files from the wrong architecture, causing cryptic linker errors.
#
# Solution: Track the last build target in .build-target and auto-clean when
# the target changes. This prevents mixing ARM and x86/ARM64 objects.
# ============================================================================
ARCH_MARKER := $(BUILD_DIR)/.build-target
# Map pi-both → pi, pi32-both → pi32 so switching between single/dual-link
# modes doesn't trigger an unnecessary clean (they share the same build dir)
_RAW_TARGET := $(if $(PLATFORM_TARGET),$(PLATFORM_TARGET),native)
CURRENT_TARGET := $(subst pi32-both,pi32,$(subst pi-both,pi,$(_RAW_TARGET)))

# Check if architecture changed and clean if needed
define check-arch-change
	@mkdir -p $(BUILD_DIR)
	@if [ -f "$(ARCH_MARKER)" ]; then \
		LAST_TARGET=$$(cat "$(ARCH_MARKER)"); \
		if [ "$$LAST_TARGET" != "$(CURRENT_TARGET)" ]; then \
			echo ""; \
			echo "$(YELLOW)$(BOLD)⚠️  Build target changed: $$LAST_TARGET → $(CURRENT_TARGET)$(RESET)"; \
			echo "$(CYAN)Auto-cleaning to avoid mixing architectures...$(RESET)"; \
			echo ""; \
			$(MAKE) clean; \
			mkdir -p $(BUILD_DIR); \
		fi; \
	fi
	@echo "$(CURRENT_TARGET)" > "$(ARCH_MARKER)"
endef

# Dependency check stamp file - created by check-deps, prevents re-checking
DEPS_CHECKED_MARKER := $(BUILD_DIR)/.deps-checked

# Phase 1: Check for unlimited -j AND architecture changes, then re-invoke if needed
# This target has NO dependencies, so it runs alone even with unlimited -j
.PHONY: all
all:
ifndef _PARALLEL_CHECKED
	@# First check for architecture change BEFORE anything else
	@mkdir -p $(BUILD_DIR)
	@if [ -f "$(ARCH_MARKER)" ]; then \
		LAST_TARGET=$$(cat "$(ARCH_MARKER)"); \
		if [ "$$LAST_TARGET" != "$(CURRENT_TARGET)" ]; then \
			echo ""; \
			echo "$(YELLOW)$(BOLD)⚠️  Build target changed: $$LAST_TARGET → $(CURRENT_TARGET)$(RESET)"; \
			echo "$(CYAN)Auto-cleaning to avoid mixing architectures...$(RESET)"; \
			echo ""; \
			$(MAKE) clean; \
			mkdir -p $(BUILD_DIR); \
		fi; \
	fi
	@echo "$(CURRENT_TARGET)" > "$(ARCH_MARKER)"
	@# Check dependencies BEFORE parallel build starts (prevents confusing errors)
	@# Only run if marker is missing or older than check script
	@# SKIP_OPTIONAL_DEPS=1 passes --minimal for cross-compilation builds (Docker)
	@if [ ! -f "$(DEPS_CHECKED_MARKER)" ] || [ "scripts/check-deps.sh" -nt "$(DEPS_CHECKED_MARKER)" ]; then \
		CC="$(CC)" CXX="$(CXX)" ENABLE_SSL="$(ENABLE_SSL)" \
			LVGL_DIR="$(LVGL_DIR)" SPDLOG_DIR="$(SPDLOG_DIR)" \
			LIBHV_DIR="$(LIBHV_DIR)" WPA_DIR="$(WPA_DIR)" VENV="$(VENV)" \
			./scripts/check-deps.sh $(if $(filter 1,$(SKIP_OPTIONAL_DEPS)),--minimal,) || exit 1; \
		touch "$(DEPS_CHECKED_MARKER)"; \
	fi
	@# Auto-parallelize: add -j$(NPROC) unless bounded -jN already set
	@if echo "$(MAKEFLAGS)" | grep -q 'jobserver'; then \
		exec $(MAKE) _PARALLEL_CHECKED=1 $(MAKECMDGOALS); \
	else \
		if echo "$(MAKEFLAGS)" | grep -q 'j'; then \
			echo ""; \
			echo "$(YELLOW)$(BOLD)⚠️  'make -j' (unlimited) detected - auto-fixing to -j$(NPROC)$(RESET)"; \
			echo ""; \
		fi; \
		exec $(MAKE) _PARALLEL_CHECKED=1 -j$(NPROC) $(MAKECMDGOALS); \
	fi
else
# Phase 2: Actual build (only runs when _PARALLEL_CHECKED is set)
ifdef PI_DUAL_LINK
all: apply-patches generate-fonts $(TRANS_GEN_C) splash watchdog $(TARGET) $(FBDEV_TARGET) verify-fbdev strip-both
else
all: apply-patches generate-fonts $(TRANS_GEN_C) splash watchdog $(TARGET) strip
endif
	$(ECHO) "$(GREEN)$(BOLD)✓ Build complete!$(RESET)"
	$(ECHO) "$(CYAN)Run with: $(YELLOW)./$(TARGET)$(RESET)"
ifndef SKIP_COMPILE_COMMANDS
	@# Auto-generate compile_commands.json from fragments (fast, <0.3s)
	@# Uses Python to produce properly indented JSON that VS Code can parse
	@# Skip with SKIP_COMPILE_COMMANDS=1 (used by pre-commit to avoid LSP churn)
	@if [ -d "$(BUILD_DIR)" ]; then \
		CCJ_COUNT=$$(find $(BUILD_DIR) -name '*.ccj' 2>/dev/null | wc -l | tr -d ' '); \
		if [ "$$CCJ_COUNT" -gt 0 ]; then \
			find $(BUILD_DIR) -name '*.ccj' -print0 2>/dev/null | xargs -0 cat | \
				python3 -c "import sys,json; entries=[json.loads(l) for l in sys.stdin if l.strip()]; json.dump(entries, open('compile_commands.json','w'), indent=2)"; \
			echo "$(CYAN)→ compile_commands.json updated ($$CCJ_COUNT entries)$(RESET)"; \
		fi; \
	fi
endif
endif

# Build libhv if not present (dependency rule)
$(LIBHV_LIB):
	$(Q)$(MAKE) libhv-build

# Link binary (SDL2_LIB is empty if using system SDL2)
# Note: Filter out library archives from $^ to avoid duplicate linking, then add via LDFLAGS
$(TARGET): $(SDL2_LIB) $(LIBHV_LIB) $(APP_C_OBJS) $(APP_OBJS) $(APP_MODULE_OBJS) $(OBJCPP_OBJS) $(LVGL_OBJS) $(HELIX_XML_OBJS) $(THORVG_OBJS) $(LVGL_OPENGLES_OBJS) $(LV_MARKDOWN_OBJS) $(FONT_OBJS) $(TRANS_OBJS) $(WPA_DEPS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) $@"
	$(Q)$(CXX) $(CXXFLAGS) $(filter-out %.a,$^) -o $@ $(LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Linking failed!$(RESET)"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) [objects] -o $@ $(LDFLAGS)"; \
		exit 1; \
	}

# Collect all .d dependency files for proper header tracking
# These are generated during compilation with -MMD -MP flags
DEPFILES := $(wildcard $(OBJ_DIR)/*.d $(OBJ_DIR)/**/*.d)

# ============================================================================
# COMPILE COMMANDS FRAGMENT GENERATION
# ============================================================================
# Each compilation emits a .ccj (compile command JSON) fragment alongside .o
# Running 'make compile_commands' merges them instantly (no full rebuild needed)
#
# Usage: $(call emit-compile-command,compiler,flags,source,output)
# Example: $(call emit-compile-command,$(CXX),$(CXXFLAGS) $(INCLUDES),$<,$@)
#
# Note: Uses sed to escape double quotes in the command for valid JSON
# ============================================================================
define emit-compile-command
	@CMD="$(1) $(2) -c $(3) -o $(4)"; \
	CMD_ESC=$$(echo "$$CMD" | sed 's/"/\\"/g'); \
	printf '{"directory": "%s", "file": "%s", "command": "%s"}\n' \
		"$(CURDIR)" \
		"$(abspath $(3))" \
		"$$CMD_ESC" \
		> $(4:.o=.ccj)
endef

# Precompiled header rule (must be built before any C++ compilation)
# PCH only depends on its source header and external libraries (LVGL, spdlog)
# Project headers are NOT included - changing app headers should not invalidate PCH
# The PCH contains only stable, rarely-changing includes (see include/lvgl_pch.h)
# CRITICAL: lv_conf.h must be listed - it controls LVGL feature flags
$(PCH): $(PCH_HEADER) $(LIBHV_LIB) lv_conf.h
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(MAGENTA)$(BOLD)[PCH]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -x c++-header -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -x c++-header -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ PCH compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile app C sources
# Uses DEPFLAGS to generate .d files for header dependency tracking
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(LIBHV_LIB)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CC]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		echo "$(YELLOW)Command:$(RESET) $(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CC),$(CFLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile app C++ sources (depend on libhv and PCH)
# Uses DEPFLAGS to generate .d files for header dependency tracking
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(LIBHV_LIB) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CXX),$(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile app Objective-C++ sources (macOS .mm files)
# Uses DEPFLAGS to generate .d files for header dependency tracking
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.mm $(LIBHV_LIB) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[OBJCXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CXX),$(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile LVGL sources (use SUBMODULE_CFLAGS to suppress third-party warnings)
# NOTE: No DEPFLAGS for internal LVGL headers - 600+ files whose deps bloat make startup.
# However, lv_conf.h IS tracked as an explicit prerequisite since it controls feature flags
# (e.g. LV_BIN_DECODER_RAM_LOAD) and missing rebuilds cause subtle runtime failures.
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c lv_conf.h
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CC]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CC) $(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CC) $(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CC),$(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile Helix XML sources (extracted from LVGL, with our patches baked in)
$(OBJ_DIR)/helix-xml/%.o: $(HELIX_XML_DIR)/%.c lv_conf.h
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CC]$(RESET) $<"
	$(Q)$(CC) $(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CC),$(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile lv_markdown C sources (markdown viewer + md4c parser)
$(OBJ_DIR)/lv_markdown/%.o: $(LV_MARKDOWN_DIR)/%.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CC]$(RESET) $<"
	$(Q)$(CC) $(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CC),$(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile LVGL C++ sources (ThorVG) - use SUBMODULE_CXXFLAGS and PCH
# NOTE: No DEPFLAGS for internal headers - see C rule above for rationale.
# lv_conf.h tracked explicitly as it controls LVGL feature flags.
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.cpp $(PCH) lv_conf.h
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(SUBMODULE_CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(SUBMODULE_CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CXX),$(SUBMODULE_CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile LVGL OpenGL ES shader assets as C++ (raw string literals require C++11)
# Only the assets/ subdirectory needs C++ — the rest compiles fine as C.
# -fpermissive allows void* implicit conversions from C-style LVGL allocations.
# Only built when ENABLE_OPENGLES=yes (LVGL_OPENGLES_OBJS is empty otherwise).
$(LVGL_OPENGLES_OBJS): $(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c lv_conf.h
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CXX/GLES]$(RESET) $<"
	$(Q)$(CXX) $(SUBMODULE_CXXFLAGS) -fpermissive $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CXX),$(SUBMODULE_CXXFLAGS) -fpermissive $(INCLUDES) $(LV_CONF),$<,$@)

# Compile lv_markdown sources (vendored C library - use SUBMODULE_CFLAGS)
# Includes both src/*.c and deps/md4c/md4c.c
$(OBJ_DIR)/lv_markdown/%.o: $(LV_MARKDOWN_DIR)/%.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CC]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CC) $(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CC) $(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CC),$(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile font sources (generated by lv_font_conv - use SUBMODULE flags)
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/assets/fonts/%.o: assets/fonts/%.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(GREEN)[FONT]$(RESET) $<"
	$(Q)$(CC) $(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)✗ Font compilation failed:$(RESET) $<"; \
		exit 1; \
	}
	$(call emit-compile-command,$(CC),$(SUBMODULE_CFLAGS) $(INCLUDES) $(LV_CONF),$<,$@)

# Run the prototype
run: $(TARGET)
	$(ECHO) "$(CYAN)Running UI prototype...$(RESET)"
	$(Q)$(TARGET)

# Clean build artifacts
# Note: Libraries are built in-tree (lib/*/), then copied to $(BUILD_DIR)/lib/
# Clean removes both the output directory AND the in-tree build artifacts
clean:
	$(ECHO) "$(YELLOW)Cleaning build artifacts...$(RESET)"
	$(Q)if [ -d "$(BUILD_DIR)" ]; then \
		echo "$(YELLOW)→ Removing:$(RESET) $(BUILD_DIR)"; \
		rm -rf $(BUILD_DIR); \
		echo "$(GREEN)✓ Clean complete$(RESET)"; \
	else \
		echo "$(GREEN)✓ Already clean (no build directory)$(RESET)"; \
	fi
	$(Q)rm -f .fonts.stamp
	$(Q)rm -f compile_commands.json
	$(Q)if [ -d "$(SDL2_BUILD_DIR)" ]; then \
		echo "$(YELLOW)→ Cleaning SDL2 build...$(RESET)"; \
		rm -rf $(SDL2_BUILD_DIR); \
	fi
	$(Q)if [ -d "$(LIBHV_DIR)" ] && [ -n "$$(find $(LIBHV_DIR) -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.dylib' 2>/dev/null)" ]; then \
		echo "$(YELLOW)→ Cleaning libhv build artifacts...$(RESET)"; \
		find $(LIBHV_DIR) -type f \( -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.dylib' \) -delete; \
	fi
ifneq ($(UNAME_S),Darwin)
	$(Q)if [ -d "$(WPA_DIR)/wpa_supplicant" ] && [ -f "$(WPA_DIR)/wpa_supplicant/libwpa_client.a" ]; then \
		echo "$(YELLOW)→ Cleaning wpa_supplicant...$(RESET)"; \
		$(MAKE) -C $(WPA_DIR)/wpa_supplicant clean; \
	fi
endif

# Deep clean - reset to fresh checkout state
# Removes everything including dependencies (node_modules, .venv, all library builds)
# Use this when you want a completely fresh start
distclean: clean
	$(ECHO) "$(YELLOW)Deep cleaning (fresh checkout state)...$(RESET)"
	@# Clean all submodule library builds
	$(Q)if [ -d "$(LIBHV_DIR)" ]; then \
		echo "$(YELLOW)→ Cleaning libhv...$(RESET)"; \
		find $(LIBHV_DIR) -type f \( -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.dylib' \) -delete 2>/dev/null || true; \
		rm -rf $(LIBHV_DIR)/build 2>/dev/null || true; \
	fi
	$(Q)if [ -d "lib/libnl" ]; then \
		echo "$(YELLOW)→ Cleaning libnl...$(RESET)"; \
		if [ -f "lib/libnl/Makefile" ]; then $(MAKE) -C lib/libnl distclean 2>/dev/null || true; fi; \
	fi
	$(Q)if [ -d "lib/sdl2/build" ]; then \
		echo "$(YELLOW)→ Cleaning SDL2 submodule build...$(RESET)"; \
		rm -rf lib/sdl2/build; \
	fi
ifneq ($(UNAME_S),Darwin)
	$(Q)if [ -d "$(WPA_DIR)/wpa_supplicant" ]; then \
		echo "$(YELLOW)→ Cleaning wpa_supplicant...$(RESET)"; \
		$(MAKE) -C $(WPA_DIR)/wpa_supplicant clean 2>/dev/null || true; \
	fi
endif
	@# Clean installed dependencies
	$(Q)if [ -d "node_modules" ]; then \
		echo "$(YELLOW)→ Removing node_modules/...$(RESET)"; \
		rm -rf node_modules; \
	fi
	$(Q)if [ -d ".venv" ]; then \
		echo "$(YELLOW)→ Removing .venv/...$(RESET)"; \
		rm -rf .venv; \
	fi
	$(Q)rm -f package-lock.json 2>/dev/null || true
	$(ECHO) "$(GREEN)$(BOLD)✓ Deep clean complete (run 'make install-deps' to reinstall)$(RESET)"

# Parallel build target with progress
build:
	$(ECHO) "$(CYAN)$(BOLD)Starting clean parallel build...$(RESET)"
	$(ECHO) "$(CYAN)Platform:$(RESET) $(PLATFORM) | $(CYAN)Jobs:$(RESET) $(NPROC) | $(CYAN)Compiler:$(RESET) $(CXX)"
	@$(MAKE) clean
	@echo ""
	@echo "$(CYAN)Building (use -j for parallel builds)...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(MAKE) all && \
	END_TIME=$$(date +%s); \
	DURATION=$$((END_TIME - START_TIME)); \
	echo ""; \
	echo "$(GREEN)$(BOLD)✓ Build completed in $${DURATION}s$(RESET)"

# Demo widgets target
demo: $(LVGL_OBJS) $(THORVG_OBJS) $(LVGL_DEMO_OBJS)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) lvgl-demo"
	$(Q)$(CXX) -o $(BIN_DIR)/lvgl-demo $(SRC_DIR)/lvgl-demo/main.cpp $(LVGL_OBJS) $(THORVG_OBJS) $(LVGL_DEMO_OBJS) \
	  $(INCLUDES) $(CXXFLAGS) $(SDL2_LIBS)
	$(ECHO) "$(GREEN)✓ Demo ready:$(RESET) ./build/bin/lvgl-demo"
	$(ECHO) ""
	$(ECHO) "$(CYAN)Run with:$(RESET) $(YELLOW)./build/bin/lvgl-demo$(RESET)"

$(OBJ_DIR)/lvgl/demos/%.o: $(LVGL_DIR)/demos/%.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[DEMO]$(RESET) $<"
	$(Q)$(CC) -c $< -o $@ $(LVGL_INC) $(SUBMODULE_CFLAGS)

# Generate compile_commands.json for IDE/LSP support
# Fast incremental approach: merges .ccj fragments generated during compilation
# First build creates fragments, subsequent 'make compile_commands' is instant
compile_commands:
	$(ECHO) "$(CYAN)Generating compile_commands.json from build fragments...$(RESET)"
	@CCJ_FILES=$$(find $(BUILD_DIR) -name '*.ccj' 2>/dev/null | head -1); \
	if [ -z "$$CCJ_FILES" ]; then \
		echo "$(YELLOW)No .ccj fragments found. Running build first...$(RESET)"; \
		$(MAKE) all test-build; \
	fi
	@echo "[" > compile_commands.json
	@find $(BUILD_DIR) -name '*.ccj' -print0 2>/dev/null | xargs -0 cat | \
		sed 's/}$$/},/' | \
		sed '$$ s/,$$//' >> compile_commands.json
	@echo "]" >> compile_commands.json
	@CCJ_COUNT=$$(find $(BUILD_DIR) -name '*.ccj' 2>/dev/null | wc -l | tr -d ' '); \
	echo "$(GREEN)✓ compile_commands.json generated ($$CCJ_COUNT entries)$(RESET)"
	$(ECHO) ""
	$(ECHO) "$(CYAN)IDE/LSP integration ready. Restart your editor to pick up changes.$(RESET)"

# Legacy full regeneration (slow) - use if fragments get out of sync
compile_commands_full:
	$(ECHO) "$(CYAN)Regenerating compile_commands.json (full rebuild)...$(RESET)"
	$(Q)if command -v compiledb >/dev/null 2>&1; then \
		echo "$(CYAN)Using compiledb...$(RESET)"; \
		compiledb make -n -B all test-build; \
	elif [ -f .venv/bin/compiledb ]; then \
		echo "$(CYAN)Using .venv/bin/compiledb...$(RESET)"; \
		.venv/bin/compiledb make -n -B all test-build; \
	elif command -v bear >/dev/null 2>&1; then \
		echo "$(CYAN)Using bear...$(RESET)"; \
		bear -- $(MAKE) clean; \
		bear -- $(MAKE) all; \
		bear --append -- $(MAKE) test-build; \
	else \
		echo "$(RED)Error: Neither 'compiledb' nor 'bear' found$(RESET)"; \
		echo "Install compiledb: $(YELLOW)pip install compiledb$(RESET)"; \
		echo "Or install bear: $(YELLOW)brew install bear$(RESET)"; \
		exit 1; \
	fi
	$(ECHO) "$(GREEN)✓ compile_commands.json generated$(RESET)"

# ============================================================================
# Automatic Header Dependency Tracking
# ============================================================================
# Include generated .d files for proper header dependency tracking.
# The - prefix means don't error if files don't exist (first build).
# Files are generated during compilation with -MMD -MP flags.
#
# Pattern explanation:
#   $(OBJ_DIR)/*.d          - App sources in obj root
#   $(OBJ_DIR)/*/*.d        - App sources in subdirs (e.g., obj/tools/)
#   $(OBJ_DIR)/tests/*.d    - Unit test sources
#   $(OBJ_DIR)/tests/*/*.d  - Mock sources and nested test dirs
#   $(OBJ_DIR)/lvgl/*.d     - LVGL sources (shallow)
#   $(OBJ_DIR)/lvgl/*/*.d   - LVGL sources (one level deep)
#   $(OBJ_DIR)/lvgl/*/*/*.d - LVGL sources (two levels deep - libs/thorvg/*)
#   $(OBJ_DIR)/lvgl/*/*/*/*.d - LVGL sources (three levels deep)
-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(OBJ_DIR)/*/*.d)
-include $(wildcard $(OBJ_DIR)/*/*/*.d)
-include $(wildcard $(OBJ_DIR)/tests/*.d)
-include $(wildcard $(OBJ_DIR)/tests/*/*.d)
# NOTE: LVGL .d files intentionally NOT included - they add 150k+ lines of deps
# that slow make startup by 4+ seconds. LVGL rarely changes; do clean build if needed.
