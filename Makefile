# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Main Makefile
# LVGL 9 + SDL2 simulator with modular build system
#
# ⚠️ CRITICAL: Always use 'make' or 'make -j' - NEVER invoke gcc/g++ directly!
# The build system handles:
#   - Dependency management (libhv, lvgl, SDL2)
#   - Platform detection (macOS vs Linux)
#   - Parallel builds (-j auto-detects cores)
#   - Patch application for multi-display support
#
# Common commands:
#   make -j       # Parallel incremental build (daily development)
#   make build    # Clean build from scratch
#   make help     # Show all available targets
#
# See: DEVELOPMENT.md for complete build instructions

# Use bash for all shell commands (needed for [[ ]] and read -n)
SHELL := /bin/bash

# Build verbosity control
# Use 'make V=1' to see full compiler commands
V ?= 0
ifeq ($(V),0)
    Q := @
    ECHO := @echo
else
    Q :=
    ECHO := @true
endif

# Color output - auto-detect terminal capabilities
# Disable with NO_COLOR=1 or when running in non-terminal environments
ifndef NO_COLOR
    # Auto-detect if terminal supports colors
    # Check if Make is running interactively (stdin is a TTY) AND stderr is a TTY
    # This disables colors when output is piped (stdout redirect) or in non-interactive contexts
    # Note: We can't check stdout directly from $(shell) because it's redirected to capture output
    TERM_SUPPORTS_COLOR := $(shell \
        if [ -t 0 ] && [ -t 2 ] && [ -n "$$TERM" ] && [ "$$TERM" != "dumb" ]; then \
            echo 1; \
        else \
            echo 0; \
        fi)

    ifeq ($(TERM_SUPPORTS_COLOR),1)
        BOLD := $(shell printf '\033[1m')
        RED := $(shell printf '\033[31m')
        GREEN := $(shell printf '\033[32m')
        YELLOW := $(shell printf '\033[33m')
        BLUE := $(shell printf '\033[34m')
        MAGENTA := $(shell printf '\033[35m')
        CYAN := $(shell printf '\033[36m')
        RESET := $(shell printf '\033[0m')
    else
        # Terminal doesn't support colors - disable
        BOLD :=
        RED :=
        GREEN :=
        YELLOW :=
        BLUE :=
        MAGENTA :=
        CYAN :=
        RESET :=
    endif
else
    # NO_COLOR=1 explicitly set
    BOLD :=
    RED :=
    GREEN :=
    YELLOW :=
    BLUE :=
    MAGENTA :=
    CYAN :=
    RESET :=
endif

# Compilers - auto-detect or use environment variables
# Priority: environment variables > clang > gcc
ifeq ($(origin CC),default)
    ifneq ($(shell command -v clang 2>/dev/null),)
        CC := clang
    else ifneq ($(shell command -v gcc 2>/dev/null),)
        CC := gcc
    else
        $(error No C compiler found. Install clang or gcc)
    endif
endif

ifeq ($(origin CXX),default)
    ifneq ($(shell command -v clang++ 2>/dev/null),)
        CXX := clang++
    else ifneq ($(shell command -v g++ 2>/dev/null),)
        CXX := g++
    else
        $(error No C++ compiler found. Install clang++ or g++)
    endif
endif

CFLAGS := -std=c11 -Wall -Wextra -O2 -g -D_GNU_SOURCE
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g

# Platform detection (needed early for conditional compilation)
UNAME_S := $(shell uname -s)

# Directories
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

# LVGL
LVGL_DIR := lvgl
LVGL_INC := -I$(LVGL_DIR) -I$(LVGL_DIR)/src
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name "*.c" 2>/dev/null)
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_SRCS))

# ThorVG sources (.cpp files for SVG support)
THORVG_SRCS := $(shell find $(LVGL_DIR)/src/libs/thorvg -name "*.cpp" 2>/dev/null)
THORVG_OBJS := $(patsubst $(LVGL_DIR)/%.cpp,$(OBJ_DIR)/lvgl/%.o,$(THORVG_SRCS))

# LVGL Demos (separate target)
LVGL_DEMO_SRCS := $(shell find $(LVGL_DIR)/demos -name "*.c" 2>/dev/null)
LVGL_DEMO_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_DEMO_SRCS))

# Application C sources
APP_C_SRCS := $(wildcard $(SRC_DIR)/*.c)
APP_C_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(APP_C_SRCS))

# Application C++ sources (exclude test binaries)
APP_SRCS := $(filter-out $(SRC_DIR)/test_dynamic_cards.cpp $(SRC_DIR)/test_responsive_theme.cpp $(SRC_DIR)/test_tinygl_triangle.cpp $(SRC_DIR)/test_gcode_geometry.cpp $(SRC_DIR)/test_gcode_analysis.cpp $(SRC_DIR)/test_sdf_reconstruction.cpp $(SRC_DIR)/test_sparse_grid.cpp $(SRC_DIR)/test_partial_extraction.cpp $(SRC_DIR)/test_render_comparison.cpp,$(wildcard $(SRC_DIR)/*.cpp))
APP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(APP_SRCS))

# Objective-C++ sources (macOS only - .mm files)
# Only include on macOS, exclude on Linux to avoid linking errors
ifeq ($(UNAME_S),Darwin)
    OBJCPP_SRCS := $(wildcard $(SRC_DIR)/*.mm)
    OBJCPP_OBJS := $(patsubst $(SRC_DIR)/%.mm,$(OBJ_DIR)/%.o,$(OBJCPP_SRCS))
else
    OBJCPP_SRCS :=
    OBJCPP_OBJS :=
endif

# Fonts
FONT_SRCS := assets/fonts/fa_icons_64.c assets/fonts/fa_icons_48.c assets/fonts/fa_icons_32.c assets/fonts/fa_icons_24.c assets/fonts/fa_icons_16.c assets/fonts/arrows_64.c assets/fonts/arrows_48.c assets/fonts/arrows_32.c
FONT_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(FONT_SRCS))

# Material Design Icons
MATERIAL_ICON_SRCS := $(wildcard assets/images/material/*.c)
MATERIAL_ICON_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(MATERIAL_ICON_SRCS))

# SDL2 - Use system version if available, otherwise build from submodule
SDL2_SYSTEM_AVAILABLE := $(shell command -v sdl2-config 2>/dev/null)
ifneq ($(SDL2_SYSTEM_AVAILABLE),)
    # System SDL2 found - use it
    SDL2_INC := $(shell sdl2-config --cflags)
    SDL2_LIBS := $(shell sdl2-config --libs)
    SDL2_LIB :=
else
    # No system SDL2 - build from submodule
    SDL2_DIR := sdl2
    SDL2_BUILD_DIR := $(SDL2_DIR)/build
    SDL2_LIB := $(SDL2_BUILD_DIR)/libSDL2.a
    SDL2_INC := -I$(SDL2_DIR)/include -I$(SDL2_BUILD_DIR)/include -I$(SDL2_BUILD_DIR)/include-config-release
    SDL2_LIBS := $(SDL2_LIB)
endif

# libhv (WebSocket client for Moonraker) - Use system version if available, otherwise build from submodule
LIBHV_PKG_CONFIG := $(shell pkg-config --exists libhv 2>/dev/null && echo "yes")
ifeq ($(LIBHV_PKG_CONFIG),yes)
    # System libhv found via pkg-config
    LIBHV_INC := $(shell pkg-config --cflags libhv)
    LIBHV_LIBS := $(shell pkg-config --libs libhv)
    LIBHV_LIB :=
else
    # No system libhv - build from submodule
    LIBHV_DIR := libhv
    LIBHV_INC := -I$(LIBHV_DIR)/include -I$(LIBHV_DIR)/cpputil -I$(LIBHV_DIR)
    # Check both possible locations for libhv.a (lib/ and root build directory)
    LIBHV_LIB_PATHS := $(LIBHV_DIR)/lib/libhv.a $(LIBHV_DIR)/libhv.a
    LIBHV_LIB := $(firstword $(wildcard $(LIBHV_LIB_PATHS)))
    ifeq ($(LIBHV_LIB),)
        # Neither exists yet - default to lib/ path for dependency rules
        LIBHV_LIB := $(LIBHV_DIR)/lib/libhv.a
    endif
    LIBHV_LIBS := $(LIBHV_LIB)
endif

# spdlog (logging library) - Use system version if available, otherwise use submodule
SPDLOG_SYSTEM_PATHS := /usr/include/spdlog /usr/local/include/spdlog /opt/homebrew/include/spdlog
SPDLOG_SYSTEM_AVAILABLE := $(firstword $(wildcard $(SPDLOG_SYSTEM_PATHS)))
ifneq ($(SPDLOG_SYSTEM_AVAILABLE),)
    # System spdlog found - use it (header-only library)
    SPDLOG_INC := -I$(dir $(SPDLOG_SYSTEM_AVAILABLE))
else
    # No system spdlog - use submodule
    SPDLOG_DIR := spdlog
    SPDLOG_INC := -I$(SPDLOG_DIR)/include
endif

# fmt (formatting library required by header-only spdlog)
FMT_PKG_CONFIG := $(shell pkg-config --exists fmt 2>/dev/null && echo "yes")
ifeq ($(FMT_PKG_CONFIG),yes)
    # System fmt found via pkg-config
    FMT_LIBS := $(shell pkg-config --libs fmt)
else
    # No system fmt - will need to be installed or use bundled version
    FMT_LIBS :=
endif

# TinyGL (software 3D rasterizer for G-code visualization)
# Set ENABLE_TINYGL_3D=no to build without 3D rendering support
ENABLE_TINYGL_3D ?= yes

ifeq ($(ENABLE_TINYGL_3D),yes)
    TINYGL_DIR := tinygl
    TINYGL_LIB := $(TINYGL_DIR)/lib/libTinyGL.a
    TINYGL_INC := -I$(TINYGL_DIR)/include
    TINYGL_DEFINES := -DENABLE_TINYGL_3D
else
    TINYGL_DIR :=
    TINYGL_LIB :=
    TINYGL_INC :=
    TINYGL_DEFINES :=
endif

# wpa_supplicant (WiFi control via wpa_ctrl interface)
WPA_DIR := wpa_supplicant
WPA_CLIENT_LIB := $(WPA_DIR)/wpa_supplicant/libwpa_client.a
WPA_INC := -I$(WPA_DIR)/src/common -I$(WPA_DIR)/src/utils

# Include paths
INCLUDES := -I. -I$(INC_DIR) $(LVGL_INC) $(LIBHV_INC) $(SPDLOG_INC) $(TINYGL_INC) $(WPA_INC) $(SDL2_INC) -Iglm -Iopenvdb/nanovdb

# Common linker flags (used by both macOS and Linux)
LDFLAGS_COMMON := $(SDL2_LIBS) $(LIBHV_LIBS) $(TINYGL_LIB) $(FMT_LIBS) -lm -lpthread

# Platform-specific configuration
ifeq ($(UNAME_S),Darwin)
    # macOS - Uses CoreWLAN framework for WiFi (with fallback to mock)
    NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

    # Set minimum macOS version (14.0 Sonoma - matches SDL2 library requirement)
    MACOS_MIN_VERSION := 14.0
    MACOS_DEPLOYMENT_TARGET := -mmacosx-version-min=$(MACOS_MIN_VERSION)

    CFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    CXXFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    LDFLAGS := $(LDFLAGS_COMMON) -framework Foundation -framework CoreFoundation -framework Security -framework CoreWLAN -framework CoreLocation -framework Cocoa -framework IOKit -framework CoreVideo -framework AudioToolbox -framework ForceFeedback -framework Carbon -framework CoreAudio -framework Metal -liconv
    PLATFORM := macOS
    WPA_DEPS :=
else
    # Linux - Include libwpa_client.a for WiFi control
    NPROC := $(shell nproc 2>/dev/null || echo 4)
    LDFLAGS := $(LDFLAGS_COMMON) $(WPA_CLIENT_LIB) -lssl -lcrypto -ldl
    PLATFORM := Linux
    WPA_DEPS := $(WPA_CLIENT_LIB)
endif

# Add TinyGL defines to compiler flags
CFLAGS += $(TINYGL_DEFINES)
CXXFLAGS += $(TINYGL_DEFINES)

# Parallel build control
# Set JOBS=N to override, or use -j directly
# Default: use all cores
JOBS ?= $(NPROC)

# Don't auto-enable parallel - let user control it
# Use 'make -j' or 'make JOBS=N' for parallel builds
ifneq ($(JOBS),1)
    MAKEFLAGS += --output-sync=target
endif

# Binary
TARGET := $(BIN_DIR)/helix-ui-proto

# LVGL configuration
LV_CONF := -DLV_CONF_INCLUDE_SIMPLE

# Test configuration
TEST_DIR := tests
TEST_UNIT_DIR := $(TEST_DIR)/unit
TEST_MOCK_DIR := $(TEST_DIR)/mocks
TEST_BIN := $(BIN_DIR)/run_tests
TEST_INTEGRATION_BIN := $(BIN_DIR)/run_integration_tests

# Unit tests (use real LVGL) - exclude mock example
TEST_SRCS := $(filter-out $(TEST_UNIT_DIR)/test_mock_example.cpp,$(wildcard $(TEST_UNIT_DIR)/*.cpp))
TEST_OBJS := $(patsubst $(TEST_UNIT_DIR)/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_SRCS))

# Integration tests (use mocks instead of real LVGL)
TEST_INTEGRATION_SRCS := $(TEST_UNIT_DIR)/test_mock_example.cpp
TEST_INTEGRATION_OBJS := $(patsubst $(TEST_UNIT_DIR)/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_INTEGRATION_SRCS))

TEST_MAIN_OBJ := $(OBJ_DIR)/tests/test_main.o
CATCH2_OBJ := $(OBJ_DIR)/tests/catch_amalgamated.o
UI_TEST_UTILS_OBJ := $(OBJ_DIR)/tests/ui_test_utils.o

# Mock objects for integration testing
MOCK_SRCS := $(wildcard $(TEST_MOCK_DIR)/*.cpp)
MOCK_OBJS := $(patsubst $(TEST_MOCK_DIR)/%.cpp,$(OBJ_DIR)/tests/mocks/%.o,$(MOCK_SRCS))

# Default target
.DEFAULT_GOAL := all

.PHONY: all build clean run test test-integration test-cards test-print-select test-size-content demo compile_commands libhv-build apply-patches generate-fonts help check-deps install-deps icon format format-staged

# Help target - checks stdout dynamically to avoid colors when piped
help:
	@if [ -t 1 ] && [ -n "$(TERM)" ] && [ "$(TERM)" != "dumb" ]; then \
		B='$(BOLD)'; R='$(RED)'; G='$(GREEN)'; Y='$(YELLOW)'; BL='$(BLUE)'; M='$(MAGENTA)'; C='$(CYAN)'; X='$(RESET)'; \
	else \
		B=''; R=''; G=''; Y=''; BL=''; M=''; C=''; X=''; \
	fi; \
	echo "$${B}HelixScreen UI Prototype Build System$${X}"; \
	echo ""; \
	echo "$${C}Build Targets:$${X}"; \
	echo "  $${G}all$${X}              - Build the main binary (default)"; \
	echo "  $${G}build$${X}            - Clean build with progress (parallel: -j$(NPROC))"; \
	echo "  $${G}clean$${X}            - Remove all build artifacts"; \
	echo "  $${G}demo$${X}             - Build LVGL demo widgets"; \
	echo ""; \
	echo "$${C}Test Targets:$${X}"; \
	echo "  $${G}test$${X}             - Run unit tests"; \
	echo "  $${G}test-integration$${X} - Run integration tests (with mocks)"; \
	echo "  $${G}test-cards$${X}       - Test dynamic card instantiation"; \
	echo "  $${G}test-print-select$${X} - Test print select panel"; \
	echo ""; \
	echo "$${C}Run Targets:$${X}"; \
	echo "  $${G}run$${X}              - Build and run the UI prototype"; \
	echo ""; \
	echo "$${C}Development Targets:$${X}"; \
	echo "  $${G}compile_commands$${X} - Generate compile_commands.json for IDE/LSP"; \
	echo "  $${G}format$${X}           - Auto-format all C/C++ and XML files"; \
	echo "  $${G}format-staged$${X}    - Auto-format only staged files (pre-commit)"; \
	echo "  $${G}check-deps$${X}       - Verify all dependencies are installed"; \
	echo "  $${G}install-deps$${X}     - Auto-install missing dependencies (interactive)"; \
	echo "  $${G}apply-patches$${X}    - Apply LVGL patches"; \
	echo "  $${G}generate-fonts$${X}   - Regenerate FontAwesome fonts from package.json"; \
	echo "  $${G}icon$${X}             - Generate macOS .icns icon from logo"; \
	echo "  $${G}material-icons-list$${X} - List all registered Material Design icons"; \
	echo "  $${G}material-icons-convert$${X} - Convert SVGs to LVGL C arrays (SVGS=...)"; \
	echo "  $${G}material-icons-add$${X} - Download and add Material icons (ICONS=...)"; \
	echo ""; \
	echo "$${C}Build Options:$${X}"; \
	echo "  $${Y}V=1$${X}              - Verbose mode (show full compiler commands)"; \
	echo "  $${Y}JOBS=N$${X}           - Set parallel job count (default: $(NPROC))"; \
	echo "  $${Y}NO_COLOR=1$${X}       - Disable colored output (auto-detected for non-TTY)"; \
	echo ""; \
	echo "$${C}Examples:$${X}"; \
	echo "  make -j$(NPROC)          # Parallel build with all cores"; \
	echo "  make V=1           # Verbose build"; \
	echo "  make clean all     # Clean rebuild"; \
	echo "  make run           # Build and run"; \
	echo ""; \
	echo "$${C}Platform:$${X} $(PLATFORM) ($(NPROC) cores available)"

# Include modular makefiles
include mk/deps.mk
include mk/patches.mk
include mk/tests.mk
include mk/fonts.mk
include mk/format.mk
include mk/rules.mk
