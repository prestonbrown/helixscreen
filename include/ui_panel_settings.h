// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h" // For ObserverGuard RAII wrapper
#include "ui_panel_base.h"

#include "subject_managed_panel.h" // For SubjectManager

#include <memory>
#include <string>
#include <vector>

class ChangeHostModal;

/**
 * @file ui_panel_settings.h
 * @brief Settings panel - Scrolling list of app and printer settings
 *
 * A comprehensive settings panel with sections for Appearance, Printer,
 * Notifications, System, and About information.
 *
 * ## Key Features:
 * - Dark mode toggle with immediate theme switching
 * - Display sleep timeout configuration
 * - LED light control (via Moonraker)
 * - Sound and notification settings (placeholder)
 * - System info display (version, printer, Klipper)
 *
 * ## Architecture:
 * Uses SettingsManager for reactive data binding and persistence.
 * Toggle switches automatically sync with SettingsManager subjects.
 *
 * @see SettingsManager for data layer
 * @see PanelBase for base class documentation
 */
class SettingsPanel : public PanelBase {
  public:
    /**
     * @brief Construct SettingsPanel with injected dependencies
     *
     * @param printer_state Reference to PrinterState
     * @param api Pointer to IMoonrakerAPI
     */
    SettingsPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    ~SettingsPanel() override;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Initialize SettingsManager subjects
     *
     * Must be called BEFORE XML creation to enable data binding.
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Calls lv_subject_deinit() on all local lv_subject_t members.
     * Must be called before lv_deinit() to prevent dangling observers.
     * Follows [L041] pattern for subject init/deinit symmetry.
     */
    void deinit_subjects();

    /**
     * @brief Setup the settings panel with event handlers and bindings
     *
     * Wires up toggle switches, dropdown, and action row click handlers.
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen (for overlay panel creation)
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Settings Panel";
    }
    const char* get_xml_component_name() const override {
        return "settings_panel";
    }

  private:
    //
    // === Widget References ===
    //

    // Toggle switches
    lv_obj_t* dark_mode_switch_ = nullptr;
    lv_obj_t* animations_switch_ = nullptr;
    lv_obj_t* gcode_3d_switch_ = nullptr;
    lv_obj_t* led_light_switch_ = nullptr;
    lv_obj_t* estop_confirm_switch_ = nullptr;
    lv_obj_t* telemetry_switch_ = nullptr;
    // Dropdowns
    lv_obj_t* completion_alert_dropdown_ = nullptr;
    lv_obj_t* display_sleep_dropdown_ = nullptr;
    lv_obj_t* language_dropdown_ = nullptr;
    // LED chip selection moved to LedSettingsOverlay

    // Restart prompt dialog
    lv_obj_t* restart_prompt_dialog_ = nullptr;

    // Action rows (clickable)
    lv_obj_t* display_settings_row_ = nullptr;
    lv_obj_t* filament_sensors_row_ = nullptr;
    lv_obj_t* network_row_ = nullptr;
    lv_obj_t* factory_reset_row_ = nullptr;

    // Change host modal is owned by helix::ui::show_change_host_modal(); the
    // connection-failed prompt reaches the same dialog, and ChangeHostModal keeps
    // a static active_instance_, so a second owner here would fight it.

    // LED state observer (syncs toggle with printer LED state)
    ObserverGuard led_state_observer_;

    //
    // === Reactive Subjects ===
    //

    /// RAII manager for automatic subject cleanup
    SubjectManager subjects_;

    // Info row subjects
    lv_subject_t printer_host_value_subject_;

    // Visibility subjects (controls which settings are shown)
    lv_subject_t show_touch_calibration_subject_;

    // Platform visibility subjects (Android hides these)
    lv_subject_t show_network_settings_subject_;
    lv_subject_t show_update_settings_subject_;
    // 1 when updates are managed by the device firmware (HELIX_DISABLE_AUTO_UPDATES):
    // hides the in-app check/install controls and shows a static notice instead.
    lv_subject_t updates_firmware_managed_subject_;
    // 1 when in-app updates are suppressed for a NON-firmware reason (self-update is
    // physically impossible because the install tree isn't writable). Drives a
    // neutral "updates aren't available" notice, mutually exclusive with the
    // firmware-managed notice above.
    lv_subject_t updates_unavailable_subject_;

    // Touch calibration status subject
    lv_subject_t touch_cal_status_subject_;
    char touch_cal_status_buf_[48]; // e.g., "Calibrated" or "Not calibrated"

    // Static buffers for string subjects
    char printer_host_value_buf_[96]; // e.g., "192.168.1.100:7125"

    // Note: Machine Limits overlay is now managed by MachineLimitsOverlay class
    // See ui_settings_machine_limits.h

    //
    // === Setup Helpers ===
    //

    void setup_toggle_handlers();
    void setup_dropdown();
    void setup_action_handlers();
    void populate_info_rows();

  public:
    /// Shown after any "requires restart" setting changes.
    void show_restart_prompt();

    /**
     * @brief Populate LED chips from discovered hardware
     *
     * Called after discovery completes. Creates chips for each discovered LED.
     */
    void populate_led_chips();

  private:
    //
    // === Event Handlers ===
    //

    void handle_dark_mode_changed(bool enabled);
    void handle_animations_changed(bool enabled);
    void handle_led_light_changed(bool enabled);
    void handle_led_settings_clicked();
    void handle_sound_settings_clicked();
    void handle_security_settings_clicked();
#if HELIX_HAS_LABEL_PRINTER
    void handle_label_printer_settings_clicked();
#endif
    void handle_estop_confirm_changed(bool enabled);
    void handle_cancel_escalation_changed(bool enabled);
    void handle_telemetry_changed(bool enabled);
    void handle_telemetry_view_data_clicked();

    void handle_debug_bundle_clicked();
    void handle_discord_clicked();
    void handle_docs_clicked();
    void handle_printers_clicked();
    void handle_display_settings_clicked();
    void handle_filament_sensors_clicked();
    void handle_fans_settings_clicked();
    void handle_ams_settings_clicked();
    void handle_spoolman_settings_clicked();
    void handle_macro_buttons_clicked();
    void handle_machine_limits_clicked();
    void handle_material_temps_clicked();
    void handle_change_host_clicked();
    void handle_network_clicked();
    void handle_power_devices_clicked();
    void handle_touch_calibration_clicked();
    void handle_restart_helix_clicked();
    void handle_factory_reset_clicked();
    void handle_about_clicked();
    // Note: populate_sensor_list() moved to SensorSettingsOverlay
    // Note: populate_macro_dropdowns() moved to MacroButtonsOverlay
    // Note: populate_hardware_issues() moved to HardwareHealthOverlay

  public:
    // Called by static modal callbacks - performs actual reset after confirmation
    void perform_factory_reset();

    // Called by toast action to navigate and open overlay
    void handle_hardware_health_clicked();

    // Called by plugin failure toast action to open plugins overlay
    void handle_plugins_clicked();

    // Opens the Performance overlay (System settings row)
    void handle_performance_clicked();

    // Note: handle_hardware_action() moved to HardwareHealthOverlay
    // See ui_settings_hardware_health.h

    // Dialog pointers accessible to static callbacks
    lv_obj_t* factory_reset_dialog_ = nullptr;

  public:
    //
    // === XML Callbacks (public for global registration) ===
    // These are registered before settings_panel.xml is parsed [L013]
    //
    static void on_animations_changed(lv_event_t* e);
    static void on_led_light_changed(lv_event_t* e);
    static void on_led_settings_clicked(lv_event_t* e);
    static void on_timelapse_settings_clicked(lv_event_t* e);
    static void on_sound_settings_clicked(lv_event_t* e);
    static void on_security_clicked(lv_event_t* e);
#if HELIX_HAS_LABEL_PRINTER
    static void on_label_printer_settings_clicked(lv_event_t* e);
#endif
    static void on_estop_confirm_changed(lv_event_t* e);
    static void on_cancel_escalation_changed(lv_event_t* e);
    static void on_debug_bundle_clicked(lv_event_t* e);
    static void on_discord_clicked(lv_event_t* e);
    static void on_docs_clicked(lv_event_t* e);
    static void on_telemetry_changed(lv_event_t* e);
    static void on_printers_clicked(lv_event_t* e);
    static void on_display_settings_clicked(lv_event_t* e);
    static void on_filament_sensors_clicked(lv_event_t* e);
    static void on_fans_settings_clicked(lv_event_t* e);
    static void on_ams_settings_clicked(lv_event_t* e);
    static void on_spoolman_settings_clicked(lv_event_t* e);
    static void on_macro_buttons_clicked(lv_event_t* e);
    static void on_machine_limits_clicked(lv_event_t* e);
    static void on_material_temps_clicked(lv_event_t* e);
    static void on_change_host_clicked(lv_event_t* e);
    static void on_network_clicked(lv_event_t* e);
    static void on_power_devices_clicked(lv_event_t* e);
    static void on_touch_calibration_clicked(lv_event_t* e);
    static void on_factory_reset_clicked(lv_event_t* e);
    static void on_hardware_health_clicked(lv_event_t* e);
    static void on_plugins_clicked(lv_event_t* e);
    static void on_system_performance_clicked(lv_event_t* e);
    static void on_telemetry_view_data(lv_event_t* e);
    static void on_restart_helix_settings_clicked(lv_event_t* e);
    static void on_about_clicked(lv_event_t* e);

    // Category navigation callbacks (open sub-panel overlays)
    static void on_display_sound_clicked(lv_event_t* e);
    static void on_printing_clicked(lv_event_t* e);
    static void on_hardware_clicked(lv_event_t* e);
    static void on_safety_clicked(lv_event_t* e);
    static void on_system_clicked(lv_event_t* e);
    static void on_help_clicked(lv_event_t* e);
    static void on_touch_input_clicked(lv_event_t* e);

  private:
    //
    // === Static Trampolines (private - only used internally) ===
    //
    static void on_dark_mode_changed(lv_event_t* e);

    // Static callbacks for overlays
    static void on_restart_later_clicked(lv_event_t* e);
    static void on_restart_now_clicked(lv_event_t* e);
    static void on_header_back_clicked(lv_event_t* e);
    // Note: on_brightness_changed is now in DisplaySettingsOverlay

    // Note: Hardware save confirmation callbacks moved to HardwareHealthOverlay
    // See ui_settings_hardware_health.h

    // Note: Machine limits overlay callbacks are now in MachineLimitsOverlay class
    // See ui_settings_machine_limits.h
};

// Global instance accessor (needed by main.cpp)
SettingsPanel& get_global_settings_panel();

// Register SettingsPanel callbacks for XML parsing (call before settings_panel.xml registration)
// This ensures callbacks exist when LVGL parses the XML component [L013]
void register_settings_panel_callbacks();
