// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file app_globals.cpp
 * @brief Global application state and accessors
 *
 * Provides centralized access to global singleton instances like MoonrakerClient,
 * PrinterState, and reactive subjects. This module exists to:
 * 1. Keep main.cpp cleaner and more focused
 * 2. Provide a single point of truth for global state
 * 3. Make it easier to add new global subjects/singletons
 */

#include "app_globals.h"

#include "ui_modal.h"

#include "config.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "printer_state.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Platform-specific includes for process restart
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h> // fork, execv, usleep
using namespace helix;

#endif

// Global singleton instances (extern declarations in header, definitions here)
// These are set by main.cpp during initialization
static MoonrakerClient* g_moonraker_client = nullptr;
static MoonrakerAPI* g_moonraker_api = nullptr;
static MoonrakerManager* g_moonraker_manager = nullptr;
static JobQueueState* g_job_queue_state = nullptr;
static PrintHistoryManager* g_print_history_manager = nullptr;
static TemperatureHistoryManager* g_temp_history_manager = nullptr;

// Global reactive subjects with RAII cleanup
static SubjectManager g_subjects;
static lv_subject_t g_notification_subject;
static lv_subject_t g_show_beta_features_subject;
static lv_subject_t g_home_edit_mode_subject;

// Application quit flag (volatile sig_atomic_t for async-signal-safety)
static volatile sig_atomic_t g_quit_requested = 0;

// Wizard active flag
static bool g_wizard_active = false;
static std::function<void()> g_wizard_completion_cb;
static std::function<void()> g_wizard_cancel_cb;

// Stored command-line arguments for restart capability
static std::vector<char*> g_stored_argv;
static std::string g_executable_path;

MoonrakerClient* get_moonraker_client() {
    return g_moonraker_client;
}

void set_moonraker_client(MoonrakerClient* client) {
    g_moonraker_client = client;
}

MoonrakerAPI* get_moonraker_api() {
    return g_moonraker_api;
}

void set_moonraker_api(MoonrakerAPI* api) {
    g_moonraker_api = api;
}

MoonrakerManager* get_moonraker_manager() {
    return g_moonraker_manager;
}

void set_moonraker_manager(MoonrakerManager* manager) {
    g_moonraker_manager = manager;
}

JobQueueState* get_job_queue_state() {
    return g_job_queue_state;
}

void set_job_queue_state(JobQueueState* state) {
    g_job_queue_state = state;
}

PrintHistoryManager* get_print_history_manager() {
    return g_print_history_manager;
}

void set_print_history_manager(PrintHistoryManager* manager) {
    g_print_history_manager = manager;
}

TemperatureHistoryManager* get_temperature_history_manager() {
    return g_temp_history_manager;
}

void set_temperature_history_manager(TemperatureHistoryManager* manager) {
    g_temp_history_manager = manager;
}

PrinterState& get_printer_state() {
    // Singleton instance - created once, lives for lifetime of program
    static PrinterState instance;
    return instance;
}

lv_subject_t& get_notification_subject() {
    return g_notification_subject;
}

lv_subject_t& get_home_edit_mode_subject() {
    return g_home_edit_mode_subject;
}

// Track if subjects are initialized
static bool g_subjects_initialized = false;

void app_globals_init_subjects() {
    if (g_subjects_initialized) {
        spdlog::debug("[App Globals] Subjects already initialized, skipping");
        return;
    }

    // Initialize notification subject (stores NotificationData pointer)
    // Note: Not using UI_MANAGED_SUBJECT_POINTER because this subject is accessed
    // programmatically via get_notification_subject(), not through XML bindings
    lv_subject_init_pointer(&g_notification_subject, nullptr);
    g_subjects.register_subject(&g_notification_subject);

    // Initialize beta features visibility subject (config-driven, used by multiple panels)
    Config* config = Config::get_instance();
    bool beta_enabled = config && config->is_beta_features_enabled();
    lv_subject_init_int(&g_show_beta_features_subject, beta_enabled ? 1 : 0);
    g_subjects.register_subject(&g_show_beta_features_subject);
    lv_xml_register_subject(nullptr, "show_beta_features", &g_show_beta_features_subject);

    // Initialize home edit mode subject (controls navbar done button visibility)
    lv_subject_init_int(&g_home_edit_mode_subject, 0);
    g_subjects.register_subject(&g_home_edit_mode_subject);
    lv_xml_register_subject(nullptr, "home_edit_mode", &g_home_edit_mode_subject);

    // Initialize modal dialog subjects (for modal_dialog.xml binding)
    helix::ui::modal_init_subjects();

    g_subjects_initialized = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit("AppGlobals", app_globals_deinit_subjects);

    spdlog::trace("[App Globals] Global subjects initialized");
}

void app_globals_deinit_subjects() {
    if (!g_subjects_initialized) {
        return;
    }
    g_subjects.deinit_all();
    helix::ui::modal_deinit_subjects(); // Clean up modal subjects
    g_subjects_initialized = false;
    spdlog::debug("[App Globals] Global subjects deinitialized");
}

void app_store_argv(int argc, char** argv) {
    // Store a copy of argv for restart capability
    g_stored_argv.clear();

    if (argc > 0 && argv && argv[0]) {
        // Store executable path, resolved to absolute for safe restart via execv()
        g_executable_path = argv[0];

        // Resolve to absolute path to prevent symlink/CWD attacks on restart
#ifdef __linux__
        {
            char buf[PATH_MAX];
            ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (len > 0) {
                buf[len] = '\0';
                g_executable_path = buf;
            }
        }
#else
        {
            char* resolved = realpath(argv[0], nullptr);
            if (resolved) {
                g_executable_path = resolved;
                free(resolved);
            }
        }
#endif

        // Copy all arguments
        for (int i = 0; i < argc; ++i) {
            if (argv[i]) {
                g_stored_argv.push_back(strdup(argv[i]));
            }
        }
        // execv requires NULL-terminated array
        g_stored_argv.push_back(nullptr);

        spdlog::debug("[App Globals] Stored {} command-line arguments for restart capability",
                      argc);
    }
}

void app_request_quit() {
    spdlog::info("[App Globals] Application quit requested");
    g_quit_requested = 1;
}

void app_request_quit_signal_safe() {
    g_quit_requested = 1;
}

void app_request_restart() {
    spdlog::info("[App Globals] Application restart requested");

    if (g_stored_argv.empty() || g_executable_path.empty()) {
        spdlog::error(
            "[App Globals] Cannot restart: argv not stored. Call app_store_argv() at startup.");
        g_quit_requested = 1; // Fall back to quit
        return;
    }

#if defined(__unix__) || defined(__APPLE__)
    // Fork a new process
    pid_t pid = fork();

    if (pid < 0) {
        // Fork failed
        spdlog::error("[App Globals] Fork failed during restart: {}", strerror(errno));
        g_quit_requested = 1; // Fall back to quit
        return;
    }

    if (pid == 0) {
        // Child process - exec the new instance
        // Small delay to let parent start cleanup
        usleep(100000); // 100ms

        execv(g_executable_path.c_str(), g_stored_argv.data());

        // If execv returns, it failed
        spdlog::error("[App Globals] execv failed during restart: {}", strerror(errno));
        _exit(1); // Exit child without cleanup
    }

    // Parent process - signal main loop to exit cleanly
    spdlog::info("[App Globals] Forked new process (PID {}), parent exiting", pid);
    g_quit_requested = 1;

#else
    // Unsupported platform - fall back to quit
    spdlog::warn("[App Globals] Restart not supported on this platform, falling back to quit");
    g_quit_requested = 1;
#endif
}

void app_request_restart_service() {
    // Under any supervisor (systemd or watchdog), just exit cleanly.
    // The supervisor will restart us — forking a new child ourselves would
    // create two instances running simultaneously.
    if (getenv("INVOCATION_ID")) {
        spdlog::info("[App Globals] Running under systemd - quitting for service restart");
        app_request_quit();
    } else if (getenv("HELIX_SUPERVISED")) {
        spdlog::info("[App Globals] Running under watchdog - quitting for supervised restart");
        app_request_quit();
    } else {
        app_request_restart();
    }
}

bool app_quit_requested() {
    return g_quit_requested != 0;
}

bool is_wizard_active() {
    return g_wizard_active;
}

void set_wizard_active(bool active) {
    bool was_active = g_wizard_active;
    g_wizard_active = active;
    spdlog::debug("[App Globals] Wizard active state set to: {}", active);

    // Fire completion callback when wizard transitions from active to inactive
    if (was_active && !active && g_wizard_completion_cb) {
        g_wizard_completion_cb();
    }
}

void set_wizard_completion_callback(std::function<void()> cb) {
    g_wizard_completion_cb = std::move(cb);
}

void set_wizard_cancel_callback(std::function<void()> cb) {
    g_wizard_cancel_cb = std::move(cb);
}

std::function<void()> get_wizard_cancel_callback() {
    return g_wizard_cancel_cb;
}

// ============================================================================
// CACHE DIRECTORY HELPER
// ============================================================================

static bool try_create_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec && std::filesystem::exists(path);
}

std::string get_helix_cache_dir(const std::string& subdir) {
    // 1. Check HELIX_CACHE_DIR env var (explicit override)
    const char* helix_cache = std::getenv("HELIX_CACHE_DIR");
    if (helix_cache && helix_cache[0] != '\0') {
        std::string path = std::string(helix_cache) + "/" + subdir;
        if (try_create_dir(path)) {
            spdlog::info("[App Globals] Cache dir (HELIX_CACHE_DIR): {}", path);
            return path;
        }
    }

    // 2. Check config /cache/base_directory
    Config* config = Config::get_instance();
    if (config) {
        std::string base = config->get<std::string>("/cache/base_directory", "");
        if (!base.empty()) {
            std::string path = base + "/" + subdir;
            if (try_create_dir(path)) {
                spdlog::info("[App Globals] Cache dir (config): {}", path);
                return path;
            }
        }
    }

    // 3. Platform-specific compile-time paths
#if defined(HELIX_PLATFORM_AD5M)
    {
        std::string path = "/data/helixscreen/cache/" + subdir;
        if (try_create_dir(path)) {
            spdlog::info("[App Globals] Cache dir (AD5M): {}", path);
            return path;
        }
    }
#elif defined(HELIX_PLATFORM_CC1)
    {
        std::string path = "/opt/helixscreen/cache/" + subdir;
        if (try_create_dir(path)) {
            spdlog::info("[App Globals] Cache dir (CC1): {}", path);
            return path;
        }
    }
#elif defined(HELIX_PLATFORM_MIPS) || defined(HELIX_PLATFORM_K2)
    {
        std::string path = "/usr/data/helixscreen/cache/" + subdir;
        if (try_create_dir(path)) {
            spdlog::info("[App Globals] Cache dir (MIPS/K2): {}", path);
            return path;
        }
    }
#endif

    // 4. Check XDG_CACHE_HOME (XDG Base Directory Specification)
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    if (xdg_cache && xdg_cache[0] != '\0') {
        std::string path = std::string(xdg_cache) + "/helix/" + subdir;
        if (try_create_dir(path)) {
            return path;
        }
    }

    // 5. Try $HOME/.cache/helix (standard Linux location)
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        std::string path = std::string(home) + "/.cache/helix/" + subdir;
        if (try_create_dir(path)) {
            return path;
        }
    }

    // 6. Try /var/tmp (persistent, often larger than /tmp on embedded)
    {
        std::string path = "/var/tmp/helix_" + subdir;
        if (try_create_dir(path)) {
            return path;
        }
    }

    // 7. Last resort: /tmp (may be RAM-backed tmpfs)
    {
        std::string path = "/tmp/helix_" + subdir;
        if (try_create_dir(path)) {
            spdlog::warn("[App Globals] Using /tmp for cache - may be RAM-backed");
            return path;
        }
    }

    spdlog::error("[App Globals] Failed to create cache directory for '{}'", subdir);
    return "";
}
