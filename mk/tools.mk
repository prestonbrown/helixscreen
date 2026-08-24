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
CPP_TERMINAL_VERSION_STUB_OBJ := $(OBJ_DIR)/tools/cpp_terminal_version_stub.o

# Inspector needs JSON-RPC request tracking (no LVGL, no UI, no SDL2)
# The tracker + format_utils have zero app dependencies after refactoring
INSPECTOR_DEPS := \
	$(OBJ_DIR)/api/moonraker_request_tracker.o \
	$(OBJ_DIR)/format_utils.o \
	$(INSPECTOR_INTERACTIVE_OBJ) \
	$(CPP_TERMINAL_VERSION_STUB_OBJ) \
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

# Compile cpp-terminal version stub (cmake normally generates version.cpp)
$(CPP_TERMINAL_VERSION_STUB_OBJ): $(TOOLS_DIR)/cpp_terminal_version_stub.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CXX]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(CPP_TERMINAL_INC) -c $< -o $@ || { \
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

# Full dependencies (app objects + LVGL + fonts + lv_markdown + quirc + translations)
# Note: lv_markdown, quirc, and translations are needed because app objects may reference them
VALIDATE_XML_DEPS := \
	$(VALIDATE_XML_APP_OBJS) \
	$(LVGL_OBJS) \
	$(HELIX_XML_OBJS) \
	$(THORVG_OBJS) \
	$(LV_MARKDOWN_OBJS) \
	$(QUIRC_OBJS) \
	$(FONT_OBJS) \
	$(TRANS_OBJS)

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
# Note: the former standalone `helixctl` tool is folded into helix-screen — build
# the app and use `helix-screen ctl <cmd>` / `helix-screen repl` instead.
tools: moonraker-inspector validate-xml-constants validate-xml-attrs

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

# ==============================================================================
# helix-xml-linter (Python)
# ==============================================================================
# Schema-driven Python linter for ui_xml/. Catches unknown attributes, invalid
# enum values, bad style props, undefined cross-refs. Pure stdlib — no install.
#
# Targets:
#   make lint-xml            — run linter against ui_xml/, errors-only (CI gate)
#   make lint-xml-all        — run linter, include warnings (local dev)
#   make regen-xml-schema    — regenerate tools/xml-linter/schema/schema.json
#                              from lib/helix-xml/src/xml/ + src/ui/ + ui_xml/

XML_LINTER_DIR   := $(TOOLS_DIR)/xml-linter
XML_LINTER_PY    := PYTHONPATH=$(XML_LINTER_DIR)/src python3 -m helix_xml_linter.cli
XML_LINTER_EXTRACT := PYTHONPATH=$(XML_LINTER_DIR)/src python3 $(XML_LINTER_DIR)/schema/extract_schema.py
XML_LINTER_SCHEMA := $(XML_LINTER_DIR)/schema/schema.json

.PHONY: lint-xml lint-xml-all regen-xml-schema

lint-xml:
	$(ECHO) "$(BLUE)[LINT]$(RESET) helix-xml-linter (errors only) on ui_xml/"
	$(Q)$(XML_LINTER_PY) --schema $(XML_LINTER_SCHEMA) --severity error ui_xml/

lint-xml-all:
	$(ECHO) "$(BLUE)[LINT]$(RESET) helix-xml-linter (errors + warnings) on ui_xml/"
	$(Q)$(XML_LINTER_PY) --schema $(XML_LINTER_SCHEMA) --severity info ui_xml/

regen-xml-schema:
	$(ECHO) "$(BLUE)[GEN]$(RESET) regenerating $(XML_LINTER_SCHEMA)"
	$(Q)$(XML_LINTER_EXTRACT) \
		lib/helix-xml/src/xml \
		--cpp-src src/ui \
		--cpp-const-dirs src \
		--xml-roots ui_xml \
		--theme-dirs assets/config/themes/defaults \
		-o $(XML_LINTER_SCHEMA)
	$(ECHO) "$(GREEN)✓ schema regenerated — commit $(XML_LINTER_SCHEMA) if it changed$(RESET)"

# ==============================================================================
# Theme token table (Python)
# ==============================================================================
# Build-time snapshot of every <color>/<px>/<string> design token in ui_xml/,
# mirroring theme_manager.cpp's runtime scan (top-level files, sorted,
# last-wins). Committed artifact consumed by src/generated/theme_token_table.cpp
# callers; staleness is caught by the Task 4 parity test, not this target.
#
# Targets:
#   make regen-tokens        — regenerate src/generated/theme_token_table.cpp
#                              from ui_xml/*.xml

.PHONY: regen-tokens

regen-tokens:
	$(ECHO) "$(BLUE)[GEN]$(RESET) regenerating src/generated/theme_token_table.cpp"
	$(Q)python3 scripts/gen_theme_tokens.py
	$(ECHO) "$(GREEN)✓ token table regenerated — commit src/generated/theme_token_table.cpp if it changed$(RESET)"

# ==============================================================================
# Architecture-guide file links (Python)
# ==============================================================================
# The guide's backticked citations (`src/printer/printer_state.cpp:622`) are the
# source of truth; the markdown link around each one is derived from that text on
# every run. Write plain backticks, run this, commit. A renamed file is fixed in
# one place — the citation — and every link follows.
#
# Targets:
#   make regen-doc-links     — rewrite docs/devel/ARCHITECTURE.md and
#                              docs/devel/architecture/*.md in place
#   make check-doc-links     — report-only; what quality-checks.sh runs

.PHONY: regen-doc-links check-doc-links

regen-doc-links:
	$(ECHO) "$(BLUE)[GEN]$(RESET) linking architecture-guide citations"
	$(Q)python3 scripts/gen_doc_links.py
	$(ECHO) "$(GREEN)✓ doc links regenerated — commit the guide if it changed$(RESET)"

check-doc-links:
	$(Q)python3 scripts/gen_doc_links.py --check
