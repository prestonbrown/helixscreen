// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - Linux Framebuffer Display Backend Implementation

#ifdef HELIX_DISPLAY_FBDEV

#include "display_backend_fbdev.h"

#include "config.h"
#include "touch_calibration.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

// System includes for device access checks
#include <algorithm>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
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

/**
 * @brief Read a line from a sysfs file
 * @param path Path to the sysfs file
 * @return File contents (first line) or empty string on error
 */
std::string read_sysfs_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::string line;
    std::getline(file, line);
    return line;
}

/**
 * @brief Get the device name from sysfs
 * @param event_num Event device number (e.g., 0 for event0)
 * @return Device name or empty string on error
 */
std::string get_device_name(int event_num) {
    std::string path = "/sys/class/input/event" + std::to_string(event_num) + "/device/name";
    return read_sysfs_file(path);
}

/**
 * @brief Check if an event device has touch/absolute input capabilities
 *
 * Reads /sys/class/input/eventN/device/capabilities/abs and checks for
 * ABS_X/ABS_Y (single-touch) or ABS_MT_POSITION_X/ABS_MT_POSITION_Y
 * (multitouch) capabilities. Some touchscreens (e.g., Goodix gt9xxnew_ts)
 * only report MT axes without legacy single-touch axes.
 *
 * @param event_num Event device number
 * @param[out] caps_out If non-null, populated with parsed capabilities
 * @return true if device has single-touch or multitouch ABS capabilities
 */
bool has_touch_capabilities(int event_num, helix::AbsCapabilities* caps_out = nullptr) {
    std::string path =
        "/sys/class/input/event" + std::to_string(event_num) + "/device/capabilities/abs";
    std::string caps = read_sysfs_file(path);

    if (caps.empty()) {
        return false;
    }

    auto result = helix::parse_abs_capabilities(caps);
    if (caps_out) {
        *caps_out = result;
    }

    if (result.has_multitouch && !result.has_single_touch) {
        spdlog::debug(
            "[Fbdev Backend] event{}: MT-only touchscreen detected (no legacy ABS_X/ABS_Y)",
            event_num);
    }

    return result.has_single_touch || result.has_multitouch;
}

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
    std::string props_str = read_sysfs_file(path);
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

/**
 * @brief Get the phys path for an input device from sysfs
 * @param event_num Event device number
 * @return Physical path string or empty string on error
 */
std::string get_device_phys(int event_num) {
    std::string path = "/sys/class/input/event" + std::to_string(event_num) + "/device/phys";
    return read_sysfs_file(path);
}

/**
 * @brief Load affine touch calibration coefficients from config
 *
 * Reads the calibration data saved by the touch calibration wizard.
 * Returns an invalid calibration if no valid data is stored.
 *
 * @return Calibration coefficients (check .valid before use)
 */
helix::TouchCalibration load_touch_calibration() {
    helix::Config* cfg = helix::Config::get_instance();
    helix::TouchCalibration cal;

    if (!cfg) {
        spdlog::debug("[Fbdev Backend] Config not available for calibration load");
        return cal;
    }

    cal.valid = cfg->get<bool>("/input/calibration/valid", false);
    if (!cal.valid) {
        spdlog::debug("[Fbdev Backend] No valid calibration in config");
        return cal;
    }

    cal.a = static_cast<float>(cfg->get<double>("/input/calibration/a", 1.0));
    cal.b = static_cast<float>(cfg->get<double>("/input/calibration/b", 0.0));
    cal.c = static_cast<float>(cfg->get<double>("/input/calibration/c", 0.0));
    cal.d = static_cast<float>(cfg->get<double>("/input/calibration/d", 0.0));
    cal.e = static_cast<float>(cfg->get<double>("/input/calibration/e", 1.0));
    cal.f = static_cast<float>(cfg->get<double>("/input/calibration/f", 0.0));
    cal.axes_swapped = cfg->get<bool>("/input/calibration/swap_axes", false);

    if (!helix::is_calibration_valid(cal)) {
        spdlog::warn("[Fbdev Backend] Stored calibration failed validation");
        cal.valid = false;
    }

    return cal;
}

/**
 * @brief Custom read callback that applies affine calibration
 *
 * Wraps the original evdev read callback, applying the affine transform
 * to touch coordinates after the linear calibration is done.
 *
 * Note: display rotation is handled by LVGL's indev_pointer_proc() which
 * calls lv_display_rotate_point() automatically — no manual rotation
 * transform is needed here.
 */
void calibrated_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* ctx = static_cast<CalibrationContext*>(lv_indev_get_user_data(indev));
    if (!ctx) {
        return;
    }

    // Call the original evdev read callback first
    if (ctx->original_read_cb) {
        ctx->original_read_cb(indev, data);
    }

    // Apply affine calibration if valid (for both PRESSED and RELEASED states)
    if (ctx->calibration.valid) {
        helix::Point raw{static_cast<int>(data->point.x), static_cast<int>(data->point.y)};
        helix::Point transformed = helix::transform_point(
            ctx->calibration, raw, ctx->screen_width - 1, ctx->screen_height - 1);
        data->point.x = transformed.x;
        data->point.y = transformed.y;
    }

    // Jitter filter: suppress small coordinate changes while finger is pressed.
    // Prevents noisy touch controllers (e.g., Goodix GT9xx) from generating
    // enough movement to trigger LVGL's scroll detection on stationary taps.
    ctx->jitter.apply(data->state, data->point.x, data->point.y);
}

} // anonymous namespace

DisplayBackendFbdev::DisplayBackendFbdev() = default;

DisplayBackendFbdev::~DisplayBackendFbdev() {
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
    std::string dev_name = (event_num >= 0) ? get_device_name(event_num) : "";
    std::string dev_phys;
    if (event_num >= 0) {
        dev_phys =
            read_sysfs_file("/sys/class/input/event" + std::to_string(event_num) + "/device/phys");
    }
    helix::AbsCapabilities abs_caps;
    bool has_abs = (event_num >= 0) && has_touch_capabilities(event_num, &abs_caps);

    needs_calibration_ = helix::device_needs_calibration(dev_name, dev_phys, has_abs);

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
        "[Fbdev Backend] Input device '{}' phys='{}' abs={} (st={} mt={}) → calibration {} [{}]",
        dev_name, dev_phys, has_abs, abs_caps.has_single_touch, abs_caps.has_multitouch,
        needs_calibration_ ? "needed" : "not needed", cal_reason);

    // Read and log ABS ranges for diagnostic purposes, and check for mismatch
    // on capacitive screens that may report coordinates for a different resolution
    if (has_abs) {
        int fd = open(touch_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct input_absinfo abs_x = {};
            struct input_absinfo abs_y = {};
            bool got_x = (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0);
            bool got_y = (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0);

            // MT-only devices (e.g., Goodix gt9xxnew_ts) don't have legacy ABS_X/ABS_Y.
            // Fall back to ABS_MT_POSITION_X/ABS_MT_POSITION_Y for range queries.
            if ((!got_x || !got_y) && abs_caps.has_multitouch) {
                spdlog::info("[Fbdev Backend] ABS_X/ABS_Y not available, falling back to MT axes");
                got_x = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0);
                got_y = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0);
            }
            close(fd);

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
    calibration_ = load_touch_calibration();
    if (calibration_.valid) {
        spdlog::info("[Fbdev Backend] Affine calibration loaded: "
                     "a={:.4f} b={:.4f} c={:.4f} d={:.4f} e={:.4f} f={:.4f}",
                     calibration_.a, calibration_.b, calibration_.c, calibration_.d, calibration_.e,
                     calibration_.f);
        if (calibration_.axes_swapped && !(swap_axes && strcmp(swap_axes, "1") == 0)) {
            spdlog::info("[Fbdev Backend] Applying auto-detected axis swap from calibration");
            lv_evdev_set_swap_axes(touch_, true);
        }
    } else {
        spdlog::info("[Fbdev Backend] No stored affine calibration found");
    }

    // Load jitter filter threshold from config (pixels, default 15 for noisy
    // touch controllers like Goodix GT9xx). Set to 0 to disable.
    int jitter_threshold = 15;
    if (auto* cfg = helix::Config::get_instance()) {
        jitter_threshold = cfg->get<int>("/input/jitter_threshold", 15);
    }
    const char* env_jitter = std::getenv("HELIX_TOUCH_JITTER");
    if (env_jitter) {
        jitter_threshold = std::atoi(env_jitter);
    }
    jitter_threshold = std::clamp(jitter_threshold, 0, 200);
    if (jitter_threshold > 0) {
        spdlog::info("[Fbdev Backend] Touch jitter filter: {}px dead zone", jitter_threshold);
    }

    // Always install the calibrated read callback — it handles both rotation
    // transform and affine calibration independently. Without this, rotation
    // transform wouldn't be applied on devices that don't need affine cal.
    calibration_context_.calibration = calibration_;
    calibration_context_.original_read_cb = lv_indev_get_read_cb(touch_);
    calibration_context_.screen_width = screen_width_;
    calibration_context_.screen_height = screen_height_;
    calibration_context_.jitter.threshold_sq = jitter_threshold * jitter_threshold;

    lv_indev_set_user_data(touch_, &calibration_context_);
    lv_indev_set_read_cb(touch_, calibrated_read_cb);

    spdlog::info("[Fbdev Backend] Evdev touch input created on {}", touch_path);
    return touch_;
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
        std::string name = get_device_name(event_num);

        // Check for ABS capabilities (single-touch or multitouch)
        helix::AbsCapabilities dev_abs_caps;
        if (!has_touch_capabilities(event_num, &dev_abs_caps)) {
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

        std::string phys = get_device_phys(event_num);
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
                    std::string fb_name = get_device_name(atoi(fb_entry->d_name + 5));
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

bool DisplayBackendFbdev::set_calibration(const helix::TouchCalibration& cal) {
    if (!helix::is_calibration_valid(cal)) {
        spdlog::warn("[Fbdev Backend] Invalid calibration rejected");
        return false;
    }

    // Update stored calibration
    calibration_ = cal;

    // If touch input exists with our custom callback, update the context
    if (touch_) {
        auto* ctx = static_cast<CalibrationContext*>(lv_indev_get_user_data(touch_));
        if (ctx) {
            // Update existing context (points to our member variable)
            ctx->calibration = cal;
            spdlog::info("[Fbdev Backend] Calibration updated at runtime: "
                         "a={:.4f} b={:.4f} c={:.4f} d={:.4f} e={:.4f} f={:.4f}",
                         cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);
        } else {
            // Need to install the callback wrapper for the first time
            calibration_context_.calibration = cal;
            calibration_context_.original_read_cb = lv_indev_get_read_cb(touch_);
            calibration_context_.screen_width = screen_width_;
            calibration_context_.screen_height = screen_height_;

            lv_indev_set_user_data(touch_, &calibration_context_);
            lv_indev_set_read_cb(touch_, calibrated_read_cb);

            spdlog::info("[Fbdev Backend] Calibration callback installed at runtime: "
                         "a={:.4f} b={:.4f} c={:.4f} d={:.4f} e={:.4f} f={:.4f}",
                         cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);
        }
    }

    // Apply or clear axis swap based on new calibration
    if (touch_) {
        bool should_swap = cal.axes_swapped;
        // Env var always wins over auto-detection
        const char* env_swap = std::getenv("HELIX_TOUCH_SWAP_AXES");
        if (env_swap && strcmp(env_swap, "1") == 0) {
            should_swap = true;
        }
        lv_evdev_set_swap_axes(touch_, should_swap);
        if (should_swap) {
            spdlog::info("[Fbdev Backend] Axis swap active at runtime");
        }
    }

    return true;
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
