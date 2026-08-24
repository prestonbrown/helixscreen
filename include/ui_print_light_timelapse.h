// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

// Forward declarations
class IMoonrakerAPI;
namespace helix {
class PrinterState;
}

/**
 * @file ui_print_light_timelapse.h
 * @brief Light and timelapse button controls extracted from PrintStatusPanel
 *
 * Manages the light and timelapse toggle buttons on the print status panel:
 * - Light button: Toggles configured LED on/off via Moonraker
 * - Timelapse button: Enables/disables timelapse recording via Moonraker plugin
 *
 * This class is a helper owned by PrintStatusPanel, not a standalone component.
 * It manages the subjects and callbacks for the buttons' reactive UI.
 *
 * @pattern Helper class with subject management
 * @threading Main thread only (LVGL)
 */
class PrintLightTimelapseControls {
  public:
    PrintLightTimelapseControls();
    ~PrintLightTimelapseControls();

    // Non-copyable
    PrintLightTimelapseControls(const PrintLightTimelapseControls&) = delete;
    PrintLightTimelapseControls& operator=(const PrintLightTimelapseControls&) = delete;

    /**
     * @brief Initialize subjects for XML binding
     *
     * Registers light_button_icon, timelapse_button_icon, timelapse_button_label
     * subjects and XML event callbacks.
     */
    void init_subjects();

    /**
     * @brief Deinitialize subjects
     *
     * Called during cleanup. Safe to call multiple times.
     */
    void deinit_subjects();

    /**
     * @brief Set the Moonraker API for sending commands
     * @param api Pointer to IMoonrakerAPI (can be nullptr for mock mode)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Set configured LEDs (multi-LED support)
     * @param leds Vector of LED names to control
     */
    void set_configured_leds(const std::vector<std::string>& leds) {
        configured_leds_ = leds;
    }

    /**
     * @brief Set single configured LED (compatibility shim)
     * @param led LED name
     */
    void set_configured_led(const std::string& led) {
        configured_leds_.clear();
        if (!led.empty()) {
            configured_leds_.push_back(led);
        }
    }

    /**
     * @brief Get configured LEDs
     * @return Vector of configured LED names (empty if none)
     */
    const std::vector<std::string>& get_configured_leds() const {
        return configured_leds_;
    }

    /**
     * @brief Handle light button click
     *
     * Toggles the LED state via Moonraker API.
     * No-op if no LED is configured.
     */
    void handle_light_button();

    /**
     * @brief Handle timelapse button click
     *
     * Toggles timelapse recording via Moonraker timelapse plugin.
     */
    void handle_timelapse_button();

    /**
     * @brief Update LED state from PrinterState observer
     *
     * Called when LED state changes (from PrinterState subject).
     * Updates the light button icon accordingly.
     *
     * @param on true if LED is on, false if off
     */
    void update_led_state(bool on);

    /**
     * @brief Check if subjects have been initialized
     * @return true if init_subjects() was called
     */
    bool are_subjects_initialized() const {
        return subjects_initialized_;
    }

  private:
    //
    // === Dependencies ===
    //

    IMoonrakerAPI* api_ = nullptr;

    //
    // === Subject Management ===
    //

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    //
    // === Light State ===
    //

    std::vector<std::string> configured_leds_;
    bool led_on_ = false;
    lv_subject_t light_button_subject_;
    char light_button_buf_[8] = "\xF3\xB0\x8C\xB6"; // MDI lightbulb_outline (off state)

    //
    // === Timelapse State ===
    //

    bool timelapse_enabled_ = false;
    lv_subject_t timelapse_button_subject_;
    lv_subject_t timelapse_label_subject_;
    char timelapse_button_buf_[8] = ""; // MDI video/video-off icon
    char timelapse_label_buf_[16] = "Off";
};

/**
 * @brief Get global PrintLightTimelapseControls instance
 *
 * Used by XML event callbacks to route events to the controls instance.
 * The instance is managed by PrintStatusPanel.
 *
 * @return Reference to PrintLightTimelapseControls
 */
PrintLightTimelapseControls& get_global_light_timelapse_controls();

/**
 * @brief Set global PrintLightTimelapseControls instance
 *
 * Called by PrintStatusPanel during initialization.
 *
 * @param instance Pointer to PrintLightTimelapseControls (or nullptr to clear)
 */
void set_global_light_timelapse_controls(PrintLightTimelapseControls* instance);
