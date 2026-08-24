# SPDX-License-Identifier: GPL-3.0-or-later
# Watchdog supervisor build rules
#
# Builds helix-watchdog, an ultra-stable crash recovery supervisor.
# This binary monitors helix-screen and displays a recovery dialog on crash.
#
# Only built for embedded Linux targets (not macOS/desktop).
# Pattern follows mk/splash.mk for minimal dependencies.

WATCHDOG_SRC := src/helix_watchdog.cpp
WATCHDOG_OBJ := $(BUILD_DIR)/watchdog/helix_watchdog.o
WATCHDOG_BIN := $(BUILD_DIR)/bin/helix-watchdog

# Watchdog needs LVGL, display library, project includes, spdlog for logging
# Use -isystem lib for "lvgl/lvgl.h" include pattern (same as main build's INCLUDES)
WATCHDOG_CXXFLAGS := $(CXXFLAGS) -I$(INC_DIR) -isystem lib $(LVGL_INC) $(SPDLOG_INC) $(LIBHV_INC) -DHELIX_WATCHDOG
# Note: LVGL is compiled as objects, not a library - link directly against LVGL_OBJS
# Include TARGET_LDFLAGS to inherit -static flag for AD5M (glibc 2.25 compatibility)
WATCHDOG_LDFLAGS := $(TARGET_LDFLAGS) -lm -lpthread
# Yocto: preserve bitbake's env LDFLAGS (--hash-style=gnu, relro, etc.) so
# do_package_qa [ldflags] passes on the watchdog binary.
ifeq ($(YOCTO_BUILD),yes)
    WATCHDOG_LDFLAGS := $(LDFLAGS) $(WATCHDOG_LDFLAGS)
endif
# GCC 7.5 (K1 dynamic) needs -lstdc++fs for <experimental/filesystem>
ifeq ($(PLATFORM_TARGET),k1-dynamic)
    WATCHDOG_LDFLAGS += -lstdc++fs
endif

# Strip binary for embedded targets (matches main binary behavior)
ifeq ($(STRIP_BINARY),yes)
    WATCHDOG_LDFLAGS += -s
endif

# AddressSanitizer linker flags must reach watchdog too — its .o files were
# compiled with -fsanitize=address (via CXXFLAGS) so the ASAN runtime needs
# to be linked in. Static so the binary is self-contained on the target.
ifeq ($(SANITIZE),address)
    WATCHDOG_LDFLAGS += $(SANITIZE_FLAGS)
    ifneq ($(CROSS_COMPILE),)
        WATCHDOG_LDFLAGS += -static-libasan
    endif
endif

# Add platform-specific libraries for display backend
ifneq ($(UNAME_S),Darwin)
    # Linux: may need DRM/input libraries
    ifeq ($(DISPLAY_BACKEND),drm)
        WATCHDOG_LDFLAGS += -ldrm -linput
        ifeq ($(ENABLE_OPENGLES),yes)
            WATCHDOG_LDFLAGS += -lEGL -lGLESv2 -lgbm -ldl
        endif
    endif
endif

# Add systemd library if available (for journal logging)
ifdef HELIX_HAS_SYSTEMD
    WATCHDOG_LDFLAGS += -lsystemd
endif

# Only build watchdog for embedded targets (DRM or fbdev backends)
# On macOS and SDL desktop builds, developers use terminal for debugging
WATCHDOG_BUILD :=
ifeq ($(DISPLAY_BACKEND),drm)
    WATCHDOG_BUILD := yes
endif
ifeq ($(DISPLAY_BACKEND),fbdev)
    WATCHDOG_BUILD := yes
endif

ifdef WATCHDOG_BUILD

# Compile watchdog source (with dependency tracking for header changes)
# Depends on LIBHV_LIB to ensure libhv headers are installed before compilation
$(BUILD_DIR)/watchdog/%.o: src/%.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Watchdog needs config.o (for reading settings.json), backlight_backend.o,
# logging_init.o (for spdlog journal/syslog detection), and notification stub.
# Note: config.o must be compiled separately with HELIX_WATCHDOG to skip runtime_config dependency.
#
# Display backends (via DISPLAY_LIB, linked with --whole-archive) reference
# drm_mode_matching.o, fbdev_size_helper.o, and pending_startup_warnings.o
# (added for #766 -s resolution support). These are small, dependency-free
# modules that can be linked into watchdog as extra objects.
WATCHDOG_EXTRA_OBJS := $(BUILD_DIR)/watchdog/config.o \
                       $(BUILD_DIR)/watchdog/config_backup.o \
                       $(BUILD_DIR)/watchdog/config_storage_file.o \
                       $(BUILD_DIR)/watchdog/backlight_backend.o \
                       $(BUILD_DIR)/watchdog/data_root_resolver.o \
                       $(BUILD_DIR)/watchdog/helix_paths.o \
                       $(BUILD_DIR)/watchdog/logging_init.o \
                       $(BUILD_DIR)/watchdog/platform_capabilities.o \
                       $(BUILD_DIR)/watchdog/ui_notification_stub.o \
                       $(BUILD_DIR)/watchdog/drm_mode_matching.o \
                       $(BUILD_DIR)/watchdog/fbdev_size_helper.o \
                       $(BUILD_DIR)/watchdog/pending_startup_warnings.o \
                       $(BUILD_DIR)/watchdog/log_redact.o \
                       $(BUILD_DIR)/watchdog/helix_lvgl_anomaly_stub.o

# Compile config for watchdog (with HELIX_WATCHDOG to guard get_runtime_config dependency)
$(BUILD_DIR)/watchdog/config.o: src/system/config.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile config_backup for watchdog (config.cpp references it)
$(BUILD_DIR)/watchdog/config_backup.o: src/system/config_backup.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile config_storage_file for watchdog (config.cpp's default ConfigStorage backend)
$(BUILD_DIR)/watchdog/config_storage_file.o: src/system/config_storage_file.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile backlight backend for watchdog (with HELIX_WATCHDOG to skip runtime_config dependency)
$(BUILD_DIR)/watchdog/backlight_backend.o: src/api/backlight_backend.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile data_root_resolver for watchdog (zero deps; provides writable_path,
# get_user_config_dir, etc. for HELIX_CONFIG_DIR-aware path resolution)
$(BUILD_DIR)/watchdog/data_root_resolver.o: src/application/data_root_resolver.cpp | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile helix_paths for watchdog (pure path primitives; data_root_resolver.cpp
# calls helix::paths::strip_trailing_slash).
$(BUILD_DIR)/watchdog/helix_paths.o: src/system/helix_paths.cpp | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile logging_init for watchdog
$(BUILD_DIR)/watchdog/logging_init.o: src/system/logging_init.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# logging_init sizes the debug ring from total RAM, so the watchdog links this too.
$(BUILD_DIR)/watchdog/platform_capabilities.o: src/system/platform_capabilities.cpp | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile notification stub for watchdog (with dependency tracking)
$(BUILD_DIR)/watchdog/ui_notification_stub.o: tools/ui_notification_stub.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog stub)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile log_redact for watchdog. Watchdog calls none of it; input_device_scanner.cpp
# does, and DISPLAY_LIB is linked --whole-archive (see the link rule below), so every
# object in that archive becomes a hard link dependency whether or not this binary
# reaches it. Without this the non-LTO targets (pi, pi32, x86) fail to link while the
# -flto ones drop the unreachable caller and link clean.
$(BUILD_DIR)/watchdog/log_redact.o: src/system/log_redact.cpp | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# No-op stub for helix_lvgl_anomaly() — patched LVGL references it but watchdog
# has no telemetry pipeline. Real impl lives in src/system/helix_lvgl_anomaly.cpp.
$(BUILD_DIR)/watchdog/helix_lvgl_anomaly_stub.o: tools/helix_lvgl_anomaly_stub.cpp | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog stub)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# DRM mode-matching helper (pure, referenced by display_backend_drm.cpp). Added for #766.
$(BUILD_DIR)/watchdog/drm_mode_matching.o: src/api/drm_mode_matching.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Fbdev kernel-size helper (pure, referenced by display_backend_fbdev.cpp). Added for #766.
$(BUILD_DIR)/watchdog/fbdev_size_helper.o: src/api/fbdev_size_helper.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# PendingStartupWarnings queue (referenced by both display backends). Added for #766.
$(BUILD_DIR)/watchdog/pending_startup_warnings.o: src/system/pending_startup_warnings.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/watchdog
	@echo "[CXX] $< (watchdog)"
	$(Q)$(CXX) $(WATCHDOG_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Link watchdog binary
# Dependencies: watchdog object, display library, LVGL objects (compiled from source), fonts
# Note: Use --whole-archive for DISPLAY_LIB to prevent LTO from stripping create_auto()
$(WATCHDOG_BIN): $(WATCHDOG_OBJ) $(DISPLAY_LIB) $(LVGL_OBJS) $(LVGL_OPENGLES_OBJS) $(THORVG_OBJS) $(FONT_OBJS) $(WATCHDOG_EXTRA_OBJS) | $(BUILD_DIR)/bin
	@echo "[LD] $@"
	$(Q)$(CXX) $(WATCHDOG_OBJ) -Wl,--whole-archive $(DISPLAY_LIB) -Wl,--no-whole-archive $(LVGL_OBJS) $(LVGL_OPENGLES_OBJS) $(THORVG_OBJS) $(FONT_OBJS) $(WATCHDOG_EXTRA_OBJS) -o $@ $(WATCHDOG_LDFLAGS)

# Create build directories
$(BUILD_DIR)/watchdog:
	$(Q)mkdir -p $@

# Build watchdog binary
.PHONY: watchdog
watchdog: $(WATCHDOG_BIN)
	@echo "Built: $(WATCHDOG_BIN)"

# Clean watchdog build artifacts
.PHONY: clean-watchdog
clean-watchdog:
	$(Q)rm -rf $(BUILD_DIR)/watchdog $(WATCHDOG_BIN)

# Include dependency files for header tracking
-include $(wildcard $(BUILD_DIR)/watchdog/*.d)

else
# No watchdog for SDL/desktop builds

.PHONY: watchdog clean-watchdog
watchdog:
	@echo "Watchdog binary not built (SDL/desktop build)"

clean-watchdog:
	@true

endif  # WATCHDOG_BUILD

# Help text
.PHONY: help-watchdog
help-watchdog:
	@echo "Watchdog supervisor targets:"
	@echo "  watchdog        - Build helix-watchdog binary (embedded only)"
	@echo "  clean-watchdog  - Remove watchdog build artifacts"
