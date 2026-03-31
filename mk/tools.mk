# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Tools Module
# Standalone diagnostic and utility programs

# ==============================================================================
# Moonraker Inspector Tool
# ==============================================================================
# Standalone diagnostic tool for querying Moonraker printer metadata
# Usage: moonraker-inspector <ip_address> [port]

# Tool sources
TOOLS_DIR := tools
INSPECTOR_SRC := $(TOOLS_DIR)/moonraker_inspector.cpp
INSPECTOR_INTERACTIVE_SRC := $(TOOLS_DIR)/moonraker_inspector_interactive.cpp
INSPECTOR_OBJ := $(OBJ_DIR)/tools/moonraker_inspector.o
INSPECTOR_INTERACTIVE_OBJ := $(OBJ_DIR)/tools/moonraker_inspector_interactive.o
INSPECTOR_STUB_OBJ := $(OBJ_DIR)/tools/ui_notification_stub.o

# CLI Moonraker client (minimal dependencies for tools)
CLI_MOONRAKER_SRC := $(TOOLS_DIR)/moonraker_client_cli.cpp
CLI_MOONRAKER_OBJ := $(OBJ_DIR)/tools/moonraker_client_cli.o

# Inspector needs moonraker_client and its dependencies, but NOT UI code
# Extract just the Moonraker-related objects (no LVGL, no UI, no SDL2)
INSPECTOR_DEPS := \
	$(CLI_MOONRAKER_OBJ) \
	$(INSPECTOR_INTERACTIVE_OBJ) \
	$(INSPECTOR_STUB_OBJ) \
	$(CPP_TERMINAL_OBJS)

# Simplified linker flags (no SDL2, no UI frameworks)
INSPECTOR_LDFLAGS := $(LIBHV_LIBS) -lm -lpthread

ifeq ($(UNAME_S),Darwin)
	INSPECTOR_LDFLAGS += -framework Foundation -framework CoreFoundation -framework Security
else
	INSPECTOR_LDFLAGS += -lssl -lcrypto -ldl
endif

# Build rule for inspector
$(MOONRAKER_INSPECTOR): $(LIBHV_LIB) $(INSPECTOR_OBJ) $(INSPECTOR_DEPS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) $@"
	$(Q)$(CXX) $(CXXFLAGS) $(INSPECTOR_OBJ) $(INSPECTOR_DEPS) -o $@ $(INSPECTOR_LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Linking failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)✓ Moonraker Inspector built: $@$(RESET)"

# Compile inspector source
$(INSPECTOR_OBJ): $(INSPECTOR_SRC) $(HEADERS) $(LIBHV_LIB)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(CPP_TERMINAL_INC) -I$(TOOLS_DIR) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(CPP_TERMINAL_INC) -I$(TOOLS_DIR) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile CLI Moonraker client
$(CLI_MOONRAKER_OBJ): $(CLI_MOONRAKER_SRC) $(HEADERS) $(LIBHV_LIB)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) -I$(TOOLS_DIR) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(TOOLS_DIR) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile interactive module
$(INSPECTOR_INTERACTIVE_OBJ): $(INSPECTOR_INTERACTIVE_SRC) $(HEADERS) $(LIBHV_LIB)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) $(CPP_TERMINAL_INC) -I$(TOOLS_DIR) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(CPP_TERMINAL_INC) -I$(TOOLS_DIR) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile cpp-terminal library (use SUBMODULE_CXXFLAGS to suppress third-party warnings)
$(OBJ_DIR)/cpp-terminal/%.o: $(CPP_TERMINAL_DIR)/%.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(SUBMODULE_CXXFLAGS) $(CPP_TERMINAL_INC) -c $< -o $@"
endif
	$(Q)$(CXX) $(SUBMODULE_CXXFLAGS) $(CPP_TERMINAL_INC) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile UI notification stubs (needed for moonraker_client linkage)
$(INSPECTOR_STUB_OBJ): $(TOOLS_DIR)/ui_notification_stub.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# ==============================================================================
# XML Constants Validator Tool
# ==============================================================================
# Pre-commit validation tool for checking responsive px and theme color pairs
# Usage: validate-xml-constants (run from repo root)
#
# This tool reuses the main application's object files, so it's built after
# the main app build. This avoids duplicating LVGL and other large dependencies.

VALIDATE_XML_SRC := $(TOOLS_DIR)/validate_xml_constants.cpp
VALIDATE_XML_BIN := $(BIN_DIR)/validate-xml-constants
VALIDATE_XML_OBJ := $(OBJ_DIR)/tools/validate_xml_constants.o

# Validator reuses main app objects - filter out main.o to avoid duplicate main symbol
VALIDATE_XML_APP_OBJS := $(filter-out $(OBJ_DIR)/main.o,$(APP_OBJS) $(APP_C_OBJS) $(OBJCPP_OBJS))

# Full dependencies (app objects + LVGL + fonts)
VALIDATE_XML_DEPS := \
	$(VALIDATE_XML_APP_OBJS) \
	$(LVGL_OBJS) \
	$(HELIX_XML_OBJS) \
	$(THORVG_OBJS) \
	$(FONT_OBJS)

# Linker flags (same as main app for full compatibility)
VALIDATE_XML_LDFLAGS := $(LDFLAGS)

# Build rule for validator - depends on TARGET being built first for objects
$(VALIDATE_XML_BIN): $(TARGET) $(VALIDATE_XML_OBJ)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) $@"
	$(Q)$(CXX) $(CXXFLAGS) $(VALIDATE_XML_OBJ) $(VALIDATE_XML_DEPS) -o $@ $(VALIDATE_XML_LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Linking failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)✓ XML Constants Validator built: $@$(RESET)"

# Compile validator source
$(VALIDATE_XML_OBJ): $(VALIDATE_XML_SRC)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
ifeq ($(V),1)
	$(Q)echo "$(YELLOW)Command:$(RESET) $(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@"
endif
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Phony targets
.PHONY: tools moonraker-inspector validate-xml-constants validate-xml-attrs

# Build all tools
tools: moonraker-inspector validate-xml-attrs

# Individual tool targets
moonraker-inspector: $(MOONRAKER_INSPECTOR)
	$(ECHO) "$(CYAN)Usage: $(YELLOW)./$(MOONRAKER_INSPECTOR) <ip_address> [port] [options]$(RESET)"
	$(ECHO) "$(CYAN)Example: $(YELLOW)./$(MOONRAKER_INSPECTOR) 192.168.1.100 7125 -i$(RESET)"
	$(ECHO) "$(CYAN)Options: $(YELLOW)-i/--interactive$(RESET) for TUI mode with collapsible sections"

validate-xml-constants: $(VALIDATE_XML_BIN)
	$(ECHO) "$(CYAN)Usage: $(YELLOW)./$(VALIDATE_XML_BIN)$(RESET)"
	$(ECHO) "$(CYAN)Run from repo root to validate ui_xml/ constant sets$(RESET)"

# ==============================================================================
# XML Attribute Validator Tool
# ==============================================================================
# Pre-commit validation tool for catching unknown attributes in XML files
# Usage: validate-xml-attributes [options] [files...]
#
# This is a standalone tool that only needs expat for XML parsing.

VALIDATE_ATTRS_SRC := $(TOOLS_DIR)/validate_xml_attributes.cpp
VALIDATE_ATTRS_LIB_SRC := src/tools/xml_attribute_validator.cpp
VALIDATE_ATTRS_BIN := $(BIN_DIR)/validate-xml-attributes
VALIDATE_ATTRS_OBJ := $(OBJ_DIR)/tools/validate_xml_attributes.o
VALIDATE_ATTRS_LIB_OBJ := $(OBJ_DIR)/tools/xml_attribute_validator.o

# Standalone linker flags - just needs expat
VALIDATE_ATTRS_LDFLAGS := -lexpat

# Build rule for attribute validator
$(VALIDATE_ATTRS_BIN): $(VALIDATE_ATTRS_OBJ) $(VALIDATE_ATTRS_LIB_OBJ)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) $@"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(VALIDATE_ATTRS_LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Linking failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)✓ XML Attribute Validator built: $@$(RESET)"

# Compile validator main source
$(VALIDATE_ATTRS_OBJ): $(VALIDATE_ATTRS_SRC)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Compile validator library source
$(VALIDATE_ATTRS_LIB_OBJ): $(VALIDATE_ATTRS_LIB_SRC)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@ || { \
		echo "$(RED)$(BOLD)✗ Compilation failed:$(RESET) $<"; \
		exit 1; \
	}

# Phony target
.PHONY: validate-xml-attrs

validate-xml-attrs: $(VALIDATE_ATTRS_BIN)
	$(ECHO) "$(CYAN)Usage: $(YELLOW)./$(VALIDATE_ATTRS_BIN) [--warn-only] [--verbose] [files...]$(RESET)"
	$(ECHO) "$(CYAN)Run from repo root to validate XML attributes$(RESET)"
