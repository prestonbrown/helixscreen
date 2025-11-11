# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Test Module
# Handles all test compilation and execution targets

# ============================================================================
# Test Dependency Groups
# ============================================================================
# These semantic groups make it clear what each test binary requires.
# When adding a new source file, add the corresponding .o to the appropriate group.
#
# Dependency Philosophy:
# - TEST_CORE_DEPS: Always required (Catch2, test utilities, test objects)
# - TEST_LVGL_DEPS: Required for all UI tests (includes ThorVG for SVG support)
# - TEST_WIZARD_DEPS: Wizard validation and UI screens
# - TEST_UI_DEPS: Common UI components (navigation, theme, modals)
# - TEST_WIFI_DEPS: Network managers and backends
# - TEST_MOONRAKER_DEPS: Printer communication (includes libhv WebSocket)
# - TEST_CONFIG_DEPS: Configuration and utilities
# - TEST_PLATFORM_DEPS: Platform-specific (wpa_supplicant on Linux)

# Core test infrastructure (always required)
TEST_CORE_DEPS := $(TEST_MAIN_OBJ) $(CATCH2_OBJ) $(UI_TEST_UTILS_OBJ) $(TEST_OBJS)

# LVGL + Graphics stack (required for all UI tests)
TEST_LVGL_DEPS := $(LVGL_OBJS) $(THORVG_OBJS)

# Wizard components (validation, UI screens)
TEST_WIZARD_DEPS := \
    $(OBJ_DIR)/wizard_validation.o \
    $(OBJ_DIR)/ui_wizard.o \
    $(OBJ_DIR)/ui_wizard_wifi.o \
    $(OBJ_DIR)/ui_wizard_connection.o \
    $(OBJ_DIR)/ui_wizard_bed_select.o \
    $(OBJ_DIR)/ui_wizard_hotend_select.o \
    $(OBJ_DIR)/ui_wizard_fan_select.o \
    $(OBJ_DIR)/ui_wizard_led_select.o \
    $(OBJ_DIR)/ui_wizard_printer_identify.o \
    $(OBJ_DIR)/ui_wizard_summary.o

# UI components (theme, modals, navigation)
TEST_UI_DEPS := \
    $(OBJ_DIR)/ui_nav.o \
    $(OBJ_DIR)/ui_temp_graph.o \
    $(OBJ_DIR)/ui_keyboard.o \
    $(OBJ_DIR)/ui_modal.o \
    $(OBJ_DIR)/ui_theme.o \
    $(OBJ_DIR)/helix_theme.o

# Network/WiFi components
TEST_WIFI_DEPS := \
    $(OBJ_DIR)/wifi_manager.o \
    $(OBJ_DIR)/wifi_backend.o \
    $(OBJ_DIR)/wifi_backend_mock.o \
    $(OBJ_DIR)/ethernet_manager.o \
    $(OBJ_DIR)/ethernet_backend.o \
    $(OBJ_DIR)/ethernet_backend_mock.o \
    $(OBJCPP_OBJS)

# Moonraker/printer components
TEST_MOONRAKER_DEPS := \
    $(OBJ_DIR)/moonraker_client.o \
    $(OBJ_DIR)/moonraker_api.o \
    $(OBJ_DIR)/printer_state.o \
    $(OBJ_DIR)/printer_detector.o \
    $(LIBHV_LIB)

# Configuration and utilities
TEST_CONFIG_DEPS := \
    $(OBJ_DIR)/config.o \
    $(OBJ_DIR)/tips_manager.o

# Platform-specific dependencies (Linux wpa_supplicant, macOS frameworks via LDFLAGS)
TEST_PLATFORM_DEPS := $(WPA_DEPS)

# ============================================================================
# Test Targets
# ============================================================================

# Clean test artifacts
clean-tests:
	$(ECHO) "$(YELLOW)Cleaning test artifacts...$(RESET)"
	$(Q)rm -f $(TEST_BIN) $(TEST_MAIN_OBJ) $(CATCH2_OBJ) $(UI_TEST_UTILS_OBJ) $(TEST_OBJS)
	$(ECHO) "$(GREEN)✓ Test artifacts cleaned$(RESET)"

# Build tests in parallel
test-build:
	$(ECHO) "$(CYAN)$(BOLD)Building tests in parallel ($(NPROC) jobs)...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(MAKE) -j$(NPROC) $(TEST_BIN) && \
	END_TIME=$$(date +%s); \
	DURATION=$$((END_TIME - START_TIME)); \
	echo "$(GREEN)✓ Tests built in $${DURATION}s$(RESET)"

# Unified test binary with all unit tests
test: $(TEST_BIN)
	$(ECHO) "$(CYAN)$(BOLD)Running unit tests...$(RESET)"
	$(Q)$(TEST_BIN) || { \
		echo "$(RED)$(BOLD)✗ Tests failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)$(BOLD)✓ All tests passed!$(RESET)"

# Backwards compatibility alias
test-wizard: test

# Run only config tests
test-config: $(TEST_BIN)
	$(ECHO) "$(CYAN)$(BOLD)Running config tests...$(RESET)"
	$(Q)$(TEST_BIN) "[config]" || { \
		echo "$(RED)$(BOLD)✗ Config tests failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)$(BOLD)✓ Config tests passed!$(RESET)"

# Unified test binary - links all dependencies using semantic groups
# If you get linker errors about missing symbols, check which group contains
# the required .o file and ensure it's included in the appropriate TEST_*_DEPS above
$(TEST_BIN): $(TEST_CORE_DEPS) \
             $(TEST_LVGL_DEPS) \
             $(TEST_WIZARD_DEPS) \
             $(TEST_UI_DEPS) \
             $(TEST_WIFI_DEPS) \
             $(TEST_MOONRAKER_DEPS) \
             $(TEST_CONFIG_DEPS) \
             $(TEST_PLATFORM_DEPS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) run_tests"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Test linking failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)✓ Unit test binary ready$(RESET)"

# Integration test binary (uses mocks instead of real LVGL)
$(TEST_INTEGRATION_BIN): $(TEST_MAIN_OBJ) $(CATCH2_OBJ) $(TEST_INTEGRATION_OBJS) $(MOCK_OBJS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) run_integration_tests"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Integration test linking failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)✓ Integration test binary ready$(RESET)"

# Run integration tests
test-integration: $(TEST_INTEGRATION_BIN)
	$(ECHO) "$(CYAN)$(BOLD)Running integration tests (with mocks)...$(RESET)"
	$(Q)$(TEST_INTEGRATION_BIN) || { \
		echo "$(RED)$(BOLD)✗ Integration tests failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)$(BOLD)✓ All integration tests passed!$(RESET)"

# Compile test main (Catch2 runner)
$(TEST_MAIN_OBJ): $(TEST_DIR)/test_main.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST-MAIN]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -I$(TEST_DIR) -c $< -o $@

# Compile Catch2 amalgamated source
$(CATCH2_OBJ): $(TEST_DIR)/catch_amalgamated.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CATCH2]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile UI test utilities
$(UI_TEST_UTILS_OBJ): $(TEST_DIR)/ui_test_utils.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[UI-TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -I$(TEST_DIR) $(INCLUDES) -c $< -o $@

# Compile test sources
$(OBJ_DIR)/tests/%.o: $(TEST_UNIT_DIR)/%.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -I$(TEST_DIR) $(INCLUDES) -c $< -o $@

# Compile mock sources
$(OBJ_DIR)/tests/mocks/%.o: $(TEST_MOCK_DIR)/%.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(YELLOW)[MOCK]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -I$(TEST_MOCK_DIR) $(INCLUDES) -c $< -o $@

# Dynamic card instantiation test
TEST_CARDS_BIN := $(BIN_DIR)/test_dynamic_cards
TEST_CARDS_OBJ := $(OBJ_DIR)/test_dynamic_cards.o

test-cards: $(TEST_CARDS_BIN)
	$(ECHO) "$(CYAN)Running dynamic card test...$(RESET)"
	$(Q)$(TEST_CARDS_BIN)

$(TEST_CARDS_BIN): $(TEST_CARDS_OBJ) $(LVGL_OBJS) $(FONT_OBJS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_dynamic_cards"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Test binary ready$(RESET)"

$(TEST_CARDS_OBJ): $(SRC_DIR)/test_dynamic_cards.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Print select panel test with mock data
TEST_PRINT_SELECT_BIN := $(BIN_DIR)/test_print_select_panel
TEST_PRINT_SELECT_OBJ := $(OBJ_DIR)/tests/test_print_select_panel.o
MOCK_FILES_OBJ := $(OBJ_DIR)/tests/mock_print_files.o

test-print-select: $(TEST_PRINT_SELECT_BIN)
	$(ECHO) "$(CYAN)Running print select panel test...$(RESET)"
	$(Q)$(TEST_PRINT_SELECT_BIN)

$(TEST_PRINT_SELECT_BIN): $(TEST_PRINT_SELECT_OBJ) $(MOCK_FILES_OBJ) $(LVGL_OBJS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_print_select_panel"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Test binary ready$(RESET)"

$(TEST_PRINT_SELECT_OBJ): $(TEST_DIR)/test_print_select_panel.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@

$(MOCK_FILES_OBJ): $(TEST_DIR)/mock_print_files.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@

# LV_SIZE_CONTENT behavior test
TEST_SIZE_CONTENT_BIN := $(BIN_DIR)/test_size_content
TEST_SIZE_CONTENT_OBJ := $(OBJ_DIR)/test_size_content.o

test-size-content: $(TEST_SIZE_CONTENT_BIN)
	$(ECHO) "$(CYAN)Running LV_SIZE_CONTENT behavior test...$(RESET)"
	$(Q)$(TEST_SIZE_CONTENT_BIN)

$(TEST_SIZE_CONTENT_BIN): $(TEST_SIZE_CONTENT_OBJ) $(LVGL_OBJS) $(THORVG_OBJS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_size_content"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Test binary ready: $@$(RESET)"

$(TEST_SIZE_CONTENT_OBJ): test_size_content.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Responsive theme and breakpoint test
TEST_RESPONSIVE_THEME_BIN := $(BIN_DIR)/test_responsive_theme
TEST_RESPONSIVE_THEME_OBJ := $(OBJ_DIR)/test_responsive_theme.o

test-responsive-theme: $(TEST_RESPONSIVE_THEME_BIN)
	$(ECHO) "$(CYAN)Running responsive theme and breakpoint tests...$(RESET)"
	$(Q)$(TEST_RESPONSIVE_THEME_BIN)

$(TEST_RESPONSIVE_THEME_BIN): $(TEST_RESPONSIVE_THEME_OBJ) $(LVGL_OBJS) $(THORVG_OBJS) $(OBJ_DIR)/ui_theme.o
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_responsive_theme"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Responsive theme test binary ready$(RESET)"

$(TEST_RESPONSIVE_THEME_OBJ): $(SRC_DIR)/test_responsive_theme.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@
