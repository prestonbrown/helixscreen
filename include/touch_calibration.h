// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#pragma once

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Parsed ABS capabilities from sysfs capabilities/abs hex string
 *
 * Distinguishes between single-touch (ABS_X/ABS_Y, bits 0-1) and multitouch
 * (ABS_MT_POSITION_X/ABS_MT_POSITION_Y, bits 53-54) capabilities.
 * Some touchscreens (e.g., Goodix gt9xxnew_ts) only report MT axes without
 * legacy single-touch axes.
 */
struct AbsCapabilities {
    bool has_single_touch = false; ///< ABS_X (bit 0) + ABS_Y (bit 1)
    bool has_multitouch = false;   ///< ABS_MT_POSITION_X (bit 53) + ABS_MT_POSITION_Y (bit 54)
};

/**
 * @brief Parse the sysfs ABS capabilities hex string
 *
 * Reads /sys/class/input/eventN/device/capabilities/abs format:
 * space-separated hex words, rightmost = bits 0-31, next = bits 32-63, etc.
 *
 * @param caps_hex Raw hex string from sysfs (e.g., "600003", "600000 3", "0")
 * @return Parsed capabilities indicating single-touch and/or multitouch support
 */
inline AbsCapabilities parse_abs_capabilities(const std::string& caps_hex) {
    AbsCapabilities result;

    if (caps_hex.empty()) {
        return result;
    }

    // Split on spaces into words (rightmost = lowest bits)
    std::vector<unsigned long> words;
    std::istringstream iss(caps_hex);
    std::string token;
    while (iss >> token) {
        try {
            words.push_back(std::stoul(token, nullptr, 16));
        } catch (...) {
            return result;
        }
    }

    if (words.empty()) {
        return result;
    }

    // Words are in order: highest bits first, lowest bits last
    // Reverse so index 0 = bits 0-31, index 1 = bits 32-63, etc.
    std::reverse(words.begin(), words.end());

    // Single-touch: ABS_X=0, ABS_Y=1 → both in word[0], mask 0x3
    if (words.size() >= 1 && (words[0] & 0x3) == 0x3) {
        result.has_single_touch = true;
    }

    // Multitouch: ABS_MT_POSITION_X=53, ABS_MT_POSITION_Y=54
    // Word index = 53/32 = 1 (bits 32-63)
    // Bit positions within word: 53-32=21, 54-32=22 → mask 0x600000
    if (words.size() >= 2 && (words[1] & 0x600000) == 0x600000) {
        result.has_multitouch = true;
    }

    return result;
}

struct Point {
    int x = 0;
    int y = 0;
};

struct TouchCalibration {
    bool valid = false;
    float a = 1.0f, b = 0.0f, c = 0.0f; // screen_x = a*x + b*y + c
    float d = 0.0f, e = 1.0f, f = 0.0f; // screen_y = d*x + e*y + f
    bool axes_swapped = false;          // auto-detected axis swap
};

/**
 * @brief Compute affine calibration coefficients from 3 point pairs
 *
 * Uses the Maxim Integrated AN5296 algorithm (determinant-based).
 * Screen points are where targets appear on display.
 * Touch points are raw coordinates from touch controller.
 *
 * @param screen_points 3 screen coordinate targets
 * @param touch_points 3 corresponding raw touch coordinates
 * @param out Output calibration coefficients
 * @return true if successful, false if points are degenerate (collinear)
 */
bool compute_calibration(const Point screen_points[3], const Point touch_points[3],
                         TouchCalibration& out);

/**
 * @brief Transform raw touch point to screen coordinates
 *
 * @param cal Calibration coefficients (must be valid)
 * @param raw Raw touch point from controller
 * @param max_x Optional maximum X value for clamping (0 = no clamp)
 * @param max_y Optional maximum Y value for clamping (0 = no clamp)
 * @return Transformed screen coordinates (or raw if cal.valid is false)
 */
Point transform_point(const TouchCalibration& cal, Point raw, int max_x = 0, int max_y = 0);

/**
 * @brief Validate calibration coefficients are finite and within reasonable bounds
 *
 * @param cal Calibration to validate
 * @return true if all coefficients are finite and within bounds
 */
bool is_calibration_valid(const TouchCalibration& cal);

/**
 * @brief Validate calibration result by checking back-transform residuals
 *
 * Transforms each raw calibration point through the matrix and checks how close
 * the result is to the expected screen position. Also checks that the center
 * of the touch range maps to somewhere on-screen.
 *
 * @param cal Computed calibration to validate
 * @param screen_points 3 expected screen positions
 * @param touch_points 3 raw touch positions used to compute cal
 * @param screen_width Display width in pixels
 * @param screen_height Display height in pixels
 * @param max_residual Maximum allowed back-transform error in pixels (default: 10)
 * @return true if calibration passes validation
 */
bool validate_calibration_result(const TouchCalibration& cal, const Point screen_points[3],
                                 const Point touch_points[3], int screen_width, int screen_height,
                                 float max_residual = 10.0f);

/**
 * @brief Check if swapping touch X/Y axes produces a significantly better calibration
 *
 * Some touchscreen controllers report X/Y axes swapped relative to the display.
 * This function detects that by comparing the cross-coupling ratio of the original
 * calibration vs one computed with swapped touch coordinates.
 *
 * @param screen_points 3 screen coordinate targets
 * @param touch_points 3 raw touch coordinates
 * @param original_cal Calibration computed from the original (unswapped) points
 * @return true if swapping axes produces a significantly better (lower cross-coupling) calibration
 */
bool calibration_suggests_axis_swap(const Point screen_points[3], const Point touch_points[3],
                                    const TouchCalibration& original_cal);

/// Maximum reasonable coefficient value for validation
constexpr float MAX_CALIBRATION_COEFFICIENT = 1000.0f;

/**
 * @brief Check if a sysfs phys path indicates a USB-connected input device
 *
 * USB HID touchscreens (HDMI displays like BTT 5") report mapped coordinates
 * natively and do not need affine calibration. Only resistive/platform
 * touchscreens (sun4i_ts on AD5M, etc.) need the calibration wizard.
 *
 * USB devices have physical paths like "usb-0000:01:00.0-1.3/input0".
 * Platform devices have empty phys or paths like "sun4i_ts" without "usb".
 *
 * @param phys The sysfs phys string from /sys/class/input/eventN/device/phys
 * @return true if the device is USB-connected
 */
inline bool is_usb_input_phys(const std::string& phys) {
    return phys.find("usb") != std::string::npos;
}

/**
 * @brief Check if any pattern from a null-terminated array matches a device name
 *
 * Performs case-insensitive substring matching against the given patterns.
 *
 * @param name The device name to check
 * @param patterns Null-terminated array of lowercase pattern strings
 * @return true if any pattern matches
 */
inline bool matches_any_pattern(const std::string& name, const char* const patterns[]) {
    std::string lower_name = name;
    for (auto& c : lower_name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    for (int i = 0; patterns[i] != nullptr; ++i) {
        if (lower_name.find(patterns[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check if a device name matches known touchscreen patterns
 *
 * Used during touch device auto-detection to prefer known touchscreen
 * controllers. Performs case-insensitive substring matching against a list
 * of known touchscreen name patterns.
 *
 * @param name The device name from /sys/class/input/eventN/device/name
 * @return true if the name matches a known touchscreen pattern
 */
inline bool is_known_touchscreen_name(const std::string& name) {
    static const char* patterns[] = {"rtp",    // Resistive touch panel (sun4i_ts on AD5M)
                                     "touch",  // Generic touchscreen
                                     "sun4i",  // Allwinner touch controller
                                     "ft5x",   // FocalTech touch controllers
                                     "goodix", // Goodix touch controllers
                                     "gt9",    // Goodix GT9xx series
                                     "ili2",   // ILI touch controllers
                                     "atmel",  // Atmel touch controllers
                                     "edt-ft", // EDT FocalTech displays
                                     "tsc",    // Touch screen controller
                                     nullptr};
    return matches_any_pattern(name, patterns);
}

/**
 * @brief Check if a device name matches a known resistive touchscreen controller
 *
 * Only resistive touchscreens need affine calibration. Capacitive controllers
 * (Goodix, FocalTech, ILI, Atmel, EDT-FT) are factory-calibrated and report
 * mapped screen coordinates via their kernel driver.
 *
 * @param name The device name from /sys/class/input/eventN/device/name
 * @return true if the name matches a known resistive touchscreen controller
 */
inline bool is_resistive_touchscreen_name(const std::string& name) {
    static const char* patterns[] = {"rtp",   // Resistive touch panel
                                     "sun4i", // Allwinner resistive controller (AD5M)
                                     "tsc",   // Generic resistive touch screen controller
                                     "ns20",  // NS2009/NS2016 I2C resistive ADC (Nebula Pad)
                                     nullptr};
    return matches_any_pattern(name, patterns);
}

/**
 * @brief Determine if a touch input device needs affine calibration
 *
 * Single source of truth for calibration decisions. Returns true ONLY for
 * resistive touchscreens that need the calibration wizard.
 *
 * Devices that do NOT need calibration:
 * - USB HID touchscreens (report mapped coordinates natively)
 * - I2C capacitive touchscreens (Goodix, FocalTech, etc. — factory-calibrated)
 * - Virtual/uinput devices (VNC virtual touchscreen, testing)
 * - Non-touch devices used as pointer fallback (CEC remotes, etc.)
 * - Unknown devices (safer to skip than show broken calibration)
 *
 * @param name Device name from /sys/class/input/eventN/device/name
 * @param phys Device phys from /sys/class/input/eventN/device/phys
 * @param has_abs_xy Whether device has ABS_X/ABS_Y capabilities
 * @return true if calibration wizard should be shown for this device
 */
inline bool device_needs_calibration(const std::string& name, const std::string& phys,
                                     bool has_abs_xy) {
    // No ABS_X/ABS_Y = not a touchscreen, nothing to calibrate
    if (!has_abs_xy) {
        return false;
    }

    // USB HID touchscreens report mapped coordinates natively
    if (is_usb_input_phys(phys)) {
        return false;
    }

    // Virtual/uinput devices (VNC injection, testing) don't need calibration
    // These have empty phys and names like "virtual-touchscreen"
    if (name.find("virtual") != std::string::npos) {
        return false;
    }

    // Only known resistive touchscreen controllers need affine calibration.
    // Capacitive controllers (Goodix, FocalTech, ILI, Atmel) are factory-calibrated
    // and report mapped screen coordinates — even when connected via I2C, not USB.
    return is_resistive_touchscreen_name(name);
}

/**
 * @brief Check if an ABS range value looks like a generic HID resolution-independent range
 *
 * USB HID touchscreens report generic ranges (4096, 32767, 65535, etc.) that LVGL's
 * evdev driver maps linearly to screen coordinates. These work correctly without
 * calibration regardless of display resolution.
 *
 * In contrast, platform touchscreens (Goodix, FocalTech) report ABS ranges that
 * correspond to an actual panel resolution (e.g., 800x480), which can mismatch
 * the display if wired to a different-resolution panel.
 *
 * @param value ABS maximum value to check
 * @return true if value looks like a generic HID range (not a real panel resolution)
 */
inline bool is_generic_hid_abs_range(int value) {
    // Common generic HID touchscreen ranges (resolution-independent)
    // These are typically powers-of-2 minus 1, or round powers-of-2
    static const int generic_ranges[] = {
        255,   // 8-bit
        1023,  // 10-bit
        4095,  // 12-bit (very common: BTT HDMI5, many USB HID panels)
        4096,  // 12-bit (alternate)
        8191,  // 13-bit
        16383, // 14-bit
        32767, // 15-bit (common USB HID)
        65535, // 16-bit
    };

    for (int range : generic_ranges) {
        if (value == range) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check if touch ABS range doesn't match display resolution
 *
 * For capacitive screens that report coordinates for a different resolution
 * than the actual display (e.g., Goodix on SV06 Ace: ABS 800x480, display 480x272).
 * When there's a mismatch, the calibration wizard should be shown even for
 * capacitive touchscreens that are normally "factory calibrated".
 *
 * Skips generic HID ranges (4096, 32767, etc.) which are resolution-independent
 * and correctly mapped by LVGL's evdev linear interpolation.
 *
 * @param abs_max_x Maximum ABS_X value from EVIOCGABS
 * @param abs_max_y Maximum ABS_Y value from EVIOCGABS
 * @param display_width Display width in pixels
 * @param display_height Display height in pixels
 * @return true if ABS range mismatches display resolution beyond tolerance
 */
inline bool has_abs_display_mismatch(int abs_max_x, int abs_max_y, int display_width,
                                     int display_height) {
    // Can't determine mismatch with invalid ranges
    if (abs_max_x <= 0 || abs_max_y <= 0 || display_width <= 0 || display_height <= 0) {
        return false;
    }

    // Generic HID ranges (4096, 32767, etc.) are resolution-independent —
    // LVGL's evdev driver maps them linearly to screen coords. No mismatch.
    if (is_generic_hid_abs_range(abs_max_x) && is_generic_hid_abs_range(abs_max_y)) {
        return false;
    }

    // Allow ~5% tolerance for rounding differences in ABS ranges
    constexpr float TOLERANCE = 0.05f;

    float x_ratio =
        static_cast<float>(std::abs(abs_max_x - display_width)) / static_cast<float>(display_width);
    float y_ratio = static_cast<float>(std::abs(abs_max_y - display_height)) /
                    static_cast<float>(display_height);

    return (x_ratio > TOLERANCE) || (y_ratio > TOLERANCE);
}

} // namespace helix
