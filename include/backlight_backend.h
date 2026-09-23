// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>

// Lowest raw backlight level a panel still renders visibly, as a percent of
// the raw range. 0 (Makefile default) keeps the plain linear percent map;
// per-target values live in mk/cross.mk. /display/backlight_floor_percent in
// settings.json overrides it per panel.
#ifndef HELIX_BACKLIGHT_FLOOR_PERCENT
#define HELIX_BACKLIGHT_FLOOR_PERCENT 0
#endif

/**
 * @brief Abstract backlight control interface
 *
 * Provides platform-agnostic backlight brightness control with intelligent
 * hardware probing. Supports multiple backends for different hardware:
 *
 * - **Sysfs**: Standard Linux backlight interface (/sys/class/backlight/)
 *   Used by Raspberry Pi and most Linux systems with proper driver support.
 *
 * - **Allwinner**: Direct ioctl on /dev/disp for Allwinner SoCs (AD5M, sunxi)
 *   Used when sysfs backlight isn't exposed by the kernel.
 *
 * - **BrightnessCLI**: Shells out to the Creality Sonic Pad `brightness` helper
 *   (`brightness -s 0|1`, `brightness -d 0..255`). For Sonic Pad variants where
 *   the sysfs/ioctl paths don't actually drive the panel (#972).
 *
 * - **None**: No-op backend for platforms without hardware control.
 *   In test mode, simulates brightness for UI testing.
 *
 * Factory auto-detection order:
 * 1. Test mode → None (simulated, UI works normally)
 * 2. HELIX_BACKLIGHT_DEVICE env override
 * 3. Sysfs (most portable Linux approach)
 * 4. Allwinner ioctl (AD5M/sunxi specific)
 * 5. BrightnessCLI (Sonic Pad `brightness` tool, if present)
 * 6. None fallback (no hardware control)
 *
 * Usage:
 * @code
 * auto backend = BacklightBackend::create();
 * spdlog::info("Using {} backlight backend", backend->name());
 *
 * if (backend->is_available()) {
 *     backend->set_brightness(75);  // 75%
 *     int current = backend->get_brightness();
 * }
 * @endcode
 */
class BacklightBackend {
  public:
    virtual ~BacklightBackend() = default;

    /**
     * @brief Set backlight brightness
     *
     * @param percent Brightness percentage (0-100). 0 turns off backlight completely.
     * @return true if brightness was set successfully, false on error
     */
    virtual bool set_brightness(int percent) = 0;

    /**
     * @brief Get current backlight brightness
     *
     * @return Brightness percentage (0-100), or -1 if unable to read
     */
    virtual int get_brightness() const = 0;

    /**
     * @brief Check if this backend can control the backlight
     *
     * For hardware backends, this verifies the device is accessible.
     * For the None backend in test mode, returns true (simulated).
     * For the None backend in production, returns false (no hardware).
     *
     * @return true if backlight control is available
     */
    virtual bool is_available() const = 0;

    /**
     * @brief Get backend name for logging
     *
     * @return Backend identifier ("Sysfs", "Allwinner", "None", "Simulated")
     */
    virtual const char* name() const = 0;

    /**
     * @brief Check if this backend supports reliable hardware display blanking
     *
     * Only returns true for backends with known-good FBIOBLANK / backlight
     * hardware (e.g. Allwinner on AD5M). All other backends default to false,
     * causing DisplayManager to use a software black overlay for sleep instead.
     *
     * @return true if hardware blank/unblank is reliable on this platform
     */
    virtual bool supports_hardware_blank() const {
        return false;
    }

    /**
     * @brief Check if this backend supports continuous brightness dimming
     *
     * Binary backlights (e.g. GPIO with max_brightness=1) can only be fully
     * on or off — intermediate values get truncated to 0, turning the screen
     * black. Returns false for such backends so the UI can hide the brightness
     * slider and skip the dim-before-sleep transition.
     *
     * @return true if brightness can be smoothly adjusted (PWM/analog)
     */
    virtual bool supports_dimming() const {
        return true;
    }

    /**
     * @brief Factory: create best available backend with auto-detection
     *
     * Detection order:
     * 1. Test mode check → Simulated (None with tracking)
     * 2. HELIX_BACKLIGHT_DEVICE env var ("sysfs", "allwinner", "brightness", "none")
     * 3. Sysfs (/sys/class/backlight/)
     * 4. Allwinner (/dev/disp with ioctl)
     * 5. BrightnessCLI (Sonic Pad `brightness` tool)
     * 6. None fallback
     *
     * @return Unique pointer to selected backend (never null)
     */
    static std::unique_ptr<BacklightBackend> create();

#ifdef __linux__
    /**
     * @brief Create a sysfs backend with a custom base path (for testing)
     *
     * @param base_path Directory to scan for backlight devices (default: /sys/class/backlight)
     * @return Unique pointer to sysfs backend (may not be available if path has no devices)
     */
    static std::unique_ptr<BacklightBackend> create_sysfs(const std::string& base_path);
#endif
};
