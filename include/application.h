// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "cli_args.h"
#include "lvgl/lvgl.h"
#include "main_loop_handler.h"
#include "splash_screen_manager.h"
#include "wizard_step.h" // helix::wizard::StepId
#include "xml_hot_reloader.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class Config;
}
namespace helix::plugin {
class PluginManager;
}
namespace helix {
class ActionPromptManager;
class AmsErrorBridge;
class GcodeErrorRouter;
class GcodeNarrationRouter;
class PrinterDiscovery;
} // namespace helix
namespace helix::ui {
class ActionPromptModal;
class RecoveryModalPresenter;
} // namespace helix::ui
class DisplayManager;
class SubjectInitializer;
class MoonrakerManager;
namespace helix {
class PanelFactory;
}
class JobQueueState;
class PrintHistoryManager;
class TemperatureHistoryManager;

/**
 * @brief Main application orchestrator
 *
 * Application coordinates all subsystems in the correct order:
 * 1. Parse CLI args and configure runtime settings
 * 2. Initialize display (LVGL, backend, input devices)
 * 3. Register fonts and images
 * 4. Initialize reactive subjects
 * 5. Create UI from XML and wire panels
 * 6. Initialize Moonraker client/API
 * 7. Connect to printer and run main loop
 * 8. Shutdown in reverse order
 *
 * Usage:
 *   Application app;
 *   return app.run(argc, argv);
 */
class Application {
  public:
    Application();
    ~Application();

    // Non-copyable, non-movable
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    /**
     * @brief Run the application
     * @param argc Command line argument count
     * @param argv Command line argument array
     * @return Exit code (0 = success)
     */
    int run(int argc, char** argv);

  private:
    /// Allow test-only accessor to reach protected/private members
    friend class ApplicationTestAccess;

    // Initialization phases
    bool parse_args(int argc, char** argv);
    bool init_config();
    bool init_logging();
    bool init_display();
    void run_rotation_probe_and_layout();
    bool init_theme();
    bool init_assets();
    bool register_widgets();
    bool register_xml_components();
    bool init_translations();
    bool init_core_subjects();
    bool init_panel_subjects();
    bool init_ui();
    bool init_moonraker();
    bool connect_moonraker();
    void apply_startup_cli_actions();
    bool run_wizard();
    bool init_plugins();

    // Main loop
    int main_loop();
    void handle_keyboard_shortcuts();
    void process_notifications();
    void check_timeouts();

    // Shutdown
    void shutdown();

    // Soft restart (printer switching)
    void switch_printer(const std::string& printer_id);
    void add_printer_via_wizard();
    void cancel_add_printer_wizard();
    void tear_down_printer_state();
    void init_printer_state();

    // Helper functions
    void ensure_project_root_cwd();
#ifdef HELIX_ENABLE_SCREENSAVER
    void show_screensaver_migration_notice_if_pending();
#endif
    void setup_discovery_callbacks();
    // Re-resolve + persist fan AND heater roles and re-init fan state so the UI
    // rebinds after a targeted hardware-reconfig wizard page finishes. Marshals to
    // the main thread internally; safe to call from a main-thread on_complete.
    void reapply_hardware_roles();
    // Offer to run the hardware wizard steps a Klipper-down setup could not show
    // (#1160). Main thread only — shows a modal. Both answers settle the debt.
    void prompt_deferred_hardware_setup(std::vector<helix::wizard::StepId> steps);
    // Run the accepted offer as a targeted wizard session, fired by a one-shot
    // timer so the wizard is built after the modal's exit animation rather than
    // underneath it. Consumes m_pending_hardware_setup_steps.
    void launch_deferred_hardware_setup();
    // Clear this printer's deferred-hardware-setup marker and persist it.
    void settle_deferred_hardware_setup();
    // Saved-vs-detected printer type check (bundle F2LNLQCC: a Voron Trident
    // saved as AD5M Pro silently received AD5M pre-print options and presets).
    // Shows the one-time actionable mismatch modal; guarded to once per session
    // and once per saved type (TYPE_MISMATCH_SHOWN_FOR). Main thread only.
    void maybe_warn_type_mismatch(const helix::PrinterDiscovery& hardware);
    // Run the accepted re-identify as a targeted wizard session (PrinterIdentify
    // step ONLY — never a full wizard run), fired by a one-shot timer so the
    // wizard is built after the modal's exit animation rather than underneath it.
    void launch_type_reidentify_wizard();
    // Record + persist the mismatch decision for the current saved type. Both
    // modal arms call this before anything else: a crash mid-wizard must not
    // leave the prompt pending forever.
    void settle_type_mismatch_warning();
    lv_obj_t* create_overlay_panel(lv_obj_t* screen, const char* component_name,
                                   const char* display_name);
    void init_action_prompt();
    void check_wifi_availability();
    void restore_flush_callback();

    // Owned managers (in initialization order)
    /// Expires the callbacks Application defers to the main thread — the
    /// hardware-role reapply, the two ActionPrompt modal hops off the WebSocket
    /// thread, and the wizard-cancel soft restart. Invalidated explicitly at the
    /// top of shutdown() rather than relying on member destruction order: the
    /// owned subsystems below are what those callbacks touch, and a guard that
    /// only expired via its own destructor would still read as live while those
    /// members were being torn down. Application is a stack local in main() with
    /// no deinit_subjects(), so shutdown()/destruction is the only teardown
    /// point — this is debt migrated to the sanctioned form, not a live
    /// use-after-free (#1165).
    helix::AsyncLifetimeGuard m_async_lifetime;

    std::unique_ptr<DisplayManager> m_display;
    std::unique_ptr<SubjectInitializer> m_subjects;
    std::unique_ptr<MoonrakerManager> m_moonraker;
    std::unique_ptr<JobQueueState> m_job_queue_state;
    std::unique_ptr<PrintHistoryManager> m_history_manager;
    std::unique_ptr<TemperatureHistoryManager> m_temp_history_manager;
    std::unique_ptr<helix::PanelFactory> m_panels;
    std::unique_ptr<helix::plugin::PluginManager> m_plugin_manager;
    std::unique_ptr<helix::XmlHotReloader> m_hot_reloader;

    // Action prompt system (Klipper action:prompt protocol)
    std::unique_ptr<helix::ActionPromptManager> m_action_prompt_manager;
    std::unique_ptr<helix::ui::ActionPromptModal> m_action_prompt_modal;

    // Source-agnostic modal presenter for CRITICAL recovery errors. Owned here
    // so Application controls its lifetime independently of GcodeErrorRouter.
    // Declared BEFORE m_gcode_error_router so it destructs AFTER the router
    // (Application teardown resets the router first, then the presenter).
    std::unique_ptr<helix::ui::RecoveryModalPresenter> m_recovery_presenter;

    // Surfaces Klipper `!!` / `Error:` lines as modals/toasts and replays
    // the most recent error from gcode_store on (re)connect. Owns the
    // notify_gcode_response and connected-observer registrations.
    std::unique_ptr<helix::GcodeErrorRouter> m_gcode_error_router;

    // Routes `//` toolchange narration lines to the active AMS backend's step
    // model, updating the toolchange_step subject. Sibling of the error router;
    // owns a SEPARATE notify_gcode_response handler key. Does NOT surface errors.
    std::unique_ptr<helix::GcodeNarrationRouter> m_gcode_narration_router;

    // Observes AmsState's action subject and routes AmsAction::ERROR edges to
    // m_recovery_presenter. Holds a reference INTO the presenter, so the
    // presenter must outlive it — declared after m_recovery_presenter (destructs
    // first) and reset before it at both teardown sites.
    std::unique_ptr<helix::AmsErrorBridge> m_ams_error_bridge;

    // Configuration
    helix::Config* m_config = nullptr; // Singleton, not owned
    helix::CliArgs m_args;

    // Screen dimensions (0 = auto-detect from display hardware)
    int m_screen_width = 0;
    int m_screen_height = 0;

    // UI objects (not owned, managed by LVGL)
    lv_obj_t* m_screen = nullptr;
    lv_obj_t* m_app_layout = nullptr;

    // Overlay panels (for lifecycle management)
    struct OverlayPanels {
        lv_obj_t* motion = nullptr;
        lv_obj_t* nozzle_temp = nullptr;
        lv_obj_t* bed_temp = nullptr;
        lv_obj_t* print_status = nullptr;
        lv_obj_t* ams = nullptr;
        lv_obj_t* bed_mesh = nullptr;
    } m_overlay_panels;

    // NOTE: Print start collector and observers are kept in main.cpp
    // until the observer pattern is refactored to support capturing lambdas.

    // Periodic timeout checking (Moonraker connection health)
    uint32_t m_last_timeout_check = 0;
    uint32_t m_timeout_check_interval = 2000;

    // Main loop timing handler (screenshot, auto-quit, benchmark)
    helix::application::MainLoopHandler m_loop_handler;

    // Single-instance lock
    bool acquire_instance_lock();
    void release_instance_lock();
    int m_lock_fd = -1;

    // Android lifecycle pause/resume
    void on_enter_background();
    void on_enter_foreground();
    bool m_backgrounded = false;

    // Debounce for force_reconnect: on_enter_foreground and the DisplayManager
    // sleep callback can both fire for the same wake event. Without this, the
    // second call bumps the connection generation and makes the first
    // discovery's subscription stale — leaving the temp overlay dead (#1245).
    std::chrono::steady_clock::time_point m_last_force_reconnect{};

    // State
    bool m_running = false;
    bool m_wizard_active = false;
    // Guards the discovery-triggered targeted hardware-reconfig wizard so it launches
    // at most once per connection. Reset when a new hardware discovery begins.
    bool m_targeted_reconfig_shown = false;
    // Guards the deferred hardware-setup offer (#1160) so a reconnect within one
    // session cannot re-ask. The persisted per-printer marker is what stops it
    // across sessions — cleared as soon as the user answers either way.
    bool m_hardware_setup_prompt_shown = false;
    // Guards the saved-vs-detected printer type mismatch warning so it shows at
    // most once per session, whichever button dismisses it. The persisted
    // TYPE_MISMATCH_SHOWN_FOR flag covers cross-boot; intentionally NOT reset on
    // reconnect.
    bool m_type_mismatch_shown = false;
    // Steps the deferred hardware-setup offer will run if accepted. Held here
    // because modal_show_confirmation() carries a single void* user_data.
    std::vector<helix::wizard::StepId> m_pending_hardware_setup_steps;
    // Guards the firmware z-offset persistence enablement (see
    // include/z_offset_persistence.h) so it is sent at most once per app session.
    // Intentionally NOT reset on reconnect.
    bool m_zoffset_persistence_enabled = false;
    // Hardware-shape fingerprint from the most recent on_discovery_complete.
    // When a reconnect's fingerprint matches (hardware unchanged), expensive
    // user-facing side-effects (LED chip population, hardware validation
    // toasts, targeted reconfig wizard, telemetry snapshots) are skipped —
    // only the subject-restoring work that the UI needs to rebind runs.
    // See on_discovery_complete in application.cpp.
    size_t m_last_hardware_fingerprint = 0;
    bool m_first_discovery_complete = true;
    bool m_shutdown_complete = false;
    bool m_soft_restart_in_progress = false;

    // Tracks previous printer when adding a new one via wizard (for cancel recovery)
    std::string m_wizard_previous_printer_id;

    // Splash screen lifecycle manager
    helix::application::SplashScreenManager m_splash_manager;

    /// Original LVGL flush callback, saved while splash no-op is active
    lv_display_flush_cb_t m_original_flush_cb = nullptr;
};

namespace helix {

/// Snapshot @p path into a fixed static buffer so the SIGTERM handler can
/// unlink(2) the crash-restart marker without constructing anything. Must be
/// called on the main thread at startup, before the handler can fire. A path
/// that does not fit is rejected (the handler then does nothing).
/// @return true if the path was cached.
bool cache_crash_marker_path_for_signal(const std::string& path);

/// Delete the crash-restart marker using only async-signal-safe calls.
/// Callable from a signal handler: no allocation, no std::filesystem, no
/// locking — just unlink(2) on the pre-cached path. A no-op when the path was
/// never cached.
void clear_crash_marker_signal_safe();

} // namespace helix
