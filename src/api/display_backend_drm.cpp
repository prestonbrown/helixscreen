// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - Linux DRM/KMS Display Backend Implementation

#ifdef HELIX_DISPLAY_DRM

#include "display_backend_drm.h"

#include "../../include/drm_mode_matching.h"
#include "../../include/pending_startup_warnings.h"
#include "config.h"
#include "drm_rotation_strategy.h"
#include "helix_display_telemetry.h"
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

    // Check driver name. simpledrm is the kernel fallback that presents the
    // firmware/bootloader framebuffer as a DRM device. It advertises dumb
    // buffer support but the buffers cannot be mmap'd into userspace, so
    // LVGL's drm_allocate_dumb() fails on the mmap() step. Skip simpledrm
    // entirely and let the backend fall back to fbdev.
    // See prestonbrown/helixscreen#766.
    drmVersionPtr version = drmGetVersion(fd);
    if (version && version->name && std::string(version->name) == "simpledrm") {
        drmFreeVersion(version);
        close(fd);
        spdlog::warn("[DRM Backend] {}: driver is 'simpledrm' (firmware framebuffer). "
                     "simpledrm cannot allocate renderable buffers; HelixScreen "
                     "will use the fbdev backend instead.",
                     device_path);
        spdlog::warn("[DRM Backend] To enable full DRM acceleration on Raspberry Pi / "
                     "RatOS: add 'dtoverlay=vc4-kms-v3d' to /boot/firmware/config.txt "
                     "and reboot.");
        helix::PendingStartupWarnings::instance().enqueue(
            helix::PendingStartupWarnings::Severity::WARNING,
            "DRM driver is 'simpledrm' — using framebuffer fallback. "
            "Add 'dtoverlay=vc4-kms-v3d' to your boot config for full DRM.");
        return false;
    }
    if (version) {
        drmFreeVersion(version);
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
    // Tear down the calibration read callback before calibration_context_ (a value
    // member) is destroyed. Nothing deletes indevs until lv_deinit(), so the
    // pointer indev outlives this backend; if an indev read fired between our
    // destruction and lv_deinit(), calibrated_read_cb would reach the freed
    // calibration_context_ (use-after-free / SIGSEGV). Mirrors ~DisplayBackendFbdev.
    helix::uninstall_calibration_wrapper(pointer_, calibration_context_);
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
    spdlog::info("[DRM Backend] Creating DRM display on {}", drm_device_);

    // Enumerate the connected connector's modes so we can honor a
    // user-requested resolution via -s. Mirrors the pattern in
    // detect_resolution() above.
    std::vector<helix::DrmModeInfo> modes;
    int enum_fd = open(drm_device_.c_str(), O_RDWR | O_CLOEXEC);
    if (enum_fd >= 0) {
        drmModeRes* resources = drmModeGetResources(enum_fd);
        if (resources) {
            for (int i = 0; i < resources->count_connectors; i++) {
                drmModeConnector* connector =
                    drmModeGetConnector(enum_fd, resources->connectors[i]);
                if (!connector)
                    continue;
                if (connector->connection == DRM_MODE_CONNECTED) {
                    for (int m = 0; m < connector->count_modes; m++) {
                        const auto& mm = connector->modes[m];
                        modes.push_back({mm.hdisplay, mm.vdisplay, mm.vrefresh,
                                         (mm.type & DRM_MODE_TYPE_PREFERRED) != 0});
                    }
                    drmModeFreeConnector(connector);
                    break; // first connected connector only
                }
                drmModeFreeConnector(connector);
            }
            drmModeFreeResources(resources);
        }
        close(enum_fd);
    }

    // Auto-downscale: if the preferred EDID mode exceeds 1920px on either
    // axis, pick the highest sub-1920 mode available. This handles phone-class
    // panels (e.g. 5.5" QHD 1440x2560) that would otherwise render with
    // unreadably small text. Skip if the user explicitly requested a resolution
    // via -s. See prestonbrown/helixscreen#773, #774.
    static constexpr uint32_t HIGH_DPI_THRESHOLD = 1920;
    int downscale_idx = size_was_explicit_
                            ? helix::DrmModeMatch::NO_MATCH
                            : helix::find_best_downscale_mode(modes, HIGH_DPI_THRESHOLD);
    if (downscale_idx != helix::DrmModeMatch::NO_MATCH) {
        int pref_idx = helix::find_preferred_mode_index(modes);
        const auto& native = modes[pref_idx];
        const auto& target = modes[downscale_idx];
        spdlog::info("[DRM Backend] Native resolution {}x{} exceeds {}px threshold, "
                     "selecting {}x{}@{} mode",
                     native.hdisplay, native.vdisplay, HIGH_DPI_THRESHOLD, target.hdisplay,
                     target.vdisplay, target.vrefresh);
        width = static_cast<int>(target.hdisplay);
        height = static_cast<int>(target.vdisplay);
    }

    // Decide which mode to use based on the user's request.
    uint32_t chosen_w = 0, chosen_h = 0;
    bool mismatch_warned = false;
    if (width > 0 && height > 0) {
        int match_idx = helix::find_matching_mode(modes, static_cast<uint32_t>(width),
                                                  static_cast<uint32_t>(height));
        if (match_idx != helix::DrmModeMatch::NO_MATCH) {
            chosen_w = modes[match_idx].hdisplay;
            chosen_h = modes[match_idx].vdisplay;
            spdlog::info("[DRM Backend] Using requested mode {}x{}", chosen_w, chosen_h);
        } else if (size_was_explicit_ && !modes.empty()) {
            int pref = helix::find_preferred_mode_index(modes);
            if (pref != helix::DrmModeMatch::NO_MATCH) {
                chosen_w = modes[pref].hdisplay;
                chosen_h = modes[pref].vdisplay;
            }
            spdlog::warn("[DRM Backend] Requested resolution {}x{} not available "
                         "on the connected display. Available modes:",
                         width, height);
            for (const auto& m : modes) {
                spdlog::warn("[DRM Backend]   {}x{}@{}{}", m.hdisplay, m.vdisplay, m.vrefresh,
                             m.is_preferred ? " (preferred)" : "");
            }
            spdlog::warn("[DRM Backend] Falling back to preferred mode {}x{}.", chosen_w, chosen_h);

            char toast_msg[160];
            snprintf(toast_msg, sizeof(toast_msg),
                     "Configured resolution %dx%d is unavailable on this display; "
                     "using %ux%u instead.",
                     width, height, chosen_w, chosen_h);
            helix::PendingStartupWarnings::instance().enqueue(
                helix::PendingStartupWarnings::Severity::WARNING, toast_msg);
            mismatch_warned = true;
        }
    }

    display_ = lv_linux_drm_create();
    if (display_ == nullptr) {
        spdlog::error("[DRM Backend] Failed to create DRM display");
        return nullptr;
    }

    // Apply the preferred-mode override BEFORE lv_linux_drm_set_file picks a mode.
    if (chosen_w > 0 && chosen_h > 0) {
        lv_linux_drm_set_preferred_mode(display_, chosen_w, chosen_h);
    }

    lv_result_t result = lv_linux_drm_set_file(display_, drm_device_.c_str(), -1);
    if (result != LV_RESULT_OK) {
        spdlog::error("[DRM Backend] Failed to initialize DRM on {}", drm_device_);
        lv_display_delete(display_); // NOLINT(helix-shutdown) init error path, not shutdown
        display_ = nullptr;
        return nullptr;
    }

    // Belt and suspenders: after LVGL sets up, check the actual resolution
    // it landed on. If it differs from what the user asked for and we
    // haven't already warned, enqueue a warning now.
    if (size_was_explicit_ && width > 0 && height > 0 && !mismatch_warned) {
        int32_t actual_w = lv_display_get_horizontal_resolution(display_);
        int32_t actual_h = lv_display_get_vertical_resolution(display_);
        if (actual_w != width || actual_h != height) {
            spdlog::warn("[DRM Backend] LVGL ended up at {}x{} despite requested {}x{}", actual_w,
                         actual_h, width, height);
            char toast_msg[160];
            snprintf(toast_msg, sizeof(toast_msg),
                     "Configured resolution %dx%d is unavailable on this display; "
                     "using %dx%d instead.",
                     width, height, actual_w, actual_h);
            helix::PendingStartupWarnings::instance().enqueue(
                helix::PendingStartupWarnings::Severity::WARNING, toast_msg);
        }
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
#if LV_USE_LIBINPUT
        pointer_ = lv_libinput_create(LV_INDEV_TYPE_POINTER, device_override.c_str());
        if (pointer_ != nullptr) {
            spdlog::info("[DRM Backend] Libinput pointer device created on {}", device_override);
            return pointer_;
        }
#endif
        // Try evdev as fallback for the specified device
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, device_override.c_str());
        if (pointer_ != nullptr) {
            spdlog::info("[DRM Backend] Evdev pointer device created on {}", device_override);
            return pointer_;
        }
        spdlog::warn("[DRM Backend] Could not open specified touch device: {}", device_override);
    }

    // Priority 3: Auto-discover touch device
    spdlog::info("[DRM Backend] Auto-detecting touch/pointer device...");

#if LV_USE_LIBINPUT
    // Use libinput to discover touch devices, then prefer evdev driver for
    // multi-touch gesture recognition (pinch-to-zoom)
    char* touch_path = lv_libinput_find_dev(LV_LIBINPUT_CAPABILITY_TOUCH, true);
    if (touch_path) {
        spdlog::info("[DRM Backend] Found touch device: {}", touch_path);
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
        if (pointer_ != nullptr) {
            spdlog::info("[DRM Backend] Evdev touch device created on {} (multi-touch enabled)",
                         touch_path);
#if LV_USE_GESTURE_RECOGNITION
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
#else
    // No libinput — use evdev scanner to find touch/pointer device
    const char* touch_path = nullptr;
    auto mouse = helix::input::find_mouse_device("/dev/input", "/sys/class/input");
    std::string touch_path_str;
    if (mouse) {
        touch_path_str = mouse->path;
        touch_path = touch_path_str.c_str();
        spdlog::info("[DRM Backend] Found touch/pointer device via evdev scan: {}", touch_path_str);
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
        if (pointer_ != nullptr) {
            spdlog::info("[DRM Backend] Evdev touch device created on {}", touch_path_str);
#if LV_USE_GESTURE_RECOGNITION
            lv_indev_set_pinch_up_threshold(pointer_, 1.15f);
            lv_indev_set_pinch_down_threshold(pointer_, 0.85f);
            lv_indev_set_rotation_rad_threshold(pointer_, 3.14f);
#endif
        }
    }
#endif

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
            supports_calibration_ = helix::device_supports_calibration(dev_name, has_abs);

            // Query the touch controller's ABS range and propagate it into LVGL's
            // internal calibration. This runs unconditionally (not gated on
            // needs_calibration_) because MT-only digitizers report zero range for
            // the legacy ABS_X/ABS_Y axes — LVGL's evdev driver derives its scaling
            // ONLY from those, so without the MT-fallback range it does passthrough
            // and raw 0..max coords flow unscaled (touch lands only top-left). The
            // fbdev backend already does this (#943/#986); the DRM backend did not.
            struct input_absinfo abs_x = {}, abs_y = {};
            bool got_range = false;
            // #943 diagnostics: did we actually install the coarse down-scale, and
            // was calibration forced because the queried ABS range was unusable /
            // mismatched? Used below to emit a loud signal when the scale is skipped
            // on a panel that needs it.
            bool coarse_scale_installed = false;
            bool needs_cal_forced_by_abs = false;
            if (has_abs && screen_width_ > 0 && screen_height_ > 0) {
                int fd = open(touch_path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    bool got_x = (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0);
                    bool got_y = (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0);

                    // MT-only devices (Goodix gt9xxnew_ts, Qidi Q2) don't have legacy
                    // ABS_X/ABS_Y, or report them all-zero. Fall back to MT axes.
                    bool range_is_zero = got_x && got_y && abs_x.maximum == 0 && abs_y.maximum == 0;
                    bool used_mt_fallback = false;
                    if (!got_x || abs_x.maximum <= 0 || range_is_zero) {
                        used_mt_fallback = true;
                        got_x = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0);
                    }
                    if (!got_y || abs_y.maximum <= 0 || range_is_zero) {
                        used_mt_fallback = true;
                        got_y = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0);
                    }
                    close(fd);
                    got_range = got_x && got_y;

                    // When the fallback provided a valid range, push it into LVGL's
                    // internal calibration so coordinate mapping works even though
                    // EVIOCGABS(ABS_X) returned zeros.
                    if (used_mt_fallback && got_range && abs_x.maximum > abs_x.minimum) {
                        lv_evdev_set_calibration(pointer_, abs_x.minimum, abs_y.minimum,
                                                 abs_x.maximum, abs_y.maximum);
                        coarse_scale_installed = true;
                        spdlog::info("[DRM Backend] Applied MT axis range to LVGL calibration: "
                                     "X({}..{}) Y({}..{})",
                                     abs_x.minimum, abs_x.maximum, abs_y.minimum, abs_y.maximum);
                    }

                    if (got_range) {
                        spdlog::info(
                            "[DRM Backend] Touch ABS range: X({}..{}), Y({}..{}) — display: {}x{}",
                            abs_x.minimum, abs_x.maximum, abs_y.minimum, abs_y.maximum,
                            screen_width_, screen_height_);

                        if (!needs_calibration_ && abs_x.maximum <= 0 && abs_y.maximum <= 0) {
                            needs_calibration_ = true;
                            needs_cal_forced_by_abs = true;
                            spdlog::warn("[DRM Backend] ABS range is zero — forcing calibration");
                        } else if (!needs_calibration_ &&
                                   helix::has_abs_display_mismatch(abs_x.maximum, abs_y.maximum,
                                                                   screen_width_, screen_height_)) {
                            needs_calibration_ = true;
                            needs_cal_forced_by_abs = true;
                            spdlog::warn("[DRM Backend] ABS range ({},{}) mismatches display "
                                         "({}x{}) — forcing calibration",
                                         abs_x.maximum, abs_y.maximum, screen_width_,
                                         screen_height_);
                        }
                    }
                }
            }

            // #943 diagnostic: our explicit lv_evdev_set_calibration call was
            // skipped (only the MT-fallback and env-override paths make it),
            // but the panel is known to mismatch the digitizer. Coordinates
            // still flow coarse-SCALED, not raw: lv_evdev performs its own
            // EVIOCGABS query on open (legacy ABS_X/Y first, then MT fallback
            // — lv_evdev.c), and this warning can only fire when that same
            // query returned a usable range here. The 0.99.114 wording claimed
            // "UNSCALED / only top-left registers", which sent diagnosis down
            // the wrong path: bundle N4ZN3YY2 shows this device booting with
            // scaling active and captures compressed 0.57/0.49 — the real
            // hazard is a controller that over-reports its emitted range
            // (taps compress toward the top-left UNTIL calibrated; the
            // calibration span-check logs the captured/target ratio). Keep it
            // loud and grep-able either way.
            bool abs_mismatch =
                got_range && helix::has_abs_display_mismatch(abs_x.maximum, abs_y.maximum,
                                                             screen_width_, screen_height_);
            if (!coarse_scale_installed && (abs_mismatch || needs_cal_forced_by_abs)) {
                spdlog::warn(
                    "[DRM Backend] Coarse touch scale not explicitly installed "
                    "(lv_evdev_set_calibration skipped) but ABS range mismatches the display — "
                    "lv_evdev's own open-time EVIOCGABS applies the same scaling, so coordinates "
                    "flow coarse-scaled, not raw. If the controller over-reports its emitted "
                    "range (Qidi Q2, #943), taps compress toward the top-left until calibrated "
                    "(the calibration span-check logs the captured/target ratio). ABS X({}..{}) "
                    "Y({}..{}) vs display {}x{} [abs_mismatch={} needs_cal_forced={} got_range={}]",
                    abs_x.minimum, abs_x.maximum, abs_y.minimum, abs_y.maximum, screen_width_,
                    screen_height_, abs_mismatch, needs_cal_forced_by_abs, got_range);

                // Report a telemetry anomaly (rate-limited, best-effort). Stable slug
                // so occurrences can be aggregated across devices/boots. Routed
                // through the C bridge rather than TelemetryManager directly so
                // this object (which is force-linked into helix-splash /
                // helix-watchdog via --whole-archive) does not drag the telemetry
                // singleton into those pipeline-less binaries. See
                // helix_display_telemetry.h.
                helix_display_telemetry_error("display", "touch-coarse-scale-skipped",
                                              "drm_abs_mismatch_evdev_self_scaling");
            }

            // Touch axis / range overrides via environment (parity with the fbdev
            // backend, which has had these; the DRM backend did not — #943). These
            // override the kernel-reported EVIOCGABS range, which some controllers
            // over-report: the digitizer advertises a wider ABS range than it
            // actually emits, so the auto-installed coarse scale over-divides and
            // taps collapse into a fraction of the panel (Qidi Q2 class — the
            // "captured/target ratio < 1.0" symptom from the span-check log).
            // Setting the true emitted range here restores 1:1 mapping. To invert
            // an axis, swap min/max (e.g. MIN_Y=3200 MAX_Y=900).
            const char* swap_axes = std::getenv("HELIX_TOUCH_SWAP_AXES");
            if (swap_axes != nullptr && strcmp(swap_axes, "1") == 0) {
                spdlog::info("[DRM Backend] Touch axes swapped (HELIX_TOUCH_SWAP_AXES=1)");
                lv_evdev_set_swap_axes(pointer_, true);
            }

            const char* env_min_x = std::getenv("HELIX_TOUCH_MIN_X");
            const char* env_max_x = std::getenv("HELIX_TOUCH_MAX_X");
            const char* env_min_y = std::getenv("HELIX_TOUCH_MIN_Y");
            const char* env_max_y = std::getenv("HELIX_TOUCH_MAX_Y");
            if (env_min_x && env_max_x && env_min_y && env_max_y) {
                int min_x = std::atoi(env_min_x);
                int max_x = std::atoi(env_max_x);
                int min_y = std::atoi(env_min_y);
                int max_y = std::atoi(env_max_y);
                spdlog::info("[DRM Backend] Touch calibration range from env: X({}->{}) Y({}->{}) "
                             "(overrides kernel EVIOCGABS)",
                             min_x, max_x, min_y, max_y);
                lv_evdev_set_calibration(pointer_, min_x, min_y, max_x, max_y);
            }

            // Load stored calibration.
            calibration_ = helix::load_touch_calibration();

            // Post-#943-fix upgrade: a stored affine on a non-resistive panel whose
            // ABS range mismatches the display was computed in the wrong (unscaled)
            // coordinate space — those panels now ride on evdev's linear scaling
            // above. The v17→v18 migration set recheck_pending; decide here, where
            // the device's resistive nature and live ABS range are known.
            if (helix::Config* cfg = helix::Config::get_instance()) {
                bool recheck_pending = cfg->get<bool>("/input/calibration/recheck_pending", false);
                bool changed = false;
                if (recheck_pending) {
                    bool is_resistive = helix::is_resistive_touchscreen_name(dev_name);
                    bool abs_mismatch =
                        got_range && helix::has_abs_display_mismatch(abs_x.maximum, abs_y.maximum,
                                                                     screen_width_, screen_height_);
                    if (helix::should_invalidate_legacy_calibration(recheck_pending, is_resistive,
                                                                    abs_mismatch)) {
                        spdlog::info("[DRM Backend] Invalidating legacy pre-#943 affine "
                                     "calibration (non-resistive panel, ABS/display mismatch)");
                        calibration_.valid = false;
                        cfg->set<bool>("/input/calibration/valid", false);
                        changed = true;
                    }
                    // One-shot: clear the flag regardless of the decision above.
                    cfg->set<bool>("/input/calibration/recheck_pending", false);
                    changed = true;
                }
                if (changed) {
                    cfg->save();
                }
            }

            // needs_calibration_ stays a STABLE capability flag: "this panel uses
            // affine calibration". It must NOT be cleared just because a valid
            // calibration is already loaded — needs_touch_calibration() also gates the
            // manual Settings "Touch Calibration" button (visibility + click), so
            // clearing it left the button dead on any already-calibrated device
            // (prestonbrown/helixscreen#943, prestonbrown/helixscreen#986). The boot
            // wizard does NOT re-fire on calibrated devices because its own gate
            // (WizardTouchCalibrationStep) independently skips when
            // /input/calibration/valid is set — so the user can still re-calibrate on
            // demand without the wizard forcing itself every boot.

            spdlog::info("[DRM Backend] Touch device '{}' phys='{}' — calibration {}", dev_name,
                         dev_phys, needs_calibration_ ? "needed" : "not needed");

            helix::install_calibration_wrapper(pointer_, calibration_context_, calibration_,
                                               screen_width_, screen_height_);
            calibration_wrapper_installed_ = true;
        }
    }

    // If no touch was found, try pointer devices
    if (!pointer_) {
#if LV_USE_LIBINPUT
        // Try pointer devices via libinput (mouse, trackpad)
        // Don't free pointer_path — points into LVGL's internal devices[] array
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
        }
#endif
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
#if LV_USE_LIBINPUT
        keyboard_ = lv_libinput_create(LV_INDEV_TYPE_KEYPAD, env_device);
        if (keyboard_) {
            spdlog::info("[DRM Backend] Keyboard created on {} (env override)", env_device);
            return keyboard_;
        }
#endif
        // Try evdev fallback
        keyboard_ = lv_evdev_create(LV_INDEV_TYPE_KEYPAD, env_device);
        if (keyboard_) {
            spdlog::info("[DRM Backend] Evdev keyboard created on {} (env override)", env_device);
            return keyboard_;
        }
        spdlog::warn("[DRM Backend] Could not open specified keyboard device: {}", env_device);
    }

    // Priority 2: sysfs evdev scanning (bypasses libinput which has heap
    // corruption bugs in _reset_scanned_devices — see issue #648)
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

// ============================================================================
// Real panel power-off via DRM connector DPMS (#1049)
// ============================================================================

bool DisplayBackendDRM::set_connector_dpms(bool on) {
    if (!display_) {
        spdlog::debug("[DRM Backend] DPMS: no display");
        return false;
    }

    // Use the DRM master fd LVGL already owns. DPMS modesetting requires DRM
    // master; LVGL acquired it via drmSetMaster() when it set up the display.
    // Opening a second (non-master) fd here would be rejected for modesetting.
    int fd = lv_linux_drm_get_fd(display_);
    if (fd < 0) {
        spdlog::warn("[DRM Backend] DPMS: could not obtain DRM master fd from LVGL");
        return false;
    }

    drmModeRes* resources = drmModeGetResources(fd);
    if (!resources) {
        spdlog::warn("[DRM Backend] DPMS: drmModeGetResources failed: {}", strerror(errno));
        return false;
    }

    bool applied = false;
    for (int i = 0; i < resources->count_connectors && !applied; i++) {
        drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (!connector) {
            continue;
        }

        if (connector->connection == DRM_MODE_CONNECTED) {
            // Find this connector's "DPMS" property id by enumerating its props.
            drmModePropertyPtr dpms_prop = nullptr;
            uint32_t dpms_prop_id = 0;
            for (int p = 0; p < connector->count_props; p++) {
                drmModePropertyPtr prop = drmModeGetProperty(fd, connector->props[p]);
                if (!prop) {
                    continue;
                }
                if (strcmp(prop->name, "DPMS") == 0) {
                    dpms_prop = prop;
                    dpms_prop_id = prop->prop_id;
                    break;
                }
                drmModeFreeProperty(prop);
            }

            if (dpms_prop_id != 0) {
                uint64_t value = on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
                int rc =
                    drmModeConnectorSetProperty(fd, connector->connector_id, dpms_prop_id, value);
                if (rc == 0) {
                    applied = true;
                    spdlog::info("[DRM Backend] Connector {} DPMS set to {}",
                                 connector->connector_id, on ? "ON" : "OFF");
                } else {
                    spdlog::warn("[DRM Backend] DPMS set on connector {} failed: {}",
                                 connector->connector_id, strerror(errno));
                }
            } else {
                spdlog::warn("[DRM Backend] Connected connector {} has no DPMS property",
                             connector->connector_id);
            }

            if (dpms_prop) {
                drmModeFreeProperty(dpms_prop);
            }
        }

        drmModeFreeConnector(connector);
    }

    drmModeFreeResources(resources);
    return applied;
}

bool DisplayBackendDRM::supports_power_off() const {
    // Capable when we hold a DRM master fd (we render via LVGL's DRM driver, which
    // is master). DisplayManager only consults this when there is no hardware
    // backlight blank; if it returns false the software overlay remains the
    // fallback. We can't cheaply pre-check the DPMS property here (const, and it
    // requires enumerating connectors), so report capable whenever the master fd
    // exists; power_off() returns false and falls back to overlay if DPMS is
    // actually absent.
    return display_ != nullptr && lv_linux_drm_get_fd(display_) >= 0;
}

bool DisplayBackendDRM::power_off() {
    return set_connector_dpms(false);
}

bool DisplayBackendDRM::power_on() {
    // Must run BEFORE the post-wake lv_refr_now() so the panel is live when LVGL
    // paints — same #303 ordering as the fbdev unblank path. DisplayManager
    // guarantees this by calling power_on() before lv_refr_now().
    return set_connector_dpms(true);
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
    // Update the owned context directly — calibrated_read_cb reads this same
    // member. Never round-trip through lv_indev_get_user_data(pointer_): that
    // slot can be stale or corrupted (bundle LG9X482B held XML string bytes),
    // and writing cal through a non-null garbage pointer faults.
    calibration_context_.calibration = cal;

    if (pointer_ && !calibration_wrapper_installed_) {
        // Wrapper not yet installed — install it now
        helix::install_calibration_wrapper(pointer_, calibration_context_, calibration_,
                                           screen_width_, screen_height_);
        calibration_wrapper_installed_ = true;
        spdlog::info("[DRM Backend] Calibration callback installed at runtime");
    } else {
        spdlog::info("[DRM Backend] Calibration updated: a={:.4f} b={:.4f} c={:.4f} d={:.4f} "
                     "e={:.4f} f={:.4f}",
                     cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);
    }

    return true;
}

void DisplayBackendDRM::disable_affine_calibration() {
    // Operate on the owned member, not lv_indev_get_user_data(pointer_): the
    // indev's user_data can be stale/corrupted and slip past an "if (ctx)" guard
    // as a non-null garbage pointer, faulting on the write (bundle LG9X482B).
    calibration_context_.calibration.valid = false;
    spdlog::debug("[DRM Backend] Affine calibration disabled for recalibration");
}

void DisplayBackendDRM::enable_affine_calibration() {
    calibration_context_.calibration = calibration_;
    spdlog::debug("[DRM Backend] Affine calibration re-enabled (valid={})", calibration_.valid);
}

void DisplayBackendDRM::clear_calibration() {
    // Both slots, so enable_affine_calibration() cannot bring the old matrix
    // back. Operates on owned members for the same reason set_calibration does.
    calibration_ = helix::TouchCalibration{};
    calibration_context_.calibration = calibration_;
    spdlog::info("[DRM Backend] Stored calibration cleared — device is uncalibrated");
}

#endif // HELIX_DISPLAY_DRM
