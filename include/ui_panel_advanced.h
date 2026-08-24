// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_base.h"
#include "ui_panel_history_dashboard.h"
#include "ui_plugin_install_modal.h"
#include "ui_shutdown_modal.h"

#include "async_lifetime_guard.h"
#include "helix_plugin_installer.h"

/**
 * @file ui_panel_advanced.h
 * @brief Advanced Panel - Hub for advanced printer tools and calibration
 *
 * The Advanced Panel serves as a navigation hub for advanced features including:
 * - Bed Leveling (auto mesh, manual screws, QGL, Z-tilt)
 * - Input Shaping (resonance testing, Klippain Shake&Tune)
 * - Spoolman (filament tracking and inventory)
 * - Z-Offset Calibration
 * - Macro Browser (execute printer macros)
 * - Diagnostics (console, restart options)
 *
 * ## Architecture:
 *
 * This panel uses the hub pattern - it's a scrollable list of action rows that
 * navigate to dedicated overlay panels for each feature. The hub itself is
 * stateless; all feature logic lives in the sub-panels.
 *
 * ## Capability-Driven UI:
 *
 * Features are conditionally shown based on PrinterCapabilities:
 * - Input Shaping requires accelerometer
 * - Spoolman requires Spoolman service
 * - Z-Offset requires probe
 *
 * @see PanelBase for base class documentation
 * @see PrinterCapabilities for capability detection
 */
class AdvancedPanel : public PanelBase {
  public:
    /**
     * @brief Construct AdvancedPanel with injected dependencies
     *
     * @param printer_state Reference to helix::PrinterState
     * @param api Pointer to IMoonrakerAPI
     */
    AdvancedPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    ~AdvancedPanel() override = default;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Initialize capability-related subjects for XML binding
     *
     * Creates subjects for:
     * - printer_has_accelerometer
     * - printer_has_spoolman
     *
     * Note: printer_has_probe is already created by SettingsPanel.
     */
    void init_subjects() override;

    /**
     * @brief Setup the advanced panel hub with navigation handlers
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen for overlay creation
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Advanced Panel";
    }
    const char* get_xml_component_name() const override {
        return "advanced_panel";
    }

    //
    // === Lifecycle Hooks ===
    //

    /**
     * @brief Refresh capability flags when panel becomes visible
     *
     * Updates subjects based on current PrinterCapabilities.
     */
    void on_activate() override;

  private:
    //
    // === Navigation Handlers ===
    //

    void handle_spoolman_clicked();
    void handle_macros_clicked();
    void handle_console_clicked();
    void handle_history_clicked();
    void handle_configure_print_start_clicked();
    void handle_pid_tuning_clicked();
    void handle_timelapse_setup_clicked();
    void handle_helix_plugin_install_clicked();
    void handle_helix_plugin_uninstall_clicked();
    void handle_phase_tracking_changed(bool enabled);

    // Both POWER rows (Shutdown, Reboot) open the same shared dialog — the
    // dialog itself presents shutdown vs. reboot buttons and, on dual-host
    // setups, the printer/screen/both scope. Single handler reflects that.
    void handle_power_clicked();

    //
    // === Static Event Callbacks (registered via lv_xml_register_event_cb) ===
    //

    static void on_spoolman_clicked(lv_event_t* e);
    static void on_macros_clicked(lv_event_t* e);
    static void on_console_clicked(lv_event_t* e);
    static void on_history_clicked(lv_event_t* e);
    static void on_configure_print_start_clicked(lv_event_t* e);
    static void on_pid_tuning_clicked(lv_event_t* e);
    static void on_timelapse_videos_clicked(lv_event_t* e);
    static void on_timelapse_setup_clicked(lv_event_t* e);
    static void on_helix_plugin_install_clicked(lv_event_t* e);
    static void on_helix_plugin_uninstall_clicked(lv_event_t* e);
    static void on_phase_tracking_changed(lv_event_t* e);
    static void on_advanced_power_clicked(lv_event_t* e);

    //
    // === HelixPrint Plugin Support ===
    //

    helix::HelixPluginInstaller plugin_installer_;
    PluginInstallModal plugin_install_modal_;

    //
    // === Shared Power Dialog ===
    //
    // Reuses the same modal the home-panel power widget shows. The lifetime
    // guard is required by the "Both" flow (defers screen-side SystemPower
    // call until the printer-side ack lands on the WS background thread).

    ShutdownModal shutdown_modal_;
    helix::AsyncLifetimeGuard shutdown_lifetime_;

    //
    // === Cached Overlay Panels ===
    //

    lv_obj_t* spoolman_panel_ = nullptr;
    lv_obj_t* macros_panel_ = nullptr;
    lv_obj_t* console_panel_ = nullptr;
    lv_obj_t* history_dashboard_panel_ = nullptr;
};

/**
 * @brief Global instance accessor
 *
 * Returns reference to singleton AdvancedPanel used by main.cpp.
 */
AdvancedPanel& get_global_advanced_panel();

/**
 * @brief Initialize the global AdvancedPanel instance
 *
 * Must be called by main.cpp before accessing get_global_advanced_panel().
 *
 * @param printer_state Reference to helix::PrinterState
 * @param api Pointer to IMoonrakerAPI
 */
void init_global_advanced_panel(helix::PrinterState& printer_state, IMoonrakerAPI* api);
