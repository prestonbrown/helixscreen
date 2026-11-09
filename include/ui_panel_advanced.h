// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_base.h"
#include "ui_panel_history_dashboard.h"
#include "ui_plugin_install_modal.h"
#include "ui_shutdown_modal.h"

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "helix_plugin_installer.h"
#include "macro_manager.h"

#include <functional>
#include <memory>

namespace helix::ui {
struct AdvancedPanelTestAccess; // test-only friend (tests/test_helpers/)
} // namespace helix::ui

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
    AdvancedPanel(helix::PrinterState& printer_state, helix::IMoonrakerAPI* api);

    ~AdvancedPanel() override;

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

    /// Release this panel's XML subjects; safe to call more than once.
    void deinit_subjects();

    /// Offer to install the HelixPrint plugin (the install modal). Also the
    /// Install action of a plugin_feature_toggle_row.
    void handle_helix_plugin_install_clicked();

  private:
    // Test-only access to the plugin uninstall flow.
    friend struct helix::ui::AdvancedPanelTestAccess;

    //
    // === Navigation Handlers ===
    //

    void handle_spoolman_clicked();
    void handle_macros_clicked();
    void handle_console_clicked();
    void handle_history_clicked();
    void handle_pid_tuning_clicked();
    void handle_pa_tuning_clicked();
    void handle_timelapse_setup_clicked();
    void handle_helix_plugin_uninstall_clicked();

    void handle_helix_macros_install_clicked();
    void handle_helix_macros_update_clicked();

    /// job_holds_machine subject as a bool: PRINTING, PAUSED, or a
    /// host-side Preparing block — everything a Klipper restart would kill.
    bool job_holds_machine_now() const;

    /// The confirmed half of the uninstall row: runs the uninstaller and
    /// reports the outcome. Blocks this thread while the script runs.
    void run_helix_plugin_uninstall();

    /// Restart Moonraker so a plugin install or uninstall takes effect; the
    /// reconnect's plugin check then reports what loaded. Returns whether a
    /// restart was requested.
    bool restart_moonraker_for_plugin();

    /// The confirmed half of both macro rows: stages the files, then either
    /// restarts Klipper right away (no active print) or marks the restart
    /// pending for the print-complete offer.
    void run_helix_macros_stage(bool update);

    /// Fires a Klipper restart unless a print is active; returns whether it
    /// fired. The re-check is load-bearing: staging takes seconds of HTTP and
    /// a print may have started after the confirm dialog.
    bool restart_helix_macros_when_idle();

    /// The one-shot "restart now?" offer for files staged during a print,
    /// popped on the job_holds_machine 1->0 transition.
    void offer_helix_macros_restart();

    /// Attaches the job_holds_machine observer (idempotent). Separate from
    /// init_subjects() because observe targets need PrinterState subjects
    /// initialized first.
    void wire_macro_restart_observer();

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
    static void on_pid_tuning_clicked(lv_event_t* e);
    static void on_timelapse_videos_clicked(lv_event_t* e);
    static void on_timelapse_setup_clicked(lv_event_t* e);
    static void on_helix_plugin_install_clicked(lv_event_t* e);
    static void on_helix_plugin_uninstall_clicked(lv_event_t* e);
    static void on_helix_macros_install_clicked(lv_event_t* e);
    static void on_helix_macros_update_clicked(lv_event_t* e);
    static void on_advanced_power_clicked(lv_event_t* e);

    //
    // === HelixPrint Plugin Support ===
    //

    helix::HelixPluginInstaller plugin_installer_;
    PluginInstallModal plugin_install_modal_;

    /// What run_helix_plugin_uninstall() calls once the user has confirmed.
    /// Empty means the bundled installer's uninstall_local(); a test installs
    /// a recorder here so the confirm flow can be driven without forking.
    using UninstallRunner = std::function<void(helix::HelixPluginInstaller::UninstallCallback)>;
    UninstallRunner uninstall_runner_;

    /// What restart_moonraker_for_plugin() calls. Empty means the API's
    /// server.restart; a test installs a recorder.
    using MoonrakerRestarter = std::function<void()>;
    MoonrakerRestarter moonraker_restarter_;

    //
    // === Helix Helper Macros (helix_macros.cfg) ===
    //
    // Install/update over the Moonraker file API — works for local AND remote
    // printers, unlike the plugin installer's shell path. The restart queue is
    // in-memory and one offer deep: losing it to an app restart is fine, the
    // staged files activate at whatever Klipper restart happens next.

    /// Lazily built on the first install/update (needs a live api_).
    std::unique_ptr<helix::MacroManager> macro_manager_;

    /// Watches job_holds_machine for the 1->0 edge that pops the restart
    /// offer — the same predicate the restart guard refuses under.
    ObserverGuard macro_job_observer_;
    bool macro_observer_wired_ = false;

    /// One offer per staging: set when the modal pops, cleared when a restart
    /// activates the macros (status leaves RestartPending).
    bool macro_restart_offer_made_ = false;

    /// Follows Klipper across the restart that loads a staged helix_skips.cfg.
    /// A config error there removes the file and restarts once more, never
    /// re-arming, so a failure that is not ours cannot loop.
    void on_klippy_state_for_skips(int state);
    ObserverGuard skips_klippy_observer_;
    helix::skip_wrappers::LoadWatch skips_load_watch_;
    bool skips_load_watching_ = false;

    //
    // === Shared Power Dialog ===
    //
    // Reuses the same modal the home-panel power widget shows. The "Both" flow
    // defers the screen-side SystemPower call until the printer-side ack lands
    // on the WS background thread, so it runs on object_lifetime_: the ack is
    // owed to the machine and must survive the user leaving this panel.

    ShutdownModal shutdown_modal_;

    //
    // === Notification History Row ===
    //

    /// 1 while no notifications widget holds a cell on the home screen, which
    /// leaves this row as the only way into notification history.
    lv_subject_t show_notification_history_subject_{};
    SubjectManager subjects_;
    void refresh_notification_history_row();

    //
    // === Cached Overlay Panels ===
    //
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
void init_global_advanced_panel(helix::PrinterState& printer_state, helix::IMoonrakerAPI* api);
