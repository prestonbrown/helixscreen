// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "cli_args.h"
#include "lvgl/lvgl.h"
#include "main_loop_handler.h"
#include "splash_screen_manager.h"
#include "xml_hot_reloader.h"

#include <memory>

// Forward declarations
namespace helix {
class Config;
}
namespace helix::plugin {
class PluginManager;
}
namespace helix {
class ActionPromptManager;
}
namespace helix::ui {
class ActionPromptModal;
}
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
    void create_overlays();
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
    void auto_configure_mock_state();
    void setup_discovery_callbacks();
    lv_obj_t* create_overlay_panel(lv_obj_t* screen, const char* component_name,
                                   const char* display_name);
    void init_action_prompt();
    void check_wifi_availability();
    void restore_flush_callback();

    // Owned managers (in initialization order)
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

    // State
    bool m_running = false;
    bool m_wizard_active = false;
    bool m_shutdown_complete = false;
    bool m_soft_restart_in_progress = false;

    // Tracks previous printer when adding a new one via wizard (for cancel recovery)
    std::string m_wizard_previous_printer_id;

    // Splash screen lifecycle manager
    helix::application::SplashScreenManager m_splash_manager;

    /// Original LVGL flush callback, saved while splash no-op is active
    lv_display_flush_cb_t m_original_flush_cb = nullptr;
};
