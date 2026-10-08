// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "gcode_response_routing.h"
#include "hardware_fingerprint.h"
#include "hardware_setup_prompter.h"
#include "lvgl/lvgl.h"
#include "printer_switch_flow.h"

#include <functional>
#include <memory>
#include <set>
#include <string>

class ApplicationTestAccess;     // NAMESPACE_OK: test accessor, declared at global scope
class DisplayManager;            // NAMESPACE_OK: declared at global scope
class JobQueueState;             // NAMESPACE_OK: declared at global scope
class MoonrakerManager;          // NAMESPACE_OK: declared at global scope
class PrintHistoryManager;       // NAMESPACE_OK: declared at global scope
class SubjectInitializer;        // NAMESPACE_OK: declared at global scope
class TemperatureHistoryManager; // NAMESPACE_OK: declared at global scope

#if HELIX_HAS_PLUGINS
namespace helix::plugin {
class PluginHost;
class PluginDirWatcher;
class PluginSyncDriver;
struct SyncResult;
} // namespace helix::plugin
#endif

namespace helix {
struct CliArgs;
class Config;
class PanelFactory;
class UpgradeBanner;

/// The state machine behind switching to another printer, adding one through the wizard, and
/// backing out of that wizard. It decides what the config says and in which order the restart
/// steps run; the steps themselves arrive as hooks.
class PrinterSession {
  public:
    /// The restart work the session hands its switch flow. Teardown destroys the current
    /// printer's scope, rebuild creates the next one, land_home navigates to the home panel.
    using Restart = PrinterSwitchFlow::Restart;

    /// What the owner of the process provides: its state, read at use, and the steps of the
    /// rebuild that belong to the process rather than the printer.
    struct Host {
        const CliArgs& args;
        const bool& shutdown_complete;
        bool& wizard_active;
        /// Process-scoped: shut down only on ProcessExit, before UpdateChecker it observes.
        UpgradeBanner& upgrade_banner;
        /// Runs the setup wizard when the active printer needs one; true when it started.
        std::function<bool()> run_wizard;
        /// Applies one-shot startup actions requested on the command line.
        std::function<void()> startup_actions;
        /// A discovery finished: the splash screen may exit.
        std::function<void()> discovery_complete;
    };

    /// `config` is read through the reference because it is assigned after construction, and
    /// `screen` because init_moonraker() can replace it.
    PrinterSession(Config*& config, AsyncLifetimeGuard& async, lv_obj_t*& screen, Host host);
    ~PrinterSession();

    PrinterSession(const PrinterSession&) = delete;
    PrinterSession& operator=(const PrinterSession&) = delete;

    /// Makes `printer_id` the active printer and restarts onto it. Ignored while a restart is
    /// running; an unknown id changes nothing.
    void switch_printer(const std::string& printer_id);

    /// switch_printer() for a user's pick: asks first when the current printer is printing.
    void request_switch(const std::string& printer_id);

    /// Creates an empty printer entry, makes it active and restarts into its setup wizard.
    void add_printer_via_wizard();

    /// Abandons the printer the wizard was adding, restores the previous one and restarts
    /// onto it. The restart is deferred past the click handler that called this.
    void cancel_add_printer_wizard();

    /// The printer to restore if the add-printer wizard is cancelled; empty when no such
    /// wizard is running.
    [[nodiscard]] const std::string& wizard_previous_printer_id() const {
        return m_flow.wizard_previous_printer_id();
    }

    /// The add-printer wizard finished: there is nothing left to cancel back to.
    void clear_wizard_previous_printer_id() {
        m_flow.clear_wizard_previous_printer_id();
    }

    // The phases of bringing a printer scope up. Boot runs them in its own order, around the
    // display and wizard; rebuild() runs them in the order a switch needs.
    bool init_core_subjects();
    bool init_moonraker();
    bool init_panel_subjects();
    bool init_ui();
    bool connect_moonraker();
#if HELIX_HAS_PLUGINS
    /// Rebuilds the plugin host, watcher and sync driver against the active printer.
    void init_plugins();
#endif

    /// Tears down the current printer scope for a switch.
    void tear_down_printer_state();

    /// Builds the next printer scope after tear_down_printer_state().
    void rebuild();

    /// What survives the teardown: a printer switch keeps the process and LVGL alive,
    /// ProcessExit ends both.
    enum class TeardownScope { PrinterSwitch, ProcessExit };

    /// The one ordered teardown behind both a printer switch and process exit. On exit,
    /// `exit_display` (null for an early exit that never built one) gets its display restored
    /// at the point the framebuffer is no longer being drawn to; the caller finishes the exit
    /// by stopping the HTTP executors and destroying the display.
    void teardown_printer_scope(TeardownScope scope, DisplayManager* exit_display = nullptr);

    // The per-printer objects are owned here so one teardown destroys them in order.
    std::unique_ptr<MoonrakerManager>& moonraker() {
        return m_moonraker;
    }
    GcodeResponseRouting& routing() {
        return m_routing;
    }

    /// Re-arms the per-printer discovery state: the fingerprint comparison and the
    /// once-per-connection prompt guards. Runs when a printer scope is torn down, so the
    /// next printer's first discovery runs the full pipeline.
    void reset_discovery_session();

  private:
    friend class ::ApplicationTestAccess;

    void setup_discovery_callbacks();
#if HELIX_HAS_PLUGINS
    /// Toasts plugin ids a sync found that no earlier load or sync had shown, then refreshes
    /// the Settings > Plugins row. Runs on the main thread from the sync driver's completion.
    void on_plugin_sync(const plugin::SyncResult& result);
    /// settings_plugins_available follows "the host exists and holds at least one plugin", so
    /// the row appears only once there is something to show.
    void update_plugins_row_visibility();
#endif

    Config*& m_config;
    AsyncLifetimeGuard& m_async;
    Host m_host;
    PrinterSwitchFlow m_flow;

    std::unique_ptr<MoonrakerManager> m_moonraker;
    std::unique_ptr<JobQueueState> m_job_queue_state;
    std::unique_ptr<PrintHistoryManager> m_history_manager;
    std::unique_ptr<TemperatureHistoryManager> m_temp_history_manager;
    std::unique_ptr<PanelFactory> m_panels;
    std::unique_ptr<SubjectInitializer> m_subjects;
#if HELIX_HAS_PLUGINS
    std::unique_ptr<plugin::PluginHost> m_plugin_host;
    /// Hot-reloads plugins from HELIX_PLUGIN_DIR while it is the source (no sync driver runs
    /// then). Holds a reference to m_plugin_host, so it must be reset before the host at
    /// every teardown.
    std::unique_ptr<plugin::PluginDirWatcher> m_plugin_watcher;
    /// Syncs the Moonraker plugin folder into the host's cache dir. Holds a reference to
    /// m_plugin_host, so it must be reset before the host at every teardown, and rebuilt
    /// with it on a printer switch.
    std::unique_ptr<plugin::PluginSyncDriver> m_plugin_sync;
    /// Plugin ids an earlier load or sync already showed; a sync finding an id outside this
    /// set toasts "new plugin available".
    std::set<std::string> m_known_plugin_ids;
#endif
    GcodeResponseRouting m_routing;

    lv_obj_t*& m_screen;
    lv_obj_t* m_app_layout = nullptr;
    struct OverlayPanels {
        lv_obj_t* motion = nullptr;
        lv_obj_t* nozzle_temp = nullptr;
        lv_obj_t* bed_temp = nullptr;
        lv_obj_t* print_status = nullptr;
        lv_obj_t* ams = nullptr;
        lv_obj_t* bed_mesh = nullptr;
    } m_overlay_panels;

    HardwareSetupPrompter m_prompter;
    /// This printer session's discoveries: a reconnect with the same hardware shape skips
    /// the user-facing side effects (hardware validation toasts, targeted reconfig wizard,
    /// telemetry snapshots).
    helix::HardwareChangeTracker m_hw_changes;
};

} // namespace helix
