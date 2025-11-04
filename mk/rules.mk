# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Compilation Rules Module
# Handles all compilation rules, linking, and main build targets

# Main build target
all: check-deps apply-patches generate-fonts $(TARGET)
	$(ECHO) "$(GREEN)$(BOLD)✓ Build complete!$(RESET)"
	$(ECHO) "$(CYAN)Run with: $(YELLOW)./$(TARGET)$(RESET)"

# Build libhv if not present (dependency rule)
$(LIBHV_LIB):
	$(Q)$(MAKE) libhv-build

# Link binary
$(TARGET): $(LIBHV_LIB) $(APP_C_OBJS) $(APP_OBJS) $(OBJCPP_OBJS) $(LVGL_OBJS) $(THORVG_OBJS) $(FONT_OBJS) $(MATERIAL_ICON_OBJS) $(WPA_DEPS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) $@"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Linking failed!$(RESET)"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) [objects] -o $@ $(LDFLAGS)"; \
		exit 1; \
	}

# Collect all header dependencies
HEADERS := $(shell find $(INC_DIR) -name "*.h" 2>/dev/null)

# Compile app C sources (depend on headers)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
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

# Compile app C++ sources (depend on headers)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"; \
		exit 1; \
	}

# Compile app Objective-C++ sources (macOS .mm files, depend on headers)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.mm $(HEADERS)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[OBJCXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"; \
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

# Compile LVGL C++ sources (ThorVG)
$(OBJ_DIR)/lvgl/%.o: $(LVGL_DIR)/%.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@ || { \
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
	@echo "$(CYAN)Building with $(NPROC) parallel jobs...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(MAKE) -j$(NPROC) all && \
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
	$(ECHO) "$(CYAN)Checking for bear...$(RESET)"
	$(Q)if ! command -v bear >/dev/null 2>&1; then \
		echo "$(RED)Error: 'bear' not found$(RESET)"; \
		echo "Install with: $(YELLOW)brew install bear$(RESET) (macOS) or $(YELLOW)apt install bear$(RESET) (Linux)"; \
		exit 1; \
	fi
	$(ECHO) "$(CYAN)Generating compile_commands.json...$(RESET)"
	$(Q)bear -- $(MAKE) clean
	$(Q)bear -- $(MAKE) -j$(NPROC)
	$(ECHO) "$(GREEN)✓ compile_commands.json generated$(RESET)"
	$(ECHO) ""
	$(ECHO) "$(CYAN)IDE/LSP integration ready. Restart your editor to pick up changes.$(RESET)"
