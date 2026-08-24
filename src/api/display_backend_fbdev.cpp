// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - Linux Framebuffer Display Backend Implementation

#ifdef HELIX_DISPLAY_FBDEV

#include "display_backend_fbdev.h"

#include "config.h"
#include "fbdev_size_helper.h"
#include "input_device_scanner.h"
#include "pending_startup_warnings.h"
#include "touch_calibration.h"
#include "touch_calibration_wrapper.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

// System includes for device access checks
#include <algorithm>
#include <climits>
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

// Optional LVGL extension in some branches/builds; weak symbol allows probing at runtime.
extern "C" void lv_linux_fbdev_set_skip_unblank(lv_display_t* disp, bool enabled)
    __attribute__((weak));

namespace {

using helix::is_known_touchscreen_name;

/**
 * @brief Check if an input device has INPUT_PROP_DIRECT set
 *
 * Reads /sys/class/input/eventN/device/properties and checks bit 0
 * (INPUT_PROP_DIRECT), which indicates a direct-input device like a
 * touchscreen (as opposed to a touchpad or mouse).
 *
 * @param event_num Event device number
 * @return true if INPUT_PROP_DIRECT is set
 */
bool has_direct_input_prop(int event_num) {
    std::string path = "/sys/class/input/event" + std::to_string(event_num) + "/device/properties";
    std::string props_str = helix::input::read_sysfs_line(path);
    if (props_str.empty())
        return false;

    try {
        // Properties file may have space-separated hex values; lowest bits are rightmost
        size_t last_space = props_str.rfind(' ');
        std::string last_hex =
            (last_space != std::string::npos) ? props_str.substr(last_space + 1) : props_str;
        unsigned long props = std::stoul(last_hex, nullptr, 16);
        return (props & 0x1) != 0; // INPUT_PROP_DIRECT
    } catch (...) {
        return false;
    }
}

} // anonymous namespace

DisplayBackendFbdev::DisplayBackendFbdev() = default;

DisplayBackendFbdev::~DisplayBackendFbdev() {
    // Tear down the calibration read callback before calibration_context_ is
    // destroyed. If LVGL's indev timer fires between our destruction and
    // lv_deinit(), calibrated_read_cb would reach freed memory (use-after-free /
    // SIGSEGV).
    helix::uninstall_calibration_wrapper(touch_, calibration_context_);

    restore_console();
}

DisplayBackendFbdev::DisplayBackendFbdev(const std::string& fb_device,
                                         const std::string& touch_device)
    : fb_device_(fb_device), touch_device_(touch_device) {}

bool DisplayBackendFbdev::is_available() const {
    struct stat st;

    // Check if framebuffer device exists and is accessible
    if (stat(fb_device_.c_str(), &st) != 0) {
        spdlog::debug("[Fbdev Backend] Framebuffer device {} not found", fb_device_);
        return false;
    }

    // Check if we can read it (need read access for display)
    if (access(fb_device_.c_str(), R_OK | W_OK) != 0) {
        spdlog::debug("[Fbdev Backend] Framebuffer device {} not accessible (need R/W permissions)",
                      fb_device_);
        return false;
    }

    return true;
}

DetectedResolution DisplayBackendFbdev::detect_resolution() const {
    int fd = open(fb_device_.c_str(), O_RDONLY);
    if (fd < 0) {
        spdlog::debug("[Fbdev Backend] Cannot open {} for resolution detection: {}", fb_device_,
                      strerror(errno));
        return {};
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        spdlog::debug("[Fbdev Backend] Cannot get vscreeninfo for resolution detection: {}",
                      strerror(errno));
        close(fd);
        return {};
    }

    close(fd);

    if (vinfo.xres == 0 || vinfo.yres == 0) {
        spdlog::warn("[Fbdev Backend] Framebuffer reports 0x0 resolution");
        return {};
    }

    spdlog::info("[Fbdev Backend] Detected resolution: {}x{}", vinfo.xres, vinfo.yres);
    return {static_cast<int>(vinfo.xres), static_cast<int>(vinfo.yres), true};
}

lv_display_t* DisplayBackendFbdev::create_display(int width, int height) {
    spdlog::info("[Fbdev Backend] Creating framebuffer display on {}", fb_device_);

    // Store screen dimensions for touch coordinate clamping
    screen_width_ = width;
    screen_height_ = height;

    // Query the kernel framebuffer's actual size. If the user explicitly
    // asked for a different size via -s, warn loudly — kernel fb size is
    // controlled by the kernel display driver and cannot be changed at
    // runtime. See prestonbrown/helixscreen#766.
    if (size_was_explicit_ && width > 0 && height > 0) {
        int fb_fd = open(fb_device_.c_str(), O_RDWR | O_CLOEXEC);
        if (fb_fd >= 0) {
            struct fb_var_screeninfo vinfo {};
            if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
                helix::FbSize actual = helix::fb_size_from_var_screeninfo(vinfo);
                if (actual.valid && (actual.width != static_cast<uint32_t>(width) ||
                                     actual.height != static_cast<uint32_t>(height))) {
                    spdlog::warn("[Fbdev Backend] Requested resolution {}x{} but kernel "
                                 "framebuffer is {}x{}. Framebuffer size is controlled "
                                 "by the kernel display driver and cannot be changed at "
                                 "runtime.",
                                 width, height, actual.width, actual.height);
                    spdlog::warn("[Fbdev Backend] To change: edit your HDMI/display "
                                 "kernel config (e.g. /boot/firmware/config.txt on "
                                 "Raspberry Pi / RatOS) and reboot.");
                    char toast_msg[160];
                    snprintf(toast_msg, sizeof(toast_msg),
                             "Configured resolution %dx%d is unavailable on this display; "
                             "using %ux%u instead.",
                             width, height, actual.width, actual.height);
                    helix::PendingStartupWarnings::instance().enqueue(
                        helix::PendingStartupWarnings::Severity::WARNING, toast_msg);
                }
            }
            close(fb_fd);
        }
    }

    // LVGL's framebuffer driver
    // Note: LVGL 9.x uses lv_linux_fbdev_create()
    display_ = lv_linux_fbdev_create();

    if (display_ == nullptr) {
        spdlog::error("[Fbdev Backend] Failed to create framebuffer display");
        return nullptr;
    }

    // Skip FBIOBLANK when splash process owns the framebuffer, if LVGL provides the hook.
    if (splash_active_) {
        if (lv_linux_fbdev_set_skip_unblank != nullptr) {
            lv_linux_fbdev_set_skip_unblank(display_, true);
            spdlog::debug("[Fbdev Backend] Splash active — FBIOBLANK skip enabled");
        } else {
            spdlog::debug(
                "[Fbdev Backend] Splash active — LVGL skip_unblank hook unavailable in this build");
        }
    }

    // Set the framebuffer device path (opens /dev/fb0 and mmaps it)
    lv_linux_fbdev_set_file(display_, fb_device_.c_str());

    // AD5M's LCD controller interprets XRGB8888's X byte as alpha.
    // By default, LVGL uses XRGB8888 for 32bpp and sets X=0x00 (transparent).
    // We must use ARGB8888 format so LVGL sets alpha=0xFF (fully opaque).
    // Without this, the display shows pink/magenta ghost overlay.
    // Only apply this fix for 32bpp displays - 16bpp displays use RGB565.
    lv_color_format_t detected_format = lv_display_get_color_format(display_);
    if (detected_format == LV_COLOR_FORMAT_XRGB8888) {
        lv_display_set_color_format(display_, LV_COLOR_FORMAT_ARGB8888);
        spdlog::info("[Fbdev Backend] Set color format to ARGB8888 (AD5M alpha fix)");
    } else {
        spdlog::info("[Fbdev Backend] Using detected color format ({}bpp)",
                     lv_color_format_get_size(detected_format) * 8);
    }

    // Check for R/B channel swap override. The LVGL fbdev driver auto-detects
    // BGR framebuffers from fb_var_screeninfo, but some drivers report incorrect
    // offsets. HELIX_COLOR_SWAP_RB=1 forces the swap on, =0 forces it off.
    const char* swap_rb_env = std::getenv("HELIX_COLOR_SWAP_RB");
    if (swap_rb_env != nullptr) {
        bool force_swap = (strcmp(swap_rb_env, "1") == 0);
        lv_linux_fbdev_set_swap_rb(display_, force_swap);
        spdlog::info("[Fbdev Backend] R/B channel swap {} (HELIX_COLOR_SWAP_RB={})",
                     force_swap ? "forced ON" : "forced OFF", swap_rb_env);
    } else {
        bool auto_swap = lv_linux_fbdev_get_swap_rb(display_);
        if (auto_swap) {
            spdlog::info("[Fbdev Backend] R/B channel swap auto-detected (BGR framebuffer)");
        }
    }

    // Suppress kernel console output to framebuffer.
    // Prevents dmesg/undervoltage warnings from bleeding through LVGL's partial repaints.
    suppress_console();

    spdlog::info("[Fbdev Backend] Framebuffer display created: {}x{} on {}", width, height,
                 fb_device_);
    return display_;
}

lv_indev_t* DisplayBackendFbdev::create_input_pointer() {
    // Determine touch device path
    std::string touch_path = touch_device_;
    if (touch_path.empty()) {
        touch_path = auto_detect_touch_device();
    }

    if (touch_path.empty()) {
        spdlog::warn("[Fbdev Backend] No touch device found - pointer input disabled");
        needs_calibration_ = false;
        supports_calibration_ = false;
        return nullptr;
    }

    spdlog::info("[Fbdev Backend] Creating evdev touch input on {}", touch_path);

    // LVGL's evdev driver for touch input
    touch_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path.c_str());

    if (touch_ == nullptr) {
        spdlog::error("[Fbdev Backend] Failed to create evdev touch input on {}", touch_path);
        return nullptr;
    }

    // Determine if touch calibration is needed using unified logic.
    // Reads device name, phys, and capabilities from sysfs.
    int event_num = -1;
    sscanf(touch_path.c_str() + touch_path.rfind("event"), "event%d", &event_num);
    std::string dev_name = (event_num >= 0) ? helix::input::get_input_device_name(event_num) : "";
    std::string dev_phys;
    if (event_num >= 0) {
        dev_phys = helix::input::get_input_device_phys(event_num);
    }
    helix::AbsCapabilities abs_caps;
    bool has_abs =
        (event_num >= 0) && helix::input::get_input_touch_capabilities(event_num, &abs_caps);

    needs_calibration_ = helix::device_needs_calibration(dev_name, dev_phys, has_abs);
    supports_calibration_ = helix::device_supports_calibration(dev_name, has_abs);

    // Log classification reason for support diagnostics
    const char* cal_reason = "unknown";
    if (!has_abs) {
        cal_reason = "no ABS axes (not a touchscreen)";
    } else if (helix::is_usb_input_phys(dev_phys)) {
        cal_reason = "USB HID (mapped coordinates)";
    } else if (dev_name.find("virtual") != std::string::npos) {
        cal_reason = "virtual device";
    } else if (helix::is_resistive_touchscreen_name(dev_name)) {
        cal_reason = "resistive controller (needs affine calibration)";
    } else {
        cal_reason = "capacitive/unknown (assumed factory-calibrated)";
    }

    spdlog::info(
        "[Fbdev Backend] Input device '{}' phys='{}' abs={} (st={} mt={}) → calibration {} "
        "[{}], manual entry point {}",
        dev_name, dev_phys, has_abs, abs_caps.has_single_touch, abs_caps.has_multitouch,
        needs_calibration_ ? "needed" : "not needed", cal_reason,
        supports_calibration_ ? "available" : "hidden");

    if (helix::is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] device detection: name='{}' phys='{}' event={}", dev_name,
                     dev_phys, event_num);
        spdlog::warn("[TouchDebug]   abs={} single_touch={} multitouch={}", has_abs,
                     abs_caps.has_single_touch, abs_caps.has_multitouch);
        spdlog::warn("[TouchDebug]   needs_calibration={} reason='{}'", needs_calibration_,
                     cal_reason);
    }

    // Read and log ABS ranges for diagnostic purposes, and check for mismatch
    // on capacitive screens that may report coordinates for a different resolution.
    // abs_x/abs_y/got_range are hoisted to this scope so the post-#943 legacy
    // calibration recheck below can consult the (MT-fallback-resolved) range.
    struct input_absinfo abs_x = {};
    struct input_absinfo abs_y = {};
    bool got_range = false;
    if (has_abs) {
        int fd = open(touch_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            bool got_x = (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0);
            bool got_y = (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0);

            // MT-only devices (e.g., Goodix gt9xxnew_ts) don't have legacy ABS_X/ABS_Y.
            // Fall back to ABS_MT_POSITION_X/ABS_MT_POSITION_Y for range queries.
            // Also fall back when EVIOCGABS succeeds but returns zero range — some
            // MT-only controllers report ABS_X with all-zero data (not an error).
            bool used_mt_fallback = false;
            bool range_is_zero = got_x && got_y && abs_x.maximum == 0 && abs_y.maximum == 0;
            if (abs_caps.has_multitouch && (!got_x || !got_y || range_is_zero)) {
                used_mt_fallback = true;
                spdlog::info("[Fbdev Backend] ABS_X/ABS_Y not available or zero-range, falling "
                             "back to MT axes");
                got_x = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0);
                got_y = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0);
            }
            close(fd);

            // When MT fallback provided valid ranges, propagate them into LVGL's
            // internal calibration so coordinate mapping works correctly even when
            // the driver's EVIOCGABS(ABS_X) returned zeros.
            if (used_mt_fallback && got_x && got_y && abs_x.maximum > abs_x.minimum) {
                lv_evdev_set_calibration(touch_, abs_x.minimum, abs_y.minimum, abs_x.maximum,
                                         abs_y.maximum);
                spdlog::info("[Fbdev Backend] Applied MT axis range to LVGL calibration: X({}..{}) "
                             "Y({}..{})",
                             abs_x.minimum, abs_x.maximum, abs_y.minimum, abs_y.maximum);
            }

            got_range = got_x && got_y;
            if (got_x && got_y) {
                spdlog::info(
                    "[Fbdev Backend] Touch ABS range: X({}..{}), Y({}..{}) — display: {}x{}",
                    abs_x.minimum, abs_x.maximum, abs_y.minimum, abs_y.maximum, screen_width_,
                    screen_height_);

                if (!needs_calibration_ && abs_x.maximum <= 0 && abs_y.maximum <= 0) {
                    // Zero ABS range means the kernel driver doesn't report proper
                    // coordinate bounds — LVGL can't map touch to screen without
                    // calibration (e.g., SonicPad gt9xxnew_ts).
                    needs_calibration_ = true;
                    spdlog::warn("[Fbdev Backend] ABS range is zero — LVGL cannot map "
                                 "coordinates to display ({}x{}), forcing calibration",
                                 screen_width_, screen_height_);
                } else if (!needs_calibration_ &&
                           helix::has_abs_display_mismatch(abs_x.maximum, abs_y.maximum,
                                                           screen_width_, screen_height_)) {
                    // Detect capacitive panels reporting a different resolution than the
                    // display (e.g., Goodix 800x480 on a 480x272 screen). Generic HID
                    // ranges (4096, 32767, etc.) are excluded — those are
                    // resolution-independent and LVGL maps them correctly.
                    needs_calibration_ = true;
                    spdlog::warn("[Fbdev Backend] ABS range ({},{}) mismatches display "
                                 "({}x{}) — forcing calibration",
                                 abs_x.maximum, abs_y.maximum, screen_width_, screen_height_);
                }
            } else {
                spdlog::warn("[Fbdev Backend] ABS range query failed for both legacy and MT axes");
            }
        } else {
            spdlog::warn("[Fbdev Backend] Could not open {} for ABS range query: {}", touch_path,
                         strerror(errno));
        }
    }

    // Check for touch axis configuration via environment variables
    // HELIX_TOUCH_SWAP_AXES=1 - swap X and Y axes
    const char* swap_axes = std::getenv("HELIX_TOUCH_SWAP_AXES");
    if (swap_axes != nullptr && strcmp(swap_axes, "1") == 0) {
        spdlog::info("[Fbdev Backend] Touch axes swapped (HELIX_TOUCH_SWAP_AXES=1)");
        lv_evdev_set_swap_axes(touch_, true);
    }

    // Check for explicit touch calibration values
    // These override the kernel-reported EVIOCGABS values which may be incorrect
    // (e.g., kernel reports 0-4095 but actual hardware uses a subset)
    // To invert an axis, swap min and max values (e.g., MIN_Y=3200, MAX_Y=900)
    const char* env_min_x = std::getenv("HELIX_TOUCH_MIN_X");
    const char* env_max_x = std::getenv("HELIX_TOUCH_MAX_X");
    const char* env_min_y = std::getenv("HELIX_TOUCH_MIN_Y");
    const char* env_max_y = std::getenv("HELIX_TOUCH_MAX_Y");

    if (env_min_x && env_max_x && env_min_y && env_max_y) {
        int min_x = std::atoi(env_min_x);
        int max_x = std::atoi(env_max_x);
        int min_y = std::atoi(env_min_y);
        int max_y = std::atoi(env_max_y);

        spdlog::info("[Fbdev Backend] Touch calibration from env: X({}->{}) Y({}->{})", min_x,
                     max_x, min_y, max_y);
        lv_evdev_set_calibration(touch_, min_x, min_y, max_x, max_y);
    }

    // Load affine calibration from config (saved by calibration wizard)
    calibration_ = helix::load_touch_calibration();

    // Post-#943-fix upgrade: a stored affine on a non-resistive panel whose ABS range
    // mismatches the display was computed in the wrong (unscaled) coordinate space —
    // those panels now ride on evdev's linear scaling applied above. The v17→v18
    // migration set recheck_pending; decide here, where the device's resistive nature
    // and live ABS range are known.
    if (helix::Config* cfg = helix::Config::get_instance()) {
        bool recheck_pending = cfg->get<bool>("/input/calibration/recheck_pending", false);
        if (recheck_pending) {
            bool is_resistive = helix::is_resistive_touchscreen_name(dev_name);
            bool abs_mismatch =
                got_range && helix::has_abs_display_mismatch(abs_x.maximum, abs_y.maximum,
                                                             screen_width_, screen_height_);
            if (helix::should_invalidate_legacy_calibration(recheck_pending, is_resistive,
                                                            abs_mismatch)) {
                spdlog::info("[Fbdev Backend] Invalidating legacy pre-#943 affine calibration "
                             "(non-resistive panel, ABS/display mismatch)");
                calibration_.valid = false;
                cfg->set<bool>("/input/calibration/valid", false);
            }
            // One-shot: clear the flag regardless of the decision above.
            cfg->set<bool>("/input/calibration/recheck_pending", false);
            cfg->save();
        }
    }

    // needs_calibration_ stays a STABLE capability flag: "this panel uses affine
    // calibration" (true for resistive controllers). It must NOT be cleared just
    // because a valid calibration is already loaded — needs_touch_calibration()
    // also gates the manual Settings "Touch Calibration" button (visibility +
    // click), so clearing it here left the button dead on any already-calibrated
    // device (prestonbrown/helixscreen#943, prestonbrown/helixscreen#986). The
    // boot wizard does NOT re-fire on calibrated devices because its own gate
    // (WizardTouchCalibrationStep) independently skips when
    // /input/calibration/valid is set — so the user can still re-calibrate on
    // demand without the wizard forcing itself every boot.

    // Always install the calibrated read callback — it handles both rotation
    // transform and affine calibration independently. Without this, rotation
    // transform wouldn't be applied on devices that don't need affine cal.
    // Note: jitter filtering is applied generically in lvgl_init.cpp after this.
    helix::install_calibration_wrapper(touch_, calibration_context_, calibration_, screen_width_,
                                       screen_height_);
    calibration_wrapper_installed_ = true;

    spdlog::info("[Fbdev Backend] Evdev touch input created on {}", touch_path);

    // Detect USB HID mouse (in addition to touchscreen)
    const char* mouse_env = std::getenv("HELIX_MOUSE_DEVICE");
    if (mouse_env && mouse_env[0] != '\0') {
        mouse_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, mouse_env);
        if (mouse_) {
            spdlog::info("[Fbdev Backend] Mouse created on {} (env override)", mouse_env);
        }
    }

    if (!mouse_) {
        auto mouse_dev = helix::input::find_mouse_device();
        if (mouse_dev) {
            mouse_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, mouse_dev->path.c_str());
            if (mouse_) {
                spdlog::info("[Fbdev Backend] Mouse created on {} ({})", mouse_dev->path,
                             mouse_dev->name);
            }
        }
    }

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
        spdlog::info("[Fbdev Backend] Mouse cursor enabled");
    }

    return touch_;
}

lv_indev_t* DisplayBackendFbdev::create_input_keyboard() {
    // Priority 1: Environment variable override
    const char* env_device = std::getenv("HELIX_KEYBOARD_DEVICE");
    if (env_device && env_device[0] != '\0') {
        keyboard_ = lv_evdev_create(LV_INDEV_TYPE_KEYPAD, env_device);
        if (keyboard_) {
            spdlog::info("[Fbdev Backend] Keyboard created on {} (env override)", env_device);
            return keyboard_;
        }
        spdlog::warn("[Fbdev Backend] Could not open specified keyboard device: {}", env_device);
    }

    // Priority 2: Auto-detect via sysfs scanning
    auto kb_dev = helix::input::find_keyboard_device();
    if (kb_dev) {
        keyboard_ = lv_evdev_create(LV_INDEV_TYPE_KEYPAD, kb_dev->path.c_str());
        if (keyboard_) {
            spdlog::info("[Fbdev Backend] Keyboard created on {} ({})", kb_dev->path, kb_dev->name);
            return keyboard_;
        }
    }

    spdlog::debug("[Fbdev Backend] No keyboard device found");
    return nullptr;
}

std::string DisplayBackendFbdev::auto_detect_touch_device() const {
    // Priority 1: Environment variable override
    const char* env_device = std::getenv("HELIX_TOUCH_DEVICE");
    if (env_device != nullptr && strlen(env_device) > 0) {
        spdlog::info("[Fbdev Backend] Using touch device from HELIX_TOUCH_DEVICE: {}", env_device);
        return env_device;
    }

    // Priority 2: Config file override
    helix::Config* cfg = helix::Config::get_instance();
    auto device_override = cfg->get<std::string>("/input/touch_device", "");
    if (!device_override.empty()) {
        spdlog::info("[Fbdev Backend] Using touch device from config: {}", device_override);
        return device_override;
    }

    // Check for common misconfiguration: touch_device at root or display level
    // instead of under /input/
    if (cfg) {
        auto root_touch = cfg->get<std::string>("/touch_device", "");
        auto display_touch = cfg->get<std::string>("/display/touch_device", "");
        if (!root_touch.empty() || !display_touch.empty()) {
            spdlog::warn("[Fbdev Backend] Found 'touch_device' at config root or display section, "
                         "but it should be under 'input'. "
                         "See docs/user/CONFIGURATION.md");
        }
    }

    // Priority 3: Capability-based detection using Linux sysfs
    // Scan /dev/input/eventN devices and check for touch capabilities
    const char* input_dir = "/dev/input";
    DIR* dir = opendir(input_dir);
    if (dir == nullptr) {
        spdlog::debug("[Fbdev Backend] Cannot open {}", input_dir);
        return "";
    }

    std::string best_device;
    std::string best_name;
    int best_score = -1;
    int best_event_num = INT_MAX;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Look for eventN devices
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        // Extract event number
        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) {
            continue;
        }

        std::string device_path = std::string(input_dir) + "/" + entry->d_name;

        // Check if accessible
        if (access(device_path.c_str(), R_OK) != 0) {
            continue;
        }

        // Get device name from sysfs (do this once, before capability check)
        std::string name = helix::input::get_input_device_name(event_num);

        // Check for ABS capabilities (single-touch or multitouch)
        helix::AbsCapabilities dev_abs_caps;
        if (!helix::input::get_input_touch_capabilities(event_num, &dev_abs_caps)) {
            spdlog::trace("[Fbdev Backend] {} ({}) - no touch capabilities", device_path, name);
            continue;
        }

        // Score this candidate
        int score = 0;

        bool is_known = is_known_touchscreen_name(name);
        if (is_known)
            score += 2;

        bool is_direct = has_direct_input_prop(event_num);
        if (is_direct)
            score += 2;

        std::string phys = helix::input::get_input_device_phys(event_num);
        bool is_usb = helix::is_usb_input_phys(phys);
        if (is_usb)
            score += 1;

        spdlog::debug("[Fbdev Backend] {} ({}) score={} [known={} direct={} usb={} phys='{}']",
                      device_path, name, score, is_known, is_direct, is_usb, phys);

        // Best score wins; ties broken by lowest event number
        if (score > best_score || (score == best_score && event_num < best_event_num)) {
            best_device = device_path;
            best_name = name;
            best_score = score;
            best_event_num = event_num;
        }
    }

    closedir(dir);

    if (best_device.empty()) {
        // No device with ABS_X/ABS_Y found. Fall back to first accessible event device
        // so VNC mouse input (uinput) or other pointer sources still work.
        DIR* fallback_dir = opendir(input_dir);
        if (fallback_dir) {
            struct dirent* fb_entry;
            while ((fb_entry = readdir(fallback_dir)) != nullptr) {
                if (strncmp(fb_entry->d_name, "event", 5) != 0)
                    continue;
                std::string fallback_path = std::string(input_dir) + "/" + fb_entry->d_name;
                if (access(fallback_path.c_str(), R_OK) == 0) {
                    std::string fb_name =
                        helix::input::get_input_device_name(atoi(fb_entry->d_name + 5));
                    spdlog::info(
                        "[Fbdev Backend] No touchscreen found, using fallback input: {} ({})",
                        fallback_path, fb_name);
                    closedir(fallback_dir);
                    return fallback_path;
                }
            }
            closedir(fallback_dir);
        }
        spdlog::debug("[Fbdev Backend] No input devices found at all");
        return "";
    }

    spdlog::info("[Fbdev Backend] Selected touchscreen: {} ({}) [score={}]", best_device, best_name,
                 best_score);
    return best_device;
}

void DisplayBackendFbdev::suppress_console() {
    // Switch the VT to KD_GRAPHICS mode so the kernel stops rendering console text
    // (dmesg, undervoltage warnings, etc.) directly to the framebuffer.
    // LVGL uses partial render mode and only repaints dirty regions, so any kernel
    // text written to /dev/fb0 persists in areas that haven't been invalidated.
    // This is the standard approach used by X11, Weston, and other fbdev applications.
    //
    // Use O_WRONLY: under systemd with SupplementaryGroups=tty, the tty group
    // only has write permission (crw--w----). O_RDWR fails with EACCES.
    static const char* tty_paths[] = {"/dev/tty0", "/dev/tty1", "/dev/tty", nullptr};

    for (int i = 0; tty_paths[i] != nullptr; ++i) {
        tty_fd_ = open(tty_paths[i], O_WRONLY | O_CLOEXEC);
        if (tty_fd_ >= 0) {
            if (ioctl(tty_fd_, KDSETMODE, KD_GRAPHICS) == 0) {
                spdlog::info("[Fbdev Backend] Console suppressed via KDSETMODE KD_GRAPHICS on {}",
                             tty_paths[i]);
                return;
            }
            spdlog::debug("[Fbdev Backend] KDSETMODE failed on {}: {}", tty_paths[i],
                          strerror(errno));
            close(tty_fd_);
            tty_fd_ = -1;
        }
    }

    spdlog::warn("[Fbdev Backend] Could not suppress console — kernel messages may bleed through");
}

void DisplayBackendFbdev::restore_console() {
    if (tty_fd_ >= 0) {
        if (ioctl(tty_fd_, KDSETMODE, KD_TEXT) != 0) {
            spdlog::warn("[Fbdev Backend] KDSETMODE KD_TEXT failed: {}", strerror(errno));
        }
        close(tty_fd_);
        tty_fd_ = -1;
        spdlog::debug("[Fbdev Backend] Console restored to KD_TEXT mode");
    }
}

bool DisplayBackendFbdev::clear_framebuffer(uint32_t color) {
    // Open framebuffer to get info and clear it
    int fd = open(fb_device_.c_str(), O_RDWR);
    if (fd < 0) {
        spdlog::error("[Fbdev Backend] Cannot open {} for clearing: {}", fb_device_,
                      strerror(errno));
        return false;
    }

    // Get variable screen info
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        spdlog::error("[Fbdev Backend] Cannot get vscreeninfo: {}", strerror(errno));
        close(fd);
        return false;
    }

    // Get fixed screen info
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        spdlog::error("[Fbdev Backend] Cannot get fscreeninfo: {}", strerror(errno));
        close(fd);
        return false;
    }

    // Calculate framebuffer size
    size_t screensize = finfo.smem_len;

    // Map framebuffer to memory
    void* fbp = mmap(nullptr, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbp == MAP_FAILED) {
        spdlog::error("[Fbdev Backend] Cannot mmap framebuffer: {}", strerror(errno));
        close(fd);
        return false;
    }

    // Determine bytes per pixel from stride
    uint32_t bpp = 32; // Default assumption
    if (vinfo.xres > 0) {
        bpp = (finfo.line_length * 8) / vinfo.xres;
    }

    // Fill framebuffer with the specified color
    // Color is in ARGB format (0xAARRGGBB)
    if (bpp == 32) {
        // 32-bit: fill with ARGB pixels
        uint32_t* pixels = static_cast<uint32_t*>(fbp);
        size_t pixel_count = screensize / 4;
        for (size_t i = 0; i < pixel_count; i++) {
            pixels[i] = color;
        }
    } else if (bpp == 16) {
        // 16-bit RGB565: convert ARGB to RGB565
        uint16_t r = ((color >> 16) & 0xFF) >> 3; // 5 bits
        uint16_t g = ((color >> 8) & 0xFF) >> 2;  // 6 bits
        uint16_t b = (color & 0xFF) >> 3;         // 5 bits
        uint16_t rgb565 = (r << 11) | (g << 5) | b;

        uint16_t* pixels = static_cast<uint16_t*>(fbp);
        size_t pixel_count = screensize / 2;
        for (size_t i = 0; i < pixel_count; i++) {
            pixels[i] = rgb565;
        }
    } else {
        // Fallback: just zero the buffer (black)
        memset(fbp, 0, screensize);
    }

    spdlog::info("[Fbdev Backend] Cleared framebuffer to 0x{:08X} ({}x{}, {}bpp)", color,
                 vinfo.xres, vinfo.yres, bpp);

    // Unmap and close
    munmap(fbp, screensize);
    close(fd);

    return true;
}

bool DisplayBackendFbdev::unblank_display() {
    // Unblank the display using standard Linux framebuffer ioctls.
    // This approach is borrowed from GuppyScreen's lv_drivers patch.
    // Essential on AD5M where other processes may blank the display during boot.

    int fd = open(fb_device_.c_str(), O_RDWR);
    if (fd < 0) {
        spdlog::warn("[Fbdev Backend] Cannot open {} for unblank: {}", fb_device_, strerror(errno));
        return false;
    }

    // 1. Unblank the display via framebuffer ioctl
    if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {
        spdlog::warn("[Fbdev Backend] FBIOBLANK unblank failed: {}", strerror(errno));
        // Continue anyway - some drivers don't support this but pan may still work
    } else {
        spdlog::info("[Fbdev Backend] Display unblanked via FBIOBLANK");
    }

    // 2. Get screen info and reset pan position to (0,0)
    // This ensures we're drawing to the visible portion of the framebuffer
    struct fb_var_screeninfo var_info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var_info) != 0) {
        spdlog::warn("[Fbdev Backend] FBIOGET_VSCREENINFO failed: {}", strerror(errno));
        close(fd);
        // Don't return - try Allwinner backlight below
    } else {
        var_info.yoffset = 0;
        if (ioctl(fd, FBIOPAN_DISPLAY, &var_info) != 0) {
            spdlog::debug("[Fbdev Backend] FBIOPAN_DISPLAY failed: {} (may be unsupported)",
                          strerror(errno));
        } else {
            spdlog::info("[Fbdev Backend] Display pan reset to yoffset=0");
        }
    }

    close(fd);

    // NOTE: Allwinner backlight control is NOT done here!
    // BacklightBackend handles all backlight control via /dev/disp ioctls.
    // Having duplicate ioctl calls here and in BacklightBackend can put the
    // Allwinner DISP2 driver into an inverted state where higher values = dimmer.
    // Let DisplayManager's m_backlight->set_brightness() handle backlight.

    return true;
}

bool DisplayBackendFbdev::blank_display() {
    // Blank the display using standard Linux framebuffer ioctl.
    // Counterpart to unblank_display() for proper display sleep.

    int fd = open(fb_device_.c_str(), O_RDWR);
    if (fd < 0) {
        spdlog::warn("[Fbdev Backend] Cannot open {} for blank: {}", fb_device_, strerror(errno));
        return false;
    }

    if (ioctl(fd, FBIOBLANK, FB_BLANK_NORMAL) != 0) {
        spdlog::warn("[Fbdev Backend] FBIOBLANK blank failed: {}", strerror(errno));
        close(fd);
        return false;
    }

    spdlog::info("[Fbdev Backend] Display blanked via FBIOBLANK");
    close(fd);
    return true;
}

bool DisplayBackendFbdev::supports_power_off() const {
    // The framebuffer node must be readable+writable for FBIOBLANK to work.
    // We can't reliably detect ahead of time whether the panel driver honors
    // FB_BLANK_POWERDOWN, but on real fbdev HDMI/DSI panels it is the standard
    // power-down request; drivers that ignore it simply leave the panel lit,
    // which is no worse than the software-overlay fallback (#1049).
    return access(fb_device_.c_str(), R_OK | W_OK) == 0;
}

bool DisplayBackendFbdev::power_off() {
    // Power the panel down via FB_BLANK_POWERDOWN — a deeper state than the
    // FB_BLANK_NORMAL used by blank_display(): it requests the driver to drop
    // the panel's backlight/transceiver entirely. Used on HDMI/fbdev devices
    // with no sysfs/ioctl backlight where a software overlay leaves the panel
    // fully powered (#1049).
    int fd = open(fb_device_.c_str(), O_RDWR);
    if (fd < 0) {
        spdlog::warn("[Fbdev Backend] Cannot open {} for power-off: {}", fb_device_,
                     strerror(errno));
        return false;
    }

    if (ioctl(fd, FBIOBLANK, FB_BLANK_POWERDOWN) != 0) {
        spdlog::warn("[Fbdev Backend] FBIOBLANK power-down failed: {}", strerror(errno));
        close(fd);
        return false;
    }

    spdlog::info("[Fbdev Backend] Display powered down via FBIOBLANK FB_BLANK_POWERDOWN");
    close(fd);
    return true;
}

bool DisplayBackendFbdev::power_on() {
    // Counterpart to power_off(): unblank and reset pan position so the next
    // render lands on the visible portion of the framebuffer. Mirrors
    // unblank_display() so the #303 wake-race fix applies to this path too.
    return unblank_display();
}

bool DisplayBackendFbdev::set_calibration(const helix::TouchCalibration& cal) {
    if (!helix::is_calibration_valid(cal)) {
        spdlog::warn("[Fbdev Backend] Invalid calibration rejected");
        return false;
    }

    // Update stored calibration
    calibration_ = cal;
    // Update the owned context directly — calibrated_read_cb reads this same
    // member. Never round-trip through lv_indev_get_user_data(touch_): that slot
    // can be stale or corrupted (bundle LG9X482B held XML string bytes), and
    // writing cal through a non-null garbage pointer faults.
    calibration_context_.calibration = cal;

    if (touch_ && !calibration_wrapper_installed_) {
        // Need to install the callback wrapper for the first time.
        // In practice this branch is unreachable because create_input_pointer()
        // always installs the calibrated callback, but we handle it defensively.
        spdlog::warn("[Fbdev Backend] Calibrated callback was not pre-installed — "
                     "installing at runtime (unexpected code path)");

        helix::install_calibration_wrapper(touch_, calibration_context_, calibration_,
                                           screen_width_, screen_height_);
        calibration_wrapper_installed_ = true;

        spdlog::info("[Fbdev Backend] Calibration callback installed at runtime: "
                     "a={:.4f} b={:.4f} c={:.4f} d={:.4f} e={:.4f} f={:.4f}",
                     cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);
    } else {
        spdlog::info("[Fbdev Backend] Calibration updated at runtime: "
                     "a={:.4f} b={:.4f} c={:.4f} d={:.4f} e={:.4f} f={:.4f}",
                     cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);
    }

    return true;
}

void DisplayBackendFbdev::disable_affine_calibration() {
    // Operate on the owned member, not lv_indev_get_user_data(touch_): the
    // indev's user_data can be stale/corrupted and slip past an "if (ctx)" guard
    // as a non-null garbage pointer, faulting on the write (bundle LG9X482B).
    calibration_context_.calibration.valid = false;
    spdlog::debug("[Fbdev Backend] Affine calibration disabled for recalibration");
}

void DisplayBackendFbdev::enable_affine_calibration() {
    calibration_context_.calibration = calibration_;
    spdlog::debug("[Fbdev Backend] Affine calibration re-enabled (valid={})", calibration_.valid);
}

void DisplayBackendFbdev::clear_calibration() {
    // Both slots, so enable_affine_calibration() cannot bring the old matrix
    // back. Operates on owned members for the same reason set_calibration does.
    calibration_ = helix::TouchCalibration{};
    calibration_context_.calibration = calibration_;
    spdlog::info("[Fbdev Backend] Stored calibration cleared — device is uncalibrated");
}

void DisplayBackendFbdev::set_display_rotation(lv_display_rotation_t rot, int phys_w, int phys_h) {
    // No-op for fbdev — LVGL's indev_pointer_proc() already calls
    // lv_display_rotate_point() to transform touch coordinates for
    // the current display rotation. No manual touch transform needed.
    (void)rot;
    (void)phys_w;
    (void)phys_h;
}

#endif // HELIX_DISPLAY_FBDEV
