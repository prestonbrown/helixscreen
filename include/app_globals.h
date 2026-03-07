// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"
#include "subject_managed_panel.h"

#include <functional>
#include <string>

// Forward declarations
namespace helix {
class MoonrakerClient;
}
class MoonrakerAPI;
class MoonrakerManager;
namespace helix {
class PrinterState;
}
class JobQueueState;
class PrintHistoryManager;
class TemperatureHistoryManager;

/**
 * @brief Get global MoonrakerClient instance
 * @return Pointer to global MoonrakerClient (may be nullptr if not initialized)
 */
helix::MoonrakerClient* get_moonraker_client();

/**
 * @brief Set global MoonrakerClient instance (called by main.cpp during init)
 * @param client Pointer to MoonrakerClient instance
 */
void set_moonraker_client(helix::MoonrakerClient* client);

/**
 * @brief Get global MoonrakerAPI instance
 * @return Pointer to global MoonrakerAPI (may be nullptr if not initialized)
 */
MoonrakerAPI* get_moonraker_api();

/**
 * @brief Set global MoonrakerAPI instance (called by main.cpp during init)
 * @param api Pointer to MoonrakerAPI instance
 */
void set_moonraker_api(MoonrakerAPI* api);

/**
 * @brief Get global MoonrakerManager instance
 * @return Pointer to global MoonrakerManager (may be nullptr if not initialized)
 */
MoonrakerManager* get_moonraker_manager();

/**
 * @brief Set global MoonrakerManager instance (called by Application during init)
 * @param manager Pointer to MoonrakerManager instance
 */
void set_moonraker_manager(MoonrakerManager* manager);

/**
 * @brief Get global JobQueueState instance
 *
 * Provides centralized job queue state for queue panels and status indicators.
 *
 * @return Pointer to global JobQueueState (may be nullptr if not initialized)
 */
JobQueueState* get_job_queue_state();

/**
 * @brief Set global JobQueueState instance (called by Application during init)
 * @param state Pointer to JobQueueState instance
 */
void set_job_queue_state(JobQueueState* state);

/**
 * @brief Get global PrintHistoryManager instance
 *
 * Provides centralized print history cache for status indicators.
 * Used by PrintSelectPanel for file status and History panels for job lists.
 *
 * @return Pointer to global PrintHistoryManager (may be nullptr if not initialized)
 */
PrintHistoryManager* get_print_history_manager();

/**
 * @brief Set global PrintHistoryManager instance (called by Application during init)
 * @param manager Pointer to PrintHistoryManager instance
 */
void set_print_history_manager(PrintHistoryManager* manager);

/**
 * @brief Get global TemperatureHistoryManager instance
 *
 * Provides centralized temperature history tracking for chart panels.
 * Collects 20 minutes of temperature samples at 1Hz for all heaters.
 *
 * @return Pointer to global TemperatureHistoryManager (may be nullptr if not initialized)
 */
TemperatureHistoryManager* get_temperature_history_manager();

/**
 * @brief Set global TemperatureHistoryManager instance (called by Application during init)
 * @param manager Pointer to TemperatureHistoryManager instance
 */
void set_temperature_history_manager(TemperatureHistoryManager* manager);

/**
 * @brief Get global PrinterState singleton instance
 *
 * Returns a reference to the singleton PrinterState instance.
 * The instance is created on first call and persists for the lifetime of the program.
 * Thread-safe initialization guaranteed by C++11 static local variable semantics.
 *
 * @return Reference to singleton PrinterState (always valid)
 */
helix::PrinterState& get_printer_state();

/**
 * @brief Get the global notification subject
 *
 * Any module can emit notifications by calling:
 * ```cpp
 * NotificationData notif = {severity, title, message, show_modal};
 * lv_subject_set_pointer(&get_notification_subject(), &notif);
 * ```
 *
 * @return Reference to the global notification subject
 */
lv_subject_t& get_notification_subject();

/**
 * @brief Get the global home edit mode subject
 *
 * Controls navbar done button visibility during grid edit mode.
 * Value 0 = not editing, 1 = editing.
 *
 * @return Reference to the home edit mode subject
 */
lv_subject_t& get_home_edit_mode_subject();

/**
 * @brief Initialize all global subjects
 *
 * Must be called during app initialization after LVGL is initialized.
 * Initializes reactive subjects used throughout the application.
 */
void app_globals_init_subjects();

/**
 * @brief Deinitialize global subjects
 *
 * Disconnects observers before shutdown. Called by StaticPanelRegistry.
 */
void app_globals_deinit_subjects();

/**
 * @brief Store original command-line arguments for restart capability
 *
 * Must be called early in main() before any argument processing.
 * Required for app_request_restart() to work.
 *
 * @param argc Argument count from main()
 * @param argv Argument vector from main()
 */
void app_store_argv(int argc, char** argv);

/**
 * @brief Request clean application shutdown
 *
 * Sets a flag that the main event loop checks. When set, the main loop
 * will exit cleanly, allowing proper cleanup (spdlog shutdown, etc.).
 * Use this instead of exit() or _Exit() for graceful termination.
 */
void app_request_quit();

/**
 * @brief Signal-safe version of app_request_quit()
 *
 * Only sets the quit flag without calling spdlog or any other
 * non-async-signal-safe function. Use this from signal handlers.
 */
void app_request_quit_signal_safe();

/**
 * @brief Request application restart
 *
 * Forks a new process and exec's the same binary with the same arguments.
 * The new process starts fresh while the current process exits cleanly.
 * On embedded (systemd), this provides seamless restart. On macOS for
 * development, the new window appears and the old one closes.
 *
 * Requires app_store_argv() to have been called during startup.
 */
void app_request_restart();

/**
 * @brief Request application restart with service-awareness
 *
 * Detects whether the app is running under systemd (INVOCATION_ID env var)
 * and uses the appropriate restart strategy:
 * - Under systemd: app_request_quit() (systemd Restart=always handles restart)
 * - Standalone/dev: app_request_restart() (fork/exec new process)
 *
 * Use this instead of app_request_restart() for all user-facing restart actions.
 */
void app_request_restart_service();

/**
 * @brief Check if quit has been requested
 * @return true if app_request_quit() or app_request_restart() was called
 */
bool app_quit_requested();

/**
 * @brief Check if setup wizard is currently active
 * @return true if wizard is running, false otherwise
 */
bool is_wizard_active();

/**
 * @brief Set wizard active state
 * @param active true when wizard starts, false when it completes
 */
void set_wizard_active(bool active);

/**
 * @brief Register a callback invoked when wizard completes (transitions false)
 *
 * Used by Application to clear add-printer recovery state on successful wizard completion.
 * Only one callback is supported; setting a new one replaces the previous.
 *
 * @param cb Callback to invoke, or nullptr to clear
 */
void set_wizard_completion_callback(std::function<void()> cb);

/**
 * @brief Register a callback invoked when wizard is cancelled (back from first step)
 *
 * Used by Application to clean up empty printer entries on add-printer cancellation.
 * Only one callback is supported; setting a new one replaces the previous.
 *
 * @param cb Callback to invoke, or nullptr to clear
 */
void set_wizard_cancel_callback(std::function<void()> cb);

/**
 * @brief Get the registered wizard cancel callback (may be nullptr)
 */
std::function<void()> get_wizard_cancel_callback();

/**
 * @brief Get appropriate cache directory for temp files
 *
 * Determines best location for cache/temp files with priority:
 * 1. HELIX_CACHE_DIR env var + /<subdir>
 * 2. Config /cache/base_directory + /<subdir>
 * 3. Platform-specific (compile-time):
 *    - AD5M:  /data/helixscreen/cache/<subdir>
 *    - K1/K2: /usr/data/helixscreen/cache/<subdir>
 * 4. XDG_CACHE_HOME/helix/<subdir>
 * 5. $HOME/.cache/helix/<subdir>
 * 6. /var/tmp/helix_<subdir>
 * 7. /tmp/helix_<subdir> (last resort, with warning)
 *
 * Creates directory if needed. On embedded systems, prefers persistent
 * storage over RAM-backed tmpfs.
 *
 * @param subdir Subdirectory name (e.g., "gcode_temp", "thumbs")
 * @return Full path to cache directory, or empty string on failure
 */
std::string get_helix_cache_dir(const std::string& subdir);