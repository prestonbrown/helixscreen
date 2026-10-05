// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "cli_args.h"
#include "gcode_response_routing.h"
#include "invalidation_suppression.h"
#include "lvgl/lvgl.h"
#include "main_loop_handler.h"
#include "printer_session.h"
#include "splash_screen_manager.h"
#include "wizard_step.h" // helix::wizard::StepId
#include "xml_hot_reloader.h"

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class Config;
}
#if HELIX_HAS_PLUGINS
namespace helix::plugin {
class PluginHost;
class PluginDirWatcher;
class PluginSyncDriver;
struct SyncResult;
} // namespace helix::plugin
#endif
namespace helix {
class PrinterDiscovery;
} // namespace helix
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
    bool rotation_probe_wanted() const;
    void run_rotation_probe_and_layout();
    bool init_theme();
    bool init_assets();
    bool register_widgets();
    bool register_xml_components();
    bool init_translations();
    void apply_startup_cli_actions();
    bool run_wizard();

    // Main loop
    int main_loop();
    void handle_keyboard_shortcuts();
    void process_notifications();
    void check_timeouts();

    // Shutdown
    void shutdown();

    // Helper functions
    void ensure_project_root_cwd();
#ifdef HELIX_ENABLE_SCREENSAVER
    void show_screensaver_migration_notice_if_pending();
#endif
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
    std::unique_ptr<helix::XmlHotReloader> m_hot_reloader;

    // Configuration
    helix::Config* m_config = nullptr; // Singleton, not owned
    helix::CliArgs m_args;

    // Screen dimensions (0 = auto-detect from display hardware)
    int m_screen_width = 0;
    int m_screen_height = 0;

    // First-boot rotation probe decision and the kernel panel_orientation it
    // read (-1 = none), both taken in init_display() before the display exists
    bool m_rotation_probe_wanted = false;
    int m_kernel_orientation = -1;

    // Screen (not owned, managed by LVGL); the session reads it through a reference
    lv_obj_t* m_screen = nullptr;

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
    bool m_shutdown_complete = false;

    /// Everything a printer switch destroys and rebuilds: the printer connection, its panels
    /// and subjects, and the state machine that switches between printers.
    helix::PrinterSession m_session;

    // Splash screen lifecycle manager
    helix::application::SplashScreenManager m_splash_manager;

    /// Original LVGL flush callback, saved while splash no-op is active
    lv_display_flush_cb_t m_original_flush_cb = nullptr;

    /// Display invalidation suppressed while the launcher's splash owns the framebuffer
    helix::InvalidationSuppression m_splash_invalidation_suppression;
};
