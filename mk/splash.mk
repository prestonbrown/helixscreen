# SPDX-License-Identifier: GPL-3.0-or-later
# Splash screen binary build rules
#
# Builds helix-splash, a minimal splash screen for embedded targets.
# This binary starts instantly while the main app initializes in parallel.
#
# Only built for embedded Linux targets (not macOS/desktop).

SPLASH_SRC := src/helix_splash.cpp
SPLASH_OBJ := $(BUILD_DIR)/splash/helix_splash.o
SPLASH_BIN := $(BUILD_DIR)/bin/helix-splash

# Splash needs LVGL, display library, project includes, and libhv (for config.h -> json.hpp)
# lv_conf.h discovery (-I. + -DLV_CONF_INCLUDE_SIMPLE) rides along in $(LVGL_INC).
SPLASH_CXXFLAGS := $(CXXFLAGS) -I$(INC_DIR) $(LVGL_INC) $(SPDLOG_INC) $(LIBHV_INC) -DHELIX_SPLASH_ONLY
# Note: LVGL is compiled as objects, not a library - link directly against LVGL_OBJS
# Include TARGET_LDFLAGS to inherit -static flag for AD5M (glibc 2.25 compatibility)
SPLASH_LDFLAGS := $(TARGET_LDFLAGS) -lm -lpthread
# Yocto/bitbake supplies --hash-style=gnu, relro, etc. via env LDFLAGS —
# preserve them or do_package_qa [ldflags] fails on the splash binary.
ifeq ($(YOCTO_BUILD),yes)
    SPLASH_LDFLAGS := $(LDFLAGS) $(SPLASH_LDFLAGS)
endif

# Strip binary for embedded targets (matches main binary behavior)
ifeq ($(STRIP_BINARY),yes)
    SPLASH_LDFLAGS += -s
endif

# AddressSanitizer linker flags must reach splash too — see watchdog.mk for
# the same pattern and rationale.
ifeq ($(SANITIZE),address)
    SPLASH_LDFLAGS += $(SANITIZE_FLAGS)
    ifneq ($(CROSS_COMPILE),)
        SPLASH_LDFLAGS += -static-libasan
    endif
endif

# Add platform-specific libraries for display backend
ifneq ($(UNAME_S),Darwin)
    # Linux: may need DRM/input libraries
    ifeq ($(DISPLAY_BACKEND),drm)
        SPLASH_LDFLAGS += -ldrm -linput
        ifeq ($(ENABLE_OPENGLES),yes)
            SPLASH_LDFLAGS += -lEGL -lGLESv2 -lgbm -ldl
        endif
    endif
endif

# Only build splash for embedded targets (DRM or fbdev backends)
# On macOS and SDL desktop builds, the main app handles its own splash
# Determine if we should build splash based on display backend
SPLASH_BUILD :=
ifeq ($(DISPLAY_BACKEND),drm)
    SPLASH_BUILD := yes
endif
ifeq ($(DISPLAY_BACKEND),fbdev)
    SPLASH_BUILD := yes
endif

ifdef SPLASH_BUILD

# Compile splash source (with dependency tracking for header changes)
# Depends on LIBHV_LIB to ensure libhv headers are installed before compilation
$(BUILD_DIR)/splash/%.o: src/%.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Splash needs config.o (display_backend_drm.cpp uses Config), backlight_backend.o (for turning
# on backlight at startup), and a UI notification stub (config.cpp calls ui_notification_error
# on save failures). Note: both config.o and backlight_backend.o must be compiled separately with
# HELIX_SPLASH_ONLY to skip runtime_config dependency (which pulls in main app symbols).
#
# Display backends also reference drm_mode_matching.o, fbdev_size_helper.o, and
# pending_startup_warnings.o (added for #766 -s resolution support). These are small,
# dependency-free modules that can be linked into splash as extra objects.
SPLASH_EXTRA_OBJS := \
    $(BUILD_DIR)/splash/config.o \
    $(BUILD_DIR)/splash/config_backup.o \
    $(BUILD_DIR)/splash/config_storage_file.o \
    $(BUILD_DIR)/splash/backlight_backend.o \
    $(BUILD_DIR)/splash/data_root_resolver.o \
    $(BUILD_DIR)/splash/helix_paths.o \
    $(BUILD_DIR)/splash/ui_notification_stub.o \
    $(BUILD_DIR)/splash/drm_mode_matching.o \
    $(BUILD_DIR)/splash/fbdev_size_helper.o \
    $(BUILD_DIR)/splash/pending_startup_warnings.o \
    $(BUILD_DIR)/splash/log_redact.o \
    $(BUILD_DIR)/splash/helix_lvgl_anomaly_stub.o

# Compile config for splash (with HELIX_SPLASH_ONLY to guard get_runtime_config dependency)
$(BUILD_DIR)/splash/config.o: src/system/config.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile config_backup for splash (config.cpp references it)
$(BUILD_DIR)/splash/config_backup.o: src/system/config_backup.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile config_storage_file for splash (config.cpp's default ConfigStorage backend)
$(BUILD_DIR)/splash/config_storage_file.o: src/system/config_storage_file.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile data_root_resolver for splash (zero deps)
$(BUILD_DIR)/splash/data_root_resolver.o: src/application/data_root_resolver.cpp | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile helix_paths for splash (pure path primitives; data_root_resolver.cpp
# calls helix::paths::strip_trailing_slash).
$(BUILD_DIR)/splash/helix_paths.o: src/system/helix_paths.cpp | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# DRM mode-matching helper (pure, referenced by display_backend_drm.cpp). Added for #766.
$(BUILD_DIR)/splash/drm_mode_matching.o: src/api/drm_mode_matching.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Fbdev kernel-size helper (pure, referenced by display_backend_fbdev.cpp). Added for #766.
$(BUILD_DIR)/splash/fbdev_size_helper.o: src/api/fbdev_size_helper.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# PendingStartupWarnings queue (referenced by both display backends). Added for #766.
$(BUILD_DIR)/splash/pending_startup_warnings.o: src/system/pending_startup_warnings.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile backlight backend for splash (with HELIX_SPLASH_ONLY to skip runtime_config dependency)
$(BUILD_DIR)/splash/backlight_backend.o: src/api/backlight_backend.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile notification stub for splash (with dependency tracking)
$(BUILD_DIR)/splash/ui_notification_stub.o: tools/ui_notification_stub.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash stub)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile log_redact for splash. Splash calls none of it; input_device_scanner.cpp
# does, and DISPLAY_LIB is linked --whole-archive (see the link rule below), so
# every object in that archive becomes a hard link dependency whether or not this
# binary reaches it. Without this the non-LTO targets (pi, pi32, x86) fail to link
# while the -flto ones drop the unreachable caller and link clean.
$(BUILD_DIR)/splash/log_redact.o: src/system/log_redact.cpp | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# No-op stub for helix_lvgl_anomaly() — patched LVGL references it but splash
# has no telemetry pipeline. Real impl lives in src/system/helix_lvgl_anomaly.cpp.
$(BUILD_DIR)/splash/helix_lvgl_anomaly_stub.o: tools/helix_lvgl_anomaly_stub.cpp | $(BUILD_DIR)/splash
	@echo "[CXX] $< (splash stub)"
	$(Q)$(CXX) $(SPLASH_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Link splash binary
# Dependencies: splash object, display library, LVGL objects (compiled from source), fonts
# Note: Use --whole-archive for DISPLAY_LIB to prevent LTO from stripping create_auto()
# which is called internally through create() but not directly referenced.
$(SPLASH_BIN): $(SPLASH_OBJ) $(DISPLAY_LIB) $(LVGL_OBJS) $(LVGL_OPENGLES_OBJS) $(THORVG_OBJS) $(FONT_OBJS) $(SPLASH_EXTRA_OBJS) | $(BUILD_DIR)/bin
	@echo "[LD] $@"
	$(Q)$(CXX) $(SPLASH_OBJ) -Wl,--whole-archive $(DISPLAY_LIB) -Wl,--no-whole-archive $(LVGL_OBJS) $(LVGL_OPENGLES_OBJS) $(THORVG_OBJS) $(FONT_OBJS) $(SPLASH_EXTRA_OBJS) -o $@ $(SPLASH_LDFLAGS)

# Create build directories
$(BUILD_DIR)/splash $(BUILD_DIR)/bin:
	$(Q)mkdir -p $@

# Build splash binary
.PHONY: splash
splash: $(SPLASH_BIN)
	@echo "Built: $(SPLASH_BIN)"

# Clean splash build artifacts
.PHONY: clean-splash
clean-splash:
	$(Q)rm -rf $(BUILD_DIR)/splash $(SPLASH_BIN)

# Include dependency files for header tracking
-include $(wildcard $(BUILD_DIR)/splash/*.d)

else
# No splash for SDL/desktop builds

.PHONY: splash clean-splash
splash:
	@echo "Splash binary not built (SDL/desktop build)"

clean-splash:
	@true

endif  # SPLASH_BUILD

# Help text
.PHONY: help-splash
help-splash:
	@echo "Splash screen targets:"
	@echo "  splash        - Build helix-splash binary (embedded only)"
	@echo "  clean-splash  - Remove splash build artifacts"
