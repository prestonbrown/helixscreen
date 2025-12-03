# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Main Makefile
# LVGL 9 + SDL2 simulator with modular build system
#
# ⚠️ CRITICAL: Always use 'make' - NEVER invoke gcc/g++ directly!
# The build system handles:
#   - Dependency management (libhv, lvgl, SDL2)
#   - Platform detection (macOS vs Linux)
#   - Parallel builds (auto-fixes 'make -j' to use correct core count)
#   - Patch application for multi-display support
#
# Common commands:
#   make -j       # Parallel incremental build (auto-detects cores)
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

# Ccache integration - auto-detect and use if available (10x faster rebuilds)
CCACHE := $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
    CC := ccache $(CC)
    CXX := ccache $(CXX)
endif

# Dependency generation flags for proper header tracking
# -MMD: Generate .d dependency files for user headers (not system headers)
# -MP: Add phony targets for headers (prevents errors when headers are deleted)
# Note: -MF path is computed in the pattern rules to get the correct output path
DEPFLAGS = -MMD -MP

# Project source flags - warnings enabled, strict mode optional
# Use WERROR=1 to treat warnings as errors (for CI or `make strict`)
CFLAGS := -std=c11 -Wall -Wextra -O2 -g -D_GNU_SOURCE
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g

# Strict mode: -Werror plus additional useful warnings
ifeq ($(WERROR),1)
    CFLAGS += -Werror -Wconversion -Wshadow -Wno-error=deprecated-declarations
    CXXFLAGS += -Werror -Wconversion -Wshadow -Wno-error=deprecated-declarations
endif

# Submodule flags - suppress warnings from third-party code we don't control
# Uses -w to completely silence warnings (cleaner build output)
# Note: No DEPFLAGS for submodules - we don't track their internal dependencies
SUBMODULE_CFLAGS := -std=c11 -O2 -g -D_GNU_SOURCE -w
SUBMODULE_CXXFLAGS := -std=c++17 -O2 -g -w

# Platform detection (needed early for conditional compilation)
UNAME_S := $(shell uname -s)

# Cross-compilation support (must come early to override CC/CXX)
# Use TARGET=pi or TARGET=ad5m for cross-compilation
include mk/cross.mk

# Remote build support (build on fast Linux host, retrieve binaries)
include mk/remote.mk

# Directories
SRC_DIR := src
INC_DIR := include
# BUILD_DIR, BIN_DIR, OBJ_DIR may be set by cross.mk for cross-compilation
# Only set defaults if not already defined
BUILD_DIR ?= build
BIN_DIR ?= $(BUILD_DIR)/bin
OBJ_DIR ?= $(BUILD_DIR)/obj

# LVGL
LVGL_DIR := lib/lvgl
# Use -isystem to suppress warnings from third-party headers in strict mode
LVGL_INC := -isystem $(LVGL_DIR) -isystem $(LVGL_DIR)/src
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name "*.c" 2>/dev/null)
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_SRCS))

# ThorVG sources (.cpp files for SVG support)
THORVG_SRCS := $(shell find $(LVGL_DIR)/src/libs/thorvg -name "*.cpp" 2>/dev/null)
THORVG_OBJS := $(patsubst $(LVGL_DIR)/%.cpp,$(OBJ_DIR)/lvgl/%.o,$(THORVG_SRCS))

# cpp-terminal (modern TUI library)
CPP_TERMINAL_DIR := lib/cpp-terminal
# Use -isystem to suppress warnings from third-party headers in strict mode
CPP_TERMINAL_INC := -isystem $(CPP_TERMINAL_DIR)
CPP_TERMINAL_SRCS := $(wildcard $(CPP_TERMINAL_DIR)/cpp-terminal/*.cpp) \
                     $(wildcard $(CPP_TERMINAL_DIR)/cpp-terminal/private/*.cpp)
CPP_TERMINAL_OBJS := $(patsubst $(CPP_TERMINAL_DIR)/%.cpp,$(OBJ_DIR)/cpp-terminal/%.o,$(CPP_TERMINAL_SRCS))

# LVGL Demos (separate target)
LVGL_DEMO_SRCS := $(shell find $(LVGL_DIR)/demos -name "*.c" 2>/dev/null)
LVGL_DEMO_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_DEMO_SRCS))

# Application C sources
APP_C_SRCS := $(wildcard $(SRC_DIR)/*.c)
APP_C_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(APP_C_SRCS))

# Application C++ sources (exclude test binaries and splash binary)
APP_SRCS := $(filter-out $(SRC_DIR)/test_dynamic_cards.cpp $(SRC_DIR)/test_responsive_theme.cpp $(SRC_DIR)/test_tinygl_triangle.cpp $(SRC_DIR)/test_gcode_geometry.cpp $(SRC_DIR)/test_gcode_analysis.cpp $(SRC_DIR)/test_sdf_reconstruction.cpp $(SRC_DIR)/test_sparse_grid.cpp $(SRC_DIR)/test_partial_extraction.cpp $(SRC_DIR)/test_render_comparison.cpp $(SRC_DIR)/helix_splash.cpp,$(wildcard $(SRC_DIR)/*.cpp))
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

# SDL2 - Only needed for native desktop builds (not embedded targets)
# Cross-compilation targets use framebuffer/DRM instead
ifeq ($(ENABLE_SDL),yes)
    SDL2_SYSTEM_AVAILABLE := $(shell command -v sdl2-config 2>/dev/null)
    ifneq ($(SDL2_SYSTEM_AVAILABLE),)
        # System SDL2 found - use it
        SDL2_INC := $(shell sdl2-config --cflags)
        SDL2_LIBS := $(shell sdl2-config --libs)
        SDL2_LIB :=
    else
        # No system SDL2 - build from submodule
        SDL2_DIR := lib/sdl2
        SDL2_BUILD_DIR := $(SDL2_DIR)/build
        SDL2_LIB := $(SDL2_BUILD_DIR)/libSDL2.a
        SDL2_INC := -I$(SDL2_DIR)/include -I$(SDL2_BUILD_DIR)/include -I$(SDL2_BUILD_DIR)/include-config-release
        SDL2_LIBS := $(SDL2_LIB)
    endif
else
    # Embedded target - no SDL2
    SDL2_INC :=
    SDL2_LIBS :=
    SDL2_LIB :=
endif

# libhv (WebSocket client for Moonraker) - Use system version if available, otherwise build from submodule
LIBHV_PKG_CONFIG := $(shell pkg-config --exists libhv 2>/dev/null && echo "yes")
ifeq ($(LIBHV_PKG_CONFIG),yes)
    # System libhv found via pkg-config (pkg-config already returns system include paths)
    LIBHV_INC := $(shell pkg-config --cflags libhv)
    LIBHV_LIBS := $(shell pkg-config --libs libhv)
    LIBHV_LIB :=
else
    # No system libhv - build from submodule to $(BUILD_DIR)/lib/ for architecture isolation
    # This allows concurrent native/pi/ad5m builds without conflicts
    LIBHV_DIR := lib/libhv
    # Use -isystem to suppress warnings from third-party headers in strict mode
    LIBHV_INC := -isystem $(LIBHV_DIR)/include -isystem $(LIBHV_DIR)/cpputil -isystem $(LIBHV_DIR)
    LIBHV_LIB := $(BUILD_DIR)/lib/libhv.a
    LIBHV_LIBS := $(LIBHV_LIB)
endif

# spdlog (logging library) - Use system version if available, otherwise use submodule
SPDLOG_SYSTEM_PATHS := /usr/include/spdlog /usr/local/include/spdlog /opt/homebrew/include/spdlog
SPDLOG_SYSTEM_AVAILABLE := $(firstword $(wildcard $(SPDLOG_SYSTEM_PATHS)))
ifneq ($(SPDLOG_SYSTEM_AVAILABLE),)
    # System spdlog found - use it (header-only library)
    # Use -isystem to suppress warnings from third-party headers in strict mode
    SPDLOG_INC := -isystem $(dir $(SPDLOG_SYSTEM_AVAILABLE))
else
    # No system spdlog - use submodule
    SPDLOG_DIR := lib/spdlog
    # Use -isystem to suppress warnings from third-party headers in strict mode
    SPDLOG_INC := -isystem $(SPDLOG_DIR)/include
endif

# fmt (formatting library required by header-only spdlog)
# For cross-compilation, we can't use host pkg-config - must detect target library directly
ifneq ($(CROSS_COMPILE),)
    # Cross-compiling: check if target fmt library exists (installed via libfmt-dev:arm64 etc.)
    FMT_TARGET_LIB := $(shell ls /usr/lib/$(TARGET_TRIPLE)/libfmt.so 2>/dev/null || ls /usr/lib/$(TARGET_TRIPLE)/libfmt.a 2>/dev/null)
    ifneq ($(FMT_TARGET_LIB),)
        FMT_LIBS := -lfmt
    else
        # No target fmt - build will fail if spdlog requires it
        FMT_LIBS :=
    endif
else
    # Native build: use pkg-config normally
    FMT_PKG_CONFIG := $(shell pkg-config --exists fmt 2>/dev/null && echo "yes")
    ifeq ($(FMT_PKG_CONFIG),yes)
        FMT_LIBS := $(shell pkg-config --libs fmt)
    else
        FMT_LIBS :=
    endif
endif

# libsystemd (for systemd journal logging on Linux)
# Only check on Linux (native or cross-compile), not macOS
SYSTEMD_LIBS :=
SYSTEMD_CXXFLAGS :=
ifneq ($(UNAME_S),Darwin)
    ifneq ($(CROSS_COMPILE),)
        # Cross-compiling: check if target libsystemd exists
        SYSTEMD_TARGET_LIB := $(shell ls /usr/lib/$(TARGET_TRIPLE)/libsystemd.so 2>/dev/null || ls /usr/lib/$(TARGET_TRIPLE)/libsystemd.a 2>/dev/null)
        ifneq ($(SYSTEMD_TARGET_LIB),)
            SYSTEMD_CXXFLAGS := -DHELIX_HAS_SYSTEMD
            SYSTEMD_LIBS := -lsystemd
        endif
    else
        # Native Linux build: use pkg-config
        SYSTEMD_PKG_CONFIG := $(shell pkg-config --exists libsystemd 2>/dev/null && echo "yes")
        ifeq ($(SYSTEMD_PKG_CONFIG),yes)
            SYSTEMD_CXXFLAGS := -DHELIX_HAS_SYSTEMD
            SYSTEMD_LIBS := $(shell pkg-config --libs libsystemd)
        endif
    endif
endif

# TinyGL (software 3D rasterizer for G-code visualization)
# Set ENABLE_TINYGL_3D=no to build without 3D rendering support
ENABLE_TINYGL_3D ?= yes

ifeq ($(ENABLE_TINYGL_3D),yes)
    TINYGL_DIR := lib/tinygl
    # Output to $(BUILD_DIR)/lib/ for architecture isolation (native/pi/ad5m)
    TINYGL_LIB := $(BUILD_DIR)/lib/libTinyGL.a
    # Use -isystem to suppress warnings from third-party headers in strict mode
    TINYGL_INC := -isystem $(TINYGL_DIR)/include
    TINYGL_DEFINES := -DENABLE_TINYGL_3D
else
    TINYGL_DIR :=
    TINYGL_LIB :=
    TINYGL_INC :=
    TINYGL_DEFINES :=
endif

# wpa_supplicant (WiFi control via wpa_ctrl interface)
WPA_DIR := lib/wpa_supplicant
# Output to $(BUILD_DIR)/lib/ for architecture isolation (native/pi/ad5m)
WPA_CLIENT_LIB := $(BUILD_DIR)/lib/libwpa_client.a
# Use -isystem to suppress warnings from third-party headers in strict mode
WPA_INC := -isystem $(WPA_DIR)/src/common -isystem $(WPA_DIR)/src/utils

# Precompiled header for LVGL (30-50% faster clean builds)
# Only supported by gcc and clang (not MSVC)
PCH_HEADER := $(INC_DIR)/lvgl_pch.h
PCH := $(BUILD_DIR)/lvgl_pch.h.gch
PCH_FLAGS := -include $(PCH_HEADER)

# Include paths
# Project includes use -I (warnings enabled), library includes use -isystem (warnings suppressed)
# This allows `make strict` to catch issues in project code while ignoring third-party header warnings
INCLUDES := -I. -I$(INC_DIR) -isystem lib -isystem lib/glm $(LVGL_INC) $(LIBHV_INC) $(SPDLOG_INC) $(TINYGL_INC) $(WPA_INC) $(SDL2_INC)

# Common linker flags (used by both macOS and Linux)
LDFLAGS_COMMON := $(SDL2_LIBS) $(LIBHV_LIBS) $(TINYGL_LIB) $(FMT_LIBS) -lm -lpthread

# Platform-specific configuration
# Cross-compilation targets (pi, ad5m) are Linux-based embedded systems
ifneq ($(CROSS_COMPILE),)
    # Cross-compilation for embedded Linux targets
    NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    # Embedded targets link against libhv, wpa_supplicant, and OpenSSL
    # No SDL2 - display handled by framebuffer/DRM
    LDFLAGS := $(LIBHV_LIBS) $(TINYGL_LIB) $(FMT_LIBS) $(WPA_CLIENT_LIB) $(SYSTEMD_LIBS) -lssl -lcrypto -ldl -lm -lpthread
    # Add target-specific linker flags (e.g., -lstdc++fs for GCC 8)
    LDFLAGS += $(TARGET_LDFLAGS)
    # Add target-specific library path for cross-compilation
    LDFLAGS += -L/usr/lib/$(TARGET_TRIPLE)
    # DRM backend requires libdrm and libinput for LVGL display/input drivers
    ifeq ($(DISPLAY_BACKEND),drm)
        LDFLAGS += -ldrm -linput
    endif
    PLATFORM := Linux-$(TARGET_ARCH)
    WPA_DEPS := $(WPA_CLIENT_LIB)
    # Strip embedded binaries for size
    ifeq ($(STRIP_BINARY),yes)
        LDFLAGS += -s
    endif
else ifeq ($(UNAME_S),Darwin)
    # macOS native build - Uses CoreWLAN framework for WiFi (with fallback to mock)
    NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

    # Set minimum macOS version (10.15 Catalina for CoreWLAN/CoreLocation modern APIs)
    MACOS_MIN_VERSION := 10.15
    MACOS_DEPLOYMENT_TARGET := -mmacosx-version-min=$(MACOS_MIN_VERSION)

    CFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    CXXFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    SUBMODULE_CFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    SUBMODULE_CXXFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    # -Wl,-w suppresses linker warnings about macOS version mismatches between
    # our 10.15 deployment target and libraries built for newer versions
    LDFLAGS := -Wl,-w $(LDFLAGS_COMMON) -framework Foundation -framework CoreFoundation -framework Security -framework CoreWLAN -framework CoreLocation -framework Cocoa -framework IOKit -framework CoreVideo -framework AudioToolbox -framework ForceFeedback -framework Carbon -framework CoreAudio -framework Metal -liconv
    PLATFORM := macOS
    WPA_DEPS :=
else
    # Linux native build - Include libwpa_client.a for WiFi control
    NPROC := $(shell nproc 2>/dev/null || echo 4)
    LDFLAGS := $(LDFLAGS_COMMON) $(WPA_CLIENT_LIB) $(SYSTEMD_LIBS) -lssl -lcrypto -ldl
    PLATFORM := Linux
    WPA_DEPS := $(WPA_CLIENT_LIB)
endif

# Add TinyGL defines to compiler flags
CFLAGS += $(TINYGL_DEFINES)
CXXFLAGS += $(TINYGL_DEFINES)

# Add systemd defines to C++ compiler flags (for logging_init.cpp)
CXXFLAGS += $(SYSTEMD_CXXFLAGS)

# Parallel build control
# CRITICAL: 'make -j' (no number) means UNLIMITED parallelism in GNU Make!
# This causes hundreds of simultaneous compiler processes, crushing the system.
#
# Detection method:
#   - 'make -j' (unlimited): MAKEFLAGS = "pj" (just flags, no jobserver)
#   - 'make -j8' (bounded):  MAKEFLAGS = "p --jobserver-fds=3,5 -j" (has jobserver)
#
# We check for 'j' in flags WITHOUT '--jobserver-fds' to detect unlimited -j.

JOBS ?= $(NPROC)

# Detect unbounded 'make -j' (unlimited parallelism)
# NOTE: Detection happens in a guard target (see mk/rules.mk) because $(MAKEFLAGS)
# isn't accessible via make functions in GNU Make 3.81. The guard target runs
# FIRST before any compilation starts and aborts if unlimited -j is detected.
#
# MAKEFLAGS format in GNU Make 3.81:
#   - 'make -j':  MAKEFLAGS = "j" (just the flag, NO jobserver)
#   - 'make -j8': MAKEFLAGS = " --jobserver-fds=3,5 -j" (has jobserver pipe)
#
# Usage: make -j$(nproc) or make -j8 (NEVER bare 'make -j')

# Output synchronization for parallel builds (requires make 4.0+, ignored on 3.81)
ifneq ($(JOBS),1)
    MAKEFLAGS += --output-sync=target
endif

# Binaries
TARGET := $(BIN_DIR)/helix-screen
MOONRAKER_INSPECTOR := $(BIN_DIR)/moonraker-inspector

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
LVGL_TEST_FIXTURE_OBJ := $(OBJ_DIR)/tests/lvgl_test_fixture.o
TEST_FIXTURES_OBJ := $(OBJ_DIR)/tests/test_fixtures.o

# Mock objects for integration testing
MOCK_SRCS := $(wildcard $(TEST_MOCK_DIR)/*.cpp)
MOCK_OBJS := $(patsubst $(TEST_MOCK_DIR)/%.cpp,$(OBJ_DIR)/tests/mocks/%.o,$(MOCK_SRCS))

# Default target
.DEFAULT_GOAL := all

.PHONY: all build clean run test tests test-integration test-cards test-print-select test-size-content demo compile_commands libhv-build apply-patches generate-fonts help check-deps install-deps venv-setup icon format format-staged screenshots tools moonraker-inspector strict

# Help target - shows common commands, references topic-specific help
help:
	@if [ -t 1 ] && [ -n "$(TERM)" ] && [ "$(TERM)" != "dumb" ]; then \
		B='$(BOLD)'; R='$(RED)'; G='$(GREEN)'; Y='$(YELLOW)'; BL='$(BLUE)'; M='$(MAGENTA)'; C='$(CYAN)'; X='$(RESET)'; D='$(shell printf "\033[2m")'; \
	else \
		B=''; R=''; G=''; Y=''; BL=''; M=''; C=''; X=''; D=''; \
	fi; \
	echo "$${B}HelixScreen Build System$${X}"; \
	echo ""; \
	echo "$${C}Quick Start:$${X}"; \
	echo "  $${G}make -j$${X}           - Build (parallel, auto-detects cores)"; \
	echo "  $${G}make run$${X}          - Build and run the UI"; \
	echo "  $${G}make test$${X}         - Run unit tests"; \
	echo "  $${G}make clean$${X}        - Remove build artifacts"; \
	echo "  $${G}make strict$${X}       - Build with -Werror (warnings = errors)"; \
	echo ""; \
	echo "$${C}Common Tasks:$${X}"; \
	echo "  $${G}check-deps$${X}        - Verify dependencies are installed"; \
	echo "  $${G}install-deps$${X}      - Auto-install missing dependencies"; \
	echo "  $${G}format$${X}            - Auto-format C/C++ and XML files"; \
	echo "  $${G}compile_commands$${X}  - Generate compile_commands.json for IDE"; \
	echo ""; \
	echo "$${C}Tools:$${X}"; \
	echo "  $${G}tools$${X}             - Build diagnostic tools"; \
	echo "  $${G}moonraker-inspector$${X} - Query Moonraker printer metadata"; \
	echo "  $${G}generate-fonts$${X}    - Regenerate FontAwesome fonts"; \
	echo "  $${G}icon$${X}              - Generate app icon from logo"; \
	echo ""; \
	echo "$${C}More Help:$${X}  $${D}(use these for detailed target lists)$${X}"; \
	echo "  $${Y}make help-build$${X}   - Build system, dependencies, patches"; \
	echo "  $${Y}make help-test$${X}    - All test targets and options"; \
	echo "  $${Y}make help-cross$${X}   - Cross-compilation and Pi deployment"; \
	echo "  $${Y}make help-all$${X}     - Show everything"; \
	echo ""; \
	echo "$${C}Platform:$${X} $(PLATFORM) $${D}($(NPROC) cores)$${X}"

# Show all help topics combined
.PHONY: help-all
help-all: help-build help-test help-cross help-remote
	@echo ""
	@if [ -t 1 ] && [ -n "$(TERM)" ] && [ "$(TERM)" != "dumb" ]; then \
		echo "$(CYAN)Material Icons:$(RESET)"; \
	else \
		echo "Material Icons:"; \
	fi
	@echo "  material-icons-list    - List registered icons"
	@echo "  material-icons-add     - Download and add icons (ICONS=...)"
	@echo "  material-icons-convert - Convert SVGs to C arrays (SVGS=...)"

# Documentation screenshot generation
screenshots: $(BIN)
	$(Q)$(ECHO) "$(CYAN)Generating documentation screenshots...$(RESET)"
	$(Q)./scripts/generate-screenshots.sh
	$(Q)$(ECHO) "$(GREEN)✓ Documentation screenshots generated in docs/images/$(RESET)"

# Strict build - treat warnings as errors (for CI)
# This catches issues that would otherwise slip through
strict:
	@echo "$(CYAN)$(BOLD)Building with strict warnings (-Werror)...$(RESET)"
	$(Q)$(MAKE) WERROR=1 all

# Include modular makefiles
include mk/deps.mk
include mk/patches.mk
include mk/tests.mk
include mk/fonts.mk
include mk/format.mk
include mk/tools.mk
include mk/display-lib.mk
include mk/splash.mk
include mk/rules.mk
