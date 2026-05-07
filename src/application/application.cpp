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
#include "ui_overlay_timelapse_videos.h"
#include "ui_update_queue.h"

#include "app_constants.h"
#include "asset_manager.h"
#include "cjk_font_manager.h"
#include "translation_loader.h"
#include "config.h"
#include "display/lv_display_private.h"
#include "display_manager.h"
#include "environment_config.h"
#include "hardware_validator.h"
#include "helix_version.h"
#include "http_executor.h"
#include "job_queue_state.h"
#include "keyboard_shortcuts.h"
#include "layout_manager.h"
#include "led/led_controller.h"
#include "ams_backend_cfs.h"
#include "moonraker_manager.h"
#include "printer_recovery_service.h"
#include "panel_factory.h"
#include "pending_startup_warnings.h"
#include "post_op_cooldown_manager.h"
#include "power_device_state.h"
#include "print_history_manager.h"
#include "screenshot.h"
#include "sensor_state.h"
#include "sound_manager.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "streaming_policy.h"
#include "subject_initializer.h"
#include "temperature_history_manager.h"
#include "thermal_rate_model.h"
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
#include "ui_error_reporting.h"
#include "ui_fan_control_overlay.h"
#include "ui_gcode_viewer.h"
#include "ui_gradient_canvas.h"
#include "ui_icon.h"
#include "ui_icon_loader.h"
#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_notification.h"
#include "ui_notification_history.h"
#include "ui_notification_manager.h"
#include "ui_observer_guard.h"
#include "ui_overlay_network_settings.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_belt_tension.h"
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
#include "ui_panel_spoolman.h"
#include "ui_panel_step_test.h"
#include "ui_panel_test.h"
#include "ui_print_tune_overlay.h"
#include "ui_printer_status_icon.h"
#include "ui_probe_overlay.h"
#include "ui_settings_about.h"
#include "ui_settings_display_sound.h"
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
#include "temperature_service.h"
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
#include "upgrade_banner.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "wifi_manager.h"

// Backend headers
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "app_globals.h"
#include "filament_consumption_tracker.h"
#include "filament_sensor_manager.h"
#include "gcode_file_modifier.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_translation.h"
#include <lvgl/src/misc/cache/instance/lv_image_cache.h>
#include "hv/hlog.h" // libhv logging - sync level with spdlog
#include "logging_init.h"
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
#include "tool_state.h"
#include "xml_registration.h"

#include <spdlog/spdlog.h>

#ifdef HELIX_DISPLAY_SDL
#include <SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <typeinfo>
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

// Android lifecycle: background/foreground state set from SDL event handler
std::atomic<bool> s_app_backgrounded{false};

// Crash loop detection marker file. Routed through the writable config
// dir so it survives RO-rootfs platforms (Yocto squashfs etc.).
const std::string& crash_marker_path() {
    static const std::string p = helix::writable_path(".crash_restart_count");
    return p;
}

// Safe-mode marker written by the watchdog when it detects a deterministic
// crash loop (CRASH_LOOP_MAX_CRASHES same-signature crashes within the
// CRASH_LOOP_WINDOW_SEC window). When present at startup, the application
// defers Moonraker connection so the user can reach Settings and clear the
// underlying state (a stuck subscription field, bad printer URL, etc.)
// without re-crashing on the same code path.
//
// One-shot: deleted once the main loop is running and the user can dismiss
// the banner. A clean reboot exits Safe Mode automatically.
const std::string& safe_mode_marker_path() {
    static const std::string p = helix::writable_path("safe_mode.flag");
    return p;
}

bool s_safe_mode_active = false;

bool consume_safe_mode_marker() {
    // Watchdog writes one of these two paths — primary (writable_path) first,
    // then /tmp fallback if the primary path is unwritable (read-only fs,
    // stuck systemd namespace, etc.). Check both so any successful write
    // by the watchdog reaches us.
    std::vector<std::string> paths{
        safe_mode_marker_path(),
        "/tmp/helix-screen-safe-mode.flag",
    };
    bool found = false;
    for (const auto& path : paths) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        spdlog::warn("[Application] Safe Mode marker present at {} — booting without "
                     "Moonraker connection",
                     path);
        std::filesystem::remove(path, ec);
        if (ec) {
            spdlog::warn("[Application] Failed to remove Safe Mode marker {}: {}", path,
                         ec.message());
        }
        found = true;
    }
    return found;
}

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
 * @brief Signal handler for SIGINT/SIGTERM
 *
 * SIGINT (Ctrl+C from terminal): set quit flag, let main loop drain into
 * Application::shutdown() for full graceful teardown.
 *
 * SIGTERM (supervisor kill — systemd, watchdog, ZMOD's killall cycle, etc.):
 * fast-exit immediately. The supervisor only cares about a clean exit code,
 * not whether we ran teardown. Skipping Application::shutdown() avoids
 * fragile teardown paths (LVGL deinit, observer cleanup, static destructors)
 * that have triggered SIGBUS on resource-constrained MIPS/ARM devices when
 * external supervisors aggressively respawn us. Persisted state (settings,
 * telemetry queue, crash history) is written on each change, so nothing is
 * lost by skipping the explicit flush.
 *
 * Async-signal-safe: only write(2) and _exit(2) used — no spdlog.
 */
void graceful_quit_signal_handler(int sig) {
    if (sig == SIGTERM) {
        static const char msg[] = "[Application] SIGTERM — fast exit\n";
        ssize_t n = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)n; // suppress unused-result warning
        _exit(0);
    }
    // SIGINT and any other caught signal: graceful path
    app_request_quit_signal_safe();
}

} // namespace

// C bridge functions called from SDL event handler (lv_sdl_window.c)
extern "C" void helix_notify_app_backgrounded() {
    s_app_backgrounded.store(true);
    spdlog::info("[Application] App entering background");
}

extern "C" void helix_notify_app_foregrounded() {
    s_app_backgrounded.store(false);
    spdlog::info("[Application] App returning to foreground");
}

const std::string& instance_lock_path() {
    static const std::string p = helix::writable_path(".helix-screen.lock");
    return p;
}

bool Application::acquire_instance_lock() {
    const std::string lock_path = instance_lock_path();
    // O_CLOEXEC: flock is per-file (not per-fd), so without CLOEXEC the lock
    // leaks to fork()ed children and survives execve() in those children.
    // That produced a deadlock during the post-install restart path in
    // UpdateChecker::do_install(): the parent _exit(0)'d, but the child (forked
    // before _exit) still held an inherited fd on the lock file, keeping the
    // lock alive.  When the child execve'd the new helix-screen, its fresh
    // acquire_instance_lock() failed with EWOULDBLOCK and the new instance
    // refused to start — leaving the device with a frozen last frame.
    m_lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (m_lock_fd < 0) {
        spdlog::error("[Application] Cannot open lock file {}: {}", lock_path, strerror(errno));
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
        crash_handler::install(helix::writable_path("crash.txt"));
    }

    // HELIX_CRASH_TEST=1 intentionally segfaults through a known call chain
    // to verify the signal handler's unwind on real hardware. Must run AFTER
    // install() so the generated crash.txt exercises the real handler.
    if (const char* t = std::getenv("HELIX_CRASH_TEST"); t && *t && std::string(t) != "0") {
        crash_handler::trigger_test_crash();
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
            std::ifstream in(crash_marker_path());
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
            std::filesystem::remove(crash_marker_path());
            return 1;
        } else {
            // Write filtered timestamps plus current restart
            std::ofstream out(crash_marker_path(), std::ios::trunc);
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

    // Read and consume the Safe Mode marker before any subscription-triggering
    // code runs. The watchdog writes this when it detects a deterministic
    // crash loop; reading it here lets connect_moonraker() (Phase 9d) skip the
    // auto-connect so the user can reach Settings without re-crashing.
    s_safe_mode_active = consume_safe_mode_marker();
    if (s_safe_mode_active) {
        spdlog::warn("[Application] Booting in SAFE MODE — Moonraker connection deferred");
    }

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

    // Phase 8a: Load translations (must be before UI creation for hot-reload support)
    if (!init_translations()) {
        shutdown();
        return 1;
    }

    // Phase 8b: Rotation probe + layout manager init
    // Must run AFTER init_translations() so lv_tr() is available for probe strings.
    // Also must run before panel creation so layout-specific XML overrides are resolved.
    run_rotation_probe_and_layout();

    // Phase 8c: Register XML components
    if (!register_xml_components()) {
        shutdown();
        return 1;
    }

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
    // Now works because PrinterState exists from phase 9a.
    // Start HTTP executors first — the Moonraker APIs submit to them on
    // every request. Two lanes (fast for REST, slow for file transfers)
    // prevent a large upload from head-of-line blocking status polls.
    helix::http::HttpExecutor::start_all();
    if (!init_moonraker()) {
        shutdown();
        return 1;
    }

    // Initialize UpdateChecker before panel subjects (subjects must exist for XML binding)
    // On Android the checker still runs (so "Check for Updates" works), but
    // "Install Update" redirects to the Play Store instead of self-updating.
    UpdateChecker::instance().init();

    // Initialize UpgradeBanner — creates the persistent top-banner widget on
    // lv_layer_top and observes UpdateChecker state. Ships hidden because the
    // /upgrade_nudge/intensity setting defaults to 'off'; flipped to
    // 'aggressive' for the 1.0 rollout (no code change needed).
    UpgradeBanner::instance().init();

    // Initialize CrashReporter (independent of telemetry)
    // Write mock crash file first if --mock-crash flag is set (requires --test)
    const std::string user_config_dir = helix::get_user_config_dir();
    if (get_runtime_config()->mock_crash) {
        crash_handler::write_mock_crash_file(user_config_dir + "/crash.txt");
        spdlog::info("[Application] Wrote mock crash file for testing");
    }
    helix::CrashHistory::instance().init(user_config_dir);
    CrashReporter::instance().init(user_config_dir);

    // Initialize TelemetryManager (opt-in, default OFF)
    // Note: record_session() is called after init_panel_subjects() so that
    // SettingsManager subjects are ready and the enabled state can be synced.
    TelemetryManager::instance().init(user_config_dir);

    // First heap snapshot, before panel construction and XML load. Later
    // snapshots are diffed against this to narrow down which startup phase
    // burns allocator arena on small-RAM devices.
    TelemetryManager::instance().record_memory_snapshot("post_telemetry_init");

    // Initialize PrinterImageManager (custom image import/resolution)
    helix::PrinterImageManager::instance().init(helix::get_user_config_dir());

    // Phase 9c: Initialize panel subjects with API injection
    // Panels receive API at construction - no deferred set_api() needed
    if (!init_panel_subjects()) {
        shutdown();
        return 1;
    }

    // Phase 9d: Start Moonraker connection early (during splash)
    // Discovery runs async — by the time UI is created and splash exits,
    // connection and discovery may already be complete, saving ~2s.
    //
    // Safe Mode skips this entirely — the watchdog wrote the marker because
    // every previous boot crashed during subscription handling, and reaching
    // Settings is more important than reconnecting. The user can re-enable
    // the connection from Settings once they've fixed the underlying state.
    if (s_safe_mode_active) {
        spdlog::warn("[Application] Safe Mode: skipping Moonraker auto-connect");
    } else if (!connect_moonraker()) {
        // Non-fatal - app can still run without connection
        spdlog::warn("[Application] Running without printer connection");
    }

    // Sync TelemetryManager's LVGL subject with its current enabled state.
    // TelemetryManager already loaded the value from settings.json in init();
    // this call no longer needs to pull from SystemSettingsManager (which
    // used to be a separate source of truth — the sync here silently
    // disabled telemetry whenever settings.json lacked /telemetry_enabled).
    // Instead, re-assert the already-loaded state so start_auto_send() runs
    // now that discovery has completed.
    TelemetryManager::instance().set_enabled(TelemetryManager::instance().is_enabled());

    // Initialize SoundManager (audio feedback)
    // Backend detection (PWM, ALSA, SDL) is checked by PrinterCapabilitiesState
    // to show sound settings even without a Klipper beeper output_pin.
    SoundManager::instance().initialize();
    SoundManager::instance().play("startup", SoundPriority::EVENT);

    // Show sound settings immediately if a local backend exists,
    // without waiting for hardware discovery / Klipper connection.
    if (SoundManager::instance().has_backend()) {
        get_printer_state().set_sound_backend_available(true);
    }

    // Initialize PostOpCooldownManager (unified filament operation cooldown)
    PostOpCooldownManager::instance().init(get_moonraker_api());

    // Begin tracking external-spool consumption across prints.
    helix::FilamentConsumptionTracker::instance().start();

    // Update DisplaySettingsManager with theme mode support (must be after both theme and settings
    // init)
    DisplaySettingsManager::instance().on_theme_changed();

    // Phase 10: Create UI and wire panels
    if (!init_ui()) {
        shutdown();
        return 1;
    }

    // Post-UI safety net: phases 11-16b run finalize_setup, plugin init,
    // overlay construction, and the first synchronous render. Any std::exception
    // escaping here unwinds out of run() into main()'s catch and exits 134,
    // which the watchdog interprets as a deterministic crash and (after
    // CRASH_LOOP_MAX_CRASHES) shows the recovery dialog. The 4ca58af52 hotfix
    // wraps main_loop() iterations but does not cover this pre-loop window —
    // the 5ac58e051 follow_overlay regression hit exactly here, in
    // HomePanel::finalize_setup() → set_config(null) (json::type_error::306).
    // Catch + log + breadcrumb + toast + continue so the user gets a degraded
    // but usable app instead of a watchdog crash loop they can only escape by
    // reflashing. main_loop() owns the runaway-streak guard for steady state.
    try {
    // Heap snapshot after XML panel load completes. Delta against
    // post_telemetry_init is the cost of init_panel_subjects + connect_moonraker
    // + init_ui — the window where #758 class aborts have fired.
    TelemetryManager::instance().record_memory_snapshot("post_init_ui");

    // Check for crash from previous session (after UI exists, before wizard)
    // Skip in test mode — don't show crash dialog during development
    // Exception: --mock-crash explicitly requests the dialog for testing
    bool show_crash_dialog =
        !get_runtime_config()->is_test_mode() || get_runtime_config()->mock_crash;
    if (show_crash_dialog && CrashReporter::instance().has_crash_report()) {
        if (TelemetryManager::instance().had_update_restart()) {
            spdlog::info("[Application] Crash from post-update restart, suppressing crash dialog");
            CrashReporter::instance().consume_crash_file();
        } else {
            auto report = CrashReporter::instance().collect_report();
            if (report.signal_name.empty()) {
                // Empty signal_name means read_crash_file() returned null because
                // the file lacked the required signal/name fields — typically a
                // signal handler killed mid-write (OOM-killer, watchdog, power
                // loss). Showing a dialog here just lets the user submit a
                // useless bundle (see CHUQCNAE 2026-05-05).
                spdlog::warn(
                    "[Application] Crash file unparseable — consuming and skipping dialog");
                CrashReporter::instance().consume_crash_file();
            } else if (CrashReporter::instance().is_duplicate(report)) {
                spdlog::info("[Application] Duplicate crash ({}), suppressing dialog",
                             CrashReporter::fingerprint(report));
                CrashReporter::instance().consume_crash_file();
            } else {
                spdlog::info("[Application] Previous crash detected — showing crash report dialog");
                auto* modal = new CrashReportModal();
                modal->set_report(report);
                modal->show_modal(lv_screen_active());
            }
        }
    }

    // Register wizard completion callback for add-printer recovery
    set_wizard_completion_callback([this]() {
        if (!m_wizard_previous_printer_id.empty()) {
            spdlog::info("[Application] Wizard completed — clearing add-printer recovery state");
            m_wizard_previous_printer_id.clear();
        }
        m_wizard_active = false;
        // The home panel's carousel + default layout were deferred so the
        // build could see ams_slot_count from Moonraker discovery. Finalize
        // now that the wizard (and thus the initial connect) is done.
        get_global_home_panel().finalize_setup();
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
        // No wizard will run — finalize the home panel immediately so its
        // default layout reflects currently-connected hardware.
        get_global_home_panel().finalize_setup();
    }

    // Phase 14: Initialize and load plugins
    // Must be after UI panels exist (injection points are registered by panels)
    if (!init_plugins()) {
        spdlog::warn("[Application] Plugin initialization had errors (non-fatal)");
    }

    // Banner: Safe Mode — UI is up, so this is the earliest the user can
    // see why the printer connection didn't come up. Sticky (no auto-dismiss)
    // because the user needs to act on it before the connection comes back.
    if (s_safe_mode_active) {
        ToastManager::instance().show(
            ToastSeverity::WARNING,
            "Safe Mode active. The printer connection is disabled because the app "
            "kept crashing on startup. Open Settings to fix the issue, then reboot.",
            0 /* sticky */);
    }

    // Phase 14b: Check WiFi availability if expected
    check_wifi_availability();

    // Phase 15: Start memory monitoring (logs at TRACE level, -vvv)
    helix::MemoryMonitor::instance().start(5000);
    helix::MemoryMonitor::instance().set_warning_callback(
        [](const helix::MemoryWarningEvent& event) {
            TelemetryManager::instance().record_memory_warning(event);
        });

    // Drop LVGL's decoded-image cache on critical pressure. Printer images,
    // thumbnails, and XML-loaded PNGs live here as full ARGB8888 pixel buffers
    // (e.g. a 300x300 printer image is ~360KB decoded). Freeing them forces
    // the next draw to re-decode, which is cheap compared to an OOM kill.
    // Responder fires on the monitor thread — defer to UI thread via
    // queue_update: lv_image_cache_drop() reaches into the draw units
    // (LV_EVENT_INVALIDATE_AREA broadcast) which is not safe off the UI thread.
    helix::MemoryMonitor::instance().add_pressure_responder(
        [](helix::MemoryPressureLevel level) {
            if (level >= helix::MemoryPressureLevel::critical) {
                helix::ui::queue_update([]() {
                    spdlog::warn("[Application] Pressure response: dropping LVGL image cache");
                    crash_handler::breadcrumb::note("lvgl_imgcache", "drop");
                    lv_image_cache_drop(nullptr);
                });
            }
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

    } catch (const std::exception& e) {
        const char* type_name = typeid(e).name();
        spdlog::error("[Application] Caught exception during post-UI init: {} ({})", e.what(),
                      type_name);
        crash_handler::breadcrumb::note("post_init_catch", type_name);
        crash_handler::breadcrumb::dump_to_fd(STDERR_FILENO);
        try {
            TelemetryManager::instance().record_error("post_init", "unhandled_exception", e.what());
        } catch (...) {
            // Telemetry must never re-throw out of the catch handler.
        }
        try {
            ToastManager::instance().show(
                ToastSeverity::ERROR,
                "App startup encountered an error. Some features may be unavailable.",
                0 /* sticky */);
        } catch (...) {
            // Toast failure is non-fatal; the user still gets a working main loop.
        }
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

    // HELIX_SCREEN_SIZE: screen size override (alternative to -s flag)
    // Only applies if -s was not passed on the command line
    if (m_screen_width == 0 && m_screen_height == 0) {
        if (auto size_str = EnvConfig::get_screen_size()) {
            if (helix::parse_screen_size_string(size_str->c_str(), m_screen_width, m_screen_height,
                                                m_args.screen_size)) {
                spdlog::info("[Application] Screen size from HELIX_SCREEN_SIZE: {}x{}",
                             m_screen_width, m_screen_height);
            } else {
                spdlog::warn("[Application] Invalid HELIX_SCREEN_SIZE='{}' — use named size "
                             "(micro/tiny/small/medium/large/xlarge) or WxH (e.g. 480x400)",
                             *size_str);
            }
        }
    }

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

    // Route per-tool writable state (tool_spools.json) through the same
    // HELIX_CONFIG_DIR override that Config::init honors. Without this,
    // ToolState::config_dir_ stays at the default "config" (relative to CWD)
    // and spool-per-tool saves silently fail on read-only baseline installs
    // where the install tree is on a squashfs rootfs.
    if (const char* env_dir = std::getenv("HELIX_CONFIG_DIR");
        env_dir != nullptr && env_dir[0] != '\0') {
        helix::ToolState::instance().set_config_dir(env_dir);
        spdlog::info("[Application] ToolState config dir: {}", env_dir);
    }

    // Initialize streaming policy from config (auto-detects thresholds from RAM)
    helix::StreamingPolicy::instance().load_from_config();

    // Load persisted thermal heating rates so estimates are available immediately
    ThermalRateManager::instance().load_from_config(*m_config);

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
    config.size_was_explicit = m_args.size_was_explicit;

    // Get scroll config from settings.json
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

#ifdef __ANDROID__
    {
        float ddpi = 0, hdpi = 0, vdpi = 0;
        if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0) {
            spdlog::info("[Application] Android display DPI: diagonal={:.0f} h={:.0f} v={:.0f}",
                         ddpi, hdpi, vdpi);
        }
        spdlog::info("[Application] Android screen: {}x{} (DPI-aware sizing via SDL)",
                     m_screen_width, m_screen_height);
    }
#endif

    // Register LVGL log handler AFTER lv_init() (called inside display->init())
    // Must be after lv_init() because it resets global state and clears callbacks
    helix::logging::register_lvgl_log_handler();

    // Always set DPI explicitly. LVGL's lv_display_create() initializes dpi to
    // LV_DPI_DEF (160), but the fbdev/DRM drivers will OVERWRITE it from the
    // kernel's reported physical screen size (FBIOGET_VSCREENINFO width/height
    // in mm, or DRM connector mmWidth). When the driver reports BOGUS physical
    // dimensions — observed on BTT CB1 / sun4i-drmdrmfb, which reports the
    // BTT HDMI5 5" panel (real ≈109mm × 65mm) as 890mm × 500mm (≈35"×20",
    // off by ~8×) — that computes to dpi≈23, and LV_DPX_CALC(23, 10) clamps
    // PAD_SMALL to 1px via
    // its MAX(.., 1) safeguard. Result: dropdown / input padding visually
    // disappears. Forcing dpi here (after the driver has had its chance) makes
    // the UI immune to lying kernel drivers.
    int32_t effective_dpi = (m_args.dpi > 0) ? m_args.dpi : LV_DPI_DEF;
    int32_t pre_set_dpi = lv_display_get_dpi(m_display->display());
    lv_display_set_dpi(m_display->display(), effective_dpi);
    spdlog::debug("[Application] Display DPI applied: {} (was {} before set)",
                  effective_dpi, pre_set_dpi);
    if (pre_set_dpi < 50 && m_args.dpi == 0) {
        spdlog::warn("[Application] Display reported dpi={} before set — backend lost LV_DPI_DEF "
                     "between create and theme init. Fix-forward applied (forced to {}).",
                     pre_set_dpi, effective_dpi);
    }

    // Get active screen
    m_screen = lv_screen_active();

    // Set window icon
    ui_set_window_icon(m_display->display());

    // Initialize resize handler
    m_display->init_resize_handler(m_screen);

    // Initialize tips manager
    TipsManager* tips_mgr = TipsManager::get_instance();
    if (!tips_mgr->init(helix::find_readable("printing_tips.json"))) {
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
            int32_t pre_w = m_screen_width;
            int32_t pre_h = m_screen_height;

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

            // Rotation changed screen dimensions — refresh theme layout constants
            // (nav_width, overlay widths, spacing tokens) that were calculated
            // during Phase 6 with pre-rotation dimensions.
            if (m_screen_width != pre_w || m_screen_height != pre_h) {
                theme_manager_refresh_layout_constants(m_display->display());
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
        m_hot_reloader->set_after_reload_callback([](const std::string& component) {
            if (NavigationManager::is_destroyed()) return;
            spdlog::debug("[HotReload] Post-reload rebuild triggered by '{}'", component);
            NavigationManager::instance().rebuild_active_views();
        });
        m_hot_reloader->start({"ui_xml"});
    }

    return true;
}

bool Application::init_translations() {
    // Suppress LVGL translation warnings during init — incomplete translations
    // are expected and produce many "language is missing from tag" warnings
    helix::logging::set_suppress_translation_warnings(true);

    // NOTE: lv_i18n (src/generated/lv_i18n_translations.c) was a parallel i18n
    // subsystem kept alongside LVGL's native lv_translation_* API. It was never
    // read from — lv_i18n_get_text() has no callers in the tree. Removing the
    // init/set_locale calls lets LTO strip the ~1 MB of compiled language pack
    // rodata. All real language switching goes through lv_translation_set_language
    // and the per-locale XML files loaded below.

    // Load ONLY the current locale's translations. Parsing the combined
    // translations.xml with all 9 languages at startup burns ~500-700 KB of
    // heap in lv_translation_pack_t. Loading a single locale uses ~60-80 KB,
    // and other locales load on demand when the user switches language.
    // See helix::ui::ensure_translation_loaded().
    std::string lang = m_config->get_language();
    helix::ui::ensure_translation_loaded(lang);

    // Set initial language. When no pack is loaded for a language (e.g. English
    // with no en.xml), lv_translation_get() returns the tag itself — and since
    // our tags ARE English, English UI works without any registered pack.
    lv_translation_set_language(lang.c_str());

    // Load CJK runtime fonts if persisted language is CJK
    helix::system::CjkFontManager::instance().on_language_changed(lang);

    // Re-enable translation warnings for runtime (post-init warnings are actionable)
    helix::logging::set_suppress_translation_warnings(false);
    spdlog::info("[Application] Language set to '{}'", lang);

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
    ui_panel_belt_tension_register_callbacks();
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

    // Drain any warnings that backends enqueued during pre-UI initialization
    // (e.g. "simpledrm detected", "requested resolution not available").
    // See prestonbrown/helixscreen#766.
    helix::PendingStartupWarnings::instance().drain(
        [](helix::PendingStartupWarnings::Severity sev, const std::string& msg) {
            ToastSeverity toast_sev = ToastSeverity::INFO;
            switch (sev) {
            case helix::PendingStartupWarnings::Severity::INFO:
                toast_sev = ToastSeverity::INFO;
                break;
            case helix::PendingStartupWarnings::Severity::SUCCESS:
                toast_sev = ToastSeverity::SUCCESS;
                break;
            case helix::PendingStartupWarnings::Severity::WARNING:
                toast_sev = ToastSeverity::WARNING;
                break;
            case helix::PendingStartupWarnings::Severity::ERROR:
                toast_sev = ToastSeverity::ERROR;
                break;
            }
            ToastManager::instance().show(toast_sev, msg.c_str(), 8000);
        });

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
    m_job_queue_state = std::make_unique<JobQueueState>(m_moonraker->api(), get_moonraker_client());
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
        get_global_step_test_panel().init_subjects();
        if (auto* p = create_overlay_panel(m_screen, "step_test_panel", "step progress")) {
            get_global_step_test_panel().setup(p, m_screen);
            NavigationManager::instance().push_overlay(p);
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
        auto& display_settings = helix::settings::get_display_sound_settings_overlay();
        display_settings.show_theme_preview(m_screen);
        spdlog::info("[Application] Opened theme preview overlay via CLI");
    }

    if (m_args.overlays.theme_edit) {
        // Push theme preview first, then theme editor on top
        auto& display_settings = helix::settings::get_display_sound_settings_overlay();
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
        auto& overlay = helix::settings::get_display_sound_settings_overlay();
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

    if (m_args.overlays.about) {
        auto& overlay = helix::settings::get_about_settings_overlay();
        overlay.show(m_screen);
        spdlog::info("[Application] Opened about overlay via CLI");
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

    if (m_args.overlays.timelapse_videos) {
        open_timelapse_videos();
        spdlog::info("[Application] Opened timelapse videos overlay via CLI");
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

    Application* app = this;

    client->set_on_hardware_discovered([api, client, app](const helix::PrinterDiscovery& hardware) {
        // Copy hardware into a mutable snapshot on the BG thread so the
        // queued main-thread callback owns a stable, non-aliased copy. Previous
        // implementations used an aggregate ctx struct which triggered multiple
        // PrinterDiscovery copies and a crash on main-thread copy-assign (#761).
        // Use std::move on the main thread to avoid iterating hash table nodes
        // during copy-assign, which is vulnerable to heap corruption (#789).
        auto snapshot = std::make_shared<helix::PrinterDiscovery>(hardware);
        helix::ui::queue_update([api, client, app, snapshot]() {
            if (app->m_shutdown_complete)
                return;
            api->hardware() = std::move(*snapshot);
            helix::init_subsystems_from_hardware(api->hardware(), api, client);
        });
    });

    client->set_on_discovery_complete([api, client, app](const helix::PrinterDiscovery& hardware,
                                                         const nlohmann::json& initial_status) {
        spdlog::debug("[Application] on_discovery_complete BG-thread entry (status keys: {})",
                      initial_status.is_object() ? initial_status.size() : 0);
        auto snapshot = std::make_shared<helix::PrinterDiscovery>(hardware);
        auto status_snapshot = std::make_shared<const nlohmann::json>(initial_status);
        helix::ui::queue_critical("Application::on_discovery_complete",
                                  [api, client, app, snapshot, status_snapshot]() {
            // Count invocations so crash bundles reveal whether we crashed on
            // the first discovery or on a reconnect-triggered re-run.
            static int s_discovery_complete_n = 0;
            long n = static_cast<long>(++s_discovery_complete_n);
            crash_handler::breadcrumb::note(
                "disc", "cb_begin", n);

            spdlog::debug("[Application] on_discovery_complete UI-thread entry (shutdown={})",
                          app->m_shutdown_complete);
            // Safety check: if Application is shutting down, skip all processing
            // This prevents use-after-free if shutdown races with callback delivery
            if (app->m_shutdown_complete) {
                return;
            }

            // Copy snapshot into API's hardware data. Copy (not move) so we can
            // move the snapshot into set_hardware below — the snapshot is the
            // only reference we own and nobody else aliases it, so this copy is
            // race-free (#789, #799).
            crash_handler::breadcrumb::note(
                "disc", "pre_api_hw",
                static_cast<long>(snapshot->macros().size()));
            api->hardware() = *snapshot;
            crash_handler::breadcrumb::note("disc", "post_api_hw", n);

            // Mark discovery complete so splash can exit
            app->m_splash_manager.on_discovery_complete();
            spdlog::info("[Application] Moonraker discovery complete, splash can exit");

            // Clean up self-update sentinel — the app started successfully,
            // so helixscreen-update.service no longer needs to be suppressed.
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                std::string sentinel =
                    AppConstants::Update::backup_fallback_dir() + "/self_restart_sentinel";
                if (fs::remove(sentinel, ec)) {
                    spdlog::info("[Application] Cleaned up self-restart sentinel");
                }
                // Legacy: best-effort under PrivateTmp (sees private /tmp,
                // not real /tmp — stale real sentinels cleaned on reboot)
                fs::remove("/tmp/helixscreen_self_restart", ec);
            }

            // Move snapshot into set_hardware (by-value param) so no hash-table
            // copy iterates against a live, potentially-mutated api->hardware_ (#799).
            // After this point *snapshot is empty — use api->hardware() for reads.
            const auto& hw = api->hardware();
            crash_handler::breadcrumb::note(
                "disc", "pre_set_hw",
                static_cast<long>(snapshot->macros().size()));
            get_printer_state().set_hardware(std::move(*snapshot));
            crash_handler::breadcrumb::note("disc", "post_set_hw", n);
            get_printer_state().init_fans(
                hw.fans(), helix::FanRoleConfig::from_config(Config::get_instance()));
            crash_handler::breadcrumb::note("disc", "post_init_fans",
                                             static_cast<long>(hw.fans().size()));

            // Dispatch initial subscription status AFTER init_fans so fan/sensor subjects
            // exist when the status data is processed. The initial status is passed from the
            // discovery sequence rather than dispatched separately to guarantee ordering.
            if (!(*status_snapshot).empty()) {
                client->dispatch_status_update((*status_snapshot));
            }
            crash_handler::breadcrumb::note("disc", "post_status_dispatch", n);

            get_printer_state().set_klipper_version(hw.software_version());
            get_printer_state().set_moonraker_version(hw.moonraker_version());
            if (!hw.os_version().empty()) {
                get_printer_state().set_os_version(hw.os_version());
            }

            // Populate LED chips now that hardware is discovered
            get_global_settings_panel().populate_led_chips();
            crash_handler::breadcrumb::note("disc", "post_led_chips", n);

            // Fetch print hours now that connection is live, and refresh on job changes
            helix::settings::get_about_settings_overlay().fetch_print_hours();
            client->register_method_callback(
                "notify_history_changed", "AboutOverlay_print_hours",
                [](const nlohmann::json& /*data*/) {
                    helix::ui::queue_update([]() {
                        helix::settings::get_about_settings_overlay().fetch_print_hours();
                    });
                });

            // Register for timelapse events when timelapse is detected
            client->register_method_callback(
                "notify_timelapse_event", "timelapse_state", [](const nlohmann::json& data) {
                    helix::TimelapseState::instance().handle_timelapse_event(data);
                });

            // Detect when Moonraker finishes updating HelixScreen (e.g. via Mainsail).
            // On SysV platforms (AD5X, AD5M, K1) there is no systemd path watcher,
            // so this WebSocket-based detection is the only restart trigger.
            client->register_method_callback(
                "notify_update_response", "external_update_restart", [](const nlohmann::json& msg) {
                    // notify_update_response params: [{"application":"helixscreen",
                    //   "proc_id":N, "message":"...", "complete":true/false}]
                    // Method callbacks always receive the full JSON-RPC message
                    if (!msg.contains("params") || !msg["params"].is_array() ||
                        msg["params"].empty())
                        return;
                    const auto& p = msg["params"][0];
                    if (!p.contains("application") || !p.contains("complete"))
                        return;
                    std::string app = p["application"].get<std::string>();
                    bool complete = p["complete"].get<bool>();
                    if (app != "helixscreen")
                        return;
                    if (!complete) {
                        spdlog::debug("[Application] Moonraker updating helixscreen: {}",
                                      p.value("message", ""));
                        return;
                    }
                    // Defer to main thread — _exit(0) from a WebSocket callback
                    // would skip flush and leave the display frozen.
                    helix::ui::queue_update(
                        []() { UpdateChecker::handle_external_update_complete(); });
                });

            // Subscribe to power device and sensor state change notifications
            if (api) {
                helix::PowerDeviceState::instance().subscribe(*api);
                helix::SensorState::instance().subscribe(*api);
            }
            crash_handler::breadcrumb::note("disc", "post_subscribe", n);

            // Auto-detect printer type if not already set (e.g., fresh install with preset).
            // MUST run BEFORE HardwareValidator::validate — otherwise the validator checks
            // the scaffolded defaults (fans/part="fan", fans/hotend="heater_fan hotend_fan")
            // against the discovered hardware and flags them as missing, even though the
            // preset that's about to be applied would map those slots to the correct
            // device-specific names. Runs even while the wizard is active so the preset
            // lands BEFORE the wizard hits its connection / printer-identify steps —
            // that's the only path for preset_mode to become true on a fresh install of
            // a known printer like the ForgeX AD5M Pro. auto_detect_and_save self-guards
            // on PRINTER_TYPE already being set, so a user's manual pick in the identify
            // step (which writes PRINTER_TYPE on cleanup) won't be overwritten by a later
            // discovery callback.
            // NOTE: use api->hardware() — snapshot was std::move'd into it above (#789).
            // Reading *snapshot here would pass an empty/moved-from PrinterDiscovery and
            // detection would fail with "0 sensors, 0 fans, hostname ''" (#802).
            PrinterDetector::auto_detect_and_save(api->hardware(), Config::get_instance());

            // Hardware validation: check config expectations vs discovered hardware.
            // Now uses the post-preset config so preset-mapped fan/heater names are
            // checked against discovery, not the pre-preset scaffolded defaults.
            HardwareValidator validator;
            auto validation_result = validator.validate(Config::get_instance(), api->hardware());
            get_printer_state().set_hardware_validation_result(validation_result);

            if (validation_result.has_issues() && !Config::get_instance()->is_wizard_required() &&
                !is_wizard_active()) {
                validator.notify_user(validation_result);
            }

            // Save session snapshot for next comparison (even if no issues)
            validator.save_session_snapshot(Config::get_instance(), api->hardware());
            crash_handler::breadcrumb::note("disc", "post_validate", n);

            // Record telemetry session event now that hardware data is available
            // (hardware_profile is deferred until after build volume is fetched below)
            TelemetryManager::instance().record_session();
            TelemetryManager::instance().record_settings_snapshot();
            TelemetryManager::instance().record_memory_snapshot("session_start");
            crash_handler::breadcrumb::note("disc", "post_telemetry", n);

            // Fetch safety limits and build volume from Klipper config (stepper ranges,
            // min_extrude_temp, max_temp, etc.) — runs for ALL discovery completions
            // (normal startup AND post-wizard) so we don't duplicate this in callers
            if (api) {
                MoonrakerAPI* api_ptr = api;
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

                        // Apply archetype-based thermal rate defaults using build volume
                        // Must marshal to main thread — runs in JSONRPC response callback
                        float bed_x_max = api_ptr->hardware().build_volume().x_max;
                        helix::ui::queue_update([bed_x_max]() {
                            ThermalRateManager::instance().apply_archetype_defaults(bed_x_max);
                        });

                        // Record hardware profile after build volume is populated
                        TelemetryManager::instance().record_hardware_profile();
                    },
                    [](const MoonrakerError& err) {
                        spdlog::warn("[Application] Failed to fetch safety limits: {}",
                                     err.message);
                        // Record hardware profile anyway, just without build volume
                        TelemetryManager::instance().record_hardware_profile();
                    });
            }

            // Detect helix_print plugin during discovery (not UI-initiated)
            // This ensures plugin status is known early for UI gating
            api->job().check_helix_plugin(
                [](bool available) { get_printer_state().set_helix_plugin_installed(available); },
                [](const MoonrakerError&) {
                    // Silently treat errors as "plugin not installed"
                    get_printer_state().set_helix_plugin_installed(false);
                });

            // Auto-assign a Spoolman spool to the active toolchanger tool when
            // no per-tool assignment has been persisted (Moonraker DB or local
            // JSON). Must run on the UI thread (called from queue_update lambdas).
            auto try_assign_active_spool_to_tool = [](const SpoolInfo& spool) {
                auto* backend = AmsState::instance().get_backend();
                if (!backend || !is_tool_changer(backend->get_type()))
                    return;

                auto& tool_state = helix::ToolState::instance();
                if (!tool_state.spool_assignments_loaded())
                    return; // assignments haven't loaded yet — can't safely auto-assign

                int tool_idx = tool_state.active_tool_index();
                if (tool_idx < 0)
                    return;

                const auto& tools = tool_state.tools();
                if (tool_idx >= static_cast<int>(tools.size()))
                    return;

                if (tools[tool_idx].spoolman_id > 0)
                    return; // already has an assignment

                tool_state.assign_spool(tool_idx, spool.id, spool.display_name(),
                                        static_cast<float>(spool.remaining_weight_g),
                                        static_cast<float>(spool.initial_weight_g));
                AmsState::instance().sync_from_backend();
                spdlog::info("[Application] Auto-assigned Spoolman spool {} to "
                             "toolchanger tool {}",
                             spool.id, tool_idx);
            };

            // Populate the external spool slot from a SpoolInfo and queue the
            // result to the UI thread. Used by both startup sync and live
            // notification paths.
            auto sync_external_spool = [try_assign_active_spool_to_tool](const SpoolInfo& spool,
                                                                         std::string log_context) {
                helix::ui::queue_update([spool, try_assign_active_spool_to_tool,
                                         log_context = std::move(log_context)]() {
                    SlotInfo slot;
                    slot.slot_index = -2;
                    slot.global_index = -2;
                    apply_spool_to_slot(slot, spool);
                    slot.spool_name = spool.display_name();
                    slot.multi_color_hexes = spool.multi_color_hexes;
                    AmsState::instance().set_external_spool_info(slot);
                    try_assign_active_spool_to_tool(spool);
                    spdlog::info("[Application] External spool {}: {} (id={})", log_context,
                                 slot.spool_name, slot.spoolman_id);
                });
            };

            // Sync external spool from Moonraker's active Spoolman spool
            // This ensures the filament panel shows the correct spool on startup,
            // even if the active spool was changed via Spoolman's web UI or another client
            {
                MoonrakerAPI* api_for_spool = api;
                api_for_spool->spoolman().get_spoolman_status(
                    [api_for_spool, sync_external_spool](bool connected, int active_spool_id) {
                        if (!connected || active_spool_id <= 0) {
                            spdlog::debug("[Application] No active Spoolman spool to sync "
                                          "(connected={}, spool_id={})",
                                          connected, active_spool_id);
                            return;
                        }

                        // Check if existing external spool already matches
                        auto existing = AmsState::instance().get_external_spool_info();
                        if (existing && existing->spoolman_id == active_spool_id) {
                            spdlog::debug("[Application] External spool already matches active "
                                          "Spoolman spool {}",
                                          active_spool_id);
                            return;
                        }

                        // Fetch spool details and populate external spool
                        spdlog::info("[Application] Syncing external spool from Moonraker active "
                                     "spool {}",
                                     active_spool_id);
                        api_for_spool->spoolman().get_spoolman_spool(
                            active_spool_id,
                            [active_spool_id,
                             sync_external_spool](const std::optional<SpoolInfo>& spool_opt) {
                                if (!spool_opt) {
                                    spdlog::warn("[Application] Active spool {} not found in "
                                                 "Spoolman",
                                                 active_spool_id);
                                    return;
                                }
                                sync_external_spool(*spool_opt, "synced");
                            },
                            [active_spool_id](const MoonrakerError& err) {
                                spdlog::warn("[Application] Failed to fetch active spool {}: {}",
                                             active_spool_id, err.message);
                            });
                    },
                    [](const MoonrakerError& err) {
                        spdlog::debug("[Application] Spoolman status unavailable: {}", err.message);
                    },
                    true); // silent: Spoolman not configured is normal
            }

            // Listen for Moonraker active spool changes (user changes spool in
            // Spoolman web UI or another client while HelixScreen is running)
            {
                MoonrakerAPI* api_for_notify = api;
                client->register_method_callback(
                    "notify_active_spool_set", "external_spool_sync",
                    [api_for_notify, sync_external_spool](const nlohmann::json& data) {
                        // Callback receives full JSON-RPC message — extract params
                        const auto& params_arr = data.contains("params") ? data["params"] : data;
                        int spool_id = 0;
                        if (params_arr.is_array() && !params_arr.empty()) {
                            const auto& params = params_arr[0];
                            if (params.contains("spool_id") && !params["spool_id"].is_null()) {
                                spool_id = params["spool_id"].get<int>();
                            }
                        }

                        if (spool_id <= 0) {
                            spdlog::info("[Application] Active spool cleared via notification");
                            helix::ui::queue_update(
                                []() { AmsState::instance().clear_external_spool_info(); });
                            return;
                        }

                        spdlog::info("[Application] Active spool changed to {} via notification",
                                     spool_id);
                        api_for_notify->spoolman().get_spoolman_spool(
                            spool_id,
                            [spool_id,
                             sync_external_spool](const std::optional<SpoolInfo>& spool_opt) {
                                if (!spool_opt)
                                    return;
                                sync_external_spool(*spool_opt, "updated via notification");
                            },
                            [spool_id](const MoonrakerError& err) {
                                spdlog::warn("[Application] Failed to fetch notified spool {}: {}",
                                             spool_id, err.message);
                            });
                    });
            }

            // Fetch job queue now that WebSocket is actually connected
            if (app->m_job_queue_state) {
                app->m_job_queue_state->fetch();
            }

            // Notify plugins that Moonraker is connected
            if (app->m_plugin_manager) {
                app->m_plugin_manager->on_moonraker_connected();
            }

            // Apply LED startup preference (turn on LED if user preference is enabled)
            helix::led::LedController::instance().apply_startup_preference();

            // Start automatic update checks (15s initial delay, then every 24h)
            UpdateChecker::instance().start_auto_check();

            // Auto-navigate to Z-Offset Calibration if manual probe is already active
            // (e.g., PROBE_CALIBRATE started from Mainsail or console before HelixScreen launched)
            // Deferred one tick: status updates from the subscription response are queued
            // via ui_queue_update and may not have landed yet at this point.
            MoonrakerAPI* api_ptr_zoffset = api;
            lv_obj_t* screen = app->m_screen;
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
    // Always connect at boot when we have a host (fresh-install scaffold seeds
    // moonraker_host=127.0.0.1, so embedded devices can reach Moonraker without
    // user intervention). Connecting during the wizard is what lets auto-detection
    // run early — without this, the preset can't be applied until the connection
    // step's manual auto-probe, defeating the purpose of preset_mode skipping
    // hardware steps. The wizard's connection step still re-uses this connection
    // (or replaces it if the user changed the host).
    // In test mode, gate on m_wizard_active so unit/integration tests that
    // launch with --wizard don't race against fixture setup.
    bool should_connect =
        has_cli_url || (get_runtime_config()->test_mode && !m_wizard_active) || !saved_host.empty();

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
        auto host = m_config->get<std::string>(m_config->df() + "moonraker_host", "localhost");
        auto port = m_config->get<int>(m_config->df() + "moonraker_port", 7125);
        moonraker_url = "ws://" + host + ":" + std::to_string(port) + "/websocket";
        http_base_url = "http://" + host + ":" + std::to_string(port);
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
            lv_obj_t* screen = lv_screen_active();
            if (m_action_prompt_modal && screen) {
                m_action_prompt_modal->show_prompt(screen, data);
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

    // Cluster:pstat-async-delete reproducer hook (#906). When
    // HELIX_AUTO_STRESS_PROMPT=1, drive a continuous show/hide cycle of
    // action_prompt_modal so a user can sit on Print Status and let the
    // bug accumulate. Period is configurable via HELIX_STRESS_PROMPT_MS
    // (default 250 ms — fast enough to outpace the modal exit animation
    // (~150 ms) so consecutive deletes pile in the same async list).
    if (const char* on = std::getenv("HELIX_AUTO_STRESS_PROMPT"); on && *on && std::string(on) != "0") {
        int period_ms = 500;
        if (const char* p = std::getenv("HELIX_STRESS_PROMPT_MS")) {
            try { period_ms = std::max(100, std::stoi(p)); } catch (...) {}
        }
        int delay_ms = 30000; // default 30s so user can navigate to Print Status first
        if (const char* d = std::getenv("HELIX_STRESS_START_DELAY_SEC")) {
            try { delay_ms = std::max(0, std::stoi(d)) * 1000; } catch (...) {}
        }
        spdlog::warn("[ActionPrompt] HELIX_AUTO_STRESS_PROMPT=1 — will drive show/hide every {}ms after {}s delay (gated on is_showing)", period_ms, delay_ms / 1000);
        // Gate transitions on ActionPromptManager state so we alternate
        // cleanly instead of stacking modals when the exit animation is
        // slower than our period. ActionPromptManager::is_showing() is the
        // canonical "is a prompt active right now" probe.
        struct StressCtx { ActionPromptManager* mgr; int period_ms; };
        auto* ctx = new StressCtx{prompt_mgr, period_ms};
        // One-shot kick-off timer; on fire it spawns the repeating stress
        // timer. Gives the operator time to navigate before modals start
        // hijacking the screen.
        auto* kickoff = lv_timer_create([](lv_timer_t* t) {
            auto* c = static_cast<StressCtx*>(lv_timer_get_user_data(t));
            if (!c) { lv_timer_delete(t); return; }
            spdlog::warn("[ActionPrompt] Stress timer ARMED — show/hide cycle starting now");
            auto* repeat = lv_timer_create([](lv_timer_t* t2) {
                auto* mgr = static_cast<ActionPromptManager*>(lv_timer_get_user_data(t2));
                if (!mgr) return;
                if (ActionPromptManager::is_showing()) {
                    mgr->process_line("// action:prompt_end");
                } else {
                    mgr->process_line("// action:prompt_begin StressTest");
                    mgr->process_line("// action:prompt_text Cluster A reproducer");
                    mgr->process_line("// action:prompt_button OK|ECHO_OK");
                    mgr->process_line("// action:prompt_show");
                }
            }, c->period_ms, c->mgr);
            (void)repeat;
            delete c;
            lv_timer_delete(t); // one-shot
        }, std::max(1, delay_ms), ctx);
        (void)kickoff;
    }

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
    //
    // [L072] We capture `api` by value into the lambda — the callback fires
    // from the WebSocket thread for the lifetime of the client. `api` outlives
    // every toast/recovery action this handler can spawn.
    MoonrakerAPI* api_for_errors = api;
    client->register_method_callback(
        "notify_gcode_response", "gcode_error_notifier",
        [api_for_errors](const nlohmann::json& msg) {
            if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
                return;
            }

            // Some Klipper builds (e.g. K1C, K2 with CFS) send errors as JSON
            // objects:
            //   !! {"code":"key851","msg":"retrude error, ...","values":[...]}
            // Prefer a friendly translation of the code over the raw msg —
            // Creality's `msg` strings are debug log text, not user-facing.
            // Falls back to msg when the code is unknown. Stashes the parsed
            // code into `out_code` so callers can offer code-specific actions
            // (e.g. one-tap deep recovery on key298 MCU-shutdown).
            auto extract_json_msg = [](std::string& text, std::string& out_code) {
                out_code.clear();
                if (text.empty() || text[0] != '{') return;
                try {
                    auto j = nlohmann::json::parse(text);
                    if (j.contains("code") && j["code"].is_string()) {
                        out_code = j["code"].get<std::string>();
                        // Pass `values` through so the decoder can splice
                        // " in unit 1 slot B" into the friendly message.
                        // Telemetry sample (2026-05-05): key849 with
                        // `values: [1, "B"]` previously surfaced a generic
                        // "Retract failed — filament stuck in connector"
                        // with no slot locator; user had to investigate.
                        nlohmann::json values = nlohmann::json::array();
                        if (j.contains("values")) {
                            values = j["values"];
                        }
                        if (auto friendly =
                                helix::printer::CfsErrorDecoder::lookup_message_with_values(
                                    out_code, values)) {
                            text = friendly->first + ". " + friendly->second;
                            return;
                        }
                    }
                    if (j.contains("msg") && j["msg"].is_string()) {
                        text = j["msg"].get<std::string>();
                    }
                } catch (...) {
                    // Not valid JSON, use as-is
                }
            };

            auto clean_for_toast = [&extract_json_msg](std::string& text, std::string& out_code) {
                extract_json_msg(text, out_code);

                // Friendly messages for common error patterns
                if (text.find("Must home axis") != std::string::npos ||
                    text.find("must home") != std::string::npos) {
                    text = lv_tr("Must home axes first");
                    return;
                }
                if (text.find("spi_transfer_response") != std::string::npos) {
                    text = lv_tr("Accelerometer communication failed. Try again.");
                    return;
                }

                // Truncate long messages for toast display
                constexpr size_t MAX_LEN = 80;
                if (text.size() > MAX_LEN) {
                    text = text.substr(0, MAX_LEN - 3) + "...";
                }
            };

            auto process_line = [&clean_for_toast, api_for_errors](const std::string& line) {
                if (line.empty()) {
                    return;
                }

                // Klipper emergency errors: "!! MCU shutdown", "!! Timer too close", etc.
                if (line.size() >= 2 && line[0] == '!' && line[1] == '!') {
                    // Strip "!! " prefix for cleaner display
                    std::string clean =
                        (line.size() > 3 && line[2] == ' ') ? line.substr(3) : line.substr(2);
                    std::string code;
                    clean_for_toast(clean, code);
                    spdlog::error("[GcodeError] Emergency: {} (code={})", clean,
                                  code.empty() ? "-" : code);

                    // Specific codes that warrant a one-tap deep-recovery action.
                    // key298: rpi MCU bridge daemon shut down — the canonical
                    // K2 case where firmware_restart alone can't recover.
                    if (code == "key298" && api_for_errors) {
                        ToastManager::instance().show_with_action(
                            ToastSeverity::ERROR, clean.c_str(), lv_tr("Recover"),
                            [](void* ud) {
                                auto* api = static_cast<MoonrakerAPI*>(ud);
                                if (!api) return;
                                spdlog::info("[GcodeError] User tapped Recover for key298");
                                helix::PrinterRecoveryService recovery(api);
                                recovery.recover(
                                    []() {
                                        spdlog::info("[Recovery] Auto-recovery initiated");
                                    },
                                    [](const MoonrakerError& err) {
                                        spdlog::error("[Recovery] Auto-recovery failed: {}",
                                                      err.message);
                                        ToastManager::instance().show(
                                            ToastSeverity::ERROR,
                                            ("Recovery failed: " + err.user_message()).c_str(),
                                            6000);
                                    });
                            },
                            api_for_errors, /*duration_ms=*/15000);
                        return;
                    }

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
                        std::string code;
                        clean_for_toast(clean, code);
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

    // Liveness breadcrumb: emits every ~30 s so a crashed session's last moments
    // always show recent activity, even if the user was idle on one panel.
    uint32_t last_tick_crumb = start_time;
    static constexpr uint32_t TICK_CRUMB_INTERVAL_MS = 30000;
    uint64_t frame_counter = 0;

    // Heap snapshot refresh cadence — cheap, called from the main thread so the
    // crash signal handler can read the cached values without touching
    // non-async-signal-safe APIs (mallinfo, open, lv_mem_monitor).
    uint32_t last_heap_refresh = start_time;
    static constexpr uint32_t HEAP_REFRESH_INTERVAL_MS = 10000;
    crash_handler::refresh_heap_snapshot();

    // Failsafe: track invalidation suppression with a hard deadline.
    // If splash handoff doesn't complete within this time, force rendering back on
    // to avoid a permanently black screen.
    bool invalidation_suppressed =
        get_runtime_config()->splash_pid > 0 && !m_splash_manager.has_exited();
    uint32_t suppression_start_tick = DisplayManager::get_ticks();
    static constexpr uint32_t INVALIDATION_FAILSAFE_MS =
        11000; // Must exceed DISCOVERY_TIMEOUT_MS (8s)

    // Configure main loop handler
    helix::application::MainLoopHandler::Config loop_config;
    loop_config.screenshot_enabled = m_args.screenshot_enabled;
    loop_config.screenshot_delay_ms = static_cast<uint32_t>(m_args.screenshot_delay_sec) * 1000U;
    loop_config.timeout_sec = m_args.timeout_sec;
    loop_config.benchmark_mode = helix::config::EnvironmentConfig::get_benchmark_mode();
    loop_config.benchmark_report_interval_ms = 5000;
    m_loop_handler.init(loop_config, start_time);

    // Show one-time post-migration notice before entering the main loop.
    // Splash handoff timing risk on target hardware (AD5M, Pi 3B) is theoretical —
    // verify on real hardware as part of Task 5 manual checks. If it's an issue,
    // move this call after the post-splash refresh block or defer via lv_async_call.
#ifdef HELIX_ENABLE_SCREENSAVER
    show_screensaver_migration_notice_if_pending();
#endif

    // Top-level safety net: any std::exception thrown from a callback invoked
    // inside lv_timer_handler() (observers, queued UpdateQueue items, LVGL
    // animations, async calls) unwinds the entire stack out of main_loop into
    // main()'s catch and exits 134 — the watchdog interprets this as a crash
    // and after CRASH_LOOP_MAX_CRASHES same-signature events triggers the
    // "HelixScreen Keeps Crashing" recovery dialog (#931, v0.99.54). The
    // 1b643f99c safety net wraps the initial-subscription dispatch path, but
    // queued/observer/timer callbacks inside lv_timer_handler are not yet
    // guarded. Catch + log + dump breadcrumbs + continue: the user gets a
    // toast and a usable app instead of a crash loop they can only escape
    // by reflashing the previous version. A streak counter breaks out if the
    // catch itself is in a tight retry loop.
    int exception_streak = 0;
    uint32_t streak_window_start = 0;
    static constexpr int RUNAWAY_THRESHOLD = 5;
    static constexpr uint32_t RUNAWAY_WINDOW_MS = 30000;

    // Main event loop
    while (lv_display_get_next(nullptr) && !app_quit_requested()) {
        try {
        uint32_t current_tick = DisplayManager::get_ticks();
        m_loop_handler.on_frame(current_tick);

        handle_keyboard_shortcuts();

        // Android lifecycle: pause/resume when backgrounded
        bool backgrounded = s_app_backgrounded.load();
        if (backgrounded && !m_backgrounded) {
            on_enter_background();
        } else if (!backgrounded && m_backgrounded) {
            on_enter_foreground();
        }

        // While backgrounded, still pump SDL events so we detect the
        // foreground transition (SDL_APP_DIDENTERFOREGROUND arrives via
        // sdl_event_handler which runs inside lv_timer_handler).
        // Rendering is suppressed via lv_display_enable_invalidation(false)
        // so the timer handler is cheap — just event processing + timers.
        if (m_backgrounded) {
            lv_timer_handler();
            DisplayManager::delay(200);
            continue;
        }

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

        // Periodic liveness breadcrumb (counts frames so crash bundles know
        // whether the loop was spinning normally or stalled).
        ++frame_counter;
        if ((current_tick - last_tick_crumb) >= TICK_CRUMB_INTERVAL_MS) {
            crash_handler::breadcrumb::note("tick", "", static_cast<long>(frame_counter));
            last_tick_crumb = current_tick;
        }

        // Refresh cached heap snapshot so any crash within the next window
        // reports recent memory state.
        if ((current_tick - last_heap_refresh) >= HEAP_REFRESH_INTERVAL_MS) {
            crash_handler::refresh_heap_snapshot();
            last_heap_refresh = current_tick;
        }

        // Run LVGL tasks — returns ms until next timer needs to fire
        auto frame_start = std::chrono::steady_clock::now();
        uint32_t time_till_next = lv_timer_handler();
        auto frame_end = std::chrono::steady_clock::now();
        auto frame_us =
            std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start).count();
        TelemetryManager::instance().record_frame_time(static_cast<uint32_t>(frame_us));

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
            if (sleep_ms < 5)
                sleep_ms = 5;
            DisplayManager::delay(sleep_ms);
        } else {
            DisplayManager::delay(1);
        }
        } catch (const std::exception& e) {
            // A callback inside this iteration threw and was not caught
            // closer to the source. Pre-#931, this unwound through main()
            // and exited 134, triggering a watchdog crash loop. Now: log
            // type+what, dump the recent breadcrumb ring, record telemetry,
            // and continue. If catches pile up faster than RUNAWAY_THRESHOLD
            // / RUNAWAY_WINDOW_MS, exit cleanly so the watchdog sees a
            // graceful shutdown instead of an infinite throw-catch tight loop.
            const char* type_name = typeid(e).name();
            spdlog::error("[Application] Caught exception in main loop: {} ({})",
                          e.what(), type_name);
            crash_handler::breadcrumb::note("loop_catch", type_name);
            // Dump breadcrumbs so the next user log captures which observer/
            // callback path threw — root cause of #931 needs this trail.
            crash_handler::breadcrumb::dump_to_fd(STDERR_FILENO);
            try {
                TelemetryManager::instance().record_error(
                    "main_loop", "unhandled_exception", e.what());
            } catch (...) {
                // Telemetry must never re-throw out of the catch handler.
            }

            uint32_t now_tick = DisplayManager::get_ticks();
            if (exception_streak == 0 ||
                (now_tick - streak_window_start) > RUNAWAY_WINDOW_MS) {
                streak_window_start = now_tick;
                exception_streak = 1;
            } else {
                ++exception_streak;
            }
            if (exception_streak >= RUNAWAY_THRESHOLD) {
                spdlog::critical(
                    "[Application] Runaway exception streak ({} in {}ms) — "
                    "exiting main loop cleanly to break the throw-catch tight loop",
                    exception_streak, RUNAWAY_WINDOW_MS);
                break;
            }

            // Best-effort recovery toast. Wrap so a throw here doesn't escape.
            try {
                ToastManager::instance().show(
                    ToastSeverity::ERROR,
                    "An internal error occurred. The app continues running — "
                    "please send a debug bundle from Settings → About if it repeats.",
                    8000);
            } catch (...) {
                // Toast subsystem itself in trouble — keep running anyway.
            }
        } catch (...) {
            spdlog::critical("[Application] Caught non-std::exception in main loop");
            crash_handler::breadcrumb::note("loop_catch", "non_std");
            crash_handler::breadcrumb::dump_to_fd(STDERR_FILENO);
            // Non-std exceptions are vanishingly rare and usually indicate
            // ABI breakage — bail cleanly rather than risk corruption.
            break;
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

void Application::on_enter_background() {
    if (m_backgrounded)
        return;
    m_backgrounded = true;
    spdlog::info("[Application] Pausing for background");

    // 1. Disconnect WebSocket (stops all status updates and reconnect timer)
    if (m_moonraker) {
        m_moonraker->client()->disconnect();
    }

    // 2. Mute sound
    SoundManager::instance().shutdown();

    // 3. Suppress rendering — save CPU/GPU
    lv_display_enable_invalidation(nullptr, false);

    spdlog::info("[Application] Background pause complete");
}

void Application::on_enter_foreground() {
    if (!m_backgrounded)
        return;
    m_backgrounded = false;
    spdlog::info("[Application] Resuming from background");

    // 1. Re-enable rendering
    lv_display_enable_invalidation(nullptr, true);

    // 2. Re-initialize sound
    SoundManager::instance().initialize();

    // 3. Reconnect WebSocket (triggers discovery + full state refresh)
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->force_reconnect();
    }

    // 4. Force full display redraw — EGL surface may have been destroyed and
    //    recreated by Android while backgrounded.  Use invalidate_all_recursive
    //    (same as post-splash handoff) because partial-render mode won't
    //    propagate a single lv_obj_invalidate() to all descendants.
    lv_obj_t* screen = lv_screen_active();
    if (screen) {
        lv_obj_update_layout(screen);
        invalidate_all_recursive(screen);
        lv_refr_now(nullptr);
    }

    spdlog::info("[Application] Foreground resume complete");
}

#ifdef HELIX_ENABLE_SCREENSAVER
void Application::show_screensaver_migration_notice_if_pending() {
    auto* cfg = Config::get_instance();
    if (!cfg)
        return;

    if (!cfg->exists("/display/screensaver_migration_notice_pending")) {
        return;
    }
    bool pending = cfg->get<bool>("/display/screensaver_migration_notice_pending", false);
    if (!pending)
        return;

    spdlog::info("[Application] Showing one-time screensaver migration notice");

    helix::ui::modal_show_alert(
        lv_tr("Screensaver disabled"),
        lv_tr("The animated screensaver has been turned off on this device to prevent "
              "it from interfering with prints. You can re-enable it in "
              "Settings > Display."),
        ModalSeverity::Info, lv_tr("OK"));

    // Clear the flag so this notice never appears again.
    cfg->set<bool>("/display/screensaver_migration_notice_pending", false);
    cfg->save();
    spdlog::info("[Screensaver] Post-migration notice shown and dismissed; "
                 "flag cleared and persisted");
}
#endif // HELIX_ENABLE_SCREENSAVER

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
                    spdlog::info(
                        "[Application] P key - creating test printer for multi-printer testing");
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

        // Android back button — pop navigation stack (overlay/modal/panel)
        // At root panel, do nothing (Android convention: don't exit on back)
        shortcuts.register_key(SDL_SCANCODE_AC_BACK, []() {
            auto& nav = NavigationManager::instance();
            if (nav.go_back()) {
                spdlog::debug("[Application] Android back button - popped navigation");
            } else {
                spdlog::trace("[Application] Android back button - at root, ignoring");
            }
        });

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

    // Register cancel callback AFTER tear_down (which clears it) but BEFORE init (which runs
    // wizard)
    set_wizard_cancel_callback([this]() { cancel_add_printer_wizard(); });

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
    //    destructors from accessing destroyed objects.
    //    Also clear SoundManager's client ref so the M300 sequencer thread
    //    won't call gcode_script() on a dangling pointer after m_moonraker.reset() (#714).
    SoundManager::instance().set_moonraker_client(nullptr);
    set_moonraker_manager(nullptr);
    set_moonraker_api(nullptr);
    set_moonraker_client(nullptr);
    set_print_history_manager(nullptr);
    set_temperature_history_manager(nullptr);

    // 2. Deactivate overlays and clear navigation registries
    NavigationManager::instance().shutdown();

    // 3. Stop UpdateChecker auto-check timer (fires API calls on background thread)
    UpdateChecker::instance().stop_auto_check();

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
        m_moonraker->client()->unregister_method_callback("notify_update_response",
                                                          "external_update_restart");
    }

    // 8b. Unsubscribe power device and sensor state
    if (m_moonraker && m_moonraker->api()) {
        helix::PowerDeviceState::instance().unsubscribe(*m_moonraker->api());
        helix::SensorState::instance().unsubscribe(*m_moonraker->api());
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

    // 13c. Stop the consumption tracker BEFORE destroying overlays — same
    //      ordering rationale as Application::shutdown() (#927). Self-registration
    //      with StaticSubjectRegistry would otherwise run stop() AFTER destroy_all(),
    //      which is the configuration that tripped a UAF on AD5X.
    helix::FilamentConsumptionTracker::instance().stop();

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

    // Clean shutdown means no crash loop -- remove the marker file
    std::filesystem::remove(crash_marker_path());

    // Crash handler stays installed through teardown so any SIGBUS/SIGSEGV
    // during widget deletion, observer cleanup, or lv_deinit still produces
    // a crash.txt. Uninstalled at the very end of shutdown(), below.

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

    // Clear SoundManager's client ref so the M300 sequencer thread
    // won't call gcode_script() on a dangling pointer (#714).
    SoundManager::instance().set_moonraker_client(nullptr);

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

    // Tear down the upgrade banner before UpdateChecker so its observers
    // release cleanly (subject-lifetime-before-observer per #705).
    UpgradeBanner::instance().shutdown();

    // Stop auto-check timer before full shutdown
    UpdateChecker::instance().stop_auto_check();
    // Shutdown UpdateChecker (cancels pending checks)
    UpdateChecker::instance().shutdown();

    // Shutdown TelemetryManager (persists queue, joins send thread)
    TelemetryManager::instance().shutdown();

    // Shutdown CrashHistory
    helix::CrashHistory::instance().shutdown();

    // Shutdown SoundManager BEFORE clearing moonraker client — the M300
    // backend's sender lambda references client_ and the sequencer thread
    // must be stopped before the client is destroyed (#714).
    SoundManager::instance().shutdown();

    // Shutdown PostOpCooldownManager (cancel pending cooldown timers)
    PostOpCooldownManager::instance().shutdown();

    // Unload plugins before destroying managers they depend on
    if (m_plugin_manager) {
        m_plugin_manager->unload_all();
        m_plugin_manager.reset();
    }

    // Reset managers in reverse order (MoonrakerManager handles print_start_collector cleanup)
    // History managers MUST be reset before moonraker (use client for unregistration).
    // JobQueueState is reset AFTER StaticSubjectRegistry::deinit_all() because it owns
    // LVGL subjects that panels still observe — destroying it early frees the subject
    // memory while panel ObserverGuards still hold observer pointers into those lists.
    m_history_manager.reset();
    m_temp_history_manager.reset();

    // Unregister timelapse event callback
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->unregister_method_callback("notify_timelapse_event",
                                                          "timelapse_state");
    }

    // Unsubscribe power device and sensor state
    if (m_moonraker && m_moonraker->api()) {
        helix::PowerDeviceState::instance().unsubscribe(*m_moonraker->api());
        helix::SensorState::instance().unsubscribe(*m_moonraker->api());
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

    // Stop the consumption tracker BEFORE destroying overlays. Bundle VHTR49QJ
    // (#927, AD5X v0.99.53) crashed in stop() when it ran AFTER destroy_all():
    // something in overlay teardown was freeing the tracker's PrinterState
    // observer struct, and the subsequent ObserverGuard::reset() dereferenced
    // freed memory. Removing observers first guarantees they're out of every
    // subject's subs_ll before any other teardown step touches LVGL. The
    // self-registration in start() is still wired up as a backstop — it runs
    // again during deinit_all() but is a no-op because observers are already
    // null after this call.
    helix::FilamentConsumptionTracker::instance().stop();

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

    // Destroy JobQueueState AFTER deinit_all() so its registered cleanup lambda runs
    // while the object is still alive. Must still be before m_moonraker.reset() so
    // client unregistration works. (Mirrors soft-restart path ordering.)
    m_job_queue_state.reset();

    // Destroy runtime CJK fonts before LVGL shutdown
    helix::system::CjkFontManager::instance().shutdown();

    // Deinitialize theme manager subjects (theme_changed_subject, swatch descriptions).
    // These are file-scope statics not tracked by StaticSubjectRegistry.
    theme_manager_deinit();

    // Invalidate all ObserverGuards so any reset() call in surviving destructors
    // releases instead of calling lv_observer_remove() on freed observer pointers.
    // CRITICAL: lv_subject_deinit() (called via deinit_all() above) iterates
    // subs_ll and calls lv_observer_remove() on EACH observer, FREEING each one.
    // Without this guard, MoonrakerManager's ObserverGuard members destruct below
    // and call lv_observer_remove(observer_) on freed memory → SIGSEGV at
    // lv_observer.c:584 in lv_ll_remove(&observer->subject->subs_ll, observer).
    // This UAF chain is the L081 family seen in #888 (Snapmaker U1), #891 (AD5X),
    // and #893 (Pi). teardown_printer_state() has carried this guard since #816/#673;
    // the global shutdown path was missing it.
    ObserverGuard::invalidate_all();

    // Destroy MoonrakerManager (its ObserverGuards now release without
    // touching freed observer memory thanks to invalidate_all() above).
    m_moonraker.reset();

    // MoonrakerManager is gone, so no code path can submit new HTTP work.
    // Stop the executors — drains the currently-executing item and breaks
    // promises on anything still queued.
    helix::http::HttpExecutor::stop_all();

    // Shutdown display (calls lv_deinit). All observer callbacks were already
    // removed above, so widget deletion is clean — no observer linked list access.
    m_display.reset();

    // Uninstall crash handler last — clean shutdown reached this point, so a
    // SIGBUS/SIGSEGV after this is the kernel's problem, not ours.
    crash_handler::uninstall();

    spdlog::info("[Application] Shutdown complete");
}
