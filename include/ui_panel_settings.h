// Copyright 2025 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_base.h"

/**
 * @file ui_panel_settings.h
 * @brief Settings panel - Scrolling list of app and printer settings
 *
 * A comprehensive settings panel with sections for Appearance, Printer,
 * Notifications, Calibration, System, and About information.
 *
 * ## Key Features:
 * - Dark mode toggle with immediate theme switching
 * - Display sleep timeout configuration
 * - LED light control (via Moonraker)
 * - Sound and notification settings (placeholder)
 * - Calibration launchers (Bed Mesh, Z-Offset, PID Tuning)
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
     * @param api Pointer to MoonrakerAPI
     */
    SettingsPanel(PrinterState& printer_state, MoonrakerAPI* api);

    ~SettingsPanel() override = default;

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
    lv_obj_t* led_light_switch_ = nullptr;
    lv_obj_t* sounds_switch_ = nullptr;
    lv_obj_t* completion_alert_switch_ = nullptr;

    // Dropdown
    lv_obj_t* display_sleep_dropdown_ = nullptr;

    // Action rows (clickable)
    lv_obj_t* display_settings_row_ = nullptr;
    lv_obj_t* bed_mesh_row_ = nullptr;
    lv_obj_t* z_offset_row_ = nullptr;
    lv_obj_t* pid_tuning_row_ = nullptr;
    lv_obj_t* network_row_ = nullptr;
    lv_obj_t* factory_reset_row_ = nullptr;

    // Info rows (for dynamic updates)
    lv_obj_t* version_value_ = nullptr;
    lv_obj_t* printer_value_ = nullptr;
    lv_obj_t* klipper_value_ = nullptr;
    lv_obj_t* moonraker_value_ = nullptr;

    // Lazily-created overlay panels
    lv_obj_t* display_settings_overlay_ = nullptr;
    lv_obj_t* network_settings_overlay_ = nullptr;
    lv_obj_t* bed_mesh_panel_ = nullptr;
    lv_obj_t* zoffset_cal_panel_ = nullptr;
    lv_obj_t* pid_cal_panel_ = nullptr;
    lv_obj_t* factory_reset_dialog_ = nullptr;
    lv_obj_t* theme_restart_dialog_ = nullptr;

    //
    // === Setup Helpers ===
    //

    void setup_toggle_handlers();
    void setup_dropdown();
    void setup_action_handlers();
    void populate_info_rows();

    //
    // === Event Handlers ===
    //

    void handle_dark_mode_changed(bool enabled);
    void handle_display_sleep_changed(int index);
    void handle_led_light_changed(bool enabled);
    void handle_sounds_changed(bool enabled);
    void handle_completion_alert_changed(bool enabled);

    void handle_display_settings_clicked();
    void handle_bed_mesh_clicked();
    void handle_z_offset_clicked();
    void handle_pid_tuning_clicked();
    void handle_network_clicked();
    void handle_factory_reset_clicked();
    void show_theme_restart_dialog();

    //
    // === Static Trampolines ===
    //

    static void on_dark_mode_changed(lv_event_t* e);
    static void on_display_sleep_changed(lv_event_t* e);
    static void on_led_light_changed(lv_event_t* e);
    static void on_sounds_changed(lv_event_t* e);
    static void on_completion_alert_changed(lv_event_t* e);

    static void on_display_settings_clicked(lv_event_t* e);
    static void on_bed_mesh_clicked(lv_event_t* e);
    static void on_z_offset_clicked(lv_event_t* e);
    static void on_pid_tuning_clicked(lv_event_t* e);
    static void on_network_clicked(lv_event_t* e);
    static void on_factory_reset_clicked(lv_event_t* e);
};

// Global instance accessor (needed by main.cpp)
SettingsPanel& get_global_settings_panel();
