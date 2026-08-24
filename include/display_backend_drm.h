// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - Linux DRM/KMS Display Backend
//
// Modern Linux display backend using Direct Rendering Manager (DRM)
// with Kernel Mode Setting (KMS). Preferred for Raspberry Pi.

#pragma once

#ifdef HELIX_DISPLAY_DRM

#include "display_backend.h"
#include "touch_calibration_wrapper.h"

#include <string>

/**
 * @brief Linux DRM/KMS display backend for modern embedded systems
 *
 * Uses LVGL's DRM driver for hardware-accelerated rendering on
 * systems with GPU support (like Raspberry Pi 4/5).
 *
 * Advantages over framebuffer:
 * - Better performance with GPU acceleration
 * - Proper vsync support
 * - Multiple display support
 * - Modern display pipeline
 *
 * Features:
 * - Direct DRM/KMS access via /dev/dri/card0
 * - Touch input via libinput (preferred) or evdev
 * - Automatic display mode detection
 *
 * Requirements:
 * - /dev/dri/card0 must exist and be accessible
 * - User must be in 'video' and 'input' groups
 * - libdrm and libinput libraries
 */
class DisplayBackendDRM : public DisplayBackend {
  public:
    /**
     * @brief Construct DRM backend with default settings
     *
     * Defaults:
     * - DRM device: /dev/dri/card0
     * - Connector: auto-detect first connected
     */
    DisplayBackendDRM();

    /**
     * @brief Construct DRM backend with custom device path
     *
     * @param drm_device Path to DRM device (e.g., "/dev/dri/card0")
     */
    explicit DisplayBackendDRM(const std::string& drm_device);

    ~DisplayBackendDRM() override;

    // Display creation
    lv_display_t* create_display(int width, int height) override;

    // Input device creation
    lv_indev_t* create_input_pointer() override;
    lv_indev_t* create_input_keyboard() override;

    // Display rotation via DRM plane property
    void set_display_rotation(lv_display_rotation_t rot, int phys_w, int phys_h) override;

    /// Check if DRM plane supports hardware rotation for the given angle
    bool supports_hardware_rotation(lv_display_rotation_t rot) const override;

    // Backend info
    DisplayBackendType type() const override {
        return DisplayBackendType::DRM;
    }
    const char* name() const override {
        return "Linux DRM/KMS";
    }
    bool is_available() const override;
    DetectedResolution detect_resolution() const override;

    // Framebuffer operations
    bool clear_framebuffer(uint32_t color) override;

    // Real panel power-off via DRM connector DPMS (#1049). This is the actual
    // power path for HDMI/no-backlight devices (CB1, Pi) where there is no
    // sysfs/ioctl backlight and a software overlay leaves the panel lit. Uses the
    // DRM master fd LVGL already holds (lv_linux_drm_get_fd) — DPMS modesetting
    // requires master, which the LVGL DRM driver acquired via drmSetMaster().
    bool supports_power_off() const override;
    bool power_off() override;
    bool power_on() override;

    // Configuration
    void set_drm_device(const std::string& path) {
        drm_device_ = path;
    }

    /**
     * @brief Tell the backend the user explicitly requested a size via -s.
     *
     * When true, the backend will log warnings and enqueue toasts if the
     * requested resolution cannot be honored. Must be called before
     * create_display().
     */
    void set_size_was_explicit(bool explicit_size) override {
        size_was_explicit_ = explicit_size;
    }

    /// Whether GPU-accelerated rendering (EGL/OpenGL ES) is active
    bool is_gpu_accelerated() const override {
        return using_egl_;
    }

    // Touch calibration
    bool set_calibration(const helix::TouchCalibration& cal) override;
    helix::TouchCalibration get_calibration() const override {
        return calibration_;
    }
    bool needs_touch_calibration() const override {
        return needs_calibration_;
    }
    bool supports_touch_calibration() const override {
        return supports_calibration_;
    }
    void disable_affine_calibration() override;
    void enable_affine_calibration() override;
    void clear_calibration() override;

  private:
    /// Switch VT to KD_GRAPHICS so fbcon stops painting over DRM output.
    /// Required on kernel 6.x where sun4i-drm registers DRM fbdev emulation.
    void suppress_console();
    void restore_console();

    /// Drive the active connector's DPMS property (DRM_MODE_DPMS_ON/OFF) using
    /// the DRM master fd LVGL holds. Returns true if the property was set on the
    /// connected connector. on=true powers on, on=false powers off (#1049).
    bool set_connector_dpms(bool on);

    std::string drm_device_;
    lv_display_t* display_ = nullptr;
    lv_indev_t* pointer_ = nullptr;
    lv_indev_t* mouse_ = nullptr;
    lv_indev_t* keyboard_ = nullptr;
    int tty_fd_ = -1;        ///< TTY fd for KD_GRAPHICS console suppression
    bool using_egl_ = false; ///< Track if GPU-accelerated path is active
    helix::TouchCalibration calibration_;
    helix::CalibrationContext calibration_context_;
    /// True once install_calibration_wrapper() has wired calibrated_read_cb onto
    /// pointer_ with user_data = &calibration_context_. Tracks install state so
    /// calibration updates never re-probe the indev's user_data (which can be
    /// stale/corrupted — bundle LG9X482B) to decide whether to install.
    bool calibration_wrapper_installed_ = false;
    /// Auto-fire the first-run wizard (resistive controllers, broken ABS ranges)
    bool needs_calibration_ = false;
    /// Offer the manual Settings entry point (any real touch panel)
    bool supports_calibration_ = false;
    int screen_width_ = 0;
    int screen_height_ = 0;
    bool size_was_explicit_ = false;
};

#endif // HELIX_DISPLAY_DRM
