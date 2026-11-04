# SPDX-License-Identifier: GPL-3.0-or-later
# Static library for display backend - shared by splash and main app
#
# This library contains the display backend abstraction layer:
# - DisplayBackend base class and factory
# - Platform-specific implementations (SDL, fbdev, DRM)
#
# Both helix-splash and helix-screen link against this library,
# ensuring consistent display detection and initialization.

DISPLAY_LIB := $(BUILD_DIR)/lib/libhelix-display.a

# Core display backend sources (always included)
# Split into API sources and other sources for proper object path handling
DISPLAY_API_SRCS := \
    src/api/display_backend.cpp

# Touch calibration and shared calibration wrapper
DISPLAY_UI_SRCS := \
    src/ui/calibration/touch_calibration.cpp

# Platform-specific backends
ifeq ($(UNAME_S),Darwin)
    # macOS: SDL only
    DISPLAY_API_SRCS += src/api/display_backend_sdl.cpp
else
    # Linux: framebuffer and DRM for embedded, SDL for desktop
    DISPLAY_API_SRCS += src/api/display_backend_fbdev.cpp
    DISPLAY_API_SRCS += src/api/display_backend_drm.cpp
    DISPLAY_API_SRCS += src/api/gpu_libs.cpp
    DISPLAY_API_SRCS += src/api/drm_rotation_strategy.cpp
    DISPLAY_API_SRCS += src/api/input_device_scanner.cpp
    DISPLAY_API_SRCS += src/api/touch_calibration_wrapper.cpp
    ifeq ($(ENABLE_SDL),yes)
        # Native Linux desktop also gets SDL
        DISPLAY_API_SRCS += src/api/display_backend_sdl.cpp
    endif
endif

# Generate object file paths for each source category
DISPLAY_API_OBJS := $(DISPLAY_API_SRCS:src/api/%.cpp=$(BUILD_DIR)/display/%.o)
DISPLAY_UI_OBJS := $(DISPLAY_UI_SRCS:src/ui/calibration/%.cpp=$(BUILD_DIR)/display/%.o)
DISPLAY_OBJS := $(DISPLAY_API_OBJS) $(DISPLAY_UI_OBJS)

# Display library needs LVGL headers, project includes, libhv (for config.h -> json.hpp), and SDL2
DISPLAY_CXXFLAGS := $(CXXFLAGS) -I$(INC_DIR) -isystem lib $(LVGL_INC) $(SPDLOG_INC) $(LIBHV_INC) $(SDL2_INC)

# Build object files from src/api/ (with dependency tracking)
# Depends on LIBHV_LIB to ensure libhv's configure runs first (creates include/hv/*.h)
$(BUILD_DIR)/display/%.o: src/api/%.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(ABI_STAMP) | $(BUILD_DIR)/display
	@echo "[CXX] $<"
	$(Q)$(CXX) $(DISPLAY_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Build object files from src/ui/calibration/ (with dependency tracking)
$(BUILD_DIR)/display/%.o: src/ui/calibration/%.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(ABI_STAMP) | $(BUILD_DIR)/display
	@echo "[CXX] $<"
	$(Q)$(CXX) $(DISPLAY_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Build static library
$(DISPLAY_LIB): $(DISPLAY_OBJS) | $(BUILD_DIR)/lib
	@echo "[AR] $@"
	$(Q)$(AR) rcs $@ $^

# Create directories
$(BUILD_DIR)/display $(BUILD_DIR)/lib:
	$(Q)mkdir -p $@

# Phony target for building just the display library
.PHONY: display-lib
display-lib: $(DISPLAY_LIB)

# Clean target
.PHONY: clean-display-lib
clean-display-lib:
	$(Q)rm -rf $(BUILD_DIR)/display $(DISPLAY_LIB)

# Include dependency files for header tracking
-include $(wildcard $(BUILD_DIR)/display/*.d)
