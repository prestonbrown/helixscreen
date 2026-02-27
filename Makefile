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
# Priority: environment variables > clang (if working) > gcc
#
# On some Linux distros (e.g., Arch with GCC 15), Clang may fail to find
# GCC's libstdc++ headers, causing "#include_next <stdlib.h>" errors.
# We test-compile to detect this and auto-fallback to GCC.

# Helper to test if a C++ compiler can use the standard library
# Returns "ok" on success, empty on failure
# We test #include <cstdlib> because that's where clang+libstdc++ breaks on some systems
HASH := \#
define test_cxx_stdlib
$(shell printf '$(HASH)include <cstdlib>\n' | $(1) -x c++ -std=c++17 -fsyntax-only - 2>/dev/null && echo ok)
endef

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
    # Try clang++ first
    ifneq ($(shell command -v clang++ 2>/dev/null),)
        # Test if clang++ can actually compile C++ with stdlib
        ifeq ($(call test_cxx_stdlib,clang++),ok)
            CXX := clang++
        else ifneq ($(shell command -v g++ 2>/dev/null),)
            # Clang has stdlib issues, fall back to g++
            CXX := g++
            CC := gcc
            $(info Note: clang++ has stdlib issues on this system, using g++ instead)
        else
            # No g++ available, try clang++ anyway and let it fail with a clear error
            CXX := clang++
        endif
    else ifneq ($(shell command -v g++ 2>/dev/null),)
        CXX := g++
    else
        $(error No C++ compiler found. Install clang++ or g++)
    endif
endif

# Set RANLIB if not defined (needed for wpa_supplicant build on Linux)
ifeq ($(origin RANLIB),undefined)
    RANLIB := ranlib
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
CFLAGS := -std=c11 -Wall -Wextra -O2 -g -D_GNU_SOURCE -fno-omit-frame-pointer
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g -fno-omit-frame-pointer

# Version information (read from VERSION.txt file)
# Format: MAJOR.MINOR.PATCH following Semantic Versioning 2.0.0
# NOTE: Must use .txt extension to avoid shadowing C++20 <version> header on macOS
#       (macOS filesystem is case-insensitive, so VERSION would match <version>)
HELIX_VERSION := $(shell cat VERSION.txt 2>/dev/null || echo "0.0.0")
HELIX_VERSION_MAJOR := $(word 1,$(subst ., ,$(HELIX_VERSION)))
HELIX_VERSION_MINOR := $(word 2,$(subst ., ,$(HELIX_VERSION)))
HELIX_VERSION_PATCH := $(word 3,$(subst ., ,$(HELIX_VERSION)))
HELIX_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")

# Installer script filename (single source of truth for Makefile packaging + C++ extraction)
INSTALLER_FILENAME := install.sh

# Add version defines to compiler flags
VERSION_DEFINES := -DHELIX_VERSION=\"$(HELIX_VERSION)\" \
                   -DHELIX_VERSION_MAJOR=$(HELIX_VERSION_MAJOR) \
                   -DHELIX_VERSION_MINOR=$(HELIX_VERSION_MINOR) \
                   -DHELIX_VERSION_PATCH=$(HELIX_VERSION_PATCH) \
                   -DHELIX_GIT_HASH=\"$(HELIX_GIT_HASH)\" \
                   -DINSTALLER_FILENAME=\"$(INSTALLER_FILENAME)\"
CFLAGS += $(VERSION_DEFINES)
CXXFLAGS += $(VERSION_DEFINES)

# Strict mode: -Werror plus additional useful warnings
ifeq ($(WERROR),1)
    CFLAGS += -Werror -Wconversion -Wshadow -Wno-error=deprecated-declarations
    CXXFLAGS += -Werror -Wconversion -Wshadow -Wno-error=deprecated-declarations
endif

# Address Sanitizer support for debugging heap corruption
# Usage: make SANITIZE=address
ifeq ($(SANITIZE),address)
    SANITIZE_FLAGS := -fsanitize=address -fno-omit-frame-pointer
    CFLAGS += $(SANITIZE_FLAGS)
    CXXFLAGS += $(SANITIZE_FLAGS)
    LDFLAGS += $(SANITIZE_FLAGS)
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
# Add GLAD include path for desktop OpenGL ES (SDL) builds only.
# DRM+EGL builds use system EGL/GLES2 headers directly — GLAD's stub headers
# shadow them (define include guards but emit no types when LV_USE_OPENGLES=0).
ifeq ($(ENABLE_OPENGLES),yes)
  ifneq ($(DISPLAY_BACKEND),drm)
    LVGL_INC += -isystem $(LVGL_DIR)/src/drivers/opengles/glad/include
  endif
endif
# Exclude XML and expat sources from LVGL — those are now in lib/helix-xml
LVGL_SRCS := $(filter-out $(wildcard $(LVGL_DIR)/src/xml/*.c $(LVGL_DIR)/src/xml/parsers/*.c $(LVGL_DIR)/src/libs/expat/*.c),$(shell find $(LVGL_DIR)/src -name "*.c" 2>/dev/null))
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_SRCS))

# When OpenGL ES is enabled, the shader asset file uses C++11 raw string literals
# (R"(...)") that can't compile as C. Filter only that file from C sources and compile
# it as C++ separately. All other opengles sources stay as C.
ifeq ($(ENABLE_OPENGLES),yes)
    LVGL_OPENGLES_CXX_SRCS := $(filter $(LVGL_DIR)/src/drivers/opengles/assets/%,$(LVGL_SRCS))
    LVGL_SRCS := $(filter-out $(LVGL_DIR)/src/drivers/opengles/assets/%,$(LVGL_SRCS))
    LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_SRCS))
    LVGL_OPENGLES_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_OPENGLES_CXX_SRCS))
else
    LVGL_OPENGLES_OBJS :=
endif

# Helix XML engine (extracted from LVGL, with our patches baked in)
HELIX_XML_DIR := lib/helix-xml
HELIX_XML_SRCS := $(wildcard $(HELIX_XML_DIR)/src/xml/*.c $(HELIX_XML_DIR)/src/xml/parsers/*.c $(HELIX_XML_DIR)/src/libs/expat/*.c)
HELIX_XML_OBJS := $(patsubst $(HELIX_XML_DIR)/%.c,$(OBJ_DIR)/helix-xml/%.o,$(HELIX_XML_SRCS))

# lv_markdown (LVGL markdown widget)
LV_MARKDOWN_DIR := lib/lv_markdown
LV_MARKDOWN_INC := -isystem $(LV_MARKDOWN_DIR)/src -isystem $(LV_MARKDOWN_DIR)/deps/md4c
LV_MARKDOWN_SRCS := $(wildcard $(LV_MARKDOWN_DIR)/src/*.c) $(LV_MARKDOWN_DIR)/deps/md4c/md4c.c
LV_MARKDOWN_OBJS := $(patsubst $(LV_MARKDOWN_DIR)/%.c,$(OBJ_DIR)/lv_markdown/%.o,$(LV_MARKDOWN_SRCS))

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

# lv_markdown (markdown viewer widget + md4c parser)
LV_MARKDOWN_DIR := lib/lv_markdown
LV_MARKDOWN_INC := -isystem $(LV_MARKDOWN_DIR)/src -isystem $(LV_MARKDOWN_DIR)/deps/md4c
LV_MARKDOWN_SRCS := $(wildcard $(LV_MARKDOWN_DIR)/src/*.c) $(LV_MARKDOWN_DIR)/deps/md4c/md4c.c
LV_MARKDOWN_OBJS := $(patsubst $(LV_MARKDOWN_DIR)/%.c,$(OBJ_DIR)/lv_markdown/%.o,$(LV_MARKDOWN_SRCS))

# LVGL Demos (separate target)
LVGL_DEMO_SRCS := $(shell find $(LVGL_DIR)/demos -name "*.c" 2>/dev/null)
LVGL_DEMO_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o,$(LVGL_DEMO_SRCS))

# 3D G-code Rendering default (must be set before APP_SRCS filtering below)
# EGL/GLES is only available on Linux — disable on macOS
ifeq ($(UNAME_S),Darwin)
    ENABLE_GLES_3D ?= no
else
    ENABLE_GLES_3D ?= yes
endif

# Application C sources
APP_C_SRCS := $(wildcard $(SRC_DIR)/*.c)
APP_C_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(APP_C_SRCS))

# Application C++ sources (exclude test binaries, splash binary, and lvgl-demo)
# Include all subdirectories: ui/, api/, rendering/, printer/, print/, system/, application/
APP_SRCS := $(filter-out $(SRC_DIR)/test_dynamic_cards.cpp $(SRC_DIR)/test_responsive_theme.cpp $(SRC_DIR)/test_gcode_geometry.cpp $(SRC_DIR)/test_gcode_analysis.cpp $(SRC_DIR)/test_sdf_reconstruction.cpp $(SRC_DIR)/test_sparse_grid.cpp $(SRC_DIR)/test_partial_extraction.cpp $(SRC_DIR)/test_render_comparison.cpp $(SRC_DIR)/test_network_tester.cpp $(SRC_DIR)/helix_splash.cpp $(SRC_DIR)/helix_watchdog.cpp $(SRC_DIR)/lvgl-demo/main.cpp,$(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/*/*.cpp) $(wildcard $(SRC_DIR)/*/*/*.cpp))
# Exclude src/tools/ — standalone build tools have their own rules in tools.mk
APP_SRCS := $(filter-out $(wildcard $(SRC_DIR)/tools/*.cpp),$(APP_SRCS))
# Exclude GLES renderer when not enabled
ifneq ($(ENABLE_GLES_3D),yes)
    APP_SRCS := $(filter-out $(SRC_DIR)/rendering/gcode_gles_renderer.cpp,$(APP_SRCS))
endif
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

# Fonts - Material Design Icons (generated by scripts/regen_mdi_fonts.sh) and Noto Sans text fonts
FONT_SRCS := assets/fonts/mdi_icons_64.c assets/fonts/mdi_icons_48.c assets/fonts/mdi_icons_32.c assets/fonts/mdi_icons_24.c assets/fonts/mdi_icons_16.c assets/fonts/mdi_icons_14.c
# Noto Sans - Regular weight (10-28px)
FONT_SRCS += assets/fonts/noto_sans_10.c assets/fonts/noto_sans_11.c assets/fonts/noto_sans_12.c assets/fonts/noto_sans_14.c assets/fonts/noto_sans_16.c assets/fonts/noto_sans_18.c assets/fonts/noto_sans_20.c assets/fonts/noto_sans_24.c assets/fonts/noto_sans_26.c assets/fonts/noto_sans_28.c
# Noto Sans - Light weight (10-18px for text_small/text_xs)
FONT_SRCS += assets/fonts/noto_sans_light_10.c assets/fonts/noto_sans_light_11.c assets/fonts/noto_sans_light_12.c assets/fonts/noto_sans_light_14.c assets/fonts/noto_sans_light_16.c assets/fonts/noto_sans_light_18.c
# Noto Sans - Bold weight (14-28px)
FONT_SRCS += assets/fonts/noto_sans_bold_14.c assets/fonts/noto_sans_bold_16.c assets/fonts/noto_sans_bold_18.c assets/fonts/noto_sans_bold_20.c assets/fonts/noto_sans_bold_24.c assets/fonts/noto_sans_bold_28.c
FONT_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(FONT_SRCS))

# Material Design Icons - REMOVED
# Icons are now font-based using MDI font glyphs (mdi_icons_*.c)
# See include/ui_icon_codepoints.h for icon mapping

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
# Check for actual header file to avoid $(dir) path issues with directory paths
SPDLOG_SYSTEM_HEADER_PATHS := /usr/include/spdlog/spdlog.h /usr/local/include/spdlog/spdlog.h /opt/homebrew/include/spdlog/spdlog.h
SPDLOG_SYSTEM_HEADER := $(firstword $(wildcard $(SPDLOG_SYSTEM_HEADER_PATHS)))
ifneq ($(SPDLOG_SYSTEM_HEADER),)
    # System spdlog found
    # /usr/include and /usr/local/include are in compiler defaults - no -isystem needed
    # /opt/homebrew/include is NOT in macOS compiler defaults - needs explicit -isystem
    ifeq ($(SPDLOG_SYSTEM_HEADER),/opt/homebrew/include/spdlog/spdlog.h)
        SPDLOG_INC := -isystem /opt/homebrew/include
    else
        # /usr/include or /usr/local/include - compiler already searches these
        SPDLOG_INC :=
    endif
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
        ifeq ($(PLATFORM_TARGET),pi)
            # Pi: skip external fmt — spdlog bundles its own, and linking against
            # system libfmt causes soname mismatches across Debian versions
            # (Bullseye ships libfmt.so.7, Bookworm ships libfmt.so.9)
            FMT_LIBS :=
        else
            FMT_LIBS := -lfmt
        endif
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

# 3D G-code Rendering
# ENABLE_GLES_3D: GPU-accelerated OpenGL ES 2.0 via EGL (Pi + desktop Linux)
# (default set earlier, before APP_SRCS filtering)

ifeq ($(ENABLE_GLES_3D),yes)
    GLES3D_DEFINES := -DENABLE_GLES_3D
else
    GLES3D_DEFINES :=
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
# stb_image headers (used for thumbnail processing)
STB_INC := -isystem lib/tinygl/include-demo
INCLUDES := -I. -I$(INC_DIR) -Isrc/generated -isystem lib -isystem lib/glm $(LVGL_INC) $(LIBHV_INC) $(SPDLOG_INC) $(STB_INC) $(LV_MARKDOWN_INC) $(WPA_INC) $(SDL2_INC)

# Common linker flags (used by both macOS and Linux)
LDFLAGS_COMMON := $(SDL2_LIBS) $(LIBHV_LIBS) $(FMT_LIBS) -lz -lm -lpthread

# Platform-specific configuration
# Cross-compilation targets (pi, ad5m, k1) are Linux-based embedded systems
ifneq ($(CROSS_COMPILE),)
    # Cross-compilation for embedded Linux targets
    NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    # libnl libraries (built from submodule for cross-compilation)
    LIBNL_LIBS := $(BUILD_DIR)/lib/libnl-genl-3.a $(BUILD_DIR)/lib/libnl-3.a
    # Embedded targets link against libhv and wpa_supplicant
    # No SDL2 - display handled by framebuffer/DRM
    # SSL is optional - only needed if connecting to remote Moonraker over HTTPS
    # Note: libnl must come AFTER wpa_client (static linking order matters)
    # Note: -L path only for glibc targets (Pi, AD5M) - musl targets (K1) are self-contained
    ifeq ($(PLATFORM_TARGET),k1-dynamic)
        # K1 Dynamic: Mixed static/dynamic linking
        # Project libraries linked statically, system libraries linked dynamically
        # -lstdc++fs: GCC 7.5 requires separate library for <experimental/filesystem>
        LDFLAGS := -Wl,-Bstatic \
            $(LIBHV_LIBS) $(FMT_LIBS) $(WPA_CLIENT_LIB) $(LIBNL_LIBS) -lstdc++fs \
            -Wl,-Bdynamic \
            -lstdc++ -lz -lm -lpthread -lrt -ldl -latomic -lgcc_s
    else ifneq ($(filter mips k1 ad5x,$(PLATFORM_TARGET)),)
        # MIPS targets (K1, AD5X) use musl - fully static, no system library paths needed
        # -latomic: Required for 64-bit atomics on 32-bit MIPS (std::atomic<int64_t>)
        LDFLAGS := $(LIBHV_LIBS) $(FMT_LIBS) $(WPA_CLIENT_LIB) $(LIBNL_LIBS) -latomic -ldl -lz -lm -lpthread
    else ifeq ($(PLATFORM_TARGET),k2)
        # K2 uses musl - fully static, no system library paths needed (same as K1)
        LDFLAGS := $(LIBHV_LIBS) $(FMT_LIBS) $(WPA_CLIENT_LIB) $(LIBNL_LIBS) -ldl -lz -lm -lpthread
    else
        LDFLAGS := -L/usr/lib/$(TARGET_TRIPLE) $(LIBHV_LIBS) $(FMT_LIBS) $(WPA_CLIENT_LIB) $(LIBNL_LIBS) $(SYSTEMD_LIBS) -ldl -lz -lm -lpthread
    endif
    ifeq ($(ENABLE_SSL),yes)
        ifneq (,$(filter pi pi-fbdev pi-both pi32 pi32-fbdev pi32-both,$(PLATFORM_TARGET)))
            # Pi: static-link OpenSSL to avoid libssl soname mismatch across Debian versions
            # (Bullseye has libssl.so.1.1, Bookworm has libssl.so.3)
            LDFLAGS += -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic
        else
            LDFLAGS += -lssl -lcrypto
        endif
    endif
    # Add target-specific linker flags (e.g., -lstdc++fs for GCC 8)
    LDFLAGS += $(TARGET_LDFLAGS)
    # DRM backend requires libdrm and libinput for LVGL display/input drivers
    ifeq ($(DISPLAY_BACKEND),drm)
        LDFLAGS += -ldrm -linput
        # GPU-accelerated rendering via EGL/OpenGL ES
        ifeq ($(ENABLE_OPENGLES),yes)
            LDFLAGS += -lEGL -lGLESv2 -lgbm
        endif
    endif
    PLATFORM := Linux-$(TARGET_ARCH)
    WPA_DEPS := $(WPA_CLIENT_LIB)
    # Strip embedded binaries for size (CI extracts symbols first, then strips)
    ifeq ($(STRIP_BINARY),yes)
        STRIP_CMD := $(CROSS_COMPILE)strip
        NM_CMD := $(CROSS_COMPILE)nm
    endif
else ifeq ($(UNAME_S),Darwin)
    # macOS native build - Uses CoreWLAN framework for WiFi (with fallback to mock)
    NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

    # Set minimum macOS version (10.15 Catalina for CoreWLAN/CoreLocation modern APIs)
    MACOS_MIN_VERSION := 10.15
    MACOS_DEPLOYMENT_TARGET := -mmacosx-version-min=$(MACOS_MIN_VERSION)

    CFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    CXXFLAGS += $(MACOS_DEPLOYMENT_TARGET)
    # macOS has no gettid() — override libhv's hconfig.h which incorrectly assumes it
    CFLAGS += -DHAVE_GETTID=0
    CXXFLAGS += -DHAVE_GETTID=0
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
    # Note: -lstdc++fs needed for std::experimental::filesystem on GCC < 9
    LDFLAGS := $(LDFLAGS_COMMON) $(WPA_CLIENT_LIB) $(SYSTEMD_LIBS) -lssl -lcrypto -ldl -lstdc++fs
    # GPU-accelerated 3D G-code rendering via OpenGL ES 2.0 (SDL GL context on desktop)
    ifeq ($(ENABLE_GLES_3D),yes)
        LDFLAGS += -lGLESv2
    endif
    PLATFORM := Linux
    WPA_DEPS := $(WPA_CLIENT_LIB)
endif

# Add 3D renderer defines to compiler flags
CFLAGS += $(GLES3D_DEFINES)
CXXFLAGS += $(GLES3D_DEFINES)

# Add systemd defines to C++ compiler flags (for logging_init.cpp)
CXXFLAGS += $(SYSTEMD_CXXFLAGS)

# Parallel build control
# Auto-parallelizes builds: plain 'make' automatically uses -j$(NPROC).
#
# Detection method (see mk/rules.mk):
#   - 'make':     No jobserver → auto-add -j$(NPROC)
#   - 'make -j':  No jobserver → auto-fix to -j$(NPROC) with warning
#   - 'make -j8': Has jobserver → pass through unchanged
#
# MAKEFLAGS format:
#   - 'make' or 'make -j': No 'jobserver' in MAKEFLAGS
#   - 'make -jN': MAKEFLAGS contains '--jobserver-fds=X,Y' or '--jobserver-auth'

JOBS ?= $(NPROC)

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
TEST_BIN := $(BIN_DIR)/helix-tests
TEST_INTEGRATION_BIN := $(BIN_DIR)/run_integration_tests

# Unit tests (use real LVGL) - exclude mock example
# Include tests from unit/ directory and unit/application/ subdirectory
TEST_SRCS := $(filter-out $(TEST_UNIT_DIR)/test_mock_example.cpp,$(wildcard $(TEST_UNIT_DIR)/*.cpp))
TEST_APP_SRCS := $(wildcard $(TEST_UNIT_DIR)/application/*.cpp)
TEST_OBJS := $(patsubst $(TEST_UNIT_DIR)/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_SRCS))
TEST_APP_OBJS_EXTRA := $(patsubst $(TEST_UNIT_DIR)/application/%.cpp,$(OBJ_DIR)/tests/application/%.o,$(TEST_APP_SRCS))

# Integration tests (use mocks instead of real LVGL)
TEST_INTEGRATION_SRCS := $(TEST_UNIT_DIR)/test_mock_example.cpp
TEST_INTEGRATION_OBJS := $(patsubst $(TEST_UNIT_DIR)/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_INTEGRATION_SRCS))

TEST_MAIN_OBJ := $(OBJ_DIR)/tests/test_main.o
CATCH2_OBJ := $(OBJ_DIR)/tests/catch_amalgamated.o
UI_TEST_UTILS_OBJ := $(OBJ_DIR)/tests/ui_test_utils.o
LVGL_TEST_FIXTURE_OBJ := $(OBJ_DIR)/tests/lvgl_test_fixture.o
TEST_FIXTURES_OBJ := $(OBJ_DIR)/tests/test_fixtures.o
LVGL_UI_TEST_FIXTURE_OBJ := $(OBJ_DIR)/tests/lvgl_ui_test_fixture.o

# Mock objects for integration testing
# Exclude moonraker_client_mock.cpp - it exists in src/api/ and is already in TEST_APP_OBJS
MOCK_SRCS := $(filter-out $(TEST_MOCK_DIR)/moonraker_client_mock.cpp, $(wildcard $(TEST_MOCK_DIR)/*.cpp))
MOCK_OBJS := $(patsubst $(TEST_MOCK_DIR)/%.cpp,$(OBJ_DIR)/tests/mocks/%.o,$(MOCK_SRCS))

# Default target
.DEFAULT_GOAL := all

.PHONY: all build clean run test tests test-integration test-cards test-print-select test-size-content demo compile_commands compile_commands_full libhv-build apply-patches generate-fonts validate-fonts regen-fonts update-mdi-cache verify-mdi-codepoints help check-deps install-deps venv-setup icon format format-staged screenshots tools moonraker-inspector strict quality setup translations symbols strip

# Developer setup - configure git hooks and commit template
setup:
	@git config core.hooksPath .githooks
	@git config commit.template .githooks/commit-template
	@echo "✓ Git configured:"
	@echo "  - Pre-commit hook enabled (.githooks/)"
	@echo "  - Commit template enabled (.githooks/commit-template)"

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
	echo "  $${G}make setup$${X}        - Configure git hooks and commit template"; \
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
	echo "  $${G}validate-fonts$${X}    - Check all icons are in compiled fonts"; \
	echo "  $${G}regen-fonts$${X}       - Regenerate MDI icon fonts"; \
	echo "  $${G}quality$${X}           - Run all quality checks"; \
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

# =============================================================================
# Symbol extraction and stripping (for crash backtrace resolution)
# Runs automatically for cross-compiled builds (STRIP_BINARY=yes).
# Extracts symbol maps first, then strips — preserving debug info for
# offline crash resolution while keeping the deployed binary small.
#
# Artifacts produced (uploaded to R2 by CI):
#   .sym    — nm -nC output for fast function-name lookup
#   .debug  — DWARF debug info for addr2line (file:line, inlined frames)
# =============================================================================
OBJCOPY_CMD := $(CROSS_COMPILE)objcopy

symbols: $(TARGET)
ifeq ($(STRIP_BINARY),yes)
	$(NM_CMD) -nC $(TARGET) > $(TARGET).sym
	@echo "Symbol map: $(TARGET).sym"
	$(OBJCOPY_CMD) --only-keep-debug $(TARGET) $(TARGET).debug
	@echo "Debug info: $(TARGET).debug ($(shell du -h $(TARGET).debug 2>/dev/null | cut -f1 || echo '?'))"
else
	@echo "STRIP_BINARY not set — skipping symbol extraction"
endif

strip: symbols
ifeq ($(STRIP_BINARY),yes)
	$(STRIP_CMD) $(TARGET)
	$(STRIP_CMD) $(SPLASH_BIN)
	$(STRIP_CMD) $(WATCHDOG_BIN)
	@echo "Stripped: $(TARGET) $(SPLASH_BIN) $(WATCHDOG_BIN)"
else
	@echo "STRIP_BINARY not set — skipping strip"
endif

# Strict build - treat warnings as errors (for CI)
# This catches issues that would otherwise slip through
strict:
	@echo "$(CYAN)$(BOLD)Building with strict warnings (-Werror)...$(RESET)"
	$(Q)$(MAKE) WERROR=1 all

# Run all quality checks (same as CI and pre-commit)
quality:
	@echo "$(CYAN)$(BOLD)Running quality checks...$(RESET)"
	$(Q)./scripts/quality-checks.sh

# Include modular makefiles
include mk/deps.mk
include mk/patches.mk
include mk/translations.mk
include mk/tests.mk
include mk/fonts.mk
include mk/images.mk
include mk/format.mk
include mk/tools.mk
include mk/display-lib.mk
include mk/splash.mk
include mk/watchdog.mk
ifdef PI_DUAL_LINK
include mk/pi-dual-link.mk
endif
include mk/rules.mk
