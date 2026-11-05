// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

// Forward declarations
namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

/**
 * @file ui_print_light_timelapse.h
 * @brief Light and timelapse button controls extracted from PrintStatusPanel
 *
 * Manages the light and timelapse toggle buttons on the print status panel:
 * - Light button: Toggles the chamber light and shows its state
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
     * Registers the light_button_icon and timelapse_button_icon subjects and
     * the light XML event callback.
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
    void set_api(helix::IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Follow the printer's global timelapse setting
     *
     * The button shows that setting, and a successful toggle writes it back here,
     * so the button and the pre-print default cannot disagree.
     */
    void set_printer_state(helix::PrinterState& printer_state);

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

    /// Show the chamber light's power on the light button icon. Runs on every
    /// led_state_version bump once init_subjects() has run.
    void refresh_light_state();
    /// Icon and translated On/Off label for timelapse_enabled_.
    void refresh_timelapse_display();

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

    helix::IMoonrakerAPI* api_ = nullptr;
    helix::PrinterState* printer_state_ = nullptr;

    //
    // === Subject Management ===
    //

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    //
    // === Light State ===
    //

    ObserverGuard led_state_observer_;
    ObserverGuard language_observer_;
    lv_subject_t light_button_subject_{};
    char light_button_buf_[8] = "\xF3\xB0\x8C\xB6"; // MDI lightbulb_outline (off state)

    //
    // === Timelapse State ===
    //

    bool timelapse_enabled_ = false;
    ObserverGuard timelapse_enabled_observer_;
    lv_subject_t timelapse_button_subject_{};
    char timelapse_button_buf_[8] = ""; // MDI video/video-off icon

    helix::AsyncLifetimeGuard lifetime_;
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
