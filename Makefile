# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Main Makefile
# LVGL 9 + SDL2 simulator with modular build system

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

# Color output (disable with NO_COLOR=1)
ifndef NO_COLOR
    BOLD := \033[1m
    RED := \033[31m
    GREEN := \033[32m
    YELLOW := \033[33m
    BLUE := \033[34m
    MAGENTA := \033[35m
    CYAN := \033[36m
    RESET := \033[0m
else
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

# Directories
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

# LVGL
LVGL_DIR := lvgl
LVGL_INC := -I$(LVGL_DIR) -I$(LVGL_DIR)/src
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name "*.c")
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_SRCS))

# ThorVG sources (.cpp files for SVG support)
THORVG_SRCS := $(shell find $(LVGL_DIR)/src/libs/thorvg -name "*.cpp" 2>/dev/null)
THORVG_OBJS := $(patsubst $(LVGL_DIR)/%.cpp,$(OBJ_DIR)/lvgl/%.o,$(THORVG_SRCS))

# LVGL Demos (separate target)
LVGL_DEMO_SRCS := $(shell find $(LVGL_DIR)/demos -name "*.c")
LVGL_DEMO_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_DEMO_SRCS))

# Application C++ sources (exclude test binaries)
APP_SRCS := $(filter-out $(SRC_DIR)/test_dynamic_cards.cpp,$(wildcard $(SRC_DIR)/*.cpp))
APP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(APP_SRCS))

# Objective-C++ sources (macOS only - .mm files)
OBJCPP_SRCS := $(wildcard $(SRC_DIR)/*.mm)
OBJCPP_OBJS := $(patsubst $(SRC_DIR)/%.mm,$(OBJ_DIR)/%.o,$(OBJCPP_SRCS))

# Fonts
FONT_SRCS := assets/fonts/fa_icons_64.c assets/fonts/fa_icons_48.c assets/fonts/fa_icons_32.c assets/fonts/fa_icons_24.c assets/fonts/fa_icons_16.c assets/fonts/arrows_64.c assets/fonts/arrows_48.c assets/fonts/arrows_32.c
FONT_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(FONT_SRCS))

# Material Design Icons
MATERIAL_ICON_SRCS := $(wildcard assets/images/material/*.c)
MATERIAL_ICON_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(MATERIAL_ICON_SRCS))

# SDL2
SDL2_CFLAGS := $(shell sdl2-config --cflags)
SDL2_LIBS := $(shell sdl2-config --libs)

# libhv (WebSocket client for Moonraker) - symlinked from parent repo submodule
LIBHV_DIR := libhv
LIBHV_INC := -I$(LIBHV_DIR)/include -I$(LIBHV_DIR)/cpputil
LIBHV_LIB := $(LIBHV_DIR)/lib/libhv.a

# spdlog (logging library) - symlinked from parent repo submodule
SPDLOG_DIR := spdlog
SPDLOG_INC := -I$(SPDLOG_DIR)/include

# wpa_supplicant (WiFi control via wpa_ctrl interface)
WPA_DIR := wpa_supplicant
WPA_CLIENT_LIB := $(WPA_DIR)/wpa_supplicant/libwpa_client.a
WPA_INC := -I$(WPA_DIR)/src/common -I$(WPA_DIR)/src/utils

# Include paths
INCLUDES := -I. -I$(INC_DIR) $(LVGL_INC) $(LIBHV_INC) $(SPDLOG_INC) $(WPA_INC) $(SDL2_CFLAGS)

# Platform detection and configuration
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS - Uses CoreWLAN framework for WiFi (with fallback to mock)
    NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

    # Set minimum macOS version (10.15 Catalina for CoreWLAN/CoreLocation modern APIs)
    MACOS_MIN_VERSION := 10.15
    MACOS_DEPLOYMENT_TARGET := -mmacosx-version-min=$(MACOS_MIN_VERSION)

    CFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    CXXFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    LDFLAGS := $(SDL2_LIBS) $(LIBHV_LIB) -lm -lpthread -framework Foundation -framework CoreFoundation -framework Security -framework CoreWLAN -framework CoreLocation
    PLATFORM := macOS
    WPA_DEPS :=
else
    # Linux - Include libwpa_client.a for WiFi control
    NPROC := $(shell nproc 2>/dev/null || echo 4)
    LDFLAGS := $(SDL2_LIBS) $(LIBHV_LIB) $(WPA_CLIENT_LIB) -lm -lpthread
    PLATFORM := Linux
    WPA_DEPS := $(WPA_CLIENT_LIB)
endif

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

.PHONY: all build clean run test test-integration test-cards test-print-select test-size-content demo compile_commands libhv-build apply-patches generate-fonts help check-deps install-deps icon

# Help target
help:
	@echo "$(BOLD)HelixScreen UI Prototype Build System$(RESET)"
	@echo ""
	@echo "$(CYAN)Build Targets:$(RESET)"
	@echo "  $(GREEN)all$(RESET)              - Build the main binary (default)"
	@echo "  $(GREEN)build$(RESET)            - Clean build with progress (parallel: -j$(NPROC))"
	@echo "  $(GREEN)clean$(RESET)            - Remove all build artifacts"
	@echo "  $(GREEN)demo$(RESET)             - Build LVGL demo widgets"
	@echo ""
	@echo "$(CYAN)Test Targets:$(RESET)"
	@echo "  $(GREEN)test$(RESET)             - Run unit tests"
	@echo "  $(GREEN)test-integration$(RESET) - Run integration tests (with mocks)"
	@echo "  $(GREEN)test-cards$(RESET)       - Test dynamic card instantiation"
	@echo "  $(GREEN)test-print-select$(RESET) - Test print select panel"
	@echo ""
	@echo "$(CYAN)Run Targets:$(RESET)"
	@echo "  $(GREEN)run$(RESET)              - Build and run the UI prototype"
	@echo ""
	@echo "$(CYAN)Development Targets:$(RESET)"
	@echo "  $(GREEN)compile_commands$(RESET) - Generate compile_commands.json for IDE/LSP"
	@echo "  $(GREEN)check-deps$(RESET)       - Verify all dependencies are installed"
	@echo "  $(GREEN)install-deps$(RESET)     - Auto-install missing dependencies (interactive)"
	@echo "  $(GREEN)apply-patches$(RESET)    - Apply LVGL patches"
	@echo "  $(GREEN)generate-fonts$(RESET)   - Regenerate FontAwesome fonts from package.json"
	@echo "  $(GREEN)icon$(RESET)             - Generate macOS .icns icon from logo"
	@echo "  $(GREEN)material-icons-list$(RESET) - List all registered Material Design icons"
	@echo "  $(GREEN)material-icons-convert$(RESET) - Convert SVGs to LVGL C arrays (SVGS=...)"
	@echo "  $(GREEN)material-icons-add$(RESET) - Download and add Material icons (ICONS=...)"
	@echo ""
	@echo "$(CYAN)Build Options:$(RESET)"
	@echo "  $(YELLOW)V=1$(RESET)              - Verbose mode (show full compiler commands)"
	@echo "  $(YELLOW)JOBS=N$(RESET)           - Set parallel job count (default: $(NPROC))"
	@echo "  $(YELLOW)NO_COLOR=1$(RESET)       - Disable colored output"
	@echo ""
	@echo "$(CYAN)Examples:$(RESET)"
	@echo "  make -j$(NPROC)          # Parallel build with all cores"
	@echo "  make V=1           # Verbose build"
	@echo "  make clean all     # Clean rebuild"
	@echo "  make run           # Build and run"
	@echo ""
	@echo "$(CYAN)Platform:$(RESET) $(PLATFORM) ($(NPROC) cores available)"

# Include modular makefiles
include mk/deps.mk
include mk/tests.mk
include mk/fonts.mk
include mk/rules.mk
