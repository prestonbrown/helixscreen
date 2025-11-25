# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Compilation Rules Module
# Handles all compilation rules, linking, and main build targets

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

# Phase 1: Check for unlimited -j and re-invoke if needed
# This target has NO dependencies, so it runs alone even with unlimited -j
.PHONY: all
all:
ifndef _PARALLEL_CHECKED
	@if echo "$(MAKEFLAGS)" | grep -qE '^j$$'; then \
		echo ""; \
		echo "$(YELLOW)$(BOLD)⚠️  'make -j' (unlimited) detected - auto-fixing to -j$(NPROC)$(RESET)"; \
		echo ""; \
		$(MAKE) _PARALLEL_CHECKED=1 -j$(NPROC) $(MAKECMDGOALS); \
	else \
		$(MAKE) _PARALLEL_CHECKED=1 $(MAKECMDGOALS); \
	fi
else
# Phase 2: Actual build (only runs when _PARALLEL_CHECKED is set)
all: check-deps apply-patches generate-fonts $(TARGET)
	$(ECHO) "$(GREEN)$(BOLD)✓ Build complete!$(RESET)"
	$(ECHO) "$(CYAN)Run with: $(YELLOW)./$(TARGET)$(RESET)"
endif

# Build libhv if not present (dependency rule)
$(LIBHV_LIB):
	$(Q)$(MAKE) libhv-build

# Build TinyGL if not present (dependency rule)
# Only build if ENABLE_TINYGL_3D=yes
ifeq ($(ENABLE_TINYGL_3D),yes)
# Track TinyGL source files so incremental builds detect changes
TINYGL_SOURCES := $(wildcard $(TINYGL_DIR)/src/*.c $(TINYGL_DIR)/src/*.h $(TINYGL_DIR)/include/*.h $(TINYGL_DIR)/include/GL/*.h)

$(TINYGL_LIB): $(TINYGL_SOURCES)
	$(ECHO) "$(CYAN)$(BOLD)Building TinyGL...$(RESET)"
	$(Q)cd $(TINYGL_DIR) && MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN_VERSION) $(MAKE)
	$(ECHO) "$(GREEN)✓ TinyGL built: $(TINYGL_LIB)$(RESET)"
endif

# Link binary (SDL2_LIB is empty if using system SDL2)
# Note: Filter out library archives from $^ to avoid duplicate linking, then add via LDFLAGS
$(TARGET): $(SDL2_LIB) $(LIBHV_LIB) $(TINYGL_LIB) $(APP_C_OBJS) $(APP_OBJS) $(OBJCPP_OBJS) $(LVGL_OBJS) $(THORVG_OBJS) $(FONT_OBJS) $(MATERIAL_ICON_OBJS) $(WPA_DEPS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) $@"
	$(Q)$(CXX) $(CXXFLAGS) $(filter-out %.a,$^) -o $@ $(LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Linking failed!$(RESET)"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) [objects] -o $@ $(LDFLAGS)"; \
		exit 1; \
	}

# Collect all header dependencies
HEADERS := $(shell find $(INC_DIR) -name "*.h" 2>/dev/null)

# Precompiled header rule (must be built before any C++ compilation)
$(PCH): $(PCH_HEADER) $(HEADERS) $(LIBHV_LIB)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(MAGENTA)$(BOLD)[PCH]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -x c++-header -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -x c++-header -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ PCH compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile app C sources (depend on headers and libhv for hv/json.hpp)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) $(LIBHV_LIB)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CC]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CC) $(CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		echo "$(YELLOW)Command:$(RESET) $(CC) $(CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"; \
		exit 1; \
	}

# Compile app C++ sources (depend on headers, libhv, and PCH)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) $(LIBHV_LIB) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"; \
		exit 1; \
	}

# Compile app Objective-C++ sources (macOS .mm files, depend on headers, libhv, and PCH)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.mm $(HEADERS) $(LIBHV_LIB) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[OBJCXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"; \
		exit 1; \
	}

# Compile LVGL sources
$(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CC]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CC) $(CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile LVGL C++ sources (ThorVG) - use PCH
$(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.cpp $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile font sources
$(OBJ_DIR)/assets/fonts/%.o: assets/fonts/%.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(GREEN)[FONT]$(RESET) $<"
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)✗ Font compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile Material Design icon sources
$(OBJ_DIR)/assets/images/material/%.o: assets/images/material/%.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(GREEN)[ICON]$(RESET) $<"
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)✗ Icon compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Run the prototype
run: $(TARGET)
	$(ECHO) "$(CYAN)Running UI prototype...$(RESET)"
	$(Q)$(TARGET)

# Clean build artifacts
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
ifeq ($(ENABLE_TINYGL_3D),yes)
	$(Q)if [ -f "$(TINYGL_LIB)" ]; then \
		echo "$(YELLOW)→ Cleaning TinyGL...$(RESET)"; \
		cd $(TINYGL_DIR) && $(MAKE) clean; \
	fi
endif
ifneq ($(UNAME_S),Darwin)
	$(Q)if [ -f "$(WPA_CLIENT_LIB)" ]; then \
		echo "$(YELLOW)→ Cleaning wpa_supplicant...$(RESET)"; \
		$(MAKE) -C $(WPA_DIR)/wpa_supplicant clean; \
	fi
endif

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
	$(Q)$(CC) -c $< -o $@ $(LVGL_INC) $(CFLAGS)

# Generate compile_commands.json for IDE/LSP support
compile_commands:
	$(ECHO) "$(CYAN)Generating compile_commands.json...$(RESET)"
	$(Q)if command -v compiledb >/dev/null 2>&1; then \
		echo "$(CYAN)Using compiledb...$(RESET)"; \
		compiledb make -n -B; \
	elif [ -f .venv/bin/compiledb ]; then \
		echo "$(CYAN)Using .venv/bin/compiledb...$(RESET)"; \
		.venv/bin/compiledb make -n -B; \
	elif command -v bear >/dev/null 2>&1; then \
		echo "$(CYAN)Using bear...$(RESET)"; \
		bear -- $(MAKE) clean; \
		bear -- $(MAKE); \
	else \
		echo "$(RED)Error: Neither 'compiledb' nor 'bear' found$(RESET)"; \
		echo "Install compiledb: $(YELLOW)pip install compiledb$(RESET)"; \
		echo "Or install bear: $(YELLOW)brew install bear$(RESET)"; \
		exit 1; \
	fi
	$(ECHO) "$(GREEN)✓ compile_commands.json generated$(RESET)"
	$(ECHO) ""
	$(ECHO) "$(CYAN)IDE/LSP integration ready. Restart your editor to pick up changes.$(RESET)"
