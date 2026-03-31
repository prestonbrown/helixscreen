// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - Linux DRM/KMS Display Backend Implementation

#ifdef HELIX_DISPLAY_DRM

#include "display_backend_drm.h"

#include "config.h"
#include "drm_rotation_strategy.h"
#include "input_device_scanner.h"
#include "touch_calibration_wrapper.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

// System includes for device access checks and DRM capability detection
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace {

/**
 * @brief Check if a DRM device supports dumb buffers and has a connected display
 *
 * Pi 5 has multiple DRM cards:
 * - card0: v3d (3D only, no display output)
 * - card1: drm-rp1-dsi (DSI touchscreen)
 * - card2: vc4-drm (HDMI output)
 *
 * We need to find one that supports dumb buffers for framebuffer allocation.
 */
bool drm_device_supports_display(const std::string& device_path) {
    int fd = open(device_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    // Check for dumb buffer support
    uint64_t has_dumb = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
        close(fd);
        spdlog::debug("[DRM Backend] {}: no dumb buffer support", device_path);
        return false;
    }

    // Check if there's at least one connected connector
    drmModeRes* resources = drmModeGetResources(fd);
    if (!resources) {
        close(fd);
        spdlog::debug("[DRM Backend] {}: failed to get DRM resources", device_path);
        return false;
    }

    bool has_connected = false;
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector) {
            if (connector->connection == DRM_MODE_CONNECTED) {
                has_connected = true;
                spdlog::debug("[DRM Backend] {}: found connected connector type {}", device_path,
                              connector->connector_type);
            }
            drmModeFreeConnector(connector);
            if (has_connected)
                break;
        }
    }

    drmModeFreeResources(resources);
    close(fd);

    if (!has_connected) {
        spdlog::debug("[DRM Backend] {}: no connected displays", device_path);
    }

    return has_connected;
}

/**
 * @brief Check if a path points to a valid DRM device (exists and responds to DRM ioctls)
 */
bool is_valid_drm_device(const std::string& path) {
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    drmVersionPtr version = drmGetVersion(fd);
    close(fd);
    if (!version) {
        return false;
    }
    drmFreeVersion(version);
    return true;
}

/**
 * @brief Auto-detect the best DRM device
 *
 * Priority order for device selection:
 * 1. Environment variable HELIX_DRM_DEVICE (for debugging/testing)
 * 2. Config file /display/drm_device (user preference)
 * 3. Auto-detection: scan /dev/dri/card* for first with dumb buffers + connected display
 *
 * Pi 5 has multiple DRM cards: card0 (v3d, 3D only), card1 (DSI), card2 (vc4/HDMI)
 */
std::string auto_detect_drm_device() {
    // Priority 1: Environment variable override (for debugging/testing)
    const char* env_device = std::getenv("HELIX_DRM_DEVICE");
    if (env_device && env_device[0] != '\0') {
        if (is_valid_drm_device(env_device)) {
            spdlog::info("[DRM Backend] Using DRM device from HELIX_DRM_DEVICE: {}", env_device);
            return env_device;
        }
        spdlog::warn("[DRM Backend] HELIX_DRM_DEVICE='{}' is not a valid DRM device, "
                     "falling back to auto-detection",
                     env_device);
    }

    // Priority 2: Config file override
    helix::Config* cfg = helix::Config::get_instance();
    std::string config_device = cfg->get<std::string>("/display/drm_device", "");
    if (!config_device.empty()) {
        if (is_valid_drm_device(config_device)) {
            spdlog::info("[DRM Backend] Using DRM device from config: {}", config_device);
            return config_device;
        }
        spdlog::warn("[DRM Backend] Config drm_device '{}' is not a valid DRM device, "
                     "falling back to auto-detection",
                     config_device);
    }

    // Priority 3: Auto-detection
    spdlog::info("[DRM Backend] Auto-detecting DRM device...");

    // Scan /dev/dri/card* in order
    DIR* dir = opendir("/dev/dri");
    if (!dir) {
        spdlog::info("[DRM Backend] /dev/dri not found, DRM not available");
        return {};
    }

    std::vector<std::string> candidates;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "card", 4) == 0) {
            candidates.push_back(std::string("/dev/dri/") + entry->d_name);
        }
    }
    closedir(dir);

    // Sort to ensure consistent order (card0, card1, card2...)
    std::sort(candidates.begin(), candidates.end());

    for (const auto& candidate : candidates) {
        spdlog::debug("[DRM Backend] Checking DRM device: {}", candidate);
        if (drm_device_supports_display(candidate)) {
            spdlog::info("[DRM Backend] Auto-detected DRM device: {}", candidate);
            return candidate;
        }
    }

    spdlog::info("[DRM Backend] No suitable DRM device found");
    return {};
}

} // namespace

DisplayBackendDRM::DisplayBackendDRM() : drm_device_(auto_detect_drm_device()) {}

DisplayBackendDRM::DisplayBackendDRM(const std::string& drm_device) : drm_device_(drm_device) {}

DisplayBackendDRM::~DisplayBackendDRM() {
    restore_console();
}

bool DisplayBackendDRM::is_available() const {
    if (drm_device_.empty()) {
        spdlog::debug("[DRM Backend] No DRM device configured");
        return false;
    }

    struct stat st;

    // Check if DRM device exists
    if (stat(drm_device_.c_str(), &st) != 0) {
        spdlog::debug("[DRM Backend] DRM device {} not found", drm_device_);
        return false;
    }

    // Check if we can access it
    if (access(drm_device_.c_str(), R_OK | W_OK) != 0) {
        spdlog::debug(
            "[DRM Backend] DRM device {} not accessible (need R/W permissions, check video group)",
            drm_device_);
        return false;
    }

    return true;
}

DetectedResolution DisplayBackendDRM::detect_resolution() const {
    int fd = open(drm_device_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        spdlog::debug("[DRM Backend] Cannot open {} for resolution detection: {}", drm_device_,
                      strerror(errno));
        return {};
    }

    drmModeRes* resources = drmModeGetResources(fd);
    if (!resources) {
        spdlog::debug("[DRM Backend] Failed to get DRM resources for resolution detection");
        close(fd);
        return {};
    }

    DetectedResolution result;

    // Find first connected connector and get its preferred mode
    for (int i = 0; i < resources->count_connectors && !result.valid; i++) {
        drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (!connector) {
            continue;
        }

        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            // Find preferred mode, or use first mode as fallback
            drmModeModeInfo* preferred = nullptr;
            for (int m = 0; m < connector->count_modes; m++) {
                if (connector->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                    preferred = &connector->modes[m];
                    break;
                }
            }

            // Use preferred mode if found, otherwise first mode
            drmModeModeInfo* mode = preferred ? preferred : &connector->modes[0];
            result.width = static_cast<int>(mode->hdisplay);
            result.height = static_cast<int>(mode->vdisplay);
            result.valid = true;

            spdlog::info("[DRM Backend] Detected resolution: {}x{} ({})", result.width,
                         result.height, mode->name);
        }

        drmModeFreeConnector(connector);
    }

    drmModeFreeResources(resources);
    close(fd);

    if (!result.valid) {
        spdlog::debug("[DRM Backend] No connected display found for resolution detection");
    }

    return result;
}

lv_display_t* DisplayBackendDRM::create_display(int width, int height) {
    LV_UNUSED(width);
    LV_UNUSED(height);

    spdlog::info("[DRM Backend] Creating DRM display on {}", drm_device_);

    display_ = lv_linux_drm_create();
    if (display_ == nullptr) {
        spdlog::error("[DRM Backend] Failed to create DRM display");
        return nullptr;
    }

    lv_result_t result = lv_linux_drm_set_file(display_, drm_device_.c_str(), -1);
    if (result != LV_RESULT_OK) {
        spdlog::error("[DRM Backend] Failed to initialize DRM on {}", drm_device_);
        lv_display_delete(display_); // NOLINT(helix-shutdown) init error path, not shutdown
        display_ = nullptr;
        return nullptr;
    }

#ifdef HELIX_ENABLE_OPENGLES
    using_egl_ = true;
    spdlog::info("[DRM Backend] GPU-accelerated display active (EGL/OpenGL ES)");
#else
    spdlog::info("[DRM Backend] DRM display active (dumb buffers, CPU rendering)");
#endif

    suppress_console();

    screen_width_ = lv_display_get_horizontal_resolution(display_);
    screen_height_ = lv_display_get_vertical_resolution(display_);

    return display_;
}

lv_indev_t* DisplayBackendDRM::create_input_pointer() {
    std::string device_override;

    // Priority 1: Environment variable override (for debugging/testing)
    const char* env_device = std::getenv("HELIX_TOUCH_DEVICE");
    if (env_device && env_device[0] != '\0') {
        device_override = env_device;
        spdlog::info("[DRM Backend] Using touch device from HELIX_TOUCH_DEVICE: {}",
                     device_override);
    }

    // Priority 2: Config file override
    if (device_override.empty()) {
        helix::Config* cfg = helix::Config::get_instance();
        device_override = cfg->get<std::string>("/input/touch_device", "");
        if (!device_override.empty()) {
            spdlog::info("[DRM Backend] Using touch device from config: {}", device_override);
        }
    }

    // If we have an explicit device, try it first
    if (!device_override.empty()) {
        pointer_ = lv_libinput_create(LV_INDEV_TYPE_POINTER, device_override.c_str());
        if (pointer_ != nullptr) {
            spdlog::info("[DRM Backend] Libinput pointer device created on {}", device_override);
            return pointer_;
        }
        // Try evdev as fallback for the specified device
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, device_override.c_str());
        if (pointer_ != nullptr) {
            spdlog::info("[DRM Backend] Evdev pointer device created on {}", device_override);
            return pointer_;
        }
        spdlog::warn("[DRM Backend] Could not open specified touch device: {}", device_override);
    }

    // Priority 3: Auto-discover touch using libinput
    // Look for touch capability devices (touchscreens like DSI displays)
    spdlog::info("[DRM Backend] Auto-detecting touch/pointer device via libinput...");

    // Use evdev driver for touch devices — it supports multi-touch gesture
    // recognition (pinch-to-zoom) while the libinput driver does not.
    char* touch_path = lv_libinput_find_dev(LV_LIBINPUT_CAPABILITY_TOUCH, true);
    if (touch_path) {
        spdlog::info("[DRM Backend] Found touch device: {}", touch_path);
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
        if (pointer_ != nullptr) {
            spdlog::info("[DRM Backend] Evdev touch device created on {} (multi-touch enabled)",
                         touch_path);
#if LV_USE_GESTURE_RECOGNITION
            // Lower pinch thresholds so PINCH recognizes quickly, and disable
            // ROTATE by setting an unreachable threshold.  Without this, ROTATE
            // (default 0.2 rad) wins the race, resets PINCH's cumulative scale
            // to 1.0, and causes visible zoom jumps.
            lv_indev_set_pinch_up_threshold(pointer_, 1.15f);
            lv_indev_set_pinch_down_threshold(pointer_, 0.85f);
            lv_indev_set_rotation_rad_threshold(pointer_, 3.14f);
#endif
        } else {
            // Fall back to libinput if evdev fails
            pointer_ = lv_libinput_create(LV_INDEV_TYPE_POINTER, touch_path);
            if (pointer_ != nullptr) {
                spdlog::info("[DRM Backend] Libinput touch device created on {}", touch_path);
            } else {
                spdlog::warn("[DRM Backend] Failed to create input device for: {}", touch_path);
            }
        }
    }

    // --- Touch calibration detection ---
    if (pointer_ && touch_path) {
        // Parse event number from path like "/dev/input/event0"
        int event_num = -1;
        const char* event_pos = strstr(touch_path, "event");
        if (event_pos) {
            sscanf(event_pos, "event%d", &event_num);
        }

        if (event_num >= 0) {
            std::string dev_name = helix::input::get_input_device_name(event_num);
            std::string dev_phys = helix::input::get_input_device_phys(event_num);
            helix::AbsCapabilities abs_caps;
            bool has_abs = helix::input::get_input_touch_capabilities(event_num, &abs_caps);

            needs_calibration_ = helix::device_needs_calibration(dev_name, dev_phys, has_abs);

            // Check for ABS range / display resolution mismatch on capacitive screens
            if (!needs_calibration_ && has_abs && screen_width_ > 0 && screen_height_ > 0) {
                struct input_absinfo abs_x = {}, abs_y = {};
                int fd = open(touch_path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    bool got_x = (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0);
                    bool got_y = (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0);
                    if (!got_x || abs_x.maximum <= 0) {
                        got_x = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0);
                    }
                    if (!got_y || abs_y.maximum <= 0) {
                        got_y = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0);
                    }
                    close(fd);

                    if (got_x && got_y) {
                        spdlog::info(
                            "[DRM Backend] Touch ABS range: X({}..{}), Y({}..{}) — display: {}x{}",
                            abs_x.minimum, abs_x.maximum, abs_y.minimum, abs_y.maximum,
                            screen_width_, screen_height_);

                        if (abs_x.maximum <= 0 && abs_y.maximum <= 0) {
                            needs_calibration_ = true;
                            spdlog::warn("[DRM Backend] ABS range is zero — forcing calibration");
                        } else if (helix::has_abs_display_mismatch(abs_x.maximum, abs_y.maximum,
                                                                   screen_width_, screen_height_)) {
                            needs_calibration_ = true;
                            spdlog::warn("[DRM Backend] ABS range ({},{}) mismatches display "
                                         "({}x{}) — forcing calibration",
                                         abs_x.maximum, abs_y.maximum, screen_width_,
                                         screen_height_);
                        }
                    }
                }
            }

            spdlog::info("[DRM Backend] Touch device '{}' phys='{}' — calibration {}", dev_name,
                         dev_phys, needs_calibration_ ? "needed" : "not needed");

            // Load stored calibration and install wrapper
            calibration_ = helix::load_touch_calibration();
            helix::install_calibration_wrapper(pointer_, calibration_context_, calibration_,
                                               screen_width_, screen_height_);
        }
    }

    // Note: Don't free touch_path - it's managed internally by LVGL's libinput module
    // and will be freed when LVGL shuts down or rescans devices.

    // If no touch was found, try evdev fallback on common device paths
    if (!pointer_) {
        // Try pointer devices via libinput (mouse, trackpad)
        char* pointer_path = lv_libinput_find_dev(LV_LIBINPUT_CAPABILITY_POINTER, false);
        if (pointer_path) {
            spdlog::info("[DRM Backend] Found pointer device: {}", pointer_path);
            pointer_ = lv_libinput_create(LV_INDEV_TYPE_POINTER, pointer_path);
            if (pointer_ != nullptr) {
                spdlog::info("[DRM Backend] Libinput pointer device created on {}", pointer_path);
            } else {
                spdlog::warn("[DRM Backend] Failed to create libinput device for: {}",
                             pointer_path);
            }
            // Note: Don't free pointer_path - it's managed internally by LVGL's libinput module
        }
    }

    if (!pointer_) {
        // Fallback to evdev on common device paths
        spdlog::warn("[DRM Backend] Libinput auto-detection failed, trying evdev fallback");

        // Try event1 first (common for touchscreens on Pi)
        const char* fallback_devices[] = {"/dev/input/event1", "/dev/input/event0"};
        for (const char* dev : fallback_devices) {
            pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, dev);
            if (pointer_ != nullptr) {
                spdlog::info("[DRM Backend] Evdev pointer device created on {}", dev);
                break;
            }
        }
    }

    // --- Mouse detection (independent of touch) ---
    // A USB HID mouse can coexist with the touchscreen as a separate input device.
    std::string mouse_override;
    const char* env_mouse = std::getenv("HELIX_MOUSE_DEVICE");
    if (env_mouse && env_mouse[0] != '\0') {
        mouse_override = env_mouse;
        spdlog::info("[DRM Backend] Using mouse device from HELIX_MOUSE_DEVICE: {}",
                     mouse_override);
    }

    if (!mouse_override.empty()) {
        mouse_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, mouse_override.c_str());
        if (mouse_) {
            spdlog::info("[DRM Backend] Mouse created on {} (env override)", mouse_override);
        } else {
            spdlog::warn("[DRM Backend] Could not open specified mouse device: {}", mouse_override);
        }
    }

    if (!mouse_) {
        // Use sysfs evdev scanning — libinput's POINTER capability is too broad
        // (matches HDMI CEC devices like vc4-hdmi which report REL_X/REL_Y but
        // are not mice). The sysfs scanner checks REL_X+REL_Y+BTN_LEFT and
        // excludes touchscreens (ABS_X+ABS_Y).
        auto mouse_dev = helix::input::find_mouse_device();
        if (mouse_dev) {
            mouse_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, mouse_dev->path.c_str());
            if (mouse_) {
                spdlog::info("[DRM Backend] Mouse created on {} via evdev ({})", mouse_dev->path,
                             mouse_dev->name);
            } else {
                spdlog::warn("[DRM Backend] Failed to create evdev mouse on {}", mouse_dev->path);
            }
        }
    }

    // Set up a cursor for the mouse so it is visible on screen
    if (mouse_) {
        lv_obj_t* cursor_obj = lv_obj_create(lv_screen_active());
        lv_obj_set_size(cursor_obj, 12, 12);
        lv_obj_set_style_radius(cursor_obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cursor_obj, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cursor_obj, LV_OPA_80, 0);
        lv_obj_set_style_border_width(cursor_obj, 1, 0);
        lv_obj_set_style_border_color(cursor_obj, lv_color_black(), 0);
        lv_obj_clear_flag(cursor_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_indev_set_cursor(mouse_, cursor_obj);
        spdlog::info("[DRM Backend] Mouse cursor enabled");
    }

    if (!pointer_ && !mouse_) {
        spdlog::error("[DRM Backend] Failed to create any input device");
    }

    return pointer_;
}

lv_indev_t* DisplayBackendDRM::create_input_keyboard() {
    // Priority 1: Environment variable override
    const char* env_device = std::getenv("HELIX_KEYBOARD_DEVICE");
    if (env_device && env_device[0] != '\0') {
        keyboard_ = lv_libinput_create(LV_INDEV_TYPE_KEYPAD, env_device);
        if (keyboard_) {
            spdlog::info("[DRM Backend] Keyboard created on {} (env override)", env_device);
            return keyboard_;
        }
        spdlog::warn("[DRM Backend] Could not open specified keyboard device: {}", env_device);
    }

    // Priority 2: Auto-discover via libinput
    char* kb_path = lv_libinput_find_dev(LV_LIBINPUT_CAPABILITY_KEYBOARD, true);
    if (kb_path) {
        keyboard_ = lv_libinput_create(LV_INDEV_TYPE_KEYPAD, kb_path);
        if (keyboard_) {
            spdlog::info("[DRM Backend] Keyboard created on {}", kb_path);
            // Note: Don't free kb_path - it's managed internally by LVGL's libinput module
            return keyboard_;
        }
        spdlog::warn("[DRM Backend] Failed to create keyboard device on {}", kb_path);
        // Note: Don't free kb_path - it's managed internally by LVGL's libinput module
    }

    // Priority 3: Fall back to sysfs evdev scanning
    auto kb_dev = helix::input::find_keyboard_device();
    if (kb_dev) {
        keyboard_ = lv_evdev_create(LV_INDEV_TYPE_KEYPAD, kb_dev->path.c_str());
        if (keyboard_) {
            spdlog::info("[DRM Backend] Keyboard created on {} via evdev ({})", kb_dev->path,
                         kb_dev->name);
            return keyboard_;
        }
        spdlog::warn("[DRM Backend] Failed to create evdev keyboard on {}", kb_dev->path);
    }

    spdlog::debug("[DRM Backend] No keyboard device found");
    return nullptr;
}

void DisplayBackendDRM::set_display_rotation(lv_display_rotation_t rot, int phys_w, int phys_h) {
    (void)phys_w;
    (void)phys_h;

    if (display_ == nullptr) {
        spdlog::warn("[DRM Backend] Cannot set rotation — display not created");
        return;
    }

    // Map LVGL rotation enum to DRM plane rotation constants
    uint64_t drm_rot = DRM_MODE_ROTATE_0;
    switch (rot) {
    case LV_DISPLAY_ROTATION_0:
        drm_rot = DRM_MODE_ROTATE_0;
        break;
    case LV_DISPLAY_ROTATION_90:
        drm_rot = DRM_MODE_ROTATE_90;
        break;
    case LV_DISPLAY_ROTATION_180:
        drm_rot = DRM_MODE_ROTATE_180;
        break;
    case LV_DISPLAY_ROTATION_270:
        drm_rot = DRM_MODE_ROTATE_270;
        break;
    }

    // Query hardware capabilities and choose strategy.
    // On EGL builds, lv_linux_drm_get_plane_rotation_mask() and
    // lv_linux_drm_set_rotation() do not exist (only in the dumb-buffer
    // driver), so force SOFTWARE fallback.
#ifdef HELIX_ENABLE_OPENGLES
    uint64_t supported_mask = 0;
#else
    uint64_t supported_mask = lv_linux_drm_get_plane_rotation_mask(display_);
#endif
    auto strategy = choose_drm_rotation_strategy(drm_rot, supported_mask);

    switch (strategy) {
    case DrmRotationStrategy::HARDWARE:
#ifndef HELIX_ENABLE_OPENGLES
        lv_linux_drm_set_rotation(display_, drm_rot);
        spdlog::info("[DRM Backend] Hardware plane rotation set to {}°",
                     static_cast<int>(rot) * 90);
#endif
        break;

    case DrmRotationStrategy::SOFTWARE:
        // CPU in-place 180° pixel reversal in drm_flush (lv_linux_drm.c patch).
        // The dumb-buffer flush callback checks lv_display_get_rotation() and
        // reverses the pixel array before the page flip. FULL render mode
        // ensures the entire buffer is redrawn each frame.
        lv_display_set_render_mode(display_, LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_rotation(display_, rot);

        spdlog::info("[DRM Backend] Software rotation set to {}° "
                     "(CPU in-place reversal, plane supports 0x{:X})",
                     static_cast<int>(rot) * 90, supported_mask);
        break;

    case DrmRotationStrategy::NONE:
        lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_0);
        lv_display_set_matrix_rotation(display_, false);
        spdlog::debug("[DRM Backend] No rotation needed");
        break;
    }
}

bool DisplayBackendDRM::supports_hardware_rotation(lv_display_rotation_t rot) const {
    if (rot == LV_DISPLAY_ROTATION_0) {
        return true;
    }

    if (display_ == nullptr) {
        return false;
    }

    uint64_t drm_rot = DRM_MODE_ROTATE_0;
    switch (rot) {
    case LV_DISPLAY_ROTATION_90:
        drm_rot = DRM_MODE_ROTATE_90;
        break;
    case LV_DISPLAY_ROTATION_180:
        drm_rot = DRM_MODE_ROTATE_180;
        break;
    case LV_DISPLAY_ROTATION_270:
        drm_rot = DRM_MODE_ROTATE_270;
        break;
    default:
        return true;
    }

#ifdef HELIX_ENABLE_OPENGLES
    // EGL rotation not yet supported: lv_display_set_rotation() triggers
    // layer_reshape_draw_buf which conflicts with the EGL-sized draw buffer.
    // Needs a GL-only rotation path that bypasses LVGL's buffer reshape.
    // For now, fall back to fbdev (works) or panel_orientation (kernel).
    return false;
#else
    uint64_t supported_mask =
        lv_linux_drm_get_plane_rotation_mask(const_cast<lv_display_t*>(display_));
    return choose_drm_rotation_strategy(drm_rot, supported_mask) == DrmRotationStrategy::HARDWARE;
#endif
}

bool DisplayBackendDRM::clear_framebuffer(uint32_t color) {
    // For DRM, we can try to clear via /dev/fb0 if it exists (legacy fbdev emulation)
    // Many DRM systems provide /dev/fb0 as a compatibility layer
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        spdlog::debug("[DRM Backend] Cannot open /dev/fb0 for clearing (DRM-only system)");
        return false;
    }

    // Get variable screen info
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        spdlog::warn("[DRM Backend] Cannot get vscreeninfo from /dev/fb0");
        close(fd);
        return false;
    }

    // Get fixed screen info
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        spdlog::warn("[DRM Backend] Cannot get fscreeninfo from /dev/fb0");
        close(fd);
        return false;
    }

    // Calculate framebuffer size
    size_t screensize = finfo.smem_len;

    // Map framebuffer to memory
    void* fbp = mmap(nullptr, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbp == MAP_FAILED) {
        spdlog::warn("[DRM Backend] Cannot mmap /dev/fb0 for clearing");
        close(fd);
        return false;
    }

    // Determine bytes per pixel from stride
    uint32_t bpp = 32;
    if (vinfo.xres > 0) {
        bpp = (finfo.line_length * 8) / vinfo.xres;
    }

    // Fill framebuffer with the specified color
    if (bpp == 32) {
        uint32_t* pixels = static_cast<uint32_t*>(fbp);
        size_t pixel_count = screensize / 4;
        for (size_t i = 0; i < pixel_count; i++) {
            pixels[i] = color;
        }
    } else if (bpp == 16) {
        uint16_t r = ((color >> 16) & 0xFF) >> 3;
        uint16_t g = ((color >> 8) & 0xFF) >> 2;
        uint16_t b = (color & 0xFF) >> 3;
        uint16_t rgb565 = (r << 11) | (g << 5) | b;

        uint16_t* pixels = static_cast<uint16_t*>(fbp);
        size_t pixel_count = screensize / 2;
        for (size_t i = 0; i < pixel_count; i++) {
            pixels[i] = rgb565;
        }
    } else {
        memset(fbp, 0, screensize);
    }

    spdlog::info("[DRM Backend] Cleared framebuffer via /dev/fb0 to 0x{:08X}", color);

    munmap(fbp, screensize);
    close(fd);
    return true;
}

/**
 * @brief Check if fbcon is actively bound to a framebuffer vtconsole.
 *
 * On kernel 6.x, sun4i-drm (and other DRM drivers) register DRM fbdev
 * emulation via drm_fbdev_dma_setup(), causing fbcon to paint the text
 * console over DRM/EGL output.  On older kernels (5.x) the DRM driver
 * doesn't register fbdev emulation, so fbcon isn't an issue and calling
 * KD_GRAPHICS actually blanks the display.
 *
 * We detect this by checking /sys/class/vtconsole/vtcon* — if a "frame
 * buffer" vtconsole exists and is bound, fbcon is active and we need to
 * suppress it.
 */
static bool is_fbcon_bound() {
    DIR* dir = opendir("/sys/class/vtconsole");
    if (!dir) {
        return false;
    }

    bool found = false;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "vtcon", 5) != 0) {
            continue;
        }

        // Check if this vtconsole is a framebuffer type
        std::string name_path = std::string("/sys/class/vtconsole/") + entry->d_name + "/name";
        std::string bind_path = std::string("/sys/class/vtconsole/") + entry->d_name + "/bind";

        // Read name — look for "frame buffer"
        int name_fd = open(name_path.c_str(), O_RDONLY);
        if (name_fd < 0) {
            continue;
        }
        char name_buf[128] = {};
        auto nr = read(name_fd, name_buf, sizeof(name_buf) - 1);
        close(name_fd);
        if (nr <= 0 || strstr(name_buf, "frame buffer") == nullptr) {
            continue;
        }

        // Check if it's bound (bind == "1\n" or "Y\n")
        int bind_fd = open(bind_path.c_str(), O_RDONLY);
        if (bind_fd < 0) {
            continue;
        }
        char bind_buf[8] = {};
        auto br = read(bind_fd, bind_buf, sizeof(bind_buf) - 1);
        close(bind_fd);
        if (br > 0 && (bind_buf[0] == '1' || bind_buf[0] == 'Y')) {
            found = true;
            spdlog::debug("[DRM Backend] fbcon bound on {}", entry->d_name);
            break;
        }
    }

    closedir(dir);
    return found;
}

void DisplayBackendDRM::suppress_console() {
    // Only suppress if fbcon is actively bound to a framebuffer vtconsole.
    // On kernel 5.x the old sun4i driver doesn't register DRM fbdev emulation,
    // so fbcon isn't painting over us.  Calling KD_GRAPHICS on those kernels
    // blanks the display entirely.  On kernel 6.x, DRM fbdev emulation causes
    // fbcon to bind and paint the text console over DRM/EGL output.
    if (!is_fbcon_bound()) {
        spdlog::info("[DRM Backend] fbcon not bound — skipping console suppression");
        return;
    }

    // Switch the VT to KD_GRAPHICS mode so the kernel stops rendering console
    // text on the framebuffer.  Standard approach used by X11, Weston, SDL2.
    //
    // Use O_WRONLY: under systemd with SupplementaryGroups=tty, the tty group
    // only has write permission (crw--w----). O_RDWR fails with EACCES.
    static const char* tty_paths[] = {"/dev/tty0", "/dev/tty1", "/dev/tty", nullptr};

    for (int i = 0; tty_paths[i] != nullptr; ++i) {
        tty_fd_ = open(tty_paths[i], O_WRONLY | O_CLOEXEC);
        if (tty_fd_ >= 0) {
            if (ioctl(tty_fd_, KDSETMODE, KD_GRAPHICS) == 0) {
                spdlog::info("[DRM Backend] Console suppressed via KDSETMODE KD_GRAPHICS on {}",
                             tty_paths[i]);
                return;
            }
            spdlog::debug("[DRM Backend] KDSETMODE failed on {}: {}", tty_paths[i],
                          strerror(errno));
            close(tty_fd_);
            tty_fd_ = -1;
        }
    }

    spdlog::warn("[DRM Backend] Could not suppress console — kernel messages may bleed through");
}

void DisplayBackendDRM::restore_console() {
    if (tty_fd_ >= 0) {
        if (ioctl(tty_fd_, KDSETMODE, KD_TEXT) != 0) {
            spdlog::warn("[DRM Backend] KDSETMODE KD_TEXT failed: {}", strerror(errno));
        }
        close(tty_fd_);
        tty_fd_ = -1;
        spdlog::debug("[DRM Backend] Console restored to KD_TEXT mode");
    }
}

bool DisplayBackendDRM::set_calibration(const helix::TouchCalibration& cal) {
    if (!helix::is_calibration_valid(cal)) {
        spdlog::warn("[DRM Backend] Invalid calibration rejected");
        return false;
    }

    calibration_ = cal;

    if (pointer_) {
        auto* ctx = static_cast<helix::CalibrationContext*>(lv_indev_get_user_data(pointer_));
        if (ctx) {
            ctx->calibration = cal;
            spdlog::info("[DRM Backend] Calibration updated: a={:.4f} b={:.4f} c={:.4f} d={:.4f} "
                         "e={:.4f} f={:.4f}",
                         cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);
        } else {
            // Wrapper not yet installed — install it now
            helix::install_calibration_wrapper(pointer_, calibration_context_, calibration_,
                                               screen_width_, screen_height_);
            spdlog::info("[DRM Backend] Calibration callback installed at runtime");
        }
    }

    return true;
}

void DisplayBackendDRM::disable_affine_calibration() {
    if (pointer_) {
        auto* ctx = static_cast<helix::CalibrationContext*>(lv_indev_get_user_data(pointer_));
        if (ctx) {
            ctx->calibration.valid = false;
            spdlog::debug("[DRM Backend] Affine calibration disabled for recalibration");
        }
    }
}

void DisplayBackendDRM::enable_affine_calibration() {
    if (pointer_) {
        auto* ctx = static_cast<helix::CalibrationContext*>(lv_indev_get_user_data(pointer_));
        if (ctx) {
            ctx->calibration = calibration_;
            spdlog::debug("[DRM Backend] Affine calibration re-enabled (valid={})",
                          calibration_.valid);
        }
    }
}

#endif // HELIX_DISPLAY_DRM
