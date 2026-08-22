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
# See: docs/devel/DEVELOPMENT.md for complete build instructions

# Use bash for all shell commands (needed for [[ ]] and read -n)
SHELL := /bin/bash

# Delete a target if its recipe fails. Without this, a failed link can leave a
# truncated/0-byte output (e.g. helix-tests) on disk with a newer mtime than its
# prerequisites — the next `make` then treats the broken artifact as up-to-date
# and never rebuilds it, so the only recovery is a manual `rm`. Deleting the
# half-written target on failure makes the next invocation retry cleanly.
.DELETE_ON_ERROR:

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

# Yocto mode: bitbake passes CC/CXX/AR/LD/STRIP/RANLIB on the make command line
# with full paths and machine flags baked in. Skip host-toolchain autodetect
# and ccache wrapping — bitbake manages its own caching upstream.
ifneq ($(PLATFORM_TARGET),yocto)

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
    # Avoid double-wrapping when CC/CXX already include ccache (common in CI env).
    ifneq ($(notdir $(firstword $(CC))),ccache)
        CC := ccache $(CC)
    endif
    ifneq ($(notdir $(firstword $(CXX))),ccache)
        CXX := ccache $(CXX)
    endif
endif

endif # PLATFORM_TARGET != yocto

# Dependency generation flags for proper header tracking
# -MMD: Generate .d dependency files for user headers (not system headers)
# -MP: Add phony targets for headers (prevents errors when headers are deleted)
# Note: -MF path is computed in the pattern rules to get the correct output path
DEPFLAGS = -MMD -MP

# Optimization level: -O2 by default, override with OPT=0 or OPT=1 for faster builds
# 'make dev' sets OPT=0 automatically (~2x faster compilation)
OPT ?= 2

# Project source flags - warnings enabled, strict mode optional
# Use WERROR=1 to treat warnings as errors (for CI or `make strict`)
# -D_FORTIFY_SOURCE=2: compile-time + runtime bounds checking for memcpy/sprintf/etc
# -fstack-protector-strong: stack canaries on functions with local arrays/alloca
# Note: _FORTIFY_SOURCE requires optimization (-O1+), so disable it at -O0
#
# Yocto mode: bitbake's CFLAGS/CXXFLAGS already include --sysroot, -march,
# optimization, and security flags. Append our project flags with += instead
# of clobbering. -Wno-psabi silences the ARM parameter-passing ABI-change
# notes that appear on cortex-a7 hard-float cross builds.
ifeq ($(PLATFORM_TARGET),yocto)
    CFLAGS += -std=c11 -Wall -Wextra -D_GNU_SOURCE -Wno-psabi
    CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-psabi
else
    CFLAGS := -std=c11 -Wall -Wextra -O$(OPT) -g -D_GNU_SOURCE -fno-omit-frame-pointer -fstack-protector-strong
    CXXFLAGS := -std=c++17 -Wall -Wextra -O$(OPT) -g -fno-omit-frame-pointer -fstack-protector-strong
    ifneq ($(OPT),0)
        CFLAGS += -D_FORTIFY_SOURCE=2
        CXXFLAGS += -D_FORTIFY_SOURCE=2
    endif
endif

# Version information (read from VERSION.txt file)
# Format: MAJOR.MINOR.PATCH following Semantic Versioning 2.0.0
# NOTE: Must use .txt extension to avoid shadowing C++20 <version> header on macOS
#       (macOS filesystem is case-insensitive, so VERSION would match <version>)
HELIX_VERSION := $(shell cat VERSION.txt 2>/dev/null || echo "0.0.0")
HELIX_VERSION_MAJOR := $(word 1,$(subst ., ,$(HELIX_VERSION)))
HELIX_VERSION_MINOR := $(word 2,$(subst ., ,$(HELIX_VERSION)))
HELIX_VERSION_PATCH := $(word 3,$(subst ., ,$(HELIX_VERSION)))
# The short git hash is produced by scripts/gen-git-hash.sh into a generated
# header, not read here: a make variable would be a second source of the same
# value, free to drift from the one the binary actually reports.

# Installer script filename (single source of truth for Makefile packaging + C++ extraction)
INSTALLER_FILENAME := install.sh

# Add version defines to compiler flags.
#
# HELIX_GIT_HASH is deliberately NOT here. Everything in this list lands on
# every translation unit's command line, and ccache's direct mode hashes that
# command line, so a value that changes every commit invalidates the whole
# project's cache on every push. The rest only move at release, which is why
# they are safe to keep global. The hash goes through a generated header that
# reaches one object instead. See scripts/gen-git-hash.sh and $(GIT_HASH_H).
VERSION_DEFINES := -DHELIX_VERSION=\"$(HELIX_VERSION)\" \
                   -DHELIX_VERSION_MAJOR=$(HELIX_VERSION_MAJOR) \
                   -DHELIX_VERSION_MINOR=$(HELIX_VERSION_MINOR) \
                   -DHELIX_VERSION_PATCH=$(HELIX_VERSION_PATCH) \
                   -DINSTALLER_FILENAME=\"$(INSTALLER_FILENAME)\"
CFLAGS += $(VERSION_DEFINES)
CXXFLAGS += $(VERSION_DEFINES)

# Strict mode: -Werror plus additional useful warnings
ifeq ($(WERROR),1)
    CFLAGS += -Werror -Wconversion -Wshadow -Wno-error=deprecated-declarations
    CXXFLAGS += -Werror -Wconversion -Wshadow -Wno-error=deprecated-declarations
endif

# Address Sanitizer support for debugging heap corruption
# Usage: make SANITIZE=address (native), or `make pi-asan-docker` (cross).
# We only DEFINE the flag variable here; mk/cross.mk applies it after
# TARGET_CFLAGS and SUBMODULE_CFLAGS have been merged so LVGL, helix-xml,
# and other in-tree subprojects get instrumented too. _FORTIFY_SOURCE is
# disabled in this mode because it conflicts with ASAN's libc wrappers.
ifeq ($(SANITIZE),address)
    SANITIZE_FLAGS := -fsanitize=address -fno-omit-frame-pointer
    CFLAGS := $(filter-out -D_FORTIFY_SOURCE=2,$(CFLAGS))
    CXXFLAGS := $(filter-out -D_FORTIFY_SOURCE=2,$(CXXFLAGS))
endif

# Thread Sanitizer, for the other half of the problem space: ASAN finds the
# corruption, TSAN finds the unsynchronized access that caused it. Added while
# chasing #1198, whose glibc abort is detected during a ThumbnailCache eviction
# that runs on both the main thread and HttpExecutor workers (#1202).
#
# Usage: make SANITIZE=thread
#
# NOT combinable with SANITIZE=address — the two runtimes are mutually
# exclusive, and asking for both silently gets you neither.
ifeq ($(SANITIZE),thread)
    SANITIZE_FLAGS := -fsanitize=thread -fno-omit-frame-pointer
    CFLAGS := $(filter-out -D_FORTIFY_SOURCE=2,$(CFLAGS))
    CXXFLAGS := $(filter-out -D_FORTIFY_SOURCE=2,$(CXXFLAGS))
endif

# Submodule flags - suppress warnings from third-party code we don't control
# Uses -w to completely silence warnings (cleaner build output)
# Note: No DEPFLAGS for submodules - we don't track their internal dependencies
SUBMODULE_CFLAGS := -std=c11 -O2 -g -D_GNU_SOURCE -w
SUBMODULE_CXXFLAGS := -std=c++17 -O2 -g -w

# XML create-cost profiling in lib/helix-xml (lv_xml.c). Counts component
# creates and separates expat/SAX time from element-handler time, which is the
# only way to tell parsing apart from widget building: XML_Parse drives the
# handlers, so one timer around it measures both.
#
# Dev-build only, and OFF even there. It reads the clock on every element, which
# distorts what it is measuring, and logs every 25 creates. Turn it on for a
# measurement run, not for everyday work:
#   make ENABLE_XML_PROFILE=yes
#
# make does not track compiler flags, so toggling this rebuilds nothing on its
# own — delete the object too:
#   rm -f build/obj/helix-xml/src/xml/lv_xml.o
ENABLE_XML_PROFILE ?= no
ifeq ($(ENABLE_XML_PROFILE),yes)
    SUBMODULE_CFLAGS += -DLV_XML_PROFILE=1
endif

# Platform detection (needed early for conditional compilation)
UNAME_S := $(shell uname -s)

# Cross-compilation support (must come early to override CC/CXX)
# Use TARGET=pi or TARGET=ad5m for cross-compilation
include mk/cross.mk

# Font tier lists (must come after cross.mk sets FONT_TIERS, before FONT_SRCS)
include mk/fonts.mk

# Remote build support (build on fast Linux host, retrieve binaries)
include mk/remote.mk

# Directories
SRC_DIR := src
INC_DIR := include
# BUILD_DIR, BIN_DIR, OBJ_DIR may be set by cross.mk for cross-compilation
# Only set defaults if not already defined
BUILD_DIR ?= build

# Native sanitizer builds need their own object tree, the same way cross.mk
# suffixes BUILD_SUBDIR with -asan/-tsan. Without this, `make SANITIZE=address`
# on an already-built tree finds every object up to date and only RELINKS them:
# the result is a binary with zero __asan symbols that passes everything,
# because it was never instrumented. That silently cost a full debugging pass
# on prestonbrown/helixscreen#960.
#
# BIN_DIR is deliberately NOT suffixed. mk/tests.mk computes TEST_ASAN_BIN from
# BIN_DIR in the PARENT make (where SANITIZE is unset) and passes only
# OBJ_DIR/PCH down, so moving BIN_DIR here would have the sub-make build into a
# directory the parent is not looking in. The app binary is therefore still
# replaced in place — but it is genuinely instrumented, which is the part that
# matters. Pass BIN_DIR=... explicitly to keep both.
ifeq ($(SANITIZE),address)
    OBJ_DIR ?= $(BUILD_DIR)/obj-asan
endif
ifeq ($(SANITIZE),thread)
    OBJ_DIR ?= $(BUILD_DIR)/obj-tsan
endif

BIN_DIR ?= $(BUILD_DIR)/bin
OBJ_DIR ?= $(BUILD_DIR)/obj

# LVGL
LVGL_DIR := lib/lvgl
# LVGL config discovery. Defined here (not further down) so it can travel inside
# $(LVGL_INC): every flag set that compiles LVGL-dependent code then reaches
# lv_conf.h via -I. (project root) + -DLV_CONF_INCLUDE_SIMPLE, instead of LVGL's
# fragile '#include "../../lv_conf.h"' fallback. That fallback only resolves by
# accident in a normal checkout and BREAKS when lib/lvgl is a symlink (git
# worktrees — see scripts/setup-worktree.sh), which silently broke every
# cross-compile from a worktree (splash + display-backend + watchdog sub-builds).
# Carrying it in LVGL_INC means new LVGL sub-builds inherit correct discovery
# automatically rather than each having to remember to append $(LV_CONF).
LV_CONF := -DLV_CONF_INCLUDE_SIMPLE
# Use -isystem to suppress warnings from third-party headers in strict mode
LVGL_INC := -isystem $(LVGL_DIR) -isystem $(LVGL_DIR)/src -I. $(LV_CONF)
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

# quirc (QR code decoder library)
QUIRC_DIR := lib/quirc
QUIRC_INC := -isystem $(QUIRC_DIR)/lib
QUIRC_SRCS := $(wildcard $(QUIRC_DIR)/lib/*.c)
QUIRC_OBJS := $(patsubst $(QUIRC_DIR)/%.c,$(OBJ_DIR)/quirc/%.o,$(QUIRC_SRCS))

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

# Screensaver default (enabled on desktop/Pi, disabled on constrained targets)
ENABLE_SCREENSAVER ?= yes

# Application C sources
APP_C_SRCS := $(wildcard $(SRC_DIR)/*.c)
APP_C_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(APP_C_SRCS))

# Application C++ sources (exclude test binaries, splash binary, and lvgl-demo)
# Include all subdirectories: ui/, api/, rendering/, printer/, print/, system/, application/
APP_SRCS := $(filter-out $(SRC_DIR)/test_dynamic_cards.cpp $(SRC_DIR)/test_responsive_theme.cpp $(SRC_DIR)/test_gcode_geometry.cpp $(SRC_DIR)/test_gcode_analysis.cpp $(SRC_DIR)/test_sdf_reconstruction.cpp $(SRC_DIR)/test_sparse_grid.cpp $(SRC_DIR)/test_partial_extraction.cpp $(SRC_DIR)/test_render_comparison.cpp $(SRC_DIR)/test_network_tester.cpp $(SRC_DIR)/helix_splash.cpp $(SRC_DIR)/helix_watchdog.cpp $(SRC_DIR)/lvgl-demo/main.cpp,$(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/*/*.cpp) $(wildcard $(SRC_DIR)/*/*/*.cpp))
# Exclude src/tools/ — standalone build tools have their own rules in tools.mk
APP_SRCS := $(filter-out $(wildcard $(SRC_DIR)/tools/*.cpp),$(APP_SRCS))
# Exclude src/bluetooth/ — built as separate shared library (mk/bluetooth.mk)
APP_SRCS := $(filter-out $(wildcard $(SRC_DIR)/bluetooth/*.cpp),$(APP_SRCS))
# Exclude GLES renderer when not enabled
ifneq ($(ENABLE_GLES_3D),yes)
    APP_SRCS := $(filter-out $(SRC_DIR)/rendering/gcode_gles_renderer.cpp,$(APP_SRCS))
endif
# Exclude screensaver when not enabled
ifneq ($(ENABLE_SCREENSAVER),yes)
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/ui_screensaver.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/screensaver_manager.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/screensaver_starfield.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/screensaver_pipes.cpp,$(APP_SRCS))
endif
# Mock backends (enabled by default, disable with ENABLE_MOCKS=no for production)
ENABLE_MOCKS ?= yes

# PWM sysfs buzzer backend — ad5m/ad5m-br/ad5x only.
#
# The backend's own runtime probe is just "does /sys/class/pwm/pwmchip0 exist",
# which is true on boards whose PWM controller drives something else entirely: a
# CC1 has 8 channels there, no beeper on any of them, and its backlight on the
# same controller. So the probe cannot be trusted to decide this — the platform
# must. M300 (the PRINTER's beeper, over gcode) is deliberately NOT gated and
# keeps working everywhere.
#
# Decided HERE, above APP_OBJS, and not down in the sound-flags section: APP_OBJS
# is computed from APP_SRCS a few lines below, so a filter-out placed after it is
# a silent no-op that still compiles and still links the backend.
ifneq (,$(filter ad5m ad5m-br ad5x,$(PLATFORM_TARGET)))
    PWM_SOUND_CXXFLAGS := -DHELIX_HAS_PWM_SOUND
else
    APP_SRCS := $(filter-out $(SRC_DIR)/system/pwm_sound_backend.cpp,$(APP_SRCS))
endif

ifneq ($(ENABLE_MOCKS),yes)
    APP_SRCS := $(filter-out $(wildcard $(SRC_DIR)/api/*_mock*.cpp),$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/printer/ams_backend_mock.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/api/moonraker_api_mock.cpp,$(APP_SRCS))
endif

# Remote-control subsystem (helixctl server + socket/HTTP transport + the folded
# `ctl`/`repl` client). Dev/test-only: default ON for the native dev build, OFF
# for release/cross builds so shipped devices don't build it or pay the overhead.
# Force it into a device dev/test image by overriding on the command line:
#   make PLATFORM_TARGET=pi ENABLE_REMOTE_CONTROL=yes
ifeq ($(PLATFORM_TARGET),native)
    ENABLE_REMOTE_CONTROL ?= yes
else
    ENABLE_REMOTE_CONTROL ?= no
endif

ifeq ($(ENABLE_REMOTE_CONTROL),yes)
    # The helixctl client is folded into helix-screen (src/remote/remote_client.cpp,
    # reached via the `ctl`/`repl` subcommands); it needs linenoise for the REPL.
    REMOTE_LINENOISE_OBJ := $(OBJ_DIR)/linenoise.o
else
    REMOTE_LINENOISE_OBJ :=
    APP_SRCS := $(filter-out $(wildcard $(SRC_DIR)/remote/*.cpp),$(APP_SRCS))
endif

# Developer-only showcase panels. Not reachable from the shipped navigation
# (no PanelId, no PanelFactory wiring) — they exist as live testbeds: XML
# binding/repeat demos (test_panel), wizard step-progress (step_test_panel),
# the 3D G-code viewer harness (gcode_test_panel), and icon-font coverage
# (glyphs_panel). Dev-only: default ON for the native dev build, OFF for
# release/cross builds. Force into a device dev image with:
#   make PLATFORM_TARGET=pi ENABLE_DEV_PANELS=yes
ifeq ($(PLATFORM_TARGET),native)
    ENABLE_DEV_PANELS ?= yes
else
    ENABLE_DEV_PANELS ?= no
endif

ifneq ($(ENABLE_DEV_PANELS),yes)
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/ui_panel_test.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/ui_panel_step_test.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/ui_panel_gcode_test.cpp,$(APP_SRCS))
    APP_SRCS := $(filter-out $(SRC_DIR)/ui/ui_panel_glyphs.cpp,$(APP_SRCS))
endif
APP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(APP_SRCS))

# Linenoise (bundled line-editing library) for the folded REPL. Built as C99 with
# _GNU_SOURCE so strdup/fchmod/fileno are declared — without it they are implicit
# int, truncating strdup's 64-bit pointer to 32 bits (corrupt history -> SIGSEGV).
$(OBJ_DIR)/linenoise.o: lib/linenoise/linenoise.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CC]$(RESET) $<"
	$(Q)$(CC) -std=c99 -D_GNU_SOURCE -Wall -Os -c $< -o $@

# libhv dns_resolv.c — needed by safe_resolve.h on statically-linked ARM/MIPS
# targets (avoids glibc __check_pf SIGSEGV). Only compiled for cross builds.
# Uses APP_ prefix to avoid collision with DNS_RESOLV_OBJ in tests.mk.
ifneq ($(CROSS_COMPILE),)
    APP_DNS_RESOLV_OBJ := $(OBJ_DIR)/dns_resolv.o
else
    APP_DNS_RESOLV_OBJ :=
endif

# Objective-C++ sources (macOS only - .mm files)
# Only include on macOS, exclude on Linux to avoid linking errors
ifeq ($(UNAME_S),Darwin)
    OBJCPP_SRCS := $(wildcard $(SRC_DIR)/*.mm)
    OBJCPP_OBJS := $(patsubst $(SRC_DIR)/%.mm,$(OBJ_DIR)/%.o,$(OBJCPP_SRCS))
else
    OBJCPP_SRCS :=
    OBJCPP_OBJS :=
endif

# Font sources — assembled per-platform from mk/fonts.mk tier lists
# FONT_TIERS is set per-platform in mk/cross.mk (default: all)
FONT_SRCS := $(TIER_FONT_SRCS)
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
# LIBHV_DIR always points to the submodule — needed for internal headers (e.g. base/dns_resolv.h)
LIBHV_DIR := lib/libhv
LIBHV_PKG_CONFIG := $(shell pkg-config --exists libhv 2>/dev/null && echo "yes")
ifeq ($(YOCTO_BUILD),yes)
    # Yocto: libhv comes from DEPENDS (sysroot include/link paths baked into CC).
    # Submodule path supplies internal headers (base/dns_resolv.h, cpputil/ifconfig.h)
    # not shipped by the libhv public install.
    LIBHV_INC := -isystem $(LIBHV_DIR)/include -isystem $(LIBHV_DIR)/cpputil -isystem $(LIBHV_DIR)
    LIBHV_LIBS := -lhv
    LIBHV_LIB :=
else ifeq ($(LIBHV_PKG_CONFIG),yes)
    # System libhv found via pkg-config (pkg-config already returns system include paths)
    # Also add submodule path for internal headers used by tests
    LIBHV_INC := $(shell pkg-config --cflags libhv) -isystem $(LIBHV_DIR)
    LIBHV_LIBS := $(shell pkg-config --libs libhv)
    LIBHV_LIB :=
else
    # No system libhv - build from submodule to $(BUILD_DIR)/lib/ for architecture isolation
    # This allows concurrent native/pi/ad5m builds without conflicts
    # Use -isystem to suppress warnings from third-party headers in strict mode
    LIBHV_INC := -isystem $(LIBHV_DIR)/include -isystem $(LIBHV_DIR)/cpputil -isystem $(LIBHV_DIR)
    LIBHV_LIB := $(BUILD_DIR)/lib/libhv.a
    LIBHV_LIBS := $(LIBHV_LIB)
endif

# libhv generates include/hv headers during libhv-build. Track json.hpp so a
# stale archive cannot be reused when generated headers are missing.
ifneq ($(LIBHV_LIB),)
    LIBHV_JSON_HEADER := $(LIBHV_DIR)/include/hv/json.hpp
else
    LIBHV_JSON_HEADER :=
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
ifeq ($(YOCTO_BUILD),yes)
    # Yocto: libfmt from DEPENDS, sysroot-aware linker resolves -lfmt.
    FMT_LIBS := -lfmt
else ifneq ($(CROSS_COMPILE),)
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

# Flying Toasters screensaver (desktop/Pi only)
ifeq ($(ENABLE_SCREENSAVER),yes)
    SCREENSAVER_DEFINES := -DHELIX_ENABLE_SCREENSAVER
else
    SCREENSAVER_DEFINES :=
endif

# Mock backend defines
ifeq ($(ENABLE_MOCKS),yes)
    MOCK_DEFINES := -DHELIX_ENABLE_MOCKS
else
    MOCK_DEFINES :=
endif

# Remote-control subsystem define (see ENABLE_REMOTE_CONTROL above)
ifeq ($(ENABLE_REMOTE_CONTROL),yes)
    REMOTE_CONTROL_DEFINES := -DHELIX_ENABLE_REMOTE_CONTROL
else
    REMOTE_CONTROL_DEFINES :=
endif

# Developer-only showcase panels define (see ENABLE_DEV_PANELS above)
ifeq ($(ENABLE_DEV_PANELS),yes)
    DEV_PANELS_DEFINES := -DHELIX_ENABLE_DEV_PANELS
else
    DEV_PANELS_DEFINES :=
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
# Sanitizer builds get their own PCH for the same reason they get their own
# OBJ_DIR: a PCH compiled without -fsanitize cannot be reused by an
# instrumented compile, and sharing one silently poisons the whole tree.
# `:=` is fine — a command-line PCH=... still overrides it.
ifeq ($(SANITIZE),address)
PCH := $(BUILD_DIR)/asan-lvgl_pch.h.gch
else ifeq ($(SANITIZE),thread)
PCH := $(BUILD_DIR)/tsan-lvgl_pch.h.gch
else
PCH := $(BUILD_DIR)/lvgl_pch.h.gch
endif
PCH_FLAGS := -include $(PCH_HEADER)

# Include paths
# Project includes use -I (warnings enabled), library includes use -isystem (warnings suppressed)
# This allows `make strict` to catch issues in project code while ignoring third-party header warnings
# stb_image headers (used for thumbnail processing)
STB_INC := -isystem lib/stb
INCLUDES := -I. -I$(INC_DIR) -Isrc/generated -I$(BUILD_DIR)/generated -isystem lib -isystem lib/glm $(LVGL_INC) $(LIBHV_INC) $(SPDLOG_INC) $(STB_INC) $(LV_MARKDOWN_INC) $(QUIRC_INC) $(WPA_INC) $(SDL2_INC)

# The folded helixctl client (src/remote/remote_client.cpp) includes linenoise.h.
ifeq ($(ENABLE_REMOTE_CONTROL),yes)
    INCLUDES += -I lib/linenoise
endif

# Common linker flags (used by both macOS and Linux)
LDFLAGS_COMMON := $(SDL2_LIBS) $(LIBHV_LIBS) $(FMT_LIBS) -lz -lm -lpthread

# Platform-specific configuration
# Cross-compilation targets (pi, ad5m, k1) are Linux-based embedded systems
ifeq ($(YOCTO_BUILD),yes)
    # Yocto/bitbake: CC already carries sysroot + mcpu/mfpu flags. All dependencies
    # come from DEPENDS in the .bb recipe (libhv, openssl, spdlog, fmt, alsa-lib,
    # libusb1, wpa-supplicant, libnl). Do not build any submodule library here —
    # link against system libraries resolved by the sysroot-aware toolchain.
    NPROC := $(shell nproc 2>/dev/null || echo 4)
    LIBNL_LIBS := -lnl-genl-3 -lnl-3
    # Preserve bitbake's env LDFLAGS (--hash-style=gnu, -Wl,-z,relro, etc.) —
    # do not clobber. Prepending $(LDFLAGS) lets Yocto's security/packaging
    # flags flow through the final link.
    LDFLAGS := $(LDFLAGS) $(LIBHV_LIBS) $(FMT_LIBS) -lwpa_client $(LIBNL_LIBS) -ldl -lz -lm -lpthread
    ifeq ($(ENABLE_SSL),yes)
        LDFLAGS += -lssl -lcrypto
    endif
    LDFLAGS += $(TARGET_LDFLAGS)
    PLATFORM := Linux-yocto
    # No submodule wpa_client to depend on — wpa-supplicant recipe installs libwpa_client.
    WPA_DEPS :=
    WPA_CLIENT_LIB :=
else ifneq ($(CROSS_COMPILE)$(filter x86 x86-fbdev x86-both,$(PLATFORM_TARGET)),)
    # Platform builds (cross-compilation or x86 Docker native)
    NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    # libnl libraries: cross-compiled targets use static archives from submodule,
    # x86 native uses system libnl via pkg-config
    ifneq (,$(filter x86 x86-fbdev x86-both,$(PLATFORM_TARGET)))
        LIBNL_LIBS := -lnl-genl-3 -lnl-3
    else
        LIBNL_LIBS := $(BUILD_DIR)/lib/libnl-genl-3.a $(BUILD_DIR)/lib/libnl-3.a
    endif
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
        # libusb only available on Pi/x86 targets (ad5m/cc1 use standalone ARM toolchains without it)
        ifneq (,$(filter pi pi-fbdev pi-both pi32 pi32-fbdev pi32-both x86 x86-fbdev x86-both,$(PLATFORM_TARGET)))
            LDFLAGS += -lusb-1.0
            CXXFLAGS += -isystem /usr/include/libusb-1.0 -DHELIX_HAS_LIBUSB=1
        endif
    endif
    ifeq ($(ENABLE_SSL),yes)
        ifneq (,$(filter pi pi-fbdev pi-both pi32 pi32-fbdev pi32-both x86 x86-fbdev x86-both k1-dynamic,$(PLATFORM_TARGET)))
            # Static-link OpenSSL to avoid soname mismatch across OS versions
            # Pi: Bullseye has libssl.so.1.1, Bookworm has libssl.so.3
            # K1-dynamic: K1 firmware may not have OpenSSL shared libs
            LDFLAGS += -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic
        else
            LDFLAGS += -lssl -lcrypto
        endif
    endif
    # Add target-specific linker flags (e.g., -lstdc++fs for GCC 8)
    LDFLAGS += $(TARGET_LDFLAGS)
    # DRM backend requires libdrm for display; libinput for LVGL input drivers
    ifeq ($(DISPLAY_BACKEND),drm)
        LDFLAGS += -ldrm
        ifneq ($(SNAPMAKER_SKIP_LIBINPUT),yes)
            LDFLAGS += -linput
        endif
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
    # libusb detection via pkg-config (Homebrew paths aren't in default search path)
    LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
    LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0 2>/dev/null)
    ifneq ($(LIBUSB_LIBS),)
        CFLAGS += $(LIBUSB_CFLAGS)
        CXXFLAGS += $(LIBUSB_CFLAGS) -DHELIX_HAS_LIBUSB=1
    endif
    # -Wl,-w suppresses linker warnings about macOS version mismatches between
    # our 10.15 deployment target and libraries built for newer versions
    LDFLAGS := -Wl,-w $(LDFLAGS_COMMON) -framework Foundation -framework CoreFoundation -framework Security -framework CoreWLAN -framework CoreLocation -framework Cocoa -framework IOKit -framework CoreVideo -framework AudioToolbox -framework ForceFeedback -framework Carbon -framework CoreAudio -framework Metal -liconv $(LIBUSB_LIBS)
    PLATFORM := macOS
    WPA_DEPS :=
else
    # Linux native build - Include libwpa_client.a for WiFi control
    NPROC := $(shell nproc 2>/dev/null || echo 4)
    # Note: -lstdc++fs needed for std::experimental::filesystem on GCC < 9
    LDFLAGS := $(LDFLAGS_COMMON) $(WPA_CLIENT_LIB) $(SYSTEMD_LIBS) -lusb-1.0 -lssl -lcrypto -ldl -lstdc++fs
    CXXFLAGS += -isystem /usr/include/libusb-1.0 -DHELIX_HAS_LIBUSB=1
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

# Add screensaver defines to compiler flags
CFLAGS += $(SCREENSAVER_DEFINES)
CXXFLAGS += $(SCREENSAVER_DEFINES)

# Add mock defines to compiler flags
CFLAGS += $(MOCK_DEFINES)
CXXFLAGS += $(MOCK_DEFINES)

# Add remote-control defines to compiler flags
CFLAGS += $(REMOTE_CONTROL_DEFINES)
CXXFLAGS += $(REMOTE_CONTROL_DEFINES)

# Add developer-panel defines to compiler flags
CFLAGS += $(DEV_PANELS_DEFINES)
CXXFLAGS += $(DEV_PANELS_DEFINES)

# Add systemd defines to C++ compiler flags (for logging_init.cpp)
CXXFLAGS += $(SYSTEMD_CXXFLAGS)

# ALSA audio backend — real waveform synthesis on Linux targets with libasound2
# Enabled for: Pi (all variants), x86 SBCs
# Not available on: macOS (no ALSA), AD5M (no sound card), K1/K2/MIPS (musl, no ALSA)
ALSA_LIBS :=
ALSA_CXXFLAGS :=
ifneq (,$(filter pi pi-fbdev pi-both pi32 pi32-fbdev pi32-both x86 x86-fbdev x86-both,$(PLATFORM_TARGET)))
    ALSA_CXXFLAGS := -DHELIX_HAS_ALSA
    ALSA_LIBS := -lasound
else ifeq ($(PLATFORM_TARGET),native)
    ifeq ($(UNAME_S),Linux)
        ALSA_PKG := $(shell pkg-config --exists alsa 2>/dev/null && echo "yes")
        ifeq ($(ALSA_PKG),yes)
            ALSA_CXXFLAGS := -DHELIX_HAS_ALSA $(shell pkg-config --cflags alsa 2>/dev/null)
            ALSA_LIBS := $(shell pkg-config --libs alsa 2>/dev/null)
        endif
    endif
endif
CXXFLAGS += $(ALSA_CXXFLAGS)
LDFLAGS += $(ALSA_LIBS)

# Re-apply AddressSanitizer linker flags AFTER the per-platform LDFLAGS
# composition above (lines 580-694) — those use `LDFLAGS :=` which clobbers
# anything cross.mk's SANITIZE block injected. Must come last so platform
# library lists are present alongside ASAN runtime.
ifeq ($(SANITIZE),address)
    LDFLAGS += $(SANITIZE_FLAGS)
    ifneq ($(CROSS_COMPILE),)
        LDFLAGS += -static-libasan
    endif
endif

# Same re-application for TSAN, and for the same reason: the per-platform
# `LDFLAGS :=` above clobbers anything injected earlier.
ifeq ($(SANITIZE),thread)
    LDFLAGS += $(SANITIZE_FLAGS)
    ifneq ($(CROSS_COMPILE),)
        LDFLAGS += -static-libtsan
    endif
endif

# Sound system — synth, sequencer, backends (PWM/M300/SDL/ALSA), themes
# Tracker player — MOD/MED file playback with PCM samples (requires HELIX_HAS_SOUND)
#
# HELIX_HAS_SOUND:   Pi, x86, AD5M, native — any platform with audio output
# HELIX_HAS_TRACKER: Pi, x86, native — platforms with multi-core CPU + audio
# AD5M/AD5X: sound only (no tracker — single-core busy-wait kills prints)
# Disabled entirely: K1, K2, MIPS — no audio hardware at all
SOUND_CXXFLAGS :=
TRACKER_CXXFLAGS :=
ifneq (,$(filter pi pi-fbdev pi-both pi32 pi32-fbdev pi32-both x86 x86-fbdev x86-both,$(PLATFORM_TARGET)))
    SOUND_CXXFLAGS := -DHELIX_HAS_SOUND
    TRACKER_CXXFLAGS := -DHELIX_HAS_TRACKER
else ifneq (,$(filter ad5m ad5m-br ad5x,$(PLATFORM_TARGET)))
    # AD5M/AD5X: PWM buzzer for tone-mode SFX only.
    # Tracker (MOD/MED) DISABLED — the PCM render thread's busy-wait loop
    # starves the single-core CPU, killing active prints and blocking
    # Moonraker commands (including firmware_restart).
    SOUND_CXXFLAGS := -DHELIX_HAS_SOUND
else ifeq ($(PLATFORM_TARGET),native)
    SOUND_CXXFLAGS := -DHELIX_HAS_SOUND
    TRACKER_CXXFLAGS := -DHELIX_HAS_TRACKER
endif
# K1, K2, MIPS — no sound at all
CXXFLAGS += $(SOUND_CXXFLAGS) $(TRACKER_CXXFLAGS) $(PWM_SOUND_CXXFLAGS)

# Feature gates — default ON for all platforms.
# Disabled per-platform in mk/cross.mk for memory-constrained targets.
HELIX_HAS_LABEL_PRINTER ?= 1
HELIX_HAS_CFS ?= 1
HELIX_HAS_IFS ?= 1
# Compile-out gates for the 2D gcode renderer and the bed-mesh 3D renderer —
# code AND their big runtime buffers (ESP32-class targets set these to 0).
HELIX_HAS_GCODE_VIEWER ?= 1
HELIX_HAS_BED_MESH_3D ?= 1
# Compile-out gate for the dlopen()-based plugin system — no dynamic linking
# on statically-linked embedded targets (ESP32-class).
HELIX_HAS_PLUGINS ?= 1
# Compile-out gate for the timelapse VIEWING UI (video list/download/playback).
# Capture-control (settings, render, save-frames) is plain JSON-RPC and is NOT
# gated — printers keep capturing timelapses even where the screen can't view them.
HELIX_HAS_TIMELAPSE_VIEWER ?= 1
CXXFLAGS += -DHELIX_HAS_LABEL_PRINTER=$(HELIX_HAS_LABEL_PRINTER) \
            -DHELIX_HAS_CFS=$(HELIX_HAS_CFS) \
            -DHELIX_HAS_IFS=$(HELIX_HAS_IFS) \
            -DHELIX_HAS_GCODE_VIEWER=$(HELIX_HAS_GCODE_VIEWER) \
            -DHELIX_HAS_BED_MESH_3D=$(HELIX_HAS_BED_MESH_3D) \
            -DHELIX_HAS_PLUGINS=$(HELIX_HAS_PLUGINS) \
            -DHELIX_HAS_TIMELAPSE_VIEWER=$(HELIX_HAS_TIMELAPSE_VIEWER)

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
HELIX_TEST_FIXTURE_OBJ := $(OBJ_DIR)/tests/helix_test_fixture.o
TEST_FIXTURES_OBJ := $(OBJ_DIR)/tests/test_fixtures.o
LVGL_UI_TEST_FIXTURE_OBJ := $(OBJ_DIR)/tests/lvgl_ui_test_fixture.o

# Mock objects for integration testing
MOCK_SRCS := $(wildcard $(TEST_MOCK_DIR)/*.cpp)
MOCK_OBJS := $(patsubst $(TEST_MOCK_DIR)/%.cpp,$(OBJ_DIR)/tests/mocks/%.o,$(MOCK_SRCS))

# Default target
.DEFAULT_GOAL := all

.PHONY: all build clean run test tests test-integration test-cards test-print-select test-size-content demo compile_commands compile_commands_full libhv-build apply-patches generate-fonts validate-fonts regen-fonts regen-doc-links check-doc-links update-mdi-cache verify-mdi-codepoints help check-deps install-deps venv-setup icon format format-staged screenshots tools moonraker-inspector strict quality setup translations symbols strip dev install regen-filaments

# Fast development build: -O0 skips optimization passes (~2x faster compilation)
# Library code still builds at -O2 (via SUBMODULE_CFLAGS) since it rarely changes
dev:
	$(Q)$(MAKE) OPT=0 -j

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
	echo "  $${G}make dev$${X}          - Fast build (-O0, ~2x faster compilation)"; \
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
	echo "  $${G}regen-doc-links$${X}   - Relink the architecture guide's file citations"; \
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

# strip must wait for the aux binaries it strips: under -j, splash/watchdog can
# still be linking when strip runs. Depend on them (only when actually built).
strip: symbols $(if $(filter yes,$(STRIP_BINARY)),$(SPLASH_BIN) $(WATCHDOG_BIN))
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

# Generated contributors header — sourced from CONTRIBUTORS.txt (committed)
# so cross-compile Docker builds and shallow CI checkouts produce correct output.
CONTRIBUTORS_H := $(BUILD_DIR)/generated/contributors.h

$(CONTRIBUTORS_H): CONTRIBUTORS.txt scripts/gen-contributors.sh
	$(Q)BUILD_DIR=$(BUILD_DIR) ./scripts/gen-contributors.sh

# Generated git-hash header. The generator runs every build (there is no single
# file to depend on: HEAD, packed-refs and worktree .git indirection all move
# independently) but rewrites the header only when the hash actually changes, so
# an unchanged HEAD leaves helix_version.o alone.
GIT_HASH_H := $(BUILD_DIR)/generated/helix_git_hash.h

.PHONY: force-git-hash
force-git-hash:

$(GIT_HASH_H): force-git-hash
	$(Q)BUILD_DIR=$(BUILD_DIR) ./scripts/gen-git-hash.sh

# Named explicitly rather than left to the .d file: on a clean tree the header
# does not exist yet, and the generic pattern rule would compile before it is
# written. OBJ_DIR follows the sanitizer variants, so this covers those too.
$(OBJ_DIR)/system/helix_version.o: $(GIT_HASH_H)

# Refresh CONTRIBUTORS.txt from git history (respects .mailmap).
# Unions primary authors (%aN) with Co-authored-by trailer names so pair- and
# co-authored work is credited too. Run before release to pick up new
# contributors, then commit the result.
.PHONY: update-contributors
update-contributors:
	@{ \
		git -c safe.directory='*' log --format='%aN'; \
		git -c safe.directory='*' log --format='%(trailers:key=Co-authored-by,valueonly,unfold)' \
			| sed '/^$$/d' \
			| git -c safe.directory='*' check-mailmap --stdin 2>/dev/null \
			| sed -E 's/ *<[^>]*>//'; \
	} | sort -u \
		| grep -ivE 'bot\b|\[bot\]|dependabot|github-actions|claude' \
		| awk 'length >= 2' > CONTRIBUTORS.txt
	@echo "$(GREEN)✓ CONTRIBUTORS.txt updated ($$(wc -l < CONTRIBUTORS.txt) contributors)$(RESET)"
	@echo "  Review the diff and commit: git diff CONTRIBUTORS.txt"

# Include modular makefiles
include mk/deps.mk
include mk/patches.mk
include mk/translations.mk
include mk/tests.mk
include mk/images.mk
include mk/format.mk
include mk/tools.mk
include mk/display-lib.mk
include mk/bluetooth.mk
include mk/splash.mk
include mk/filaments.mk
include mk/watchdog.mk
ifdef PI_DUAL_LINK
include mk/pi-dual-link.mk
endif
include mk/rules.mk

# Debug helpers — print computed variables for bats tests.
.PHONY: print-ldflags print-target-ldflags print-strip print-target-cflags print-cxxflags
print-ldflags:
	@echo "$(LDFLAGS)"
print-target-ldflags:
	@echo "$(TARGET_LDFLAGS)"
print-strip:
	@echo "STRIP_BINARY=$(STRIP_BINARY)"
print-target-cflags:
	@echo "$(TARGET_CFLAGS)"
print-cxxflags:
	@echo "$(CXXFLAGS)"

# =============================================================================
# Install target — stages binary + assets under $(DESTDIR)/opt/helixscreen/
#
# Used by external buildroot/yocto/debian packaging (e.g. kmod's helixscreen.mk).
# DESTDIR is mandatory; no system-wide install supported (by design — HelixScreen
# runs as a dedicated embedded UI, not a general-purpose package).
#
# Layout:
#   $(DESTDIR)/opt/helixscreen/
#     bin/          helix-screen, helix-splash, helix-watchdog (if built)
#     ui_xml/       runtime XML layouts (components, panels, translations)
#     assets/
#       fonts/      (only tiers enabled for this platform)
#       images/     LVGL bitmaps, SVGs
#       sounds/     platform-compatible sounds
#       config/     default printer database, presets, platform hooks
#     certs/        ca-certificates.crt (for HTTPS, if bundled)
#
# Runtime writable state (config, cache, logs) is NOT installed. Init scripts
# create /data/helixscreen/{config,cache,log}/ on first boot.
# =============================================================================
.PHONY: install
install:
	@if [ -z "$(DESTDIR)" ]; then \
		echo "$(RED)error: DESTDIR is required (e.g. make install DESTDIR=/tmp/staging)$(RESET)" >&2; \
		exit 1; \
	fi
	@if [ -z "$(BUILD_SUBDIR)" ]; then \
		echo "$(RED)error: PLATFORM_TARGET must be set (e.g. PLATFORM_TARGET=ad5m-br)$(RESET)" >&2; \
		exit 1; \
	fi
	@if [ ! -x "$(BIN_DIR)/helix-screen" ]; then \
		echo "$(RED)error: $(BIN_DIR)/helix-screen not found — run '$(MAKE) PLATFORM_TARGET=$(PLATFORM_TARGET)' first$(RESET)" >&2; \
		exit 1; \
	fi
	@echo "$(BOLD)Installing to $(DESTDIR)/opt/helixscreen/$(RESET)"
	@install -d "$(DESTDIR)/opt/helixscreen/bin"
	@install -m 0755 "$(BIN_DIR)/helix-screen" "$(DESTDIR)/opt/helixscreen/bin/helix-screen"
	@if [ -x "$(BIN_DIR)/helix-splash" ]; then \
		install -m 0755 "$(BIN_DIR)/helix-splash" "$(DESTDIR)/opt/helixscreen/bin/helix-splash"; \
	fi
	@if [ -x "$(BIN_DIR)/helix-watchdog" ]; then \
		install -m 0755 "$(BIN_DIR)/helix-watchdog" "$(DESTDIR)/opt/helixscreen/bin/helix-watchdog"; \
	fi
	@echo "  → binaries"
	@# ui_xml: copy tree, then prune source-tree build scaffolding that isn't runtime data.
	@install -d "$(DESTDIR)/opt/helixscreen/ui_xml"
	@cp -a ui_xml/. "$(DESTDIR)/opt/helixscreen/ui_xml/"
	@find "$(DESTDIR)/opt/helixscreen/ui_xml" \
		\( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
		-type f -delete
	@echo "  → ui_xml/"
	@# assets: fonts, images, sounds, config, plus root-level runtime files
	@install -d "$(DESTDIR)/opt/helixscreen/assets"
	@if [ -d assets/fonts ]; then cp -a assets/fonts "$(DESTDIR)/opt/helixscreen/assets/"; fi
	@if [ -d assets/images ]; then cp -a assets/images "$(DESTDIR)/opt/helixscreen/assets/"; fi
	@if [ -d assets/sounds ]; then cp -a assets/sounds "$(DESTDIR)/opt/helixscreen/assets/"; fi
	@if [ -d assets/config ]; then cp -a assets/config "$(DESTDIR)/opt/helixscreen/assets/"; fi
	@# filaments.json: unified filament catalog, loaded on demand by FilamentCatalog
	@if [ -f assets/filaments.json ]; then cp -a assets/filaments.json "$(DESTDIR)/opt/helixscreen/assets/"; fi
	@echo "  → assets/"
	@# certs (optional — only present after `make ad5m-docker` fetched them)
	@if [ -f "$(BUILD_DIR)/certs/ca-certificates.crt" ]; then \
		install -d "$(DESTDIR)/opt/helixscreen/certs"; \
		install -m 0644 "$(BUILD_DIR)/certs/ca-certificates.crt" "$(DESTDIR)/opt/helixscreen/certs/"; \
		echo "  → certs/"; \
	fi
	@echo "$(GREEN)Install complete: $(DESTDIR)/opt/helixscreen/$(RESET)"
