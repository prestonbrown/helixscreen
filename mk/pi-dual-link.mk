# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pi Dual-Link Build — compile once, link DRM + fbdev binaries
#
# Included only when PI_DUAL_LINK=yes (set by pi-both/pi32-both targets).
# All ~900 source files compile once with DRM superset defines. Then we link
# two binaries: DRM (with GPU libs) and fbdev (without). A handful of files
# need variant-specific compilation: display backends, crash_reporter, and
# GLES-dependent files (gcode_gles_renderer, ui_gcode_viewer).
#
# Build output:
#   build/pi/bin/helix-screen          — DRM binary
#   build/pi-fbdev/bin/helix-screen    — fbdev binary (linked from shared objects)

# =============================================================================
# Fbdev binary output path
# =============================================================================

# Determine the fbdev build directory based on the DRM build directory
# pi -> pi-fbdev, pi32 -> pi32-fbdev
FBDEV_BUILD_DIR := $(subst /pi/,/pi-fbdev/,$(subst /pi32/,/pi32-fbdev/,$(BUILD_DIR)/))
# Handle the case where BUILD_DIR is just build/pi or build/pi32 (no trailing content)
ifeq ($(BUILD_SUBDIR),pi)
    FBDEV_BUILD_DIR := build/pi-fbdev
else ifeq ($(BUILD_SUBDIR),pi32)
    FBDEV_BUILD_DIR := build/pi32-fbdev
endif

FBDEV_TARGET := $(FBDEV_BUILD_DIR)/bin/helix-screen

# =============================================================================
# Fbdev display backend library
# =============================================================================
# Compile display backend sources with fbdev-only defines (no DRM/GLES).
# These go into a separate directory to avoid colliding with the DRM objects.

FBDEV_DISPLAY_DIR := $(BUILD_DIR)/display-fbdev
DISPLAY_LIB_FBDEV := $(BUILD_DIR)/lib/libhelix-display-fbdev.a

# Fbdev display flags: start from DISPLAY_CXXFLAGS but strip DRM/GLES defines
# and add fbdev-only defines
FBDEV_DISPLAY_CXXFLAGS := $(filter-out -DHELIX_DISPLAY_DRM -DHELIX_ENABLE_OPENGLES -DENABLE_GLES_3D,$(DISPLAY_CXXFLAGS))

# Fbdev display sources — same as DRM but we exclude display_backend_drm.cpp
FBDEV_DISPLAY_API_SRCS := \
    src/api/display_backend.cpp \
    src/api/display_backend_fbdev.cpp

FBDEV_DISPLAY_UI_SRCS := \
    src/ui/touch_calibration.cpp

FBDEV_DISPLAY_API_OBJS := $(FBDEV_DISPLAY_API_SRCS:src/api/%.cpp=$(FBDEV_DISPLAY_DIR)/%.o)
FBDEV_DISPLAY_UI_OBJS := $(FBDEV_DISPLAY_UI_SRCS:src/ui/%.cpp=$(FBDEV_DISPLAY_DIR)/%.o)
FBDEV_DISPLAY_OBJS := $(FBDEV_DISPLAY_API_OBJS) $(FBDEV_DISPLAY_UI_OBJS)

# Build fbdev display objects
$(FBDEV_DISPLAY_DIR)/%.o: src/api/%.cpp $(LIBHV_LIB) | $(FBDEV_DISPLAY_DIR)
	@echo "[CXX/fbdev] $<"
	$(Q)$(CXX) $(FBDEV_DISPLAY_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(FBDEV_DISPLAY_DIR)/%.o: src/ui/%.cpp $(LIBHV_LIB) | $(FBDEV_DISPLAY_DIR)
	@echo "[CXX/fbdev] $<"
	$(Q)$(CXX) $(FBDEV_DISPLAY_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(DISPLAY_LIB_FBDEV): $(FBDEV_DISPLAY_OBJS) | $(BUILD_DIR)/lib
	@echo "[AR] $@"
	$(Q)$(AR) rcs $@ $^

$(FBDEV_DISPLAY_DIR):
	$(Q)mkdir -p $@

# =============================================================================
# Fbdev crash reporter (different HELIX_BINARY_VARIANT)
# =============================================================================

FBDEV_VARIANT_DIR := $(BUILD_DIR)/fbdev-variant
FBDEV_CRASH_OBJ := $(FBDEV_VARIANT_DIR)/crash_reporter.o

# The DRM crash_reporter.o is at $(OBJ_DIR)/system/crash_reporter.o
DRM_CRASH_OBJ := $(OBJ_DIR)/system/crash_reporter.o

# Compile crash_reporter.cpp with fbdev variant define
# Strip the DRM variant define and add fbdev
FBDEV_VARIANT_CXXFLAGS := $(subst -DHELIX_BINARY_VARIANT=\"drm\",-DHELIX_BINARY_VARIANT=\"fbdev\",$(CXXFLAGS))

$(FBDEV_CRASH_OBJ): src/system/crash_reporter.cpp $(LIBHV_LIB) $(PCH) | $(FBDEV_VARIANT_DIR)
	@echo "[CXX/fbdev] $< (variant=fbdev)"
	$(Q)$(CXX) $(FBDEV_VARIANT_CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

$(FBDEV_VARIANT_DIR):
	$(Q)mkdir -p $@

# =============================================================================
# Fbdev GLES-variant objects (recompiled without ENABLE_GLES_3D)
# =============================================================================
# Files that use #ifdef ENABLE_GLES_3D to switch between GLES and CPU code paths.
# Must be recompiled for fbdev without that define so the CPU fallback is used,
# avoiding dynamic library dependencies on libGLESv2/libEGL/libgbm.

FBDEV_GLES_VARIANT_DIR := $(BUILD_DIR)/fbdev-gles-variant

# Flags: same as main build but strip GLES/DRM defines
FBDEV_GLES_CXXFLAGS := $(filter-out -DENABLE_GLES_3D -DHELIX_DISPLAY_DRM -DHELIX_ENABLE_OPENGLES,$(CXXFLAGS))

FBDEV_GLES_VARIANT_SRCS := \
    src/rendering/gcode_gles_renderer.cpp \
    src/ui/ui_gcode_viewer.cpp

# Map source paths to fbdev variant object paths (flatten into single dir)
FBDEV_GLES_VARIANT_OBJS := \
    $(FBDEV_GLES_VARIANT_DIR)/gcode_gles_renderer.o \
    $(FBDEV_GLES_VARIANT_DIR)/ui_gcode_viewer.o

# DRM-compiled originals to exclude from fbdev link
DRM_GLES_APP_OBJS := \
    $(OBJ_DIR)/rendering/gcode_gles_renderer.o \
    $(OBJ_DIR)/ui/ui_gcode_viewer.o

$(FBDEV_GLES_VARIANT_DIR)/gcode_gles_renderer.o: src/rendering/gcode_gles_renderer.cpp $(LIBHV_LIB) $(PCH) | $(FBDEV_GLES_VARIANT_DIR)
	@echo "[CXX/fbdev] $< (no GLES)"
	$(Q)$(CXX) $(FBDEV_GLES_CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

$(FBDEV_GLES_VARIANT_DIR)/ui_gcode_viewer.o: src/ui/ui_gcode_viewer.cpp $(LIBHV_LIB) $(PCH) | $(FBDEV_GLES_VARIANT_DIR)
	@echo "[CXX/fbdev] $< (no GLES)"
	$(Q)$(CXX) $(FBDEV_GLES_CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

$(FBDEV_GLES_VARIANT_DIR):
	$(Q)mkdir -p $@

# =============================================================================
# LVGL DRM-specific objects (excluded from fbdev link)
# =============================================================================
# These LVGL sources reference DRM/libinput symbols and must not be linked
# into the fbdev binary.

LVGL_DRM_DRIVER_OBJS := \
    $(OBJ_DIR)/lvgl/src/drivers/display/drm/lv_linux_drm.o \
    $(OBJ_DIR)/lvgl/src/drivers/display/drm/lv_linux_drm_egl.o \
    $(OBJ_DIR)/lvgl/src/drivers/display/drm/lv_linux_drm_common.o \
    $(OBJ_DIR)/lvgl/src/drivers/libinput/lv_libinput.o

# =============================================================================
# Fbdev app objects — swap display backend, crash reporter, and GLES variants
# =============================================================================
# Start with all the objects from the DRM link, then:
# 1. Remove DRM display backend objects (replaced by DISPLAY_LIB_FBDEV)
# 2. Remove DRM crash_reporter.o (replaced by FBDEV_CRASH_OBJ)
# 3. Remove DRM-compiled GLES objects (replaced by FBDEV_GLES_VARIANT_OBJS)
# 4. Remove LVGL DRM driver objects

# DRM display backend objects that are part of APP_OBJS
DRM_DISPLAY_APP_OBJS := \
    $(OBJ_DIR)/api/display_backend.o \
    $(OBJ_DIR)/api/display_backend_drm.o \
    $(OBJ_DIR)/api/display_backend_fbdev.o \
    $(OBJ_DIR)/ui/touch_calibration.o

# All common app objects (swap out DRM-specific and GLES-specific objects)
FBDEV_APP_OBJS := $(filter-out $(DRM_DISPLAY_APP_OBJS) $(DRM_CRASH_OBJ) $(DRM_GLES_APP_OBJS),$(APP_OBJS))
FBDEV_LVGL_OBJS := $(filter-out $(LVGL_DRM_DRIVER_OBJS),$(LVGL_OBJS))

# =============================================================================
# Fbdev linker flags (no DRM/GLES libraries)
# =============================================================================

FBDEV_LDFLAGS := $(filter-out -ldrm -linput -lEGL -lGLESv2 -lgbm,$(LDFLAGS))

# =============================================================================
# Fbdev link target
# =============================================================================

$(FBDEV_TARGET): $(APP_C_OBJS) $(FBDEV_APP_OBJS) $(FBDEV_GLES_VARIANT_OBJS) $(FBDEV_CRASH_OBJ) \
                 $(FBDEV_LVGL_OBJS) $(HELIX_XML_OBJS) $(THORVG_OBJS) $(LV_MARKDOWN_OBJS) \
                 $(FONT_OBJS) $(TRANS_OBJS) $(DISPLAY_LIB_FBDEV) $(WPA_DEPS)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD/fbdev]$(RESET) $@"
	$(Q)$(CXX) $(CXXFLAGS) \
		$(filter-out %.a,$^) \
		-Wl,--whole-archive $(DISPLAY_LIB_FBDEV) -Wl,--no-whole-archive \
		-o $@ $(FBDEV_LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Fbdev linking failed!$(RESET)"; \
		exit 1; \
	}

# =============================================================================
# Symbol extraction and stripping for both binaries
# =============================================================================

.PHONY: symbols-both strip-both verify-fbdev

symbols-both: $(TARGET) $(FBDEV_TARGET)
ifeq ($(STRIP_BINARY),yes)
	@echo "Extracting symbols for DRM binary..."
	$(NM_CMD) -nC $(TARGET) > $(TARGET).sym
	@echo "Symbol map: $(TARGET).sym"
	$(OBJCOPY_CMD) --only-keep-debug $(TARGET) $(TARGET).debug
	@echo "Debug info: $(TARGET).debug"
	@echo "Extracting symbols for fbdev binary..."
	@mkdir -p $(FBDEV_BUILD_DIR)/bin
	$(NM_CMD) -nC $(FBDEV_TARGET) > $(FBDEV_TARGET).sym
	@echo "Symbol map: $(FBDEV_TARGET).sym"
	$(OBJCOPY_CMD) --only-keep-debug $(FBDEV_TARGET) $(FBDEV_TARGET).debug
	@echo "Debug info: $(FBDEV_TARGET).debug"
else
	@echo "STRIP_BINARY not set — skipping symbol extraction"
endif

strip-both: symbols-both
ifeq ($(STRIP_BINARY),yes)
	$(STRIP_CMD) $(TARGET)
	$(STRIP_CMD) $(FBDEV_TARGET)
	$(STRIP_CMD) $(SPLASH_BIN)
	$(STRIP_CMD) $(WATCHDOG_BIN)
	@echo "Stripped: $(TARGET) $(FBDEV_TARGET) $(SPLASH_BIN) $(WATCHDOG_BIN)"
else
	@echo "STRIP_BINARY not set — skipping strip"
endif

# =============================================================================
# Fbdev binary verification — catch accidental DRM/GLES symbol dependencies
# =============================================================================

verify-fbdev: $(FBDEV_TARGET)
	@echo "Verifying fbdev binary has no DRM/GLES dependencies..."
	@if $(NM_CMD) -u $(FBDEV_TARGET) 2>/dev/null | grep -qiE 'drm|gbm|egl|gles|libinput'; then \
		echo "$(RED)ERROR: fbdev binary has DRM/GLES undefined symbols:$(RESET)"; \
		$(NM_CMD) -u $(FBDEV_TARGET) | grep -iE 'drm|gbm|egl|gles|libinput'; \
		exit 1; \
	fi
	@echo "$(GREEN)✓ fbdev binary clean — no DRM/GLES symbols$(RESET)"

# Include dependency files for fbdev-specific compilations
-include $(wildcard $(FBDEV_DISPLAY_DIR)/*.d)
-include $(wildcard $(FBDEV_VARIANT_DIR)/*.d)
-include $(wildcard $(FBDEV_GLES_VARIANT_DIR)/*.d)
