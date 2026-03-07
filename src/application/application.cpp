// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file application.cpp
 * @brief Application lifecycle orchestrator - startup, main loop, and shutdown coordination
 *
 * @pattern Singleton orchestrator with ordered dependency initialization/teardown
 * @threading Main thread only; shutdown guards against double-call
 * @gotchas m_shutdown_complete prevents destructor re-entry
 *
 * @see display_manager.cpp, moonraker_manager.cpp
 */

#include "application.h"

// Private LVGL header needed to read display->flush_cb for splash no-op swap
#include "ui_update_queue.h"

#include "asset_manager.h"
#include "config.h"
#include "display/lv_display_private.h"
#include "display_manager.h"
#include "environment_config.h"
#include "hardware_validator.h"
#include "helix_version.h"
#include "keyboard_shortcuts.h"
#include "layout_manager.h"
#include "led/led_controller.h"
#include "moonraker_manager.h"
#include "panel_factory.h"
#include "job_queue_state.h"
#include "print_history_manager.h"
#include "screenshot.h"
#include "sound_manager.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "streaming_policy.h"
#include "subject_initializer.h"
#include "temperature_history_manager.h"
#include "timelapse_state.h"
#include "wizard_config_paths.h"

// UI headers
#include "ui_ams_mini_status.h"
#include "ui_bed_mesh.h"
#include "ui_card.h"
#include "ui_component_header_bar.h"
#include "ui_crash_report_modal.h"
#include "ui_dialog.h"
#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_error_reporting.h"
#include "ui_fan_control_overlay.h"
#include "ui_gcode_viewer.h"
#include "ui_gradient_canvas.h"
#include "ui_icon.h"
#include "ui_icon_loader.h"
#include "ui_keyboard_manager.h"
#include "ui_nav_manager.h"
#include "ui_notification.h"
#include "ui_notification_history.h"
#include "ui_notification_manager.h"
#include "ui_observer_guard.h"
#include "ui_overlay_network_settings.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_filament.h"
#include "ui_panel_gcode_test.h"
#include "ui_panel_glyphs.h"
#include "ui_panel_history_dashboard.h"
#include "ui_panel_home.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_macros.h"
#include "ui_panel_memory_stats.h"
#include "ui_panel_motion.h"
#include "ui_panel_print_select.h"
#include "ui_panel_print_status.h"
#include "ui_panel_screws_tilt.h"
#include "ui_panel_settings.h"
#include "ui_settings_about.h"
#include "ui_panel_spoolman.h"
#include "ui_panel_step_test.h"
#include "ui_panel_temp_control.h"
#include "ui_panel_test.h"
#include "ui_print_tune_overlay.h"
#include "ui_printer_status_icon.h"
#include "ui_probe_overlay.h"
#include "ui_settings_display.h"
#include "ui_settings_hardware_health.h"
#include "ui_settings_sensors.h"
#include "ui_severity_card.h"
#include "ui_status_pill.h"
#include "ui_switch.h"
#include "ui_temp_display.h"
#include "ui_theme_editor_overlay.h"
#include "ui_toast_manager.h"
#include "ui_touch_calibration_overlay.h"
#include "ui_utils.h"
#include "ui_wizard.h"
#include "ui_wizard_ams_identify.h"
#include "ui_wizard_language_chooser.h"
#include "ui_wizard_touch_calibration.h"
#include "ui_wizard_wifi.h"

#include "active_print_media_manager.h"
#include "android_asset_extractor.h"
#include "data_root_resolver.h"
#include "display_settings_manager.h"
#ifdef HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif
#include "led/ui_led_control_overlay.h"
#include "platform_info.h"
#include "printer_detector.h"
#include "printer_image_manager.h"
#include "safety_settings_manager.h"
#include "system/crash_handler.h"
#include "system/crash_history.h"
#include "system/crash_reporter.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "wifi_manager.h"

// Backend headers
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "gcode_file_modifier.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_translation.h"
#include "hv/hlog.h" // libhv logging - sync level with spdlog
#include "logging_init.h"
#include "lv_i18n_translations.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "lvgl_log_handler.h"
#include "memory_monitor.h"
#include "memory_profiling.h"
#include "memory_utils.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "plugin_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "splash_screen.h"
#include "standard_macros.h"
#include "tips_manager.h"
#include "xml_registration.h"

#include <spdlog/spdlog.h>

#ifdef HELIX_DISPLAY_SDL
#include <SDL.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

using namespace helix;

// External globals for logging (defined in cli_args.cpp, populated by parse_cli_args)
extern std::string g_log_dest_cli;
extern std::string g_log_file_cli;
extern std::string g_log_level_cli;

namespace {

// Crash loop detection marker file path
constexpr const char* CRASH_MARKER_PATH = "config/.crash_restart_count";

/**
 * @brief Recursively invalidate all widgets in the tree
 *
 * With LV_DISPLAY_RENDER_MODE_PARTIAL, lv_obj_invalidate() on a parent may not
 * propagate to all descendants. This ensures every widget's area is explicitly
 * marked dirty for the initial framebuffer paint.
 */
void invalidate_all_recursive(lv_obj_t* obj) {
    if (!obj)
        return;
    lv_obj_invalidate(obj);
    uint32_t child_cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_cnt; i++) {
        invalidate_all_recursive(lv_obj_get_child(obj, i));
    }
}

/**
 * @brief Signal handler for SIGINT/SIGTERM — requests graceful quit
 *
 * Uses app_request_quit_signal_safe() which only sets the volatile quit
 * flag, avoiding spdlog and other non-async-signal-safe calls.
 */
void graceful_quit_signal_handler(int /*sig*/) {
    app_request_quit_signal_safe();
}

} // namespace

static constexpr const char* INSTANCE_LOCK_PATH = "config/.helix-screen.lock";

bool Application::acquire_instance_lock() {
    m_lock_fd = open(INSTANCE_LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (m_lock_fd < 0) {
        spdlog::error("[Application] Cannot open lock file {}: {}", INSTANCE_LOCK_PATH,
                      strerror(errno));
        return false;
    }
    if (flock(m_lock_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            spdlog::error("[Application] Another instance of helix-screen is already running");
        } else {
            spdlog::error("[Application] Failed to acquire lock: {}", strerror(errno));
        }
        close(m_lock_fd);
        m_lock_fd = -1;
        return false;
    }
    return true;
}

void Application::release_instance_lock() {
    if (m_lock_fd >= 0) {
        flock(m_lock_fd, LOCK_UN);
        close(m_lock_fd);
        m_lock_fd = -1;
    }
}

Application::Application() = default;

Application::~Application() {
    shutdown();
    release_instance_lock();
}

int Application::run(int argc, char** argv) {
    // Initialize minimal logging first so early log calls don't crash
    helix::logging::init_early();

    // Set libhv log level to WARN immediately - before ANY libhv usage
    // libhv's DEFAULT_LOG_LEVEL is INFO, which causes unwanted output on first start
    hlog_set_level(LOG_LEVEL_WARN);

    spdlog::info("[Application] Starting HelixScreen...");

    // Store argv early for restart capability
    app_store_argv(argc, argv);

#ifdef __ANDROID__
    // Extract APK assets to internal storage before data root resolution
    helix::android_extract_assets_if_needed();
#endif

    // Ensure we're running from the project root
    ensure_project_root_cwd();

    // Prevent multiple instances from running simultaneously.
    // Two instances fighting for DRM causes 100% CPU (flush retry loop) and segfaults.
    if (!acquire_instance_lock()) {
        return 1;
    }

    // Phase 1: Parse command line args
    if (!parse_args(argc, argv)) {
        return 0; // Help shown or parse error
    }

    // Install crash handler early (before other init that could crash)
    // Uses the config directory for the crash file so TelemetryManager can find it on next startup
    // Skip in test mode — don't record or report crashes during development
    if (!get_runtime_config()->is_test_mode()) {
        crash_handler::install("config/crash.txt");
    }

    // Install graceful shutdown signal handlers (Ctrl+C, kill)
    // When running under the watchdog, SIGINT/SIGTERM are handled there.
    // When running standalone (e.g., --test), these allow clean shutdown.
    std::signal(SIGINT, graceful_quit_signal_handler);
    std::signal(SIGTERM, graceful_quit_signal_handler);

    // Phase 2: Initialize config system
    if (!init_config()) {
        return 1;
    }

    // Crash loop detection: track rapid restarts via marker file
    {
        constexpr size_t MAX_CRASH_RESTARTS = 3;
        constexpr long long CRASH_WINDOW_SEC = 120;
        auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

        // Read existing timestamps and filter to recent window
        std::vector<long long> recent_timestamps;
        {
            std::ifstream in(CRASH_MARKER_PATH);
            long long ts;
            while (in >> ts) {
                if (now_epoch - ts < CRASH_WINDOW_SEC) {
                    recent_timestamps.push_back(ts);
                }
            }
        }

        if (recent_timestamps.size() >= MAX_CRASH_RESTARTS) {
            spdlog::error("[Application] Crash loop detected: {} restarts within {}s — "
                          "halting to prevent infinite restart loop",
                          recent_timestamps.size(), CRASH_WINDOW_SEC);
            std::filesystem::remove(CRASH_MARKER_PATH);
            return 1;
        } else {
            // Write filtered timestamps plus current restart
            std::ofstream out(CRASH_MARKER_PATH, std::ios::trunc);
            for (auto ts : recent_timestamps) {
                out << ts << "\n";
            }
            out << now_epoch << "\n";
        }
    }

    // Phase 3: Initialize logging
    if (!init_logging()) {
        return 1;
    }

    spdlog::info("[Application] ========================");
    spdlog::info("[Application] HelixScreen {} ({})", helix_version(), HELIX_GIT_HASH);
    spdlog::debug("[Application] Target: {}x{}", m_screen_width, m_screen_height);
    spdlog::debug("[Application] DPI: {}{}", (m_args.dpi > 0 ? m_args.dpi : LV_DPI_DEF),
                  (m_args.dpi > 0 ? " (custom)" : " (default)"));
    spdlog::debug("[Application] Initial Panel: {}", m_args.initial_panel);

    // Cleanup stale temp files from G-code modifications
    size_t cleaned = helix::gcode::GCodeFileModifier::cleanup_temp_files();
    if (cleaned > 0) {
        spdlog::info("[Application] Cleaned up {} stale G-code temp file(s)", cleaned);
    }

    // Phase 4: Initialize display
    if (!init_display()) {
        return 1;
    }

    // Phase 5: Register fonts and images (fonts needed for globals.xml parsing)
    if (!init_assets()) {
        shutdown();
        return 1;
    }

    // Phase 6: Initialize theme
    if (!init_theme()) {
        shutdown();
        return 1;
    }

    // Phase 7: Register widgets
    if (!register_widgets()) {
        shutdown();
        return 1;
    }

    // Phase 8: Register XML components
    if (!register_xml_components()) {
        shutdown();
        return 1;
    }

    // Phase 8b: Load translations (must be before UI creation for hot-reload support)
    if (!init_translations()) {
        shutdown();
        return 1;
    }

    // Phase 8c: Rotation probe + layout manager init
    // Deferred from init_display() so lv_tr() is available for probe strings.
    // Must run before Phase 9 (UI creation depends on layout dimensions).
    run_rotation_probe_and_layout();

    // Phase 9a: Initialize core subjects and state (PrinterState, AmsState)
    // Must happen before Moonraker init because API creation needs PrinterState
    if (!init_core_subjects()) {
        shutdown();
        return 1;
    }

    // Set multi-printer subjects from config (needed for navbar badge binding)
    {
        auto printer_ids = m_config->get_printer_ids();
        auto active_id = m_config->get_active_printer_id();
        std::string printer_name =
            m_config->get<std::string>(m_config->df() + "printer_name", active_id);
        get_printer_state().set_active_printer_name(printer_name);
        get_printer_state().set_multi_printer_enabled(printer_ids.size() > 1);
    }

    // Phase 9b: Initialize Moonraker (creates client + API)
    // Now works because PrinterState exists from phase 9a
    if (!init_moonraker()) {
        shutdown();
        return 1;
    }

    // Initialize UpdateChecker before panel subjects (subjects must exist for XML binding)
    // On Android, updates are managed by the Play Store — skip self-update system
    if (!helix::is_android_platform()) {
        UpdateChecker::instance().init();
    }

    // Initialize CrashReporter (independent of telemetry)
    // Write mock crash file first if --mock-crash flag is set (requires --test)
    if (get_runtime_config()->mock_crash) {
        crash_handler::write_mock_crash_file("config/crash.txt");
        spdlog::info("[Application] Wrote mock crash file for testing");
    }
    helix::CrashHistory::instance().init("config");
    CrashReporter::instance().init("config");

    // Initialize TelemetryManager (opt-in, default OFF)
    // Note: record_session() is called after init_panel_subjects() so that
    // SettingsManager subjects are ready and the enabled state can be synced.
    TelemetryManager::instance().init();

    // Initialize PrinterImageManager (custom image import/resolution)
    helix::PrinterImageManager::instance().init("config");

    // Phase 9c: Initialize panel subjects with API injection
    // Panels receive API at construction - no deferred set_api() needed
    if (!init_panel_subjects()) {
        shutdown();
        return 1;
    }

    // Sync telemetry enabled state from SystemSettingsManager (now that its subjects are
    // initialized) Note: record_session() is deferred to on_discovery_complete callback so hardware
    // data is available
    TelemetryManager::instance().set_enabled(
        SystemSettingsManager::instance().get_telemetry_enabled());

    // Initialize SoundManager (beta feature - audio feedback)
    if (Config::get_instance()->is_beta_features_enabled()) {
        SoundManager::instance().initialize();
    }

    // Update DisplaySettingsManager with theme mode support (must be after both theme and settings
    // init)
    DisplaySettingsManager::instance().on_theme_changed();

    // Phase 10: Create UI and wire panels
    if (!init_ui()) {
        shutdown();
        return 1;
    }

    // Check for crash from previous session (after UI exists, before wizard)
    // Skip in test mode — don't show crash dialog during development
    // Exception: --mock-crash explicitly requests the dialog for testing
    bool show_crash_dialog =
        !get_runtime_config()->is_test_mode() || get_runtime_config()->mock_crash;
    if (show_crash_dialog && CrashReporter::instance().has_crash_report()) {
        spdlog::info("[Application] Previous crash detected — showing crash report dialog");
        auto report = CrashReporter::instance().collect_report();
        auto* modal = new CrashReportModal();
        modal->set_report(report);
        modal->show_modal(lv_screen_active());
    }

    // Register wizard completion callback for add-printer recovery
    set_wizard_completion_callback([this]() {
        if (!m_wizard_previous_printer_id.empty()) {
            spdlog::info("[Application] Wizard completed — clearing add-printer recovery state");
            m_wizard_previous_printer_id.clear();
        }
        m_wizard_active = false;
    });

    // Cancel callback registered by add_printer_via_wizard() when recovery state exists.
    // Don't register here — initial wizard has nowhere to cancel back to.

    // Phase 11b: Graceful recovery — clean up stale incomplete printer entries
    // If the active printer never finished the wizard (e.g., crash during add-printer),
    // switch to a completed printer and remove the stale entry.
    if (m_config && m_config->is_wizard_required()) {
        auto printer_ids = m_config->get_printer_ids();
        auto active_id = m_config->get_active_printer_id();

        // Find a completed printer to fall back to
        std::string fallback_id;
        for (const auto& id : printer_ids) {
            if (id == active_id)
                continue;
            bool completed = m_config->get<bool>("/printers/" + id + "/wizard_completed", false);
            if (completed) {
                fallback_id = id;
                break;
            }
        }

        if (!fallback_id.empty()) {
            spdlog::info("[Application] Recovering from stale printer '{}' — "
                         "switching to completed printer '{}'",
                         active_id, fallback_id);
            m_config->remove_printer(active_id);
            m_config->set_active_printer(fallback_id);
            m_config->save();
        }
    }

    // Phase 12: Run wizard if needed
    if (run_wizard()) {
        // Wizard is active - it handles its own flow
        m_wizard_active = true;
        set_wizard_active(true);
    }

    // Phase 13: Create overlay panels (if not in wizard)
    if (!m_wizard_active) {
        create_overlays();
    }

    // Phase 14: Initialize and load plugins
    // Must be after UI panels exist (injection points are registered by panels)
    if (!init_plugins()) {
        spdlog::warn("[Application] Plugin initialization had errors (non-fatal)");
    }

    // Phase 14b: Check WiFi availability if expected
    check_wifi_availability();

    // Phase 15: Connect to printer
    if (!connect_moonraker()) {
        // Non-fatal - app can still run without connection
        spdlog::warn("[Application] Running without printer connection");
    }

    // Phase 16: Start memory monitoring (logs at TRACE level, -vvv)
    helix::MemoryMonitor::instance().start(5000);
    helix::MemoryMonitor::instance().set_warning_callback(
        [](const helix::MemoryWarningEvent& event) {
            TelemetryManager::instance().record_memory_warning(event);
        });

    // Phase 16b: Force full screen refresh
    // On framebuffer displays with PARTIAL render mode, some widgets may not paint
    // on the first frame. Schedule a deferred refresh after the first few frames
    // to ensure all widgets are fully rendered.
    //
    // Skip when splash is active: the external splash process owns the framebuffer.
    // lv_display_create() queues an initial dirty area that would flush the wizard
    // UI to fb0 before splash exits, causing a visible flash. The post-splash
    // handler in main_loop() performs this refresh after splash exits.
    if (get_runtime_config()->splash_pid <= 0 || m_splash_manager.has_exited()) {
        lv_obj_update_layout(m_screen);
        invalidate_all_recursive(m_screen);
        lv_refr_now(nullptr);

        // Deferred refresh: Some widgets (nav icons, printer image) may not have their
        // content fully set until after the first frame. Schedule a second refresh.
        static auto deferred_refresh_cb = [](lv_timer_t* timer) {
            lv_obj_t* screen = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
            if (screen) {
                lv_obj_update_layout(screen);
                invalidate_all_recursive(screen);
                lv_refr_now(nullptr);
            }
            lv_timer_delete(timer);
        };
        lv_timer_create(deferred_refresh_cb, 100, m_screen); // 100ms delay
    }

    // Phase 17: Main loop
    helix::MemoryMonitor::log_now("before_main_loop");
    int result = main_loop();

    // Phase 18: Shutdown
    shutdown();

    return result;
}

void Application::ensure_project_root_cwd() {
    using EnvConfig = helix::config::EnvironmentConfig;

    // HELIX_DATA_DIR takes priority - allows standalone deployment
    // Validate BEFORE chdir to avoid corrupting the working directory
    if (auto data_dir = EnvConfig::get_data_dir()) {
        if (helix::is_valid_data_root(data_dir->c_str())) {
            if (chdir(data_dir->c_str()) == 0) {
                spdlog::info("[Application] Using HELIX_DATA_DIR: {}", *data_dir);
                return;
            }
            spdlog::warn("[Application] HELIX_DATA_DIR '{}' valid but chdir failed: {}", *data_dir,
                         strerror(errno));
        } else {
            spdlog::warn("[Application] HELIX_DATA_DIR '{}' has no ui_xml/ directory", *data_dir);
        }
    }

    // Fall back to auto-detection from executable path
    char exe_path[PATH_MAX];

#ifdef __APPLE__
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        spdlog::warn("[Application] Could not get executable path");
        return;
    }
    char resolved[PATH_MAX];
    if (realpath(exe_path, resolved)) {
        strncpy(exe_path, resolved, PATH_MAX - 1);
        exe_path[PATH_MAX - 1] = '\0';
    }
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        spdlog::warn("[Application] Could not read /proc/self/exe");
        return;
    }
    exe_path[len] = '\0';
#else
    return;
#endif

    std::string data_root = helix::resolve_data_root_from_exe(exe_path);
    if (!data_root.empty()) {
        if (chdir(data_root.c_str()) == 0) {
            spdlog::info("[Application] Auto-detected data root: {}", data_root);
            return;
        }
        spdlog::warn("[Application] Found data root '{}' but chdir failed: {}", data_root,
                     strerror(errno));
    }

    // Last resort: check if CWD already has what we need
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) && helix::is_valid_data_root(cwd)) {
        spdlog::debug("[Application] Current working directory is already valid: {}", cwd);
        return;
    }

    spdlog::error("[Application] Could not find HelixScreen data root (ui_xml/ directory). "
                  "Set HELIX_DATA_DIR or run from the install directory.");
}

bool Application::parse_args(int argc, char** argv) {
    // Parse CLI args first
    if (!helix::parse_cli_args(argc, argv, m_args, m_screen_width, m_screen_height)) {
        return false;
    }

    // Auto-configure mock state based on requested panel (after parsing args)
    auto_configure_mock_state();

    // Apply environment variable overrides using type-safe EnvironmentConfig
    using EnvConfig = helix::config::EnvironmentConfig;

    // HELIX_AUTO_QUIT_MS: auto-quit timeout (100ms - 1hr)
    if (m_args.timeout_sec == 0) {
        if (auto timeout = EnvConfig::get_auto_quit_seconds()) {
            m_args.timeout_sec = *timeout;
        }
    }

    // HELIX_AUTO_SCREENSHOT: enable screenshot mode
    if (EnvConfig::get_screenshot_enabled()) {
        m_args.screenshot_enabled = true;
    }

    // HELIX_AMS_GATES: mock AMS gate count (1-16)
    if (auto gates = EnvConfig::get_mock_ams_gates()) {
        get_runtime_config()->mock_ams_gate_count = *gates;
    }

    // HELIX_BENCHMARK: benchmark mode
    if (EnvConfig::get_benchmark_mode()) {
        spdlog::info("[Application] Benchmark mode enabled");
    }

    return true;
}

void Application::auto_configure_mock_state() {
    RuntimeConfig* config = get_runtime_config();

    if (config->test_mode && !config->use_real_moonraker) {
        if (m_args.overlays.print_status) {
            config->mock_auto_start_print = true;
            if (!config->gcode_test_file) {
                config->gcode_test_file = RuntimeConfig::get_default_test_file_path();
            }
            spdlog::info("[Auto] Mock will simulate active print with '{}'",
                         config->gcode_test_file);
        }

        // Auto-select a file only when explicitly requesting detail view (print-detail)
        if (m_args.overlays.file_detail && !config->select_file) {
            config->select_file = RuntimeConfig::DEFAULT_TEST_FILE;
            spdlog::info("[Auto] Auto-selecting '{}' for print-detail panel",
                         RuntimeConfig::DEFAULT_TEST_FILE);
        }

        if (m_args.overlays.history_dashboard) {
            config->mock_auto_history = true;
            spdlog::info("[Auto] Mock will generate history data for history panel");
        }
    }
}

bool Application::init_config() {
    m_config = Config::get_instance();

    // Use separate config file for test mode to avoid conflicts with real printer settings
    const char* config_path = get_runtime_config()->test_mode ? RuntimeConfig::TEST_CONFIG_PATH
                                                              : RuntimeConfig::PROD_CONFIG_PATH;
    spdlog::info("[Application] Using config: {}", config_path);
    m_config->init(config_path);

    // Initialize streaming policy from config (auto-detects thresholds from RAM)
    helix::StreamingPolicy::instance().load_from_config();

    return true;
}

bool Application::init_logging() {
    using namespace helix::logging;

    LogConfig log_config;

    // Resolve log level with precedence: --log-level > -v flags > config file > defaults
    std::string config_level = m_config->get<std::string>("/log_level", "");
    if (!g_log_level_cli.empty()) {
        log_config.level = parse_level(g_log_level_cli, spdlog::level::warn);
    } else {
        log_config.level =
            resolve_log_level(m_args.verbosity, config_level, get_runtime_config()->test_mode);
    }

    // Resolve log destination: CLI > config > auto
    std::string log_dest_str = g_log_dest_cli;
    if (log_dest_str.empty()) {
        log_dest_str = m_config->get<std::string>("/log_dest", "auto");
    }
    log_config.target = parse_log_target(log_dest_str);

    // Resolve log file path: CLI > config
    log_config.file_path = g_log_file_cli;
    if (log_config.file_path.empty()) {
        log_config.file_path = m_config->get<std::string>("/log_path", "");
    }

    init(log_config);

    // Set libhv log level from config (CLI -v flags don't affect libhv)
    spdlog::level::level_enum hv_spdlog_level = parse_level(config_level, spdlog::level::warn);
    hlog_set_level(to_hv_level(hv_spdlog_level));

    return true;
}

bool Application::init_display() {
#ifdef HELIX_DISPLAY_SDL
    // Set window position environment variables
    if (m_args.display_num >= 0) {
        char display_str[32];
        snprintf(display_str, sizeof(display_str), "%d", m_args.display_num);
        setenv("HELIX_SDL_DISPLAY", display_str, 1);
    }
    if (m_args.x_pos >= 0 && m_args.y_pos >= 0) {
        char x_str[32], y_str[32];
        snprintf(x_str, sizeof(x_str), "%d", m_args.x_pos);
        snprintf(y_str, sizeof(y_str), "%d", m_args.y_pos);
        setenv("HELIX_SDL_XPOS", x_str, 1);
        setenv("HELIX_SDL_YPOS", y_str, 1);
    }
#endif

    m_display = std::make_unique<DisplayManager>();
    DisplayManager::Config config;
    config.width = m_screen_width;
    config.height = m_screen_height;
    config.rotation = m_args.rotation;

    // Get scroll config from helixconfig.json
    config.scroll_throw = m_config->get<int>("/input/scroll_throw", 25);
    config.scroll_limit = m_config->get<int>("/input/scroll_limit", 10);

    // Allow headless/VNC operation without a touchscreen
    const char* req_ptr = std::getenv("HELIX_REQUIRE_POINTER");
    if (req_ptr && (std::string(req_ptr) == "0" || std::string(req_ptr) == "false")) {
        config.require_pointer = false;
        spdlog::info("[Application] Pointer input not required (HELIX_REQUIRE_POINTER={})",
                     req_ptr);
    }

    // Tell DisplayManager to skip framebuffer ioctls (FBIOBLANK, FBIOPAN_DISPLAY)
    // when splash is active — the splash process already owns and configured the display.
    config.splash_active = (get_runtime_config()->splash_pid > 0);

    if (!m_display->init(config)) {
        spdlog::error("[Application] Display initialization failed");
        return false;
    }

    // Update screen dimensions from what the display actually resolved to.
    // DisplayManager::init() handles config rotation AND kernel panel orientation
    // auto-detection (with DRM→fbdev fallback), so dimensions already reflect
    // any rotation applied.
    m_screen_width = m_display->width();
    m_screen_height = m_display->height();

    // Register LVGL log handler AFTER lv_init() (called inside display->init())
    // Must be after lv_init() because it resets global state and clears callbacks
    helix::logging::register_lvgl_log_handler();

    // Apply custom DPI if specified
    if (m_args.dpi > 0) {
        lv_display_set_dpi(m_display->display(), m_args.dpi);
    }

    // Get active screen
    m_screen = lv_screen_active();

    // Set window icon
    ui_set_window_icon(m_display->display());

    // Initialize resize handler
    m_display->init_resize_handler(m_screen);

    // Initialize tips manager
    TipsManager* tips_mgr = TipsManager::get_instance();
    if (!tips_mgr->init("config/printing_tips.json")) {
        spdlog::warn("[Application] Failed to initialize tips manager");
    }

    spdlog::debug("[Application] Display initialized");
    helix::MemoryMonitor::log_now("after_display_init");

    // Initialize splash screen manager for deferred exit
    m_splash_manager.start(get_runtime_config()->splash_pid);

    // Suppress LVGL rendering while splash is alive — prevents framebuffer flicker
    // from both processes writing to the same framebuffer simultaneously.
    // Re-enabled in main loop when splash exits.
    // Validate PID exists: a stale PID from a crashed launcher would cause an
    // unnecessary wait until the 8-second failsafe kicks in.
    pid_t splash_pid = get_runtime_config()->splash_pid;
    if (splash_pid > 0 && kill(splash_pid, 0) == 0 && !m_splash_manager.has_exited()) {
        lv_display_enable_invalidation(nullptr, false);

        // Replace the flush callback with a no-op while splash is active.
        // LVGL's invalidation system sends LV_EVENT_REFR_REQUEST which resumes
        // the refresh timer (undoing lv_timer_pause). This means pausing the timer
        // alone is insufficient — rendering still happens. By replacing the flush
        // callback, we ensure nothing reaches the framebuffer even if LVGL renders.
        lv_display_t* disp = lv_display_get_default();
        if (disp) {
            m_original_flush_cb = disp->flush_cb;
            lv_display_set_flush_cb(disp, [](lv_display_t* d, const lv_area_t*, uint8_t*) {
                lv_display_flush_ready(d); // Must signal ready to avoid hang
            });
            spdlog::debug("[Application] Flush callback replaced with no-op (splash PID {})",
                          get_runtime_config()->splash_pid);
        }
        spdlog::debug("[Application] Display invalidation suppressed while splash is active");
    }

    return true;
}

bool Application::init_theme() {
    // Determine theme mode
    bool dark_mode;
    if (m_args.dark_mode_cli >= 0) {
        dark_mode = (m_args.dark_mode_cli == 1);
    } else {
        dark_mode = m_config->get<bool>("/dark_mode", true);
    }

    // Register globals.xml first (required for theme constants, fonts, spacing tokens)
    // Note: fonts must be registered before this (done in init_assets phase)
    lv_result_t globals_result = lv_xml_register_component_from_file("A:ui_xml/globals.xml");
    if (globals_result != LV_RESULT_OK) {
        spdlog::error("[Application] FATAL: Failed to load globals.xml - "
                      "all XML constants (fonts, colors, spacing) will be missing. "
                      "Check working directory and verify ui_xml/globals.xml exists.");
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            spdlog::error("[Application] Current working directory: {}", cwd);
        }
        return false;
    }

    // Initialize theme
    theme_manager_init(m_display->display(), dark_mode);

    // Apply background color to screen
    theme_manager_apply_bg_color(m_screen, "screen_bg", LV_PART_MAIN);

    // Show LVGL splash screen only when no external splash process is running.
    // On embedded targets, helix-splash provides visual coverage during startup;
    // showing the internal splash too causes a visible double-splash.
    if (!get_runtime_config()->should_skip_splash() && get_runtime_config()->splash_pid <= 0) {
        helix::show_splash_screen(m_screen_width, m_screen_height);
    }

    spdlog::debug("[Application] Theme initialized (dark={})", dark_mode);
    return true;
}

bool Application::init_assets() {
    AssetManager::register_all();

    // TJPGD (built-in JPEG decoder) is auto-initialized by LVGL when LV_USE_TJPGD=1
    spdlog::debug("[Application] Assets registered");
    helix::MemoryMonitor::log_now("after_fonts_loaded");
    return true;
}

void Application::run_rotation_probe_and_layout() {
    // Run rotation probe on first boot if no rotation is configured.
    // Skip if: CLI rotation set, env var set, already probed, or config already
    // has a /display/rotate key (even if 0 — means user already configured it).
    // HELIX_FORCE_ROTATION_PROBE=1 bypasses all guards (for testing on SDL).
    {
        bool force_probe = (std::getenv("HELIX_FORCE_ROTATION_PROBE") != nullptr);
        bool should_probe = false;

        if (force_probe) {
            should_probe = true;
            spdlog::info("[Application] Rotation probe forced via HELIX_FORCE_ROTATION_PROBE");
        }
#if defined(HELIX_DISPLAY_FBDEV) || defined(HELIX_DISPLAY_DRM)
        else if (m_args.rotation == 0 && !std::getenv("HELIX_DISPLAY_ROTATION")) {
            bool probed = m_config->get<bool>("/display/rotation_probed", false);
            bool has_rotate_key = m_config->exists("/display/rotate");
            should_probe = !probed && !has_rotate_key;
            if (!should_probe) {
                spdlog::info("[Application] Rotation probe skipped: probed={}, has_rotate_key={}",
                             probed, has_rotate_key);
            }
        } else {
            spdlog::info(
                "[Application] Rotation probe skipped: cli_rotation={}, env={}", m_args.rotation,
                std::getenv("HELIX_DISPLAY_ROTATION") ? std::getenv("HELIX_DISPLAY_ROTATION")
                                                      : "unset");
        }
#else
        spdlog::debug("[Application] Rotation probe skipped: not embedded build");
#endif

        if (should_probe) {
            // Try auto-detecting panel orientation from kernel first.
            // panel_orientation is informational — the kernel does NOT rotate
            // the framebuffer for us. We must apply the rotation ourselves.
            int kernel_orientation = DisplayBackend::detect_panel_orientation();
            if (kernel_orientation >= 0) {
                // Orientation detected (0=Normal, 90, 180, 270).
                if (kernel_orientation > 0) {
                    spdlog::info("[Application] Auto-detected panel orientation: {}° — "
                                 "applying now and saving to config",
                                 kernel_orientation);
                    m_display->apply_rotation(kernel_orientation);
                    m_screen_width = m_display->width();
                    m_screen_height = m_display->height();
                } else {
                    spdlog::info("[Application] Auto-detected panel orientation: Normal (0°) — "
                                 "no rotation needed, saving to config");
                }
                m_config->set("/display/rotate", kernel_orientation);
                m_config->set("/display/rotation_probed", true);
                m_config->save();
            } else {
                // kernel_orientation == -1: not detected, run interactive probe.
                // Dismiss splash first — the probe renders full-screen UI to the
                // framebuffer, which is invisible while the splash process is
                // painting over it and the flush callback is suppressed.
                if (!m_splash_manager.has_exited()) {
                    spdlog::info("[Application] Dismissing splash for rotation probe");
                    m_splash_manager.on_discovery_complete();
                    m_splash_manager.check_and_signal();
                    restore_flush_callback();
                    lv_display_enable_invalidation(nullptr, true);
                }
                m_display->run_rotation_probe();
                m_screen_width = m_display->width();
                m_screen_height = m_display->height();
            }
        }
    }

    // Initialize layout manager (after display dimensions are known)
    auto& layout_mgr = helix::LayoutManager::instance();
    if (!m_args.layout.empty() && m_args.layout != "auto") {
        layout_mgr.set_override(m_args.layout);
    } else {
        std::string config_layout = m_config->get<std::string>("/display/layout", "auto");
        if (config_layout != "auto") {
            layout_mgr.set_override(config_layout);
        }
    }
    layout_mgr.init(m_screen_width, m_screen_height);
    spdlog::info("[Application] Layout: {} ({})", layout_mgr.name(),
                 layout_mgr.is_standard() ? "default" : "override");
}

bool Application::register_widgets() {
    ui_icon_register_widget();
    ui_status_pill_register_widget();
    ui_switch_register();
    ui_card_register();
    ui_temp_display_init();
    ui_ams_mini_status_init();
    ui_severity_card_register();
    ui_dialog_register();
    ui_bed_mesh_register();
    ui_gcode_viewer_register();
    ui_gradient_canvas_register();

    // Initialize component systems
    ui_component_header_bar_init();

    // Small delay to stabilize display
    DisplayManager::delay(100);

    // Initialize memory profiling
    helix::MemoryProfiler::init(m_args.memory_report);

    // Log system memory info
    auto mem = helix::get_system_memory_info();
    spdlog::debug("[Application] System memory: total={}MB, available={}MB", mem.total_kb / 1024,
                  mem.available_mb());

    spdlog::debug("[Application] Widgets registered");
    return true;
}

bool Application::register_xml_components() {
    helix::register_xml_components();
    spdlog::debug("[Application] XML components registered");

    // Start XML hot reloader if enabled (dev-only, HELIX_HOT_RELOAD=1)
    if (RuntimeConfig::hot_reload_enabled()) {
        m_hot_reloader = std::make_unique<helix::XmlHotReloader>();
        m_hot_reloader->start({"ui_xml", "ui_xml/components"});
    }

    return true;
}

bool Application::init_translations() {
    // Load translation strings from XML (for LVGL's native translation system)
    // This must happen before UI creation but after the XML system is initialized
    lv_result_t result =
        lv_xml_register_translation_from_file("A:ui_xml/translations/translations.xml");
    if (result != LV_RESULT_OK) {
        spdlog::warn(
            "[Application] Failed to load LVGL translations - UI will use English defaults");
        // Not fatal - English will work via fallback (tag = English text)
    } else {
        spdlog::debug("[Application] LVGL translations loaded successfully");
    }

    // Initialize lv_i18n translation system (for plural forms and runtime lookups)
    int i18n_result = lv_i18n_init(lv_i18n_language_pack);
    if (i18n_result != 0) {
        spdlog::warn(
            "[Application] Failed to initialize lv_i18n - plural translations unavailable");
    } else {
        spdlog::debug("[Application] lv_i18n initialized successfully");
    }

    // Set initial language from config (sync both systems)
    std::string lang = m_config->get_language();
    lv_translation_set_language(lang.c_str());
    lv_i18n_set_locale(lang.c_str());
    spdlog::info("[Application] Language set to '{}' (both translation systems)", lang);

    return true;
}

bool Application::init_core_subjects() {
    m_subjects = std::make_unique<SubjectInitializer>();

    // Phase 1-3: Core subjects, PrinterState, AmsState
    // These must exist before MoonrakerManager::init() can create the API
    m_subjects->init_core_and_state();

    spdlog::debug("[Application] Core subjects initialized");
    helix::MemoryMonitor::log_now("after_core_subjects_init");
    return true;
}

bool Application::init_panel_subjects() {
    // Phase 4: Panel subjects with API injection
    // API is now available from MoonrakerManager
    m_subjects->init_panels(m_moonraker->api(), *get_runtime_config());

    // Phase 5-7: Observers and utility subjects
    m_subjects->init_post(*get_runtime_config());

    // Initialize EmergencyStopOverlay (moved from MoonrakerManager)
    // Must happen after both API and EmergencyStopOverlay::init_subjects()
    EmergencyStopOverlay::instance().init(get_printer_state(), m_moonraker->api());
    EmergencyStopOverlay::instance().create();
    EmergencyStopOverlay::instance().set_require_confirmation(
        SafetySettingsManager::instance().get_estop_require_confirmation());

    // Initialize AbortManager for smart print cancellation
    // Must happen after both API and AbortManager::init_subjects()
    helix::AbortManager::instance().init(m_moonraker->api(), &get_printer_state());

    // Register notification callbacks
    helix::ui::notification_register_callbacks();
    ui_panel_screws_tilt_register_callbacks();
    ui_panel_input_shaper_register_callbacks();
    ui_probe_overlay_register_callbacks();

    // Create temperature history manager (collects temp samples from PrinterState subjects)
    m_temp_history_manager = std::make_unique<TemperatureHistoryManager>(get_printer_state());
    set_temperature_history_manager(m_temp_history_manager.get());
    spdlog::debug("[Application] TemperatureHistoryManager created");

    spdlog::debug("[Application] Panel subjects initialized");
    helix::MemoryMonitor::log_now("after_panel_subjects_init");
    return true;
}

bool Application::init_ui() {
    // Create entire UI from XML
    m_app_layout = static_cast<lv_obj_t*>(lv_xml_create(m_screen, "app_layout", nullptr));
    if (!m_app_layout) {
        spdlog::error("[Application] Failed to create app_layout from XML");
        return false;
    }

    // Disable scrollbars on screen
    lv_obj_clear_flag(m_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(m_screen, LV_SCROLLBAR_MODE_OFF);

    // Force layout calculation
    lv_obj_update_layout(m_screen);

    // Register app_layout with navigation
    NavigationManager::instance().set_app_layout(m_app_layout);

    // Initialize printer status icon (sets up observers on PrinterState)
    PrinterStatusIcon::instance().init();

    // Initialize notification system
    helix::ui::notification_manager_init();

    // Seed test notifications in --test mode for debugging
    if (get_runtime_config()->is_test_mode()) {
        auto& history = NotificationHistory::instance();
        history.seed_test_data();
        // Update notification badge to show unread count and severity
        helix::ui::notification_update_count(history.get_unread_count());
        // Map ToastSeverity to NotificationStatus for bell color
        auto severity = history.get_highest_unread_severity();
        NotificationStatus status = NotificationStatus::NONE;
        if (severity == ToastSeverity::ERROR) {
            status = NotificationStatus::ERROR;
        } else if (severity == ToastSeverity::WARNING) {
            status = NotificationStatus::WARNING;
        } else if (severity == ToastSeverity::INFO || severity == ToastSeverity::SUCCESS) {
            status = NotificationStatus::INFO;
        }
        helix::ui::notification_update(status);
    }

    // Initialize toast system
    ToastManager::instance().init();

    // Initialize overlay backdrop
    NavigationManager::instance().init_overlay_backdrop(m_screen);

    // Find navbar and content area
    lv_obj_t* navbar = lv_obj_find_by_name(m_app_layout, "navbar");
    lv_obj_t* content_area = lv_obj_find_by_name(m_app_layout, "content_area");

    if (!navbar || !content_area) {
        spdlog::error("[Application] Failed to find navbar/content_area");
        return false;
    }

    // Wire navigation
    NavigationManager::instance().wire_events(navbar);

    // Register printer switch/add callbacks so navbar badge menu can trigger actions
    NavigationManager::instance().set_printer_callbacks(
        [this](const std::string& printer_id) { switch_printer(printer_id); },
        [this]() { add_printer_via_wizard(); });

    // Find panel container
    lv_obj_t* panel_container = lv_obj_find_by_name(content_area, "panel_container");
    if (!panel_container) {
        spdlog::error("[Application] Failed to find panel_container");
        return false;
    }

    // Initialize panels
    m_panels = std::make_unique<PanelFactory>();
    if (!m_panels->find_panels(panel_container)) {
        return false;
    }
    m_panels->setup_panels(m_screen);

    // Create print status overlay
    if (!m_panels->create_print_status_overlay(m_screen)) {
        spdlog::error("[Application] Failed to create print status overlay");
        return false;
    }
    // print_status is now lazily created via PrintStatusPanel::push_overlay()
    m_overlay_panels.print_status = nullptr;

    // Initialize keypad
    m_panels->init_keypad(m_screen);

    spdlog::info("[Application] UI created successfully");
    helix::MemoryMonitor::log_now("after_ui_created");
    return true;
}

bool Application::init_moonraker() {
    m_moonraker = std::make_unique<MoonrakerManager>();
    if (!m_moonraker->init(*get_runtime_config(), m_config)) {
        spdlog::error("[Application] Moonraker initialization failed");
        return false;
    }

    // API is now injected at panel construction in init_panel_subjects()
    // No need for deferred inject_api() call

    // Register MoonrakerManager globally (for Advanced panel access to MacroModificationManager)
    set_moonraker_manager(m_moonraker.get());

    // Set up discovery callbacks on client (must be after API creation since API constructor
    // also sets these callbacks - we intentionally overwrite with combined callbacks that
    // both update the API's hardware_ and perform Application-level initialization)
    setup_discovery_callbacks();

    // Create print history manager (shared cache for history panels and file status indicators)
    m_history_manager =
        std::make_unique<PrintHistoryManager>(m_moonraker->api(), get_moonraker_client());
    set_print_history_manager(m_history_manager.get());
    spdlog::debug("[Application] PrintHistoryManager created");

    // Create job queue state manager
    m_job_queue_state =
        std::make_unique<JobQueueState>(m_moonraker->api(), get_moonraker_client());
    m_job_queue_state->init_subjects();
    set_job_queue_state(m_job_queue_state.get());
    spdlog::debug("[Application] JobQueueState created");

    // Initialize macro modification manager (for PRINT_START wizard)
    m_moonraker->init_macro_analysis(m_config);

    // Validate screen before keyboard init (debugging potential race condition)
    if (!m_screen) {
        spdlog::error("[Application] m_screen is NULL before keyboard init!");
        return false;
    }
    lv_obj_t* active_screen = lv_screen_active();
    if (m_screen != active_screen) {
        spdlog::error("[Application] m_screen ({:p}) differs from active screen ({:p})!",
                      static_cast<void*>(m_screen), static_cast<void*>(active_screen));
        // Use the current active screen instead
        m_screen = active_screen;
    }

    // Initialize global keyboard
    KeyboardManager::instance().init(m_screen);

    // Initialize memory stats overlay
    MemoryStatsOverlay::instance().init(m_screen, m_args.show_memory);

    spdlog::debug("[Application] Moonraker initialized");
    helix::MemoryMonitor::log_now("after_moonraker_init");
    return true;
}

bool Application::init_plugins() {
    spdlog::debug("[Application] Initializing plugin system");

    m_plugin_manager = std::make_unique<helix::plugin::PluginManager>();

    // Set core services - API and client may be nullptr if mock mode
    m_plugin_manager->set_core_services(m_moonraker->api(), m_moonraker->client(),
                                        get_printer_state(), m_config);

    // Read enabled plugins from config
    auto enabled_plugins =
        m_config->get<std::vector<std::string>>("/plugins/enabled", std::vector<std::string>{});
    m_plugin_manager->set_enabled_plugins(enabled_plugins);
    spdlog::debug("[Application] Enabled plugins from config: {}", enabled_plugins.size());

    // Discover plugins in the plugins directory
    if (!m_plugin_manager->discover_plugins("plugins")) {
        spdlog::error("[Application] Plugin discovery failed");
        return false;
    }

    // Load all enabled plugins
    bool all_loaded = m_plugin_manager->load_all();

    // Log any errors and show toast notification with action buttons
    auto errors = m_plugin_manager->get_load_errors();
    if (!errors.empty()) {
        spdlog::warn("[Application] {} plugin(s) failed to load", errors.size());
        for (const auto& err : errors) {
            spdlog::warn("[Application]   - {}: {}", err.plugin_id, err.message);
        }

        if (errors.size() == 1) {
            // Single failure: Show [Disable] button for quick action
            // Context struct to pass plugin_id and manager pointer to callback
            struct PluginDisableContext {
                helix::plugin::PluginManager* manager;
                std::string plugin_id;
            };
            auto* ctx = new PluginDisableContext{m_plugin_manager.get(), errors[0].plugin_id};

            char toast_msg[96];
            snprintf(toast_msg, sizeof(toast_msg), "\"%s\" failed to load",
                     errors[0].plugin_id.c_str());

            ToastManager::instance().show_with_action(
                ToastSeverity::WARNING, toast_msg, "Disable",
                [](void* user_data) {
                    auto* ctx = static_cast<PluginDisableContext*>(user_data);
                    if (ctx->manager && ctx->manager->disable_plugin(ctx->plugin_id)) {
                        ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                      lv_tr("Plugin disabled"), 3000);
                    }
                    delete ctx;
                },
                ctx, 8000);
        } else {
            // Multiple failures: Show [Manage] button to open Settings > Plugins
            char toast_msg[64];
            snprintf(toast_msg, sizeof(toast_msg), "%zu plugins failed to load", errors.size());

            ToastManager::instance().show_with_action(
                ToastSeverity::WARNING, toast_msg, "Manage",
                [](void* /*user_data*/) {
                    NavigationManager::instance().set_active(PanelId::Settings);
                    get_global_settings_panel().handle_plugins_clicked();
                },
                nullptr, 8000);
        }
    }

    auto loaded = m_plugin_manager->get_loaded_plugins();
    spdlog::debug("[Application] {} plugin(s) loaded successfully", loaded.size());

    helix::MemoryMonitor::log_now("after_plugins_loaded");
    return all_loaded;
}

bool Application::run_wizard() {
    bool wizard_required = (m_args.force_wizard || m_config->is_wizard_required()) &&
                           !m_args.overlays.step_test && !m_args.overlays.test_panel &&
                           !m_args.overlays.keypad && !m_args.overlays.keyboard &&
                           !m_args.overlays.gcode_test && !m_args.overlays.wizard_ams_identify &&
                           !m_args.panel_requested;

    if (!wizard_required) {
        return false;
    }

    spdlog::info("[Application] Starting first-run wizard");

    // When re-running wizard (--wizard), clear all wizard-managed config so
    // stale hardware selections don't trigger false hardware health warnings
    if (m_args.force_wizard && m_config) {
        spdlog::info("[Application] Re-running wizard — clearing wizard configuration");

        // Clear hardware validation state
        m_config->set<nlohmann::json>(m_config->df() + "hardware/expected",
                                      nlohmann::json::array());
        m_config->set<nlohmann::json>(m_config->df() + "hardware/optional",
                                      nlohmann::json::array());
        m_config->set<nlohmann::json>(m_config->df() + "hardware/last_snapshot",
                                      nlohmann::json::object());

        // Clear wizard hardware selections (heaters, fans, LEDs, sensors)
        const char* wizard_suffixes[] = {
            helix::wizard::BED_HEATER,    helix::wizard::HOTEND_HEATER, helix::wizard::BED_SENSOR,
            helix::wizard::HOTEND_SENSOR, helix::wizard::HOTEND_FAN,    helix::wizard::PART_FAN,
            helix::wizard::CHAMBER_FAN,   helix::wizard::EXHAUST_FAN,   helix::wizard::LED_STRIP,
        };
        for (const auto* suffix : wizard_suffixes) {
            m_config->set<std::string>(m_config->df() + suffix, "");
        }
        m_config->set<nlohmann::json>(m_config->df() + helix::wizard::LED_SELECTED,
                                      nlohmann::json::array());
        m_config->set<nlohmann::json>(m_config->df() + "filament_sensors/sensors",
                                      nlohmann::json::array());

        m_config->save();
    }

    ui_wizard_register_event_callbacks();
    ui_wizard_container_register_responsive_constants();

    lv_obj_t* wizard = ui_wizard_create(m_screen);
    if (!wizard) {
        spdlog::error("[Application] Failed to create wizard");
        return false;
    }

    // Determine initial wizard step (step 0 = touch calibration, auto-skipped if not needed)
    int initial_step = (m_args.wizard_step >= 0) ? m_args.wizard_step : 0;

    // If step 0 was explicitly requested, force-show touch calibration (for visual testing)
    if (m_args.wizard_step == 0) {
        force_touch_calibration_step(true);
    }

    // If step 1 was explicitly requested, force-show language chooser (for visual testing)
    if (m_args.wizard_step == 1) {
        force_language_chooser_step(true);
    }

    ui_wizard_navigate_to_step(initial_step);

    // Move keyboard above wizard
    lv_obj_t* keyboard = KeyboardManager::instance().get_instance();
    if (keyboard) {
        lv_obj_move_foreground(keyboard);
    }

    return true;
}

void Application::create_overlays() {
    // Navigate to initial panel
    if (m_args.initial_panel >= 0) {
        NavigationManager::instance().set_active(static_cast<PanelId>(m_args.initial_panel));
    }

    // Create requested overlay panels
    if (m_args.overlays.motion) {
        auto& motion = get_global_motion_panel();

        // Initialize subjects and callbacks if not already done
        if (!motion.are_subjects_initialized()) {
            motion.init_subjects();
        }
        motion.register_callbacks();

        // Create overlay UI
        auto* p = motion.create(m_screen);
        if (p) {
            m_overlay_panels.motion = p;
            NavigationManager::instance().register_overlay_instance(p, &motion);
            NavigationManager::instance().push_overlay(p);
        }
    }

    if (m_args.overlays.nozzle_temp) {
        if (auto* p = create_overlay_panel(m_screen, "nozzle_temp_panel", "nozzle temp")) {
            m_overlay_panels.nozzle_temp = p;
            m_subjects->temp_control_panel()->setup_nozzle_panel(p, m_screen);
            NavigationManager::instance().push_overlay(p);
        }
    }

    if (m_args.overlays.bed_temp) {
        if (auto* p = create_overlay_panel(m_screen, "bed_temp_panel", "bed temp")) {
            m_overlay_panels.bed_temp = p;
            m_subjects->temp_control_panel()->setup_bed_panel(p, m_screen);
            NavigationManager::instance().push_overlay(p);
        }
    }

    if (m_args.overlays.fan) {
        auto& overlay = get_fan_control_overlay();

        // Initialize subjects and callbacks if not already done
        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();

        // Pass API reference for fan commands
        overlay.set_api(get_moonraker_api());

        // Create overlay UI
        auto* p = overlay.create(m_screen);
        if (p) {
            NavigationManager::instance().register_overlay_instance(p, &overlay);
            NavigationManager::instance().push_overlay(p);
        }
    }

    if (m_args.overlays.led) {
        auto& overlay = get_led_control_overlay();

        // Initialize subjects and callbacks if not already done
        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();

        // Pass API reference for LED commands
        overlay.set_api(get_moonraker_api());

        // Create overlay UI
        auto* p = overlay.create(m_screen);
        if (p) {
            NavigationManager::instance().register_overlay_instance(p, &overlay);
            NavigationManager::instance().push_overlay(p);
        }
    }

    if (m_args.overlays.print_status) {
        PrintStatusPanel::push_overlay(m_screen);
    }

    if (m_args.overlays.bed_mesh) {
        auto& overlay = get_global_bed_mesh_panel();

        // Initialize subjects and callbacks if not already done
        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();

        // Create overlay UI
        auto* p = overlay.create(m_screen);
        if (p) {
            m_overlay_panels.bed_mesh = p;
            NavigationManager::instance().register_overlay_instance(p, &overlay);
            NavigationManager::instance().push_overlay(p);
        }
    }

    if (m_args.overlays.zoffset) {
        auto& overlay = get_global_zoffset_cal_panel();
        // init_subjects already called by SubjectInitializer
        overlay.set_api(m_moonraker->api());
        if (overlay.create(m_screen)) {
            overlay.show();
        }
    }

    if (m_args.overlays.pid) {
        auto& overlay = get_global_pid_cal_panel();
        // init_subjects already called by SubjectInitializer
        overlay.set_api(m_moonraker->api());
        if (get_runtime_config()->test_mode) {
            overlay.request_demo_inject();
        }
        if (overlay.create(m_screen)) {
            overlay.show();
        }
    }

    if (m_args.overlays.screws_tilt) {
        auto& overlay = get_global_screws_tilt_panel();
        // init_subjects already called by SubjectInitializer
        overlay.set_client(m_moonraker->client(), m_moonraker->api());
        if (overlay.create(m_screen)) {
            overlay.show();
        }
    }

    if (m_args.overlays.input_shaper) {
        auto& panel = get_global_input_shaper_panel();
        panel.set_api(m_moonraker->client(), m_moonraker->api());
        if (get_runtime_config()->test_mode) {
            panel.request_demo_inject();
        }
        if (panel.create(m_screen)) {
            panel.show();
        }
    }

    if (m_args.overlays.history_dashboard) {
        auto& overlay = get_global_history_dashboard_panel();
        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();
        auto* p = overlay.create(m_screen);
        if (p) {
            NavigationManager::instance().register_overlay_instance(p, &overlay);
            NavigationManager::instance().push_overlay(p);
        }
    }

    if (m_args.overlays.step_test) {
        if (auto* p = create_overlay_panel(m_screen, "step_progress_test", "step progress")) {
            get_global_step_test_panel().setup(p, m_screen);
        }
    }

    if (m_args.overlays.test_panel) {
        if (auto* p = create_overlay_panel(m_screen, "test_panel", "test")) {
            get_global_test_panel().setup(p, m_screen);
        }
    }

    if (m_args.overlays.gcode_test) {
        ui_panel_gcode_test_create(m_screen);
    }

    if (m_args.overlays.glyphs) {
        ui_panel_glyphs_create(m_screen);
    }

    if (m_args.overlays.gradient_test) {
        create_overlay_panel(m_screen, "gradient_test_panel", "gradient test");
    }

    if (m_args.overlays.ams) {
        // Use multi-unit-aware navigation: shows overview for multi-unit,
        // detail panel directly for single-unit
        navigate_to_ams_panel();
    }

    if (m_args.overlays.spoolman) {
        auto& spoolman = get_global_spoolman_panel();
        if (!spoolman.are_subjects_initialized()) {
            spoolman.init_subjects();
        }
        spoolman.register_callbacks();
        lv_obj_t* panel_obj = spoolman.create(m_screen);
        if (panel_obj) {
            NavigationManager::instance().register_overlay_instance(panel_obj, &spoolman);
            NavigationManager::instance().push_overlay(panel_obj);
        }
    }

    if (m_args.overlays.wizard_ams_identify) {
        auto* step = get_wizard_ams_identify_step();
        step->init_subjects();
        lv_obj_t* panel_obj = step->create(m_screen);
        if (panel_obj) {
            NavigationManager::instance().push_overlay(panel_obj);
        }
    }

    if (m_args.overlays.theme) {
        // Use the proper flow through DisplaySettingsOverlay which handles:
        // - callback registration
        // - dropdown population
        // - theme preview creation
        auto& display_settings = helix::settings::get_display_settings_overlay();
        display_settings.show_theme_preview(m_screen);
        spdlog::info("[Application] Opened theme preview overlay via CLI");
    }

    if (m_args.overlays.theme_edit) {
        // Push theme preview first, then theme editor on top
        auto& display_settings = helix::settings::get_display_settings_overlay();
        display_settings.show_theme_preview(m_screen);

        // Now push theme editor overlay on top
        auto& theme_editor = get_theme_editor_overlay();
        theme_editor.register_callbacks();
        theme_editor.init_subjects();
        lv_obj_t* editor_panel = theme_editor.create(m_screen);
        if (editor_panel) {
            // Load current theme for editing
            std::string current_theme = DisplaySettingsManager::instance().get_theme_name();
            theme_editor.set_editing_dark_mode(DisplaySettingsManager::instance().get_dark_mode());
            theme_editor.load_theme(current_theme);
            NavigationManager::instance().push_overlay(editor_panel);
            spdlog::info("[Application] Opened theme editor overlay via CLI");
        }
    }

    // Settings overlays (for CLI screenshot automation)
    if (m_args.overlays.display_settings) {
        auto& overlay = helix::settings::get_display_settings_overlay();
        overlay.show(m_screen);
        spdlog::info("[Application] Opened display settings overlay via CLI");
    }

    if (m_args.overlays.sensor_settings) {
        auto& overlay = helix::settings::get_sensor_settings_overlay();
        overlay.show(m_screen);
        spdlog::info("[Application] Opened sensor settings overlay via CLI");
    }

    // Force touch calibration: --calibrate-touch flag, env var, OR config force_calibration option
    bool force_touch_cal = m_args.calibrate_touch;
    if (!force_touch_cal) {
        force_touch_cal = (std::getenv("HELIX_TOUCH_CALIBRATE") != nullptr);
    }
    if (!force_touch_cal && m_config) {
        force_touch_cal = m_config->get<bool>("/input/force_calibration", false);
    }

    if (force_touch_cal || m_args.overlays.touch_calibration) {
        auto& overlay = helix::ui::get_touch_calibration_overlay();
        overlay.init_subjects();
        overlay.register_callbacks();
        overlay.create(m_screen);

        // Completion callback: clear config flag on success if it was set
        bool clear_config = m_config && m_config->get<bool>("/input/force_calibration", false);
        overlay.show([this, clear_config](bool success) {
            if (success && clear_config && m_config) {
                m_config->set<bool>("/input/force_calibration", false);
                m_config->save();
                spdlog::info("[Application] Cleared force_calibration config flag after success");
            }
        });
        spdlog::info("[Application] Opened touch calibration overlay (force={})", force_touch_cal);
    }

    if (m_args.overlays.hardware_health) {
        auto& overlay = helix::settings::get_hardware_health_overlay();
        overlay.show(m_screen);
        spdlog::info("[Application] Opened hardware health overlay via CLI");
    }

    if (m_args.overlays.network_settings) {
        auto& overlay = get_network_settings_overlay();
        overlay.init_subjects();
        lv_obj_t* panel_obj = overlay.create(m_screen);
        if (panel_obj) {
            NavigationManager::instance().push_overlay(panel_obj);
            spdlog::info("[Application] Opened network settings overlay via CLI");
        }
    }

    if (m_args.overlays.macros) {
        auto& overlay = get_global_macros_panel();
        overlay.register_callbacks();
        overlay.init_subjects();
        lv_obj_t* panel_obj = overlay.create(m_screen);
        if (panel_obj) {
            NavigationManager::instance().push_overlay(panel_obj);
            spdlog::info("[Application] Opened macros overlay via CLI");
        }
    }

    if (m_args.overlays.print_tune) {
        auto& overlay = get_print_tune_overlay();
        overlay.init_subjects();
        lv_obj_t* panel_obj = overlay.create(m_screen);
        if (panel_obj) {
            NavigationManager::instance().push_overlay(panel_obj);
            spdlog::info("[Application] Opened print tune overlay via CLI");
        }
    }

    // Handle --release-notes flag: fetch latest release notes and show in modal
    if (m_args.overlays.release_notes) {
        auto& checker = UpdateChecker::instance();
        spdlog::info("[Application] Fetching latest release notes via CLI...");
        // check_for_updates callback runs on the LVGL thread (dispatched by report_result)
        checker.check_for_updates([](UpdateChecker::Status status,
                                     std::optional<UpdateChecker::ReleaseInfo> info) {
            auto& checker = UpdateChecker::instance();
            // Show release notes regardless of version comparison (even if "up to date")
            if (!info) {
                spdlog::warn("[Application] --release-notes: no release info available (status={})",
                             static_cast<int>(status));
                return;
            }

            // Populate subjects with real release data
            // (report_result already set version_text for UpdateAvailable,
            //  but we override for UpToDate/other statuses too)
            char version_text[128];
            snprintf(version_text, sizeof(version_text), "v%s (latest release)",
                     info->version.c_str());
            lv_subject_copy_string(checker.version_text_subject(), version_text);
            lv_subject_copy_string(checker.release_notes_subject(), info->release_notes.c_str());
            lv_subject_set_int(checker.changelog_visible_subject(), 1);
            checker.show_update_notification();
            spdlog::info("[Application] Showing release notes for v{}", info->version);
        });
    }

    // Handle --select-file flag
    RuntimeConfig* runtime_config = get_runtime_config();
    if (runtime_config->select_file != nullptr) {
        NavigationManager::instance().set_active(PanelId::PrintSelect);
        auto* print_panel = get_print_select_panel(get_printer_state(), m_moonraker->api());
        if (print_panel) {
            print_panel->set_pending_file_selection(runtime_config->select_file);
        }
    }
}

void Application::setup_discovery_callbacks() {
    MoonrakerClient* client = m_moonraker->client();
    MoonrakerAPI* api = m_moonraker->api();

    client->set_on_hardware_discovered([api, client](const helix::PrinterDiscovery& hardware) {
        struct HardwareDiscoveredCtx {
            helix::PrinterDiscovery hardware;
            MoonrakerAPI* api;
            MoonrakerClient* client;
        };
        auto ctx =
            std::make_unique<HardwareDiscoveredCtx>(HardwareDiscoveredCtx{hardware, api, client});
        helix::ui::queue_update<HardwareDiscoveredCtx>(
            std::move(ctx), [](HardwareDiscoveredCtx* c) {
                // Update API's hardware data (replaces MoonrakerAPI constructor callback)
                c->api->hardware() = c->hardware;
                helix::init_subsystems_from_hardware(c->hardware, c->api, c->client);
            });
    });

    // Capture Application pointer for callback - used to check shutdown state and access plugin
    // manager
    Application* app = this;

    client->set_on_discovery_complete([api, client, app](const helix::PrinterDiscovery& hardware) {
        struct DiscoveryCompleteCtx {
            helix::PrinterDiscovery hardware;
            MoonrakerAPI* api;
            MoonrakerClient* client;
            Application* app;
        };
        auto ctx = std::make_unique<DiscoveryCompleteCtx>(
            DiscoveryCompleteCtx{hardware, api, client, app});
        helix::ui::queue_update<DiscoveryCompleteCtx>(std::move(ctx), [](DiscoveryCompleteCtx* c) {
            // Safety check: if Application is shutting down, skip all processing
            // This prevents use-after-free if shutdown races with callback delivery
            if (c->app->m_shutdown_complete) {
                return;
            }

            // Update API's hardware data (replaces MoonrakerAPI constructor callback)
            c->api->hardware() = c->hardware;

            // Mark discovery complete so splash can exit
            c->app->m_splash_manager.on_discovery_complete();
            spdlog::info("[Application] Moonraker discovery complete, splash can exit");

            get_printer_state().set_hardware(c->hardware);
            get_printer_state().init_fans(
                c->hardware.fans(), helix::FanRoleConfig::from_config(Config::get_instance()));
            get_printer_state().set_klipper_version(c->hardware.software_version());
            get_printer_state().set_moonraker_version(c->hardware.moonraker_version());
            if (!c->hardware.os_version().empty()) {
                get_printer_state().set_os_version(c->hardware.os_version());
            }

            // Populate LED chips now that hardware is discovered
            get_global_settings_panel().populate_led_chips();

            // Fetch print hours now that connection is live, and refresh on job changes
            helix::settings::get_about_settings_overlay().fetch_print_hours();
            c->client->register_method_callback(
                "notify_history_changed", "AboutOverlay_print_hours",
                [](const nlohmann::json& /*data*/) {
                    helix::ui::queue_update([]() {
                        helix::settings::get_about_settings_overlay().fetch_print_hours();
                    });
                });

            // Register for timelapse events when timelapse is detected
            c->client->register_method_callback(
                "notify_timelapse_event", "timelapse_state", [](const nlohmann::json& data) {
                    helix::TimelapseState::instance().handle_timelapse_event(data);
                });

            // Hardware validation: check config expectations vs discovered hardware
            HardwareValidator validator;
            auto validation_result = validator.validate(Config::get_instance(), c->hardware);
            get_printer_state().set_hardware_validation_result(validation_result);

            if (validation_result.has_issues() && !Config::get_instance()->is_wizard_required() &&
                !is_wizard_active()) {
                validator.notify_user(validation_result);
            }

            // Save session snapshot for next comparison (even if no issues)
            validator.save_session_snapshot(Config::get_instance(), c->hardware);

            // Auto-detect printer type if not already set (e.g., fresh install with preset)
            PrinterDetector::auto_detect_and_save(c->hardware, Config::get_instance());

            // Record telemetry session event now that hardware data is available
            TelemetryManager::instance().record_session();
            TelemetryManager::instance().record_hardware_profile();
            TelemetryManager::instance().record_settings_snapshot();
            TelemetryManager::instance().record_memory_snapshot("session_start");

            // Fetch safety limits and build volume from Klipper config (stepper ranges,
            // min_extrude_temp, max_temp, etc.) — runs for ALL discovery completions
            // (normal startup AND post-wizard) so we don't duplicate this in callers
            if (c->api) {
                MoonrakerAPI* api_ptr = c->api;
                api_ptr->update_safety_limits_from_printer(
                    [api_ptr]() {
                        const auto& limits = api_ptr->get_safety_limits();
                        int min_extrude = static_cast<int>(limits.min_extrude_temp_celsius);
                        int max_temp = static_cast<int>(limits.max_temperature_celsius);
                        int min_temp = static_cast<int>(limits.min_temperature_celsius);

                        helix::ui::queue_update([min_temp, max_temp, min_extrude]() {
                            get_global_filament_panel().set_limits(min_temp, max_temp, min_extrude);
                            spdlog::debug("[Application] Safety limits propagated to panels");
                        });
                    },
                    [](const MoonrakerError& err) {
                        spdlog::warn("[Application] Failed to fetch safety limits: {}",
                                     err.message);
                    });
            }

            // Detect helix_print plugin during discovery (not UI-initiated)
            // This ensures plugin status is known early for UI gating
            c->api->job().check_helix_plugin(
                [](bool available) { get_printer_state().set_helix_plugin_installed(available); },
                [](const MoonrakerError&) {
                    // Silently treat errors as "plugin not installed"
                    get_printer_state().set_helix_plugin_installed(false);
                });

            // Fetch job queue now that WebSocket is actually connected
            if (c->app->m_job_queue_state) {
                c->app->m_job_queue_state->fetch();
            }

            // Notify plugins that Moonraker is connected
            if (c->app->m_plugin_manager) {
                c->app->m_plugin_manager->on_moonraker_connected();
            }

            // Apply LED startup preference (turn on LED if user preference is enabled)
            helix::led::LedController::instance().apply_startup_preference();

            // Start automatic update checks (15s initial delay, then every 24h)
            UpdateChecker::instance().start_auto_check();

            // Auto-navigate to Z-Offset Calibration if manual probe is already active
            // (e.g., PROBE_CALIBRATE started from Mainsail or console before HelixScreen launched)
            // Deferred one tick: status updates from the subscription response are queued
            // via ui_queue_update and may not have landed yet at this point.
            MoonrakerAPI* api_ptr_zoffset = c->api;
            lv_obj_t* screen = c->app->m_screen;
            helix::ui::queue_update([api_ptr_zoffset, screen]() {
                auto& ps = get_printer_state();
                int probe_active = lv_subject_get_int(ps.get_manual_probe_active_subject());
                spdlog::info("[Application] Checking manual_probe at startup: is_active={}",
                             probe_active);
                if (probe_active == 1) {
                    spdlog::info("[Application] Manual probe active at startup, auto-opening "
                                 "Z-Offset Calibration");
                    auto& overlay = get_global_zoffset_cal_panel();
                    overlay.set_api(api_ptr_zoffset);
                    if (overlay.create(screen)) {
                        overlay.show();
                    }
                }
            });
        });
    });
}

bool Application::connect_moonraker() {
    // Determine if we should connect
    std::string saved_host = m_config->get<std::string>(m_config->df() + "moonraker_host", "");
    bool has_cli_url = !m_args.moonraker_url.empty();
    // In test mode, still respect wizard state - don't connect until wizard completes
    bool should_connect =
        has_cli_url || (get_runtime_config()->test_mode && !m_wizard_active) ||
        (!m_args.force_wizard && !m_config->is_wizard_required() && !saved_host.empty());

    if (!should_connect) {
        return true; // Not connecting is not an error
    }

    std::string moonraker_url;
    std::string http_base_url;

    if (has_cli_url) {
        moonraker_url = m_args.moonraker_url;
        std::string host_port = moonraker_url.substr(5);
        auto ws_pos = host_port.find("/websocket");
        if (ws_pos != std::string::npos) {
            host_port = host_port.substr(0, ws_pos);
        }
        http_base_url = "http://" + host_port;
    } else {
        moonraker_url =
            "ws://" + m_config->get<std::string>(m_config->df() + "moonraker_host") + ":" +
            std::to_string(m_config->get<int>(m_config->df() + "moonraker_port")) + "/websocket";
        http_base_url = "http://" + m_config->get<std::string>(m_config->df() + "moonraker_host") +
                        ":" + std::to_string(m_config->get<int>(m_config->df() + "moonraker_port"));
    }

    // Discovery callbacks are already registered (setup_discovery_callbacks in init_moonraker)

    // Set HTTP base URL for API
    MoonrakerAPI* api = m_moonraker->api();
    api->set_http_base_url(http_base_url);

    // Connect
    spdlog::debug("[Application] Connecting to {}", moonraker_url);
    int result = m_moonraker->connect(moonraker_url, http_base_url);

    if (result != 0) {
        spdlog::error("[Application] Failed to initiate connection (code {})", result);
        return false;
    }

    // Start auto-discovery (client handles this internally after connect)

    // Initialize print start collector (monitors PRINT_START macro progress)
    m_moonraker->init_print_start_collector();

    // Initialize action prompt system (Klipper action:prompt protocol)
    init_action_prompt();

    // Start telemetry auto-send timer (periodic try_send)
    TelemetryManager::instance().start_auto_send();

    return true;
}

lv_obj_t* Application::create_overlay_panel(lv_obj_t* screen, const char* component_name,
                                            const char* display_name) {
    spdlog::debug("[Application] Opening {} overlay", display_name);
    lv_obj_t* panel = static_cast<lv_obj_t*>(lv_xml_create(screen, component_name, nullptr));
    if (!panel) {
        spdlog::error("[Application] Failed to create {} overlay from '{}'", display_name,
                      component_name);
    }
    return panel;
}

void Application::init_action_prompt() {
    MoonrakerClient* client = m_moonraker->client();
    MoonrakerAPI* api = m_moonraker->api();

    if (!client) {
        spdlog::warn("[Application] Cannot init action prompt - no client");
        return;
    }

    // Create ActionPromptManager and register global instance for cross-TU access
    m_action_prompt_manager = std::make_unique<helix::ActionPromptManager>();
    helix::ActionPromptManager::set_instance(m_action_prompt_manager.get());

    // Create ActionPromptModal
    m_action_prompt_modal = std::make_unique<helix::ui::ActionPromptModal>();

    // Set up gcode callback to send button commands via API
    if (api) {
        m_action_prompt_modal->set_gcode_callback([api](const std::string& gcode) {
            spdlog::info("[ActionPrompt] Sending gcode: {}", gcode);
            api->execute_gcode(
                gcode, []() { spdlog::debug("[ActionPrompt] Gcode executed successfully"); },
                [gcode](const MoonrakerError& err) {
                    spdlog::error("[ActionPrompt] Gcode execution failed: {}", err.message);
                });
        });
    }

    // Wire on_show callback to display modal (uses ui_async_call for thread safety)
    m_action_prompt_manager->set_on_show([this](const helix::PromptData& data) {
        spdlog::info("[ActionPrompt] Showing prompt: {}", data.title);
        // WebSocket callbacks run on background thread - must use ui_queue_update
        helix::ui::queue_update([this, data]() {
            if (m_action_prompt_modal && m_screen) {
                m_action_prompt_modal->show_prompt(m_screen, data);
            }
        });
    });

    // Wire on_close callback to hide modal
    m_action_prompt_manager->set_on_close([this]() {
        spdlog::info("[ActionPrompt] Closing prompt");
        helix::ui::queue_update([this]() {
            if (m_action_prompt_modal) {
                m_action_prompt_modal->hide();
            }
        });
    });

    // Wire on_notify callback for standalone notifications (action:notify)
    m_action_prompt_manager->set_on_notify([](const std::string& message) {
        spdlog::info("[ActionPrompt] Notification: {}", message);
        helix::ui::queue_update([message]() {
            ToastManager::instance().show(ToastSeverity::INFO, message.c_str(), 5000);
        });
    });

    // Allow mock AMS backends to inject action_prompt lines (e.g., calibration wizard)
    auto* prompt_mgr = m_action_prompt_manager.get();
    AmsState::instance().set_gcode_response_callback(
        [prompt_mgr](const std::string& line) { prompt_mgr->process_line(line); });

    // Register for notify_gcode_response messages from Moonraker
    // All lines from G-code console output come through this notification
    client->register_method_callback(
        "notify_gcode_response", "action_prompt_manager", [this](const nlohmann::json& msg) {
            // notify_gcode_response has params: [["line1", "line2", ...]]
            if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
                return;
            }

            const auto& params = msg["params"];
            // params can be an array of strings, or an array containing an array of strings
            // Handle both formats
            if (params[0].is_array()) {
                for (const auto& line : params[0]) {
                    if (line.is_string()) {
                        m_action_prompt_manager->process_line(line.get<std::string>());
                    }
                }
            } else if (params[0].is_string()) {
                for (const auto& line : params) {
                    if (line.is_string()) {
                        m_action_prompt_manager->process_line(line.get<std::string>());
                    }
                }
            }
        });

    // Register global handler to surface Klipper gcode errors as toasts.
    // Klipper errors come through notify_gcode_response with "!!" or "Error:" prefix.
    // Multiple handlers per method are supported (unique handler names).
    client->register_method_callback(
        "notify_gcode_response", "gcode_error_notifier", [](const nlohmann::json& msg) {
            if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
                return;
            }

            auto process_line = [](const std::string& line) {
                if (line.empty()) {
                    return;
                }

                // Klipper emergency errors: "!! MCU shutdown", "!! Timer too close", etc.
                if (line.size() >= 2 && line[0] == '!' && line[1] == '!') {
                    // Strip "!! " prefix for cleaner display
                    std::string clean =
                        (line.size() > 3 && line[2] == ' ') ? line.substr(3) : line.substr(2);
                    spdlog::error("[GcodeError] Emergency: {}", clean);
                    ui_notification_error("Klipper Error", clean.c_str(), false);
                    return;
                }

                // Command errors: "Error: Must home before probe", etc.
                if (line.size() >= 5) {
                    std::string prefix = line.substr(0, 5);
                    for (auto& c : prefix)
                        c = static_cast<char>(std::tolower(c));
                    if (prefix == "error") {
                        // Strip "Error: " prefix if present
                        std::string clean = line;
                        if (line.size() > 7 && line[5] == ':' && line[6] == ' ') {
                            clean = line.substr(7);
                        } else if (line.size() > 6 && line[5] == ':') {
                            clean = line.substr(6);
                        }
                        spdlog::error("[GcodeError] {}", clean);
                        ui_notification_error(nullptr, clean.c_str(), false);
                        return;
                    }
                }
            };

            const auto& params = msg["params"];
            if (params[0].is_array()) {
                for (const auto& line : params[0]) {
                    if (line.is_string()) {
                        process_line(line.get<std::string>());
                    }
                }
            } else if (params[0].is_string()) {
                for (const auto& line : params) {
                    if (line.is_string()) {
                        process_line(line.get<std::string>());
                    }
                }
            }
        });

    // Register layer tracking fallback via gcode responses.
    // Some slicers don't emit SET_PRINT_STATS_INFO, so Moonraker's print_stats.info
    // never updates current_layer. This parses gcode responses as a fallback.
    client->register_method_callback(
        "notify_gcode_response", "layer_tracker", [this](const nlohmann::json& msg) {
            if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
                return;
            }

            // Only track layers while printing or paused
            auto job_state = get_printer_state().get_print_job_state();
            if (job_state != PrintJobState::PRINTING && job_state != PrintJobState::PAUSED) {
                return;
            }

            auto process_line = [this](const std::string& line) {
                if (line.empty()) {
                    return;
                }

                int layer = -1;
                int total = -1;

                // Pattern 1: SET_PRINT_STATS_INFO CURRENT_LAYER=N [TOTAL_LAYER=N]
                // Klipper echoes this command in gcode responses
                if (line.find("SET_PRINT_STATS_INFO") != std::string::npos) {
                    auto pos = line.find("CURRENT_LAYER=");
                    if (pos != std::string::npos) {
                        layer = std::atoi(line.c_str() + pos + 14);
                    }
                    pos = line.find("TOTAL_LAYER=");
                    if (pos != std::string::npos) {
                        total = std::atoi(line.c_str() + pos + 12);
                    }
                }

                // Pattern 2: ;LAYER:N (OrcaSlicer, PrusaSlicer, Cura comment format)
                if (layer < 0 && line.size() >= 8 && line[0] == ';' && line[1] == 'L' &&
                    line[2] == 'A' && line[3] == 'Y' && line[4] == 'E' && line[5] == 'R' &&
                    line[6] == ':') {
                    layer = std::atoi(line.c_str() + 7);
                }

                if (layer >= 0) {
                    spdlog::debug("[LayerTracker] Layer {} from gcode response: {}", layer, line);
                    get_printer_state().set_print_layer_current(layer);
                }
                if (total >= 0) {
                    spdlog::debug("[LayerTracker] Total layers {} from gcode response", total);
                    get_printer_state().set_print_layer_total(total);
                }
            };

            const auto& params = msg["params"];
            if (params[0].is_array()) {
                for (const auto& line : params[0]) {
                    if (line.is_string()) {
                        process_line(line.get<std::string>());
                    }
                }
            } else if (params[0].is_string()) {
                for (const auto& line : params) {
                    if (line.is_string()) {
                        process_line(line.get<std::string>());
                    }
                }
            }
        });

    spdlog::debug("[Application] Action prompt system initialized");
}

void Application::restore_flush_callback() {
    if (m_original_flush_cb) {
        lv_display_t* disp = lv_display_get_default();
        if (disp) {
            lv_display_set_flush_cb(disp, m_original_flush_cb);
        }
        m_original_flush_cb = nullptr;
    }
}

void Application::check_wifi_availability() {
    if (!m_config || !m_config->is_wifi_expected()) {
        return; // WiFi not expected, no need to check
    }

    auto wifi = get_wifi_manager();
    if (wifi && !wifi->has_hardware()) {
        NOTIFY_ERROR_MODAL("WiFi Unavailable", "WiFi was configured but hardware is not available. "
                                               "Check system configuration.");
    }
}

int Application::main_loop() {
    spdlog::info("[Application] Entering main loop");
    m_running = true;

    // Initialize timing
    uint32_t start_time = DisplayManager::get_ticks();
    m_last_timeout_check = start_time;
    m_timeout_check_interval = static_cast<uint32_t>(
        m_config->get<int>(m_config->df() + "moonraker_timeout_check_interval_ms", 2000));

    // fbdev self-heal: periodic full-screen invalidation to overwrite any kernel
    // console text that bleeds through LVGL's partial render. KDSETMODE KD_GRAPHICS
    // is the primary defense; this is belt-and-suspenders for robustness.
    bool needs_fb_self_heal =
        m_display->backend() && m_display->backend()->type() == DisplayBackendType::FBDEV;
    uint32_t last_fb_selfheal_tick = start_time;
    static constexpr uint32_t FB_SELFHEAL_INTERVAL_MS = 10000; // 10 seconds

    // Failsafe: track invalidation suppression with a hard deadline.
    // If splash handoff doesn't complete within this time, force rendering back on
    // to avoid a permanently black screen.
    bool invalidation_suppressed =
        get_runtime_config()->splash_pid > 0 && !m_splash_manager.has_exited();
    uint32_t suppression_start_tick = DisplayManager::get_ticks();
    static constexpr uint32_t INVALIDATION_FAILSAFE_MS =
        8000; // Must exceed DISCOVERY_TIMEOUT_MS (5s)

    // Configure main loop handler
    helix::application::MainLoopHandler::Config loop_config;
    loop_config.screenshot_enabled = m_args.screenshot_enabled;
    loop_config.screenshot_delay_ms = static_cast<uint32_t>(m_args.screenshot_delay_sec) * 1000U;
    loop_config.timeout_sec = m_args.timeout_sec;
    loop_config.benchmark_mode = helix::config::EnvironmentConfig::get_benchmark_mode();
    loop_config.benchmark_report_interval_ms = 5000;
    m_loop_handler.init(loop_config, start_time);

    // Main event loop
    while (lv_display_get_next(nullptr) && !app_quit_requested()) {
        uint32_t current_tick = DisplayManager::get_ticks();
        m_loop_handler.on_frame(current_tick);

        handle_keyboard_shortcuts();

        // Break immediately if quit was requested (e.g., Cmd+Q) to avoid
        // running lv_timer_handler() with stale queued callbacks that may
        // reference destroyed objects (e.g., update_button_text_contrast
        // on a button whose user_data was freed by Modal destruction).
        if (app_quit_requested()) {
            break;
        }

        // Auto-screenshot
        if (m_loop_handler.should_take_screenshot()) {
            helix::save_screenshot();
            m_loop_handler.mark_screenshot_taken();
        }

        // Auto-quit timeout
        if (m_loop_handler.should_quit()) {
            spdlog::info("[Application] Timeout reached ({} seconds)", m_args.timeout_sec);
            break;
        }

        // Process timeouts
        check_timeouts();

        // Process Moonraker notifications
        process_notifications();

        // Check display sleep
        m_display->check_display_sleep();

        // Periodic full-screen invalidation on fbdev (self-heal kernel console bleed-through)
        if (needs_fb_self_heal &&
            (current_tick - last_fb_selfheal_tick) >= FB_SELFHEAL_INTERVAL_MS) {
            lv_obj_invalidate(lv_screen_active());
            last_fb_selfheal_tick = current_tick;
        }

        // Run LVGL tasks — returns ms until next timer needs to fire
        uint32_t time_till_next = lv_timer_handler();

        // Signal splash to exit when discovery completes (or timeout)
        m_splash_manager.check_and_signal();

        // Post-splash handoff: re-enable rendering and repaint
        // Display invalidation was suppressed to prevent framebuffer flicker
        // while both splash and main app were running simultaneously.
        if (invalidation_suppressed && m_splash_manager.needs_post_splash_refresh()) {
            invalidation_suppressed = false;
            lv_display_enable_invalidation(nullptr, true);
            restore_flush_callback();
            spdlog::info("[Application] Post-splash handoff: flush callback restored, painting UI");

            lv_obj_t* screen = lv_screen_active();
            if (screen) {
                lv_obj_update_layout(screen);
                invalidate_all_recursive(screen);
                lv_refr_now(nullptr);
            }
            m_splash_manager.mark_refresh_done();
        }

        // Failsafe: if invalidation is still suppressed after hard deadline, force it back on.
        // Prevents permanent black screen if splash handoff fails for any reason.
        if (invalidation_suppressed &&
            (current_tick - suppression_start_tick) >= INVALIDATION_FAILSAFE_MS) {
            invalidation_suppressed = false;
            lv_display_enable_invalidation(nullptr, true);
            restore_flush_callback();
            spdlog::warn("[Application] Invalidation failsafe triggered after {}ms",
                         INVALIDATION_FAILSAFE_MS);
            lv_obj_invalidate(lv_screen_active());
        }

        // Benchmark mode - force redraws and report FPS
        if (loop_config.benchmark_mode) {
            lv_obj_invalidate(lv_screen_active());
            if (m_loop_handler.benchmark_should_report()) {
                auto report = m_loop_handler.benchmark_get_report();
                spdlog::info("[Application] Benchmark FPS: {:.1f}", report.fps);
            }
        }

        // Sleep adaptively: use LVGL's hint for when next work is due,
        // capped to keep UI responsive. In benchmark mode, minimize delay.
        // When display is sleeping, extend sleep to 200ms — no rendering
        // needed, just need to stay responsive to wake events.
        if (!loop_config.benchmark_mode) {
            uint32_t max_sleep = m_display->is_display_sleeping() ? 200 : 33;
            uint32_t sleep_ms = std::min(time_till_next, max_sleep);
            if (sleep_ms < 5) sleep_ms = 5;
            DisplayManager::delay(sleep_ms);
        } else {
            DisplayManager::delay(1);
        }
    }

    m_running = false;

    if (loop_config.benchmark_mode) {
        auto final_report = m_loop_handler.benchmark_get_final_report();
        spdlog::info("[Application] Benchmark total runtime: {:.1f}s",
                     final_report.total_runtime_sec);
    }

    return 0;
}

void Application::handle_keyboard_shortcuts() {
#ifdef HELIX_DISPLAY_SDL
    // Static shortcut registry - initialized once
    static helix::input::KeyboardShortcuts shortcuts;
    static bool shortcuts_initialized = false;

    if (!shortcuts_initialized) {
        // Cmd+Q / Win+Q to quit
        shortcuts.register_combo(KMOD_GUI, SDL_SCANCODE_Q, []() {
            spdlog::info("[Application] Cmd+Q/Win+Q pressed - exiting");
            app_request_quit();
        });

        // S key - take screenshot
        shortcuts.register_key(SDL_SCANCODE_S, []() {
            spdlog::info("[Application] S key - taking screenshot");
            auto path = helix::save_screenshot();
            if (!path.empty()) {
                auto basename = path.substr(path.rfind('/') + 1);
                auto msg = "Screenshot " + basename + " taken!";
                ToastManager::instance().show(ToastSeverity::SUCCESS, msg.c_str(), 3000);
            }
        });

        // M key - toggle memory stats
        shortcuts.register_key(SDL_SCANCODE_M, []() { MemoryStatsOverlay::instance().toggle(); });

        // D key - toggle dark/light mode
        shortcuts.register_key(SDL_SCANCODE_D, []() {
            spdlog::info("[Application] D key - toggling dark/light mode");
            theme_manager_toggle_dark_mode();
        });

        // F key - toggle filament runout simulation (needs m_moonraker)
        shortcuts.register_key_if(
            SDL_SCANCODE_F,
            [this]() {
                spdlog::info("[Application] F key - toggling filament runout simulation");
                m_moonraker->client()->toggle_filament_runout_simulation();
            },
            [this]() { return m_moonraker && m_moonraker->client(); });

        // P key - cycle through configured printers (test mode only)
        shortcuts.register_key_if(
            SDL_SCANCODE_P,
            [this]() {
                auto ids = m_config->get_printer_ids();
                if (ids.size() > 1) {
                    auto current = m_config->get_active_printer_id();
                    auto it = std::find(ids.begin(), ids.end(), current);
                    auto next = (it != ids.end() && std::next(it) != ids.end()) ? *std::next(it)
                                                                                : ids.front();
                    spdlog::info("[Application] P key - switching to printer '{}'", next);
                    switch_printer(next);
                } else {
                    // Create a second test printer so we can test switching
                    spdlog::info("[Application] P key - creating test printer for multi-printer testing");
                    nlohmann::json test_data;
                    test_data["printer_name"] = "Voron 2.4";
                    test_data["type"] = "Voron 2.4 350mm";
                    test_data["moonraker_host"] = "127.0.0.1";
                    test_data["moonraker_port"] = 7125;
                    m_config->add_printer("voron-24", test_data);
                    m_config->save();
                    // Update subjects so the badge appears
                    get_printer_state().set_multi_printer_enabled(true);
                }
            },
            [this]() { return get_runtime_config()->is_test_mode() && m_config; });

        // A key - test action prompt (test mode only)
        shortcuts.register_key_if(
            SDL_SCANCODE_A,
            [this]() {
                spdlog::info("[Application] A key - triggering test action prompt");
                m_action_prompt_manager->trigger_test_prompt();
            },
            [this]() { return get_runtime_config()->is_test_mode() && m_action_prompt_manager; });

        // N key - test action notification (test mode only)
        shortcuts.register_key_if(
            SDL_SCANCODE_N,
            [this]() {
                spdlog::info("[Application] N key - triggering test action notification");
                m_action_prompt_manager->trigger_test_notify();
            },
            [this]() { return get_runtime_config()->is_test_mode() && m_action_prompt_manager; });

#ifdef HELIX_ENABLE_SCREENSAVER
        // Z key - cycle through screensavers (Off → Toasters → Starfield → Pipes → Off)
        shortcuts.register_key(SDL_SCANCODE_Z, []() {
            auto& mgr = ScreensaverManager::instance();
            if (mgr.is_active()) {
                mgr.stop();
                spdlog::info("[Application] Z key - screensaver stopped");
            } else {
                auto type = ScreensaverManager::configured_type();
                if (type == ScreensaverType::OFF) {
                    type = ScreensaverType::FLYING_TOASTERS;
                }
                mgr.start(type);
                spdlog::info("[Application] Z key - screensaver started (type {})",
                             static_cast<int>(type));
            }
        });
#endif

        shortcuts_initialized = true;
    }

    // Suppress plain-key shortcuts when a textarea has focus (e.g., typing a password)
    lv_obj_t* focused = lv_group_get_focused(lv_group_get_default());
    bool text_input_active = focused != nullptr && lv_obj_check_type(focused, &lv_textarea_class);

    // Process shortcuts with SDL key state
    const Uint8* keyboard_state = SDL_GetKeyboardState(nullptr);
    shortcuts.process([keyboard_state](int scancode) { return keyboard_state[scancode] != 0; },
                      SDL_GetModState(), text_input_active);
#endif
}

void Application::process_notifications() {
    if (m_moonraker) {
        m_moonraker->process_notifications();
    }
}

void Application::check_timeouts() {
    uint32_t current_time = DisplayManager::get_ticks();
    if (current_time - m_last_timeout_check >= m_timeout_check_interval) {
        if (m_moonraker) {
            m_moonraker->process_timeouts();
        }
        m_last_timeout_check = current_time;
    }
}

// ============================================================================
// SOFT RESTART (printer switching)
// ============================================================================

void Application::switch_printer(const std::string& printer_id) {
    if (m_soft_restart_in_progress) {
        spdlog::warn("[Application] Ignoring switch_printer during active soft restart");
        return;
    }
    m_soft_restart_in_progress = true;

    spdlog::info("[Application] Switching to printer '{}'...", printer_id);

    // Validate printer exists in config
    if (!m_config->set_active_printer(printer_id)) {
        spdlog::error("[Application] Failed to switch — unknown printer '{}'", printer_id);
        m_soft_restart_in_progress = false;
        return;
    }
    m_config->save();

    tear_down_printer_state();
    init_printer_state();

    // Navigate to home
    NavigationManager::instance().set_active(PanelId::Home);

    // Show toast with the new printer name
    std::string printer_name =
        m_config->get<std::string>(m_config->df() + "printer_name", printer_id);
    std::string toast_msg = "Connected to " + printer_name;
    ToastManager::instance().show(ToastSeverity::INFO, toast_msg.c_str());

    m_soft_restart_in_progress = false;
    spdlog::info("[Application] Switched to printer '{}'", printer_id);
}

void Application::add_printer_via_wizard() {
    if (m_soft_restart_in_progress) {
        spdlog::warn("[Application] Ignoring add_printer_via_wizard during active soft restart");
        return;
    }
    m_soft_restart_in_progress = true;

    // Generate a unique ID for the new printer entry (loop to avoid collisions after deletes)
    auto existing_ids = m_config->get_printer_ids();
    int counter = static_cast<int>(existing_ids.size()) + 1;
    std::string new_id;
    do {
        new_id = "printer-" + std::to_string(counter++);
    } while (std::find(existing_ids.begin(), existing_ids.end(), new_id) != existing_ids.end());
    std::string previous_id = m_config->get_active_printer_id();

    // Create empty printer entry with wizard_completed=false so is_wizard_required()
    // returns true (without this, root-level wizard_completed fallback blocks the wizard)
    nlohmann::json printer_data = {{"wizard_completed", false}};
    m_config->add_printer(new_id, printer_data);
    m_config->set_active_printer(new_id);
    m_config->save();

    // Store previous ID so wizard cancellation can recover
    m_wizard_previous_printer_id = previous_id;

    spdlog::info("[Application] Adding new printer '{}' via wizard (previous: '{}')", new_id,
                 previous_id);

    // Soft restart and launch wizard for the new printer.
    // init_printer_state() calls run_wizard() internally when is_wizard_required() returns true
    // (which it does for the new empty printer entry with wizard_completed=false).
    // Do NOT call run_wizard() again here — that creates a duplicate wizard container.
    tear_down_printer_state();

    // Register cancel callback AFTER tear_down (which clears it) but BEFORE init (which runs wizard)
    set_wizard_cancel_callback([this]() {
        cancel_add_printer_wizard();
    });

    init_printer_state();

    m_soft_restart_in_progress = false;
}

void Application::cancel_add_printer_wizard() {
    if (m_soft_restart_in_progress) {
        spdlog::warn("[Application] Ignoring cancel_add_printer_wizard during active soft restart");
        return;
    }

    if (m_wizard_previous_printer_id.empty()) {
        spdlog::debug("[Application] No add-printer recovery state — ignoring cancel");
        return;
    }

    std::string failed_id = m_config->get_active_printer_id();
    std::string restore_id = m_wizard_previous_printer_id;
    spdlog::info("[Application] Cancelling add-printer wizard — removing '{}', restoring '{}'",
                 failed_id, restore_id);

    m_config->remove_printer(failed_id);
    m_config->set_active_printer(restore_id);
    m_config->save();
    m_wizard_previous_printer_id.clear();

    // Defer wizard teardown + soft restart — we're called from a wizard button click handler,
    // so the wizard_container must survive until the event callback returns.
    helix::ui::queue_update([this]() {
        m_soft_restart_in_progress = true;

        set_wizard_active(false);
        ui_wizard_deinit_subjects();

        tear_down_printer_state();
        init_printer_state();
        NavigationManager::instance().set_active(PanelId::Home);

        m_soft_restart_in_progress = false;
    });
}

void Application::tear_down_printer_state() {
    spdlog::info("[Application] Tearing down printer state...");

    // Teardown mirrors shutdown() ordering. Subjects stay alive until step 12
    // so ObserverGuards can properly call lv_observer_remove() during destruction.

    // 0. Clear wizard cancel callback (prevent stale captures across soft restart)
    set_wizard_cancel_callback(nullptr);

    // 1. Clear app_globals BEFORE destroying managers to prevent
    //    destructors from accessing destroyed objects
    set_moonraker_manager(nullptr);
    set_moonraker_api(nullptr);
    set_moonraker_client(nullptr);
    set_print_history_manager(nullptr);
    set_temperature_history_manager(nullptr);

    // 2. Deactivate overlays and clear navigation registries
    NavigationManager::instance().shutdown();

    // 3. Stop UpdateChecker auto-check timer (fires API calls on background thread)
    if (!helix::is_android_platform()) {
        UpdateChecker::instance().stop_auto_check();
    }

    // 4. Unload plugins (may hold refs to managers)
    if (m_plugin_manager) {
        m_plugin_manager->unload_all();
        m_plugin_manager.reset();
    }

    // 5. Freeze update queue to prevent new callbacks during teardown
    auto queue_freeze = helix::ui::UpdateQueue::instance().scoped_freeze();

    // 5b. Disconnect WebSocket thread to stop background callbacks
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->disconnect();
    }

    // 6. Discard pending async callbacks queued by background threads.
    //    Must happen AFTER disconnect (no more producers) and BEFORE destroying
    //    objects referenced by queued callbacks.
    helix::ui::update_queue_shutdown();

    // 6b. Clear global pointer to JobQueueState (prevents access via global pointer).
    //     Actual destruction deferred to after StaticSubjectRegistry::deinit_all()
    //     so the registered lambda doesn't run on freed memory.
    set_job_queue_state(nullptr);

    // 7. Release history managers
    m_history_manager.reset();
    m_temp_history_manager.reset();

    // 8. Unregister timelapse event callback
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->unregister_method_callback("notify_timelapse_event",
                                                          "timelapse_state");
    }

    // 9. Unregister action prompt callback
    if (m_moonraker && m_moonraker->client() && m_action_prompt_manager) {
        m_moonraker->client()->unregister_method_callback("notify_gcode_response",
                                                          "action_prompt_manager");
    }
    AmsState::instance().set_gcode_response_callback(nullptr);
    m_action_prompt_modal.reset();
    helix::ActionPromptManager::set_instance(nullptr);
    m_action_prompt_manager.reset();

    // 10. Clear AMS backends (hold subscription guards with raw client pointers)
    AmsState::instance().clear_backends();

    // 11. Deinit LedController (holds API/client pointers about to be freed)
    helix::led::LedController::instance().deinit();

    // 12. Release PanelFactory and SubjectInitializer
    m_panels.reset();
    m_subjects.reset();

    // 13. Kill all LVGL animations (hold widget pointers)
    lv_anim_delete_all();

    // 13b. Clear ModalStack tracking (widgets destroyed by lv_obj_del below)
    ModalStack::instance().clear();

    // 14. Destroy all static panel/overlay globals (releases ObserverGuards).
    //     Subjects are still alive here, so lv_observer_remove() works correctly.
    StaticPanelRegistry::instance().destroy_all();

    // 15. Release global observer guards that observe subjects about to be freed
    ui_notification_deinit();
    helix::deinit_active_print_media_manager();

    // 16. Deinit core singleton subjects (LIFO order via StaticSubjectRegistry).
    //     lv_subject_deinit() removes+frees all remaining observers from each subject.
    StaticSubjectRegistry::instance().deinit_all();

    // 16b. Destroy JobQueueState (after deinit_all so its registered lambda runs safely)
    m_job_queue_state.reset();

    // 17. Invalidate all observer guards. From this point, any ObserverGuard::reset()
    //     in surviving singletons (not destroyed by StaticPanelRegistry) will release
    //     instead of calling lv_observer_remove() on freed observer pointers.
    //     This protects the reinit path where old guards get reassigned.
    ObserverGuard::invalidate_all();

    // 18. Release MoonrakerManager
    m_moonraker.reset();

    // 19. Reset KeyboardManager (widget pointers become dangling after tree delete)
    KeyboardManager::instance().reset();

    // 20. Delete LVGL widget tree (panels already released references)
    //     DO NOT call lv_deinit() — display stays alive
    if (m_app_layout) {
        lv_obj_del(m_app_layout);
        m_app_layout = nullptr;
    }
    m_overlay_panels = {};

    spdlog::info("[Application] Printer state torn down");
}

void Application::init_printer_state() {
    spdlog::info("[Application] Initializing printer state...");

    // Show error on screen so user isn't left with blank display after init failure.
    // Exceptional error path — imperative LVGL is acceptable here.
    auto show_init_error = [this]() {
        lv_obj_t* err_label = lv_label_create(m_screen);
        lv_label_set_text(err_label,
                          "Printer initialization failed.\nPlease restart the application.");
        lv_obj_center(err_label);
        lv_obj_set_style_text_color(err_label, lv_color_hex(0xFF4444), 0);
        lv_obj_set_style_text_font(err_label, lv_font_get_default(), 0);
    };

    // NOTE: ObserverGuard::invalidate_all() was called at the end of teardown.
    // Guards in surviving singletons hold freed observer pointers. When they get
    // reassigned (guard = observe_*()), the move-assignment calls reset() which
    // safely releases instead of calling lv_observer_remove() on freed memory.
    // We revalidate at the END of init after all old guards have been cleared.

    // 1. Reinitialize update queue BEFORE moonraker so background thread callbacks
    //    (hardware discovery, WebSocket messages) have a functioning queue.
    helix::ui::update_queue_init();

    // 2. Initialize core subjects (PrinterState, AmsState, etc.)
    if (!init_core_subjects()) {
        spdlog::error("[Application] Failed to reinitialize core subjects");
        show_init_error();
        ObserverGuard::revalidate_all();
        return;
    }

    // 2b. Set multi-printer subjects from config
    {
        auto printer_ids = m_config->get_printer_ids();
        auto active_id = m_config->get_active_printer_id();
        std::string printer_name =
            m_config->get<std::string>(m_config->df() + "printer_name", active_id);
        get_printer_state().set_active_printer_name(printer_name);
        get_printer_state().set_multi_printer_enabled(printer_ids.size() > 1);
    }

    // 3. Initialize Moonraker (creates client + API + history managers)
    if (!init_moonraker()) {
        spdlog::error("[Application] Failed to reinitialize Moonraker");
        show_init_error();
        ObserverGuard::revalidate_all();
        return;
    }

    // 4. Initialize panel subjects with API injection + post-init
    if (!init_panel_subjects()) {
        spdlog::error("[Application] Failed to reinitialize panel subjects");
        show_init_error();
        ObserverGuard::revalidate_all();
        return;
    }

    // 5. Recreate UI (app_layout from XML, wire navigation)
    if (!init_ui()) {
        spdlog::error("[Application] Failed to reinitialize UI");
        show_init_error();
        ObserverGuard::revalidate_all();
        return;
    }

    // 6. Run wizard if needed for new printer
    if (run_wizard()) {
        m_wizard_active = true;
        set_wizard_active(true);
    }

    // 7. Create CLI overlay panels (if any)
    if (!m_wizard_active) {
        create_overlays();
    }

    // 8. Reload plugins
    if (!init_plugins()) {
        spdlog::warn("[Application] Plugin reinitialization had errors (non-fatal)");
    }

    // 9. Connect to new printer's Moonraker
    if (!connect_moonraker()) {
        spdlog::warn("[Application] Running without printer connection after switch");
    }

    // 10. Revalidate observer guards — all old guards have been reassigned (released)
    //     during init, and all new observers are attached to live subjects.
    ObserverGuard::revalidate_all();

    // Force full screen refresh
    lv_obj_update_layout(m_screen);
    invalidate_all_recursive(m_screen);
    lv_refr_now(nullptr);

    spdlog::info("[Application] Printer state initialized");
}

void Application::shutdown() {
    // Guard against multiple calls (destructor + explicit shutdown)
    if (m_shutdown_complete) {
        return;
    }
    m_shutdown_complete = true;

    // Uninstall crash handler (clean shutdown is not a crash)
    crash_handler::uninstall();

    // Clean shutdown means no crash loop -- remove the marker file
    std::filesystem::remove(CRASH_MARKER_PATH);

    // Stop hot reloader thread before anything else
    if (m_hot_reloader) {
        m_hot_reloader->stop();
        m_hot_reloader.reset();
    }

    // Stop memory monitor first
    helix::MemoryMonitor::instance().stop();

    spdlog::info("[Application] Shutting down...");

    // Disconnect the WebSocket client FIRST to stop background threads
    // (mock simulation, WebSocket I/O). This prevents races where the
    // background thread delivers notifications that trigger new API requests
    // (history fetch, metascan, webcam detection) after we've started teardown.
    // The client object remains valid for later unregister_method_callback() calls.
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->disconnect();
    }

    // Clear app_globals references BEFORE destroying managers to prevent
    // destructors (e.g., PrintSelectPanel) from accessing destroyed objects
    set_moonraker_manager(nullptr);
    set_moonraker_api(nullptr);
    set_moonraker_client(nullptr);
    set_job_queue_state(nullptr);
    set_print_history_manager(nullptr);
    set_temperature_history_manager(nullptr);

    // Deactivate UI and clear navigation registries
    NavigationManager::instance().shutdown();

    // Stop auto-check timer before full shutdown
    UpdateChecker::instance().stop_auto_check();
    // Shutdown UpdateChecker (cancels pending checks)
    UpdateChecker::instance().shutdown();

    // Shutdown TelemetryManager (persists queue, joins send thread)
    TelemetryManager::instance().shutdown();

    // Shutdown CrashHistory
    helix::CrashHistory::instance().shutdown();

    // Shutdown SoundManager (stops sequencer, closes audio backends)
    SoundManager::instance().shutdown();

    // Unload plugins before destroying managers they depend on
    if (m_plugin_manager) {
        m_plugin_manager->unload_all();
        m_plugin_manager.reset();
    }

    // Reset managers in reverse order (MoonrakerManager handles print_start_collector cleanup)
    // Job queue and history managers MUST be reset before moonraker (use client for unregistration)
    m_job_queue_state.reset();
    m_history_manager.reset();
    m_temp_history_manager.reset();

    // Unregister timelapse event callback
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->unregister_method_callback("notify_timelapse_event",
                                                          "timelapse_state");
    }

    // Unregister action prompt callback before moonraker is destroyed
    if (m_moonraker && m_moonraker->client() && m_action_prompt_manager) {
        m_moonraker->client()->unregister_method_callback("notify_gcode_response",
                                                          "action_prompt_manager");
    }
    // Clear mock gcode injection callback before destroying ActionPromptManager
    // (AmsState singleton outlives Application — callback would dangle)
    AmsState::instance().set_gcode_response_callback(nullptr);
    m_action_prompt_modal.reset();
    helix::ActionPromptManager::set_instance(nullptr);
    m_action_prompt_manager.reset();

    // Stop AMS backend subscriptions BEFORE destroying MoonrakerClient.
    // Backends hold SubscriptionGuards with raw MoonrakerClient* pointers —
    // they must unsubscribe while the client's mutex is still alive.
    AmsState::instance().clear_backends();

    // Drain deferred UI callbacks BEFORE destroying panels.
    // observe_int_sync/observe_string defer via ui_queue_update(), so queued
    // callbacks may hold 'this' pointers to living panels. Processing them
    // after m_panels.reset() causes use-after-free (SIGSEGV).
    helix::ui::update_queue_shutdown();

    // Stop ALL LVGL animations before destroying panels.
    // Animations hold widget pointers; completion callbacks fired during
    // lv_anim_delete_all() would dereference freed objects if panels are gone.
    lv_anim_delete_all();

    m_panels.reset();
    m_subjects.reset();

    // Restore display backlight (guard for early exit paths like --help)
    if (m_display) {
        m_display->restore_display_on_shutdown();
    }

    // Destroy ALL static panel/overlay globals via self-registration pattern.
    // This deinits local subjects (via SubjectManager) and releases ObserverGuards.
    // Must happen while LVGL is still initialized so lv_observer_remove() can
    // properly remove unsubscribe_on_delete_cb from widget event lists.
    StaticPanelRegistry::instance().destroy_all();

    // Deinitialize core singleton subjects (PrinterState, AmsState, SettingsManager, etc.)
    // BEFORE lv_deinit(). lv_subject_deinit() calls lv_observer_remove() for each
    // observer, which removes unsubscribe_on_delete_cb from widget event lists.
    // After this, widgets have no observer callbacks, so lv_deinit() deletes them
    // cleanly without firing stale unsubscribe callbacks on corrupted linked lists.
    StaticSubjectRegistry::instance().deinit_all();

    // Deinitialize theme manager subjects (theme_changed_subject, swatch descriptions).
    // These are file-scope statics not tracked by StaticSubjectRegistry.
    theme_manager_deinit();

    // Destroy MoonrakerManager (releases its ObserverGuards and client).
    // Safe here: LVGL subjects are deinitialized but lv_deinit() hasn't run yet,
    // so lv_observer_remove() can still operate on the observer linked lists.
    m_moonraker.reset();

    // Shutdown display (calls lv_deinit). All observer callbacks were already
    // removed above, so widget deletion is clean — no observer linked list access.
    m_display.reset();

    spdlog::info("[Application] Shutdown complete");
}
