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

#include "detect_printer_cmd.h"
#include "discovery_steps.h"
#include "env_knobs.h"

// Private LVGL header needed to read display->flush_cb for splash no-op swap
#include "ui_overlay_timelapse_videos.h"
#include "ui_update_queue.h"

#include "ams_backend_cfs.h"
#include "ams_error_bridge.h"
#include "app_constants.h"
#include "asset_manager.h"
#include "cjk_font_manager.h"
#include "config.h"
#include "display/lv_display_private.h"
#include "display_manager.h"
#include "env_refusal_notice.h"
#include "environment_config.h"
#include "gcode_response_routing.h"
#include "hardware_fingerprint.h"
#include "hardware_role_registry.h"
#include "hardware_validator.h"
#include "helix_version.h"
#include "http_executor.h"
#include "input_settings_manager.h"
#include "job_queue_state.h"
#include "keyboard_shortcuts.h"
#include "lan_client_auth_router.h"
#include "layout_manager.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "light_button_config.h"
#include "moonraker_manager.h"
#include "page_scroll_auto_inject.h"
#include "panel_factory.h"
#include "panel_widget_manager.h"
#include "pending_startup_warnings.h"
#include "power_device_state.h"
#include "print_history_manager.h"
#include "printer_cache_registry.h"
#include "printer_recovery_service.h"
#include "printer_session.h"
#include "process_guards.h"
#ifdef HELIX_HAS_PWM_SOUND
#include "pwm_sound_backend.h"
#endif
#include "recovery_modal_presenter.h"
#include "refresh_period_hold.h"
#ifdef HELIX_ENABLE_REMOTE_CONTROL
#include "remote_control_server.h"
#endif
#include "audio_settings_manager.h"
#include "rpc_error_correlation.h"
#include "screenshot.h"
#include "sensor_state.h"
#include "sound_manager.h"
#include "spoolman_active_spool_sync.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "subject_initializer.h"
#include "temp_graph_controller.h"
#include "temperature_history_manager.h"
#include "thermal_rate_model.h"
#include "timelapse_state.h"
#include "timezone_env.h"
#include "translation_loader.h"
#include "wizard_config_paths.h"

// UI headers
#include "ui_ams_environment_overlay.h"
#include "ui_ams_loading_error_modal.h"
#include "ui_ams_mini_status.h"
#include "ui_ams_tool_text.h"
#include "ui_bed_mesh.h"
#include "ui_card.h"
#include "ui_component_header_bar.h"
#include "ui_crash_report_modal.h"
#include "ui_dialog.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_gcode_viewer.h"
#include "ui_gradient_canvas.h"
#include "ui_icon.h"
#include "ui_icon_loader.h"
#include "ui_keyboard_manager.h"
#include "ui_language_refresh.h"
#include "ui_lock_screen.h"
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
#include "ui_preflight_check_modal.h"
#include "ui_print_tune_overlay.h"
#include "ui_printer_status_icon.h"
#include "ui_probe_overlay.h"
#include "ui_runout_guidance_modal.h"
#include "ui_settings_about.h"
#include "ui_settings_barcode_scanner.h"
#include "ui_settings_fans.h"
#include "ui_settings_hardware_health.h"
#include "ui_settings_label_printer.h"
#include "ui_settings_security.h"
#include "ui_settings_sensors.h"
#include "ui_severity_card.h"
#include "ui_spaghetti_detection_modal.h"
#include "ui_status_pill.h"
#include "ui_switch.h"
#include "ui_temp_display.h"
#include "ui_theme_editor_overlay.h"
#include "ui_tile_rung.h"
#include "ui_toast_manager.h"
#include "ui_touch_calibration_overlay.h"
#include "ui_utils.h"
#include "ui_wizard.h"
#include "ui_wizard_ams_identify.h"
#include "ui_wizard_language_chooser.h"
#include "ui_wizard_touch_calibration.h"
#include "ui_wizard_wifi.h"

#include "color_utils.h"
#include "preflight_validator.h"
#include "ui/ui_widget_helpers.h"

// Developer-only showcase panel (ENABLE_DEV_PANELS, excluded from release
// builds). Not wired into PanelFactory — kept as a live testbed for icon-font
// coverage.
#ifdef HELIX_ENABLE_DEV_PANELS
#include "ui_panel_glyphs.h"
#endif

#include "active_print_media_manager.h"
#include "android_asset_extractor.h"
#include "data_root_resolver.h"
#include "display_settings_manager.h"
#include "helix_sparkline.h"
#include "setting_group.h"
#include "temperature_service.h"
#ifdef HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif
#include "display_metrics.h"
#include "k2_stock_detection_source.h"
#include "led/ui_led_control_overlay.h"
#include "platform_info.h"
#include "printer_detector.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "system/afc_message_dedup.h"
#include "system/config_trust.h"
#include "system/crash_handler.h"
#include "system/crash_history.h"
#include "system/crash_reporter.h"
#include "system/diagnostics.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "u1_stock_detection_source.h"
#include "upgrade_banner.h"
#include "wifi_link_monitor.h"
#include "wifi_manager.h"

// Backend headers
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "app_globals.h"
#include "detection_manager.h"
#include "filament_sensor_manager.h"
#include "gcode_file_modifier.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_translation.h"
#include "hv/hlog.h" // libhv logging - sync level with spdlog
#include "json_utils.h"
#include "logging_init.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "lvgl_log_handler.h"
#include "main_loop_heartbeat.h"
#include "memory_monitor.h"
#include "memory_profiling.h"
#include "memory_utils.h"
#include "mock_performance_source.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_performance_source.h"
#include "performance_state.h"
#if HELIX_HAS_PLUGINS
#include "plugin_dir_watcher.h"
#include "plugin_host.h"
#include "plugin_source_app.h"
#endif
#include "platform_table.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "splash_screen.h"
#include "standard_macros.h"
#include "tips_manager.h"
#include "tool_state.h"
#include "xml_registration.h"
#include "z_offset_persistence.h"

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>
#include <spdlog/spdlog.h>

#include "hv/json.hpp"

#ifdef HELIX_DISPLAY_SDL
#include <SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sys/file.h>
#include <sys/stat.h>
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

// SIGUSR1: remote screenshot trigger. Handler is signal-safe; main loop polls.
std::atomic<bool> s_screenshot_requested{false};

bool s_safe_mode_active = false;

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
 * lost by skipping the explicit flush. The exception is plugin storage, which
 * is written up to 500 ms after a change: a SIGTERM inside that window drops
 * the plugin's latest helix.storage.set calls.
 *
 * Async-signal-safe: only write(2) and _exit(2) used — no spdlog.
 */
void graceful_quit_signal_handler(int sig) {
    if (sig == SIGTERM) {
        // A supervisor stop is not a crash. Because this path skips
        // Application::shutdown() — the only other place the crash-restart
        // marker is cleared — every SIGTERM used to leave its start timestamp
        // behind. On the Elegoo CC1, COSMOS's resonance-calibration macro
        // stops and starts the UI through gui-switcher (SIGTERM by pidfile);
        // three of those inside the 120s window tripped "Crash loop detected"
        // and HelixScreen refused to boot. unlink(2) is async-signal-safe;
        // the path was cached at startup so nothing is constructed here.
        helix::clear_crash_marker_signal_safe();
#ifdef HELIX_HAS_PWM_SOUND
        PWMSoundBackend::silence_signal_safe();
#endif
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

Application::Application()
    : m_session(m_config, m_async_lifetime, m_screen,
                {m_args, m_shutdown_complete, m_wizard_active, m_upgrade_banner,
                 [this] { return run_wizard(); }, [this] { apply_startup_cli_actions(); },
                 [this] { m_splash_manager.on_discovery_complete(); }}) {}

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

    // Phase 1: Parse command line args
    if (!parse_args(argc, argv)) {
        return 0; // Help shown or parse error
    }

    // Prevent multiple instances from running simultaneously.
    // Two instances fighting for DRM causes 100% CPU (flush retry loop) and segfaults.
    //
    // Claimed after arg parsing so the flags that print and exit (-V/--version,
    // -h/--help) still work on a device where helix-screen is already running —
    // taking the lock first made them fail with "another instance is running",
    // which is exactly when you most want to ask the binary its version.
    if (!acquire_instance_lock()) {
        return 1;
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
    if (helix::env_flag("HELIX_CRASH_TEST")) {
        crash_handler::trigger_test_crash();
    }

    // Install graceful shutdown signal handlers (Ctrl+C, kill)
    // When running under the watchdog, SIGINT/SIGTERM are handled there.
    // When running standalone (e.g., --test), these allow clean shutdown.
    std::signal(SIGINT, graceful_quit_signal_handler);
    std::signal(SIGTERM, graceful_quit_signal_handler);

    // SIGUSR1: trigger save_screenshot from the main loop. Useful for remote
    // debugging on touch-only devices (Snapmaker U1, K1, K2) where the 'S'
    // keyboard shortcut isn't reachable. The signal handler only flips an
    // atomic — actual capture happens on the main thread next tick.
    std::signal(SIGUSR1, [](int) { s_screenshot_requested.store(true); });

    // Phase 2: Initialize config system
    if (!init_config()) {
        return 1;
    }

    // Tell the user when the launcher refused helixscreen.env: without this
    // the display comes up on defaults and the only trace is a log line
    // (prestonbrown/helixscreen#1712). No-op when the launcher exported
    // neither handoff variable (dev runs, clean file).
    helix::surface_env_refusal_from_launcher();

    // Snapshot the marker path for the SIGTERM handler. init_config() has run,
    // so writable_path() now resolves; the handler cannot construct this itself.
    helix::cache_crash_marker_path_for_signal(crash_marker_path());

    // Crash loop detection: track rapid restarts via marker file. Skipped in
    // test mode — automation (screenshot pipeline, helixctl-driven runs) relaunches
    // the binary rapidly by design, and this guard exists to protect users on a
    // real device from an infinite restart loop, never a dev running --test.
    // A loop boots this run in crash-loop safe mode.
    helix::crash_loop_detected_and_record();

    helix::promote_surviving_gpu_guards();

    // Before Phase 3: a platform state-root rename has to precede logging,
    // which opens HELIX_LOG_FILE — a path a pre-rename platform hook still
    // exports from the old root. Skipped in test mode, where the harness owns
    // the filesystem layout.
    if (!get_runtime_config()->is_test_mode()) {
        helix::migrate_legacy_state_roots();
    }

    // Phase 3: Initialize logging
    if (!init_logging()) {
        return 1;
    }

    spdlog::info("[Application] ========================");
    spdlog::info("[Application] HelixScreen {} ({})", helix_version(), helix_git_hash());

    // Every path this process uses is chosen by a cascade with a user override
    // on every rung, so the log has to carry the resolved values: a support case
    // on an overridden box cannot be answered from the rules alone.
    helix::diagnostics::log_diagnostics();
    spdlog::debug("[Application] Target: {}x{}", m_screen_width, m_screen_height);
    spdlog::debug("[Application] DPI: {}{}", (m_args.dpi > 0 ? m_args.dpi : LV_DPI_DEF),
                  (m_args.dpi > 0 ? " (custom)" : " (default)"));

    // Volunteer as the kernel's first OOM victim when helix-launcher.sh found
    // Klipper co-hosted on this board. No-op when the variable is unset.
    //
    // Placed after init_logging() rather than at the top of run(): anything
    // logged before Phase 3 is dropped, and on a printer this line is the only
    // way to confirm the handoff happened at all. Still ahead of LVGL, the
    // display, the printer database, and every large allocation.
    helix::apply_oom_score_adj_from_env();

    // Headless one-shot: detect printer via Moonraker REST, print JSON verdict, exit.
    // Must run after logging init but before any display/LVGL init.
    if (m_args.detect_printer) {
        return helix::detect::run_detect_printer(m_args.detect_host, m_args.detect_port);
    }

    // Read and consume the Safe Mode marker before any subscription-triggering
    // code runs. The watchdog writes this when it detects a deterministic
    // crash loop; reading it here lets m_session.connect_moonraker() (Phase 9d) skip the
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

    // Reclaim cache directories an older layout left on the wrong filesystem.
    //
    // Here, not later: the sweep must run before anything reaches
    // get_thumbnail_cache(), whose singleton latches its directory on first use.
    // Skipped in test mode, where the harness pins the cache into a sandbox and
    // every other rung would look stale by construction.
    if (!get_runtime_config()->is_test_mode()) {
        const int reclaimed = sweep_stale_helix_cache_dirs();
        if (reclaimed > 0) {
            spdlog::info("[Application] Reclaimed {} stale cache director(ies)", reclaimed);
        }
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
    if (!m_session.init_core_subjects()) {
        shutdown();
        return 1;
    }

    // init_display() forces the backlight to 100% so the panel is visible before
    // any setting is loaded; the saved brightness exists only from here on.
    m_display->ensure_display_on();

    get_printer_state().set_active_printer_name(m_config->get_active_printer_name());

    // Phase 9b: Initialize Moonraker (creates client + API)
    // Now works because PrinterState exists from phase 9a.
    // Start HTTP executors first — the Moonraker APIs submit to them on
    // every request. Two lanes (fast for REST, slow for file transfers)
    // prevent a large upload from head-of-line blocking status polls.
    helix::http::HttpExecutor::start_all();
    if (!m_session.init_moonraker()) {
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
    m_upgrade_banner.init();

    // Initialize CrashReporter (independent of telemetry)
    // Write mock crash file first if --mock-crash flag is set (requires --test)
    const std::string user_config_dir = helix::get_user_config_dir();
    if (get_runtime_config()->mock_crash) {
        crash_handler::write_mock_crash_file(user_config_dir + "/crash.txt");
        spdlog::info("[Application] Wrote mock crash file for testing");
    }
    helix::CrashHistory::instance().init(user_config_dir);
    m_crash_reporter.init(user_config_dir);
    // Cross-session seed for AFC's latched message dedup (uninitialized
    // before this point, which reads as "every message is new").
    AfcMessageDedup::instance().init(user_config_dir);

    // Initialize TelemetryManager (opt-in, default OFF)
    // Note: record_session() is called after m_session.init_panel_subjects() so that
    // SettingsManager subjects are ready and the enabled state can be synced.
    TelemetryManager::instance().init(user_config_dir);

    // First heap snapshot, before panel construction and XML load. Later
    // snapshots are diffed against this to narrow down which startup phase
    // burns allocator arena on small-RAM devices.
    TelemetryManager::instance().record_memory_snapshot("post_telemetry_init");

    // Phase 9c: Initialize panel subjects with API injection
    // Panels receive API at construction - no deferred set_api() needed
    if (!m_session.init_panel_subjects()) {
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
    } else if (!m_session.connect_moonraker()) {
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

    // Backend is now picked: seed the backend subjects so the Sound overlay's
    // device-row and Test Tracker bindings resolve correctly. Subjects init
    // before SoundManager, so the values are stale until this refresh.
    AudioSettingsManager::instance().refresh_backend_subjects();

    // Show sound settings immediately if a local backend exists,
    // without waiting for hardware discovery / Klipper connection.
    if (SoundManager::instance().has_backend()) {
        get_printer_state().capabilities_state().set_sound_backend_available(true);
    }

    // --test fails loudly where the XML and the C++ disagree (a required
    // widget missing from its component), as the unit tests do.
    helix::ui::set_strict_ui_checks(get_runtime_config()->is_test_mode());

    // Phase 10: Create UI and wire panels
    if (!m_session.init_ui()) {
        shutdown();
        return 1;
    }

    // Post-UI safety net: phases 11-16b run finalize_setup,
    // overlay construction, and the first synchronous render. Any std::exception
    // escaping here unwinds out of run() into main()'s catch and exits 134,
    // which the watchdog interprets as a deterministic crash and (after
    // CRASH_LOOP_MAX_CRASHES) shows the recovery dialog. main_loop()'s own
    // crash guard wraps its iterations but does not cover this pre-loop
    // window — a follow_overlay regression can hit exactly here, in
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
        if (show_crash_dialog && m_crash_reporter.has_crash_report()) {
            if (TelemetryManager::instance().had_update_restart()) {
                spdlog::info(
                    "[Application] Crash from post-update restart, suppressing crash dialog");
                m_crash_reporter.consume_crash_file();
            } else {
                auto report = m_crash_reporter.collect_report();
                if (report.signal_name.empty()) {
                    // Empty signal_name means read_crash_file() returned null because
                    // the file lacked the required signal/name fields — typically a
                    // signal handler killed mid-write (OOM-killer, watchdog, power
                    // loss). Showing a dialog here just lets the user submit a
                    // useless bundle (see CHUQCNAE 2026-05-05).
                    spdlog::warn(
                        "[Application] Crash file unparseable — consuming and skipping dialog");
                    m_crash_reporter.consume_crash_file();
                } else if (m_crash_reporter.is_duplicate(report)) {
                    spdlog::info("[Application] Duplicate crash ({}), suppressing dialog",
                                 CrashReporter::fingerprint(report));
                    m_crash_reporter.consume_crash_file();
                } else {
                    spdlog::info(
                        "[Application] Previous crash detected — showing crash report dialog");
                    CrashReportModal::show_owned(m_crash_reporter, report);
                }
            }
        }

        // Register wizard completion callback for add-printer recovery
        set_wizard_completion_callback([this]() {
            if (!m_session.wizard_previous_printer_id().empty()) {
                spdlog::info(
                    "[Application] Wizard completed — clearing add-printer recovery state");
                m_session.clear_wizard_previous_printer_id();
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
                bool completed =
                    m_config->get<bool>("/printers/" + id + "/wizard_completed", false);
                if (completed) {
                    fallback_id = id;
                    break;
                }
            }

            if (!fallback_id.empty()) {
                spdlog::info("[Application] Recovering from stale printer '{}' — "
                             "switching to completed printer '{}'",
                             active_id, fallback_id);
                // Archive rather than erase: the user never asked for this
                // deletion, and a wizard_completed flag that got cleared by
                // something other than a real interrupted setup would otherwise
                // silently destroy a fully configured printer.
                m_config->archive_printer(active_id);
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

        // Phase 13: Apply startup CLI actions (if not in wizard)
        if (!m_wizard_active) {
            apply_startup_cli_actions();
            // No wizard will run — finalize the home panel immediately so its
            // default layout reflects currently-connected hardware.
            get_global_home_panel().finalize_setup();
        }

        // Phase 14: Load plugins (HELIX_PLUGIN_DIR for authors, otherwise the
        // per-printer cache of the Moonraker plugin folder, synced on connect)
#if HELIX_HAS_PLUGINS
        m_session.init_plugins();
#endif

        // Banner: Safe Mode — UI is up, so this is the earliest the user can
        // see why the printer connection didn't come up. Sticky (no auto-dismiss)
        // because the user needs to act on it before the connection comes back.
        if (s_safe_mode_active) {
            ToastManager::instance().show(
                ToastSeverity::WARNING,
                lv_tr("Safe Mode active. The printer connection is disabled because the app "
                      "kept crashing on startup. Open Settings to fix the issue, then reboot."),
                0 /* sticky */);
        }
        if (get_runtime_config()->crash_loop_safe_mode) {
            ToastManager::instance().show(
                ToastSeverity::WARNING,
                lv_tr("Safe mode: the app kept crashing on startup, so plugins are off and "
                      "widget layouts show their defaults. Restart to return to normal."),
                0 /* sticky */);
        }

        // Phase 14b: Check WiFi availability if expected
        check_wifi_availability();

        // Phase 14c: Start remote control server (dev/test builds only)
        // Auto-enabled in --test mode, opt-in via --remote otherwise
#ifdef HELIX_ENABLE_REMOTE_CONTROL
        if (m_args.remote_control || get_runtime_config()->test_mode) {
            helix::RemoteConfig rc;
            if (m_args.remote_transport == "http") {
                rc.transport = helix::RemoteConfig::Transport::Http;
                rc.http_bind = m_args.remote_http_bind;
                rc.http_port = m_args.remote_http_port;
                // Read from the environment rather than a flag: argv is world
                // readable through /proc, so a token there leaks to every local
                // user. An off-box bind without one is refused in
                // HttpTransport::create_listener().
                if (const char* tok = getenv("HELIX_REMOTE_HTTP_TOKEN")) {
                    rc.http_token = tok;
                }
            } else {
                rc.transport = helix::RemoteConfig::Transport::UnixSocket;
                rc.socket_path = helix::resolve_socket_path(m_args.remote_socket);
            }
            if (!m_remote_control.start(rc)) {
                // Name the target and say what the user will see instead. A bare
                // "failed to start" sends people back to the flag they already
                // set, because `ctl` reports only that it found no instance.
                const std::string target = rc.transport == helix::RemoteConfig::Transport::Http
                                               ? rc.http_bind + ":" + std::to_string(rc.http_port)
                                               : rc.socket_path;
                spdlog::error("[Application] Remote control was requested but did not start on "
                              "{}; `ctl` will report that no instance is running",
                              target);
            }
        }
#else
        // Nothing in src/remote/ is compiled in, so there is no server to name and
        // no `ctl` to warn about it before the request even reaches here (m_args.
        // remote_control can never be true either, since the flag parsing that
        // sets it is gated on the same define).
        if (m_args.remote_control || get_runtime_config()->test_mode) {
            spdlog::error("[Application] Remote control was requested, but this build has no "
                          "remote-control server (rebuild with ENABLE_REMOTE_CONTROL=yes); "
                          "`ctl` will report that no instance is running");
        }
#endif

        // Phase 15: Start memory monitoring (logs at TRACE level, -vvv)
        helix::MemoryMonitor::instance().start(5000);
        helix::MemoryMonitor::instance().set_warning_callback(
            [](const helix::MemoryWarningEvent& event) {
                TelemetryManager::instance().record_memory_warning(event);
            });

        // Main-loop hang detection. A deadlocked UI thread leaves the process
        // alive and the screen lit, so the watchdog (which only supervises exit)
        // cannot see it and the user just gets a panel that ignores touch.
        //
        // Detection only for now — this reports and does not kill. The abort
        // lives behind one guarded call site below so turning it on later is a
        // single change rather than a refactor.
        {
            uint32_t hang_ms = helix::MainLoopHangDetector::DEFAULT_THRESHOLD_MS;
            if (const char* env = std::getenv("HELIX_HANG_THRESHOLD_SEC")) {
                char* end = nullptr;
                const long secs = std::strtol(env, &end, 10);
                if (end != env && secs >= 0 && secs <= 3600) {
                    hang_ms = static_cast<uint32_t>(secs) * 1000u;
                    spdlog::info("[Application] Main-loop hang threshold overridden to {}s{}", secs,
                                 secs == 0 ? " (disabled)" : "");
                } else {
                    spdlog::warn("[Application] Ignoring bad HELIX_HANG_THRESHOLD_SEC='{}' "
                                 "(want 0-3600)",
                                 env);
                }
            }
            helix::MemoryMonitor::instance().set_hang_threshold_ms(hang_ms);
        }
        helix::MemoryMonitor::instance().set_hang_callback([](uint32_t stalled_ms) {
            // Runs on the monitor thread. record_error() is documented as safe
            // from background threads, and deliberately so here: the UI thread
            // is the thing that is wedged, so anything routed through
            // UpdateQueue would never be delivered.
            TelemetryManager::instance().record_error(
                "ui", "main_loop_hang", fmt::format("stalled_{}s", stalled_ms / 1000));
            crash_handler::breadcrumb::note("main_loop", "hang");
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
                    helix::ui::queue_update("Application::run", []() {
                        spdlog::warn("[Application] Pressure response: dropping LVGL image cache");
                        crash_handler::breadcrumb::note("lvgl_imgcache", "drop");
                        lv_image_cache_drop(nullptr);
                    });
                }
            });

        // Drop all live G-code viewer state on critical pressure. ParsedGCodeFile
        // + GPU geometry can easily run hundreds of MB; on devices that nominally
        // have plenty of RAM but accumulate process RSS (telemetry: pi32 held
        // 632MB through and post-print), this is the largest single reclamation
        // available. Each viewer's clear callback (installed by the owning panel)
        // also flips the panel's mode subject back to thumbnail so the user sees
        // the slicer preview rather than a blank rectangle.
        helix::MemoryMonitor::instance().add_pressure_responder(
            [](helix::MemoryPressureLevel level) {
                if (level >= helix::MemoryPressureLevel::critical) {
                    helix::ui::queue_update("Application::run", []() {
                        crash_handler::breadcrumb::note("gcode_viewer", "pressure_clear");
                        ui_gcode_viewer_clear_all_active();
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
            helix::ui::invalidate_all_recursive(m_screen);
            lv_refr_now(nullptr);

            // Deferred refresh: Some widgets (nav icons, printer image) may not have their
            // content fully set until after the first frame. Schedule a second refresh.
            static auto deferred_refresh_cb = [](lv_timer_t* timer) {
                lv_obj_t* screen = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
                if (screen) {
                    lv_obj_update_layout(screen);
                    helix::ui::invalidate_all_recursive(screen);
                    lv_refr_now(nullptr);
                }
                lv_timer_delete(timer);
            };
            lv_timer_create(deferred_refresh_cb, 100, m_screen); // 100ms delay
        }

    } catch (const std::exception& e) {
        const std::string type_name = crash_handler::current_exception_type_name();
        spdlog::error("[Application] Caught exception during post-UI init: {} ({})", e.what(),
                      type_name);
        crash_handler::breadcrumb::note("post_init_catch", type_name.c_str());
        crash_handler::breadcrumb::dump_to_fd(STDERR_FILENO);
        try {
            TelemetryManager::instance().record_error("post_init", "unhandled_exception", e.what());
        } catch (...) {
            // Telemetry must never re-throw out of the catch handler.
        }
        try {
            ToastManager::instance().show(
                ToastSeverity::ERROR,
                lv_tr("App startup encountered an error. Some features may be unavailable."),
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

    // Apply environment variable overrides using type-safe EnvironmentConfig
    using EnvConfig = helix::config::EnvironmentConfig;

    // HELIX_SCREEN_SIZE: screen size override (alternative to -s flag)
    // Only applies if -s was not passed on the command line
    if (m_screen_width == 0 && m_screen_height == 0) {
        if (auto size_str = EnvConfig::get_screen_size()) {
            if (helix::parse_screen_size_string(size_str->c_str(), m_screen_width,
                                                m_screen_height)) {
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

    // Load persisted thermal heating rates so estimates are available immediately
    ThermalRateManager::instance().load_from_config(*m_config);

    return true;
}

bool Application::init_logging() {
    using namespace helix::logging;

    // Apply the configured timezone BEFORE the first timestamped line. It is
    // applied again later by DisplaySettingsManager::init_subjects() (which owns
    // the setting and its subject) — idempotent, and by then the value already
    // matches. Without this the log's wall clock jumps mid-startup on any device
    // whose configured zone differs from the host's, which reads as a stall in a
    // debug bundle (#1218: a 5-hour jump inside a single startup sequence).
    // Unknown zones are left to glibc, which treats an unparseable TZ as UTC —
    // the same fallback DisplaySettingsManager applies.
    helix::timezone_env::apply(m_config->get<std::string>("/display/timezone", "UTC").c_str());

    LogConfig log_config;

    // HELIX_LOG_* is read HERE, not only translated by scripts/helix-launcher.sh
    // into --log-dest/--log-file/--log-level. The launcher is not always in the
    // picture: a systemd unit with Environment=, a hand-run binary over SSH, and
    // third-party init scripts (ZMOD ships its own fork of ours) all start
    // helix-screen directly, and until now every HELIX_LOG_* they exported was
    // silently ignored — the variables looked configurable and were not (#1249).
    // The launcher's flags still win, because a CLI flag outranks the env below.
    const std::string env_log_dest =
        log_env_override("HELIX_LOG_DEST", &is_valid_log_target, log_target_accepted_values());
    const std::string env_log_level =
        log_env_override("HELIX_LOG_LEVEL", &is_valid_log_level, log_level_accepted_values());
    // No validity predicate for a path: any string is a candidate, and an
    // unopenable one degrades to the platform's normal sink inside init().
    const std::string env_log_file = log_env_override("HELIX_LOG_FILE", nullptr, nullptr);

    // Resolve log level: --log-level > HELIX_LOG_LEVEL > -v flags > config file > defaults
    std::string config_level = m_config->get<std::string>("/log_level", "");
    if (!g_log_level_cli.empty()) {
        log_config.level = parse_level(g_log_level_cli, spdlog::level::warn);
    } else if (!env_log_level.empty()) {
        log_config.level = parse_level(env_log_level, spdlog::level::warn);
    } else {
        log_config.level =
            resolve_log_level(m_args.verbosity, config_level, get_runtime_config()->test_mode);
    }

    // An explicit -v/--log-level means the user asked to watch the logs, so attach
    // the console sink even when stdout is a pipe. Without this, `helix-screen -vv |
    // tee run.log` on any box with a systemd journal socket produces no output at
    // all — auto-detection picks the Journal target, whose console gate is
    // isatty(stdout). A bare run with no flag keeps the journal-only behavior.
    //
    // HELIX_LOG_LEVEL counts as explicit for the same reason the flag does: the
    // launcher already turns that variable into --log-level=, so it has ALWAYS
    // set force_console on a launcher-started device. Treating the direct-env
    // path differently would make the same helixscreen.env behave one way under
    // the launcher and another under systemd/a forked init script, which is the
    // exact inconsistency this block exists to remove. The blast radius is
    // bounded: force_console only adds a sink for a PIPE — should_add_console()
    // still refuses a regular file or socket, which is where the daemon
    // double-log (the Snapmaker U1 tmpfs blowout) came from.
    log_config.force_console =
        m_args.verbosity > 0 || !g_log_level_cli.empty() || !env_log_level.empty();

    // --test always gets a console sink, whatever stdout is (pipe, file, socket).
    // Read here rather than inside logging_init.cpp because that TU is linked into
    // the watchdog build, which does not link runtime_config.o.
    log_config.test_mode = get_runtime_config()->test_mode;

    // Resolve log destination: CLI > HELIX_LOG_DEST > config > auto
    log_config.target = parse_log_target(resolve_log_setting(
        g_log_dest_cli, env_log_dest, m_config->get<std::string>("/log_dest", "auto")));

    // Resolve log file path: CLI > HELIX_LOG_FILE > config. A config-sourced
    // path must pass the same confinement the launcher applies to
    // HELIX_LOG_FILE (see helix::config_trust::log_path_allowed): settings.json
    // is web-writable and must not aim a root-written log at arbitrary files.
    // A refused path falls through to CLI/env, then the default location.
    std::string config_log_path = m_config->get<std::string>("/log_path", "");
    if (!config_log_path.empty() && !helix::config_trust::log_path_allowed(config_log_path)) {
        spdlog::warn("[Application] /log_path '{}' refused (must be a *.log file under /tmp, "
                     "/var/log or the install dir, no .. , not a symlink) - using the default",
                     config_log_path);
        config_log_path.clear();
    }
    log_config.file_path = resolve_log_setting(g_log_file_cli, env_log_file, config_log_path);

    init(log_config);

    // Set libhv log level from config (CLI -v flags don't affect libhv)
    spdlog::level::level_enum hv_spdlog_level = parse_level(config_level, spdlog::level::warn);
    hlog_set_level(libhv_level_for(hv_spdlog_level));

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
    // The first-boot probe's kernel panel_orientation is applied here, as the
    // configured rotation, so it is in place before the backends create their
    // input devices and gate the stored touch range on it (#1428).
    m_rotation_probe_wanted = rotation_probe_wanted();
    m_kernel_orientation =
        m_rotation_probe_wanted ? DisplayBackend::detect_panel_orientation() : -1;
    config.rotation =
        helix::startup_rotation(m_args.rotation, m_rotation_probe_wanted, m_kernel_orientation);
    config.size_was_explicit = m_args.size_was_explicit;

    // Get scroll config from settings.json
    config.scroll_throw =
        m_config->get<int>("/input/scroll_throw", InputSettingsManager::DEFAULT_SCROLL_THROW);
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

    // Reconnect the WebSocket when the display wakes from sleep. The app
    // background/foreground path (on_enter_foreground) already force-reconnects,
    // but on Android the SDL background/foreground event pair is unreliable for
    // the display-off/on round trip — the Activity may not get a clean
    // onPause/onResume, so m_backgrounded never flips and the reconnect is
    // skipped. This sleep callback closes that gap (#1245).
    //
    // Android ONLY, deliberately. force_reconnect() tears the socket down and
    // rebuilds it synchronously on whatever thread calls it, and this callback
    // runs on the UI thread. The Linux fbdev/DRM fleet never backgrounds the
    // process on display sleep — the connection stays up and the health timer
    // keeps running — so there is nothing to re-establish on wake, and running
    // the teardown anyway only exposes the main loop to blocking inside it.
    //
    // Registered here, alongside the DisplayManager that owns the callback list,
    // rather than in m_session.connect_moonraker(): that runs again on every printer
    // switch, and register_sleep_callback() only appends — there is no
    // unregister — so each switch would stack another copy and fire one extra
    // force_reconnect() per wake. init_display() runs once per process, and the
    // captured `this` owns m_display, so the callback list cannot outlive it.
    // m_session.moonraker() is read lazily at wake time and need not exist yet.
#ifdef __ANDROID__
    m_display->register_sleep_callback([this](bool sleeping) {
        if (!sleeping && m_session.moonraker() && m_session.moonraker()->client()) {
            // Debounce: on_enter_foreground() may have already called
            // force_reconnect for the same wake event. Skip if it ran
            // within the last 5 seconds — the second call would bump
            // the connection generation and make the first discovery's
            // subscription stale (#1245).
            auto now = std::chrono::steady_clock::now();
            if (now - m_last_force_reconnect < std::chrono::seconds(5)) {
                spdlog::debug("[Application] Display woke — skipping reconnect (debounced, "
                              "on_enter_foreground ran recently)");
                return;
            }
            spdlog::info("[Application] Display woke — reconnecting WebSocket");
            m_last_force_reconnect = now;
            m_session.moonraker()->client()->force_reconnect();
        }
    });
#endif

    // Android is the one platform whose reported DPI can be trusted: OEMs must
    // declare it and SDL reads it straight from DisplayMetrics. Every Linux
    // target is excluded on purpose — a survey of the eight test devices found
    // the reported physical size wrong or absent on six, including a Pi that
    // reports the official 7" panel's exact active area for a ~115mm screen.
    std::optional<double> measured_dpi;
#ifdef __ANDROID__
    {
        float ddpi = 0, hdpi = 0, vdpi = 0;
        if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0) {
            spdlog::info("[Application] Android display DPI: diagonal={:.0f} h={:.0f} v={:.0f}",
                         ddpi, hdpi, vdpi);
            if (ddpi > 0) {
                measured_dpi = static_cast<double>(ddpi);
            }
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
    // Resolve the panel's physical DPI and turn it into a UI scale factor. The
    // scale is flat (exactly 1.0) through 225 DPI, which covers every shipping
    // printer measured, so this is an identity everywhere except phone-class
    // panels.
    const helix::ResolvedDpi resolved = helix::DisplayMetrics::resolve_dpi(
        m_args.dpi, measured_dpi, helix::platform::current_key());
    const double auto_scale = helix::DisplayMetrics::ui_scale_for_dpi(resolved.dpi);
    helix::DisplayMetrics::set_auto_scale(auto_scale);

    // The stored UI Scale setting wins over the measurement, which is the
    // whole point of exposing it: a panel we scale wrongly, or a user who
    // simply wants larger type, has a way out that does not need a rebuild.
    // Config rather than DisplaySettingsManager because this runs inside
    // init_display(), before that manager's subjects exist — both read the
    // same key. `--dpi` stays upstream of this, overriding the measurement
    // that Automatic then follows.
    const int scale_setting = Config::get_instance()->get<int>(
        "/display/ui_scale_percent", helix::DisplayMetrics::kScaleSettingAutomatic);
    const double ui_scale = helix::DisplayMetrics::scale_for_setting(scale_setting, auto_scale);
    helix::DisplayMetrics::set_active_scale(ui_scale);

    // What LVGL gets is LV_DPI_DEF scaled by the SAME factor — never a
    // kernel-derived number. LVGL turns its DPI into padding via LV_DPX_CALC,
    // so this makes its internal chrome (and the four lv_dpx() call sites in
    // our own code) grow in step with the design tokens theme_manager scales.
    // The two paths are disjoint, so they add rather than compound. At scale
    // 1.0 this is exactly LV_DPI_DEF, i.e. byte-identical to the previous
    // behaviour, which is what keeps a lying kernel driver from reaching the
    // UI at all (a sun4i-drm CB1 reports 23 DPI and would collapse padding to
    // 1px via LV_DPX's MAX(..,1) floor).
    int32_t effective_dpi = static_cast<int32_t>(std::lround(LV_DPI_DEF * ui_scale));
    int32_t pre_set_dpi = lv_display_get_dpi(m_display->display());
    lv_display_set_dpi(m_display->display(), effective_dpi);
    spdlog::info("[Application] Display metrics: dpi={:.0f} (source={}) → ui_scale={:.3f} "
                 "(auto={:.3f}, setting={}), lvgl_dpi={} (was {} before set)",
                 resolved.dpi, helix::dpi_source_name(resolved.source), ui_scale, auto_scale,
                 scale_setting == helix::DisplayMetrics::kScaleSettingAutomatic
                     ? std::string("automatic")
                     : std::to_string(scale_setting) + "%",
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

    // Refresh theme tokens (nav_width, overlay widths, spacing) and the
    // LayoutManager state on every debounced resize.  Without this hook,
    // theme_manager_refresh_layout_constants() only fires once at startup
    // (after the rotation probe) and a runtime size change — e.g. fold/
    // unfold on a Samsung Fold or Flip — leaves the ui_breakpoint subject
    // and overlay widths stuck at startup values, and home-screen widgets
    // do not reflow (#941).  Captureless lambda — must use singletons.
    m_display->register_resize_callback([]() {
        auto* dm = DisplayManager::instance();
        if (!dm)
            return;
        lv_display_t* disp = dm->display();
        if (!disp)
            return;

        const int w = dm->width();
        const int h = dm->height();
        auto& layout = helix::LayoutManager::instance();

        // LayoutManager first: theme_manager_refresh_layout_constants() now
        // derives ui_is_portrait from LayoutManager::type() (override-aware),
        // so the type must reflect the new geometry before refresh reads it.
        // #1255.
        layout.init(w, h);
        theme_manager_refresh_layout_constants(disp);
        // Components register with the tokens and layout variant of the moment;
        // idle ones register again at the new geometry on their next use.
        helix::unregister_idle_xml_components();

        // Overlays cache their root widget across show/hide cycles, so the
        // width applied at push time goes stale when the canvas changes size
        // (e.g., Android Keep Navigation Bar pins a side bar that insets the
        // LVGL surface). NavigationManager remembers each live overlay's
        // resolved width class, so this re-derives the pixel width from the
        // constants just refreshed above (#941, #1178).
        NavigationManager::instance().reapply_overlay_widths();

        spdlog::info("[Application] Resize: refreshed theme + layout for {}x{} ({})", w, h,
                     layout.name());
    });

    // Tips are NOT loaded here. TipsManager::get_instance() parses the database
    // on first use instead, so a session that never displays the tips widget
    // never pays the 105 KB parse or keeps its cache resident.

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
        m_splash_invalidation_suppression.begin();

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
    lv_result_t globals_result = lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/globals.xml").c_str());
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

bool Application::rotation_probe_wanted() const {
    // Run rotation probe on first boot if no rotation is configured.
    // Skip if: CLI rotation set, env var set, already probed, or config already
    // has a /display/rotate key (even if 0 — means user already configured it).
    // HELIX_FORCE_ROTATION_PROBE=1 bypasses all guards (for testing on SDL).
    if (std::getenv("HELIX_FORCE_ROTATION_PROBE") != nullptr) {
        spdlog::info("[Application] Rotation probe forced via HELIX_FORCE_ROTATION_PROBE");
        return true;
    }
#if defined(HELIX_DISPLAY_FBDEV) || defined(HELIX_DISPLAY_DRM)
    if (m_args.rotation == 0 && !std::getenv("HELIX_DISPLAY_ROTATION")) {
        bool probed = m_config->get<bool>("/display/rotation_probed", false);
        bool has_rotate_key = m_config->exists("/display/rotate");
        if (probed || has_rotate_key) {
            spdlog::info("[Application] Rotation probe skipped: probed={}, has_rotate_key={}",
                         probed, has_rotate_key);
            return false;
        }
        return true;
    }
    spdlog::info("[Application] Rotation probe skipped: cli_rotation={}, env={}", m_args.rotation,
                 std::getenv("HELIX_DISPLAY_ROTATION") ? std::getenv("HELIX_DISPLAY_ROTATION")
                                                       : "unset");
    return false;
#else
    spdlog::debug("[Application] Rotation probe skipped: not embedded build");
    return false;
#endif
}

void Application::run_rotation_probe_and_layout() {
    {
        const bool should_probe = m_rotation_probe_wanted;

        if (should_probe) {
            int32_t pre_w = m_screen_width;
            int32_t pre_h = m_screen_height;

            // Kernel panel_orientation first. It is informational — the kernel
            // does NOT rotate the framebuffer — and init_display() has already
            // applied it, so only the config write remains.
            const int kernel_orientation = m_kernel_orientation;
            if (kernel_orientation >= 0) {
                // Orientation detected (0=Normal, 90, 180, 270).
                if (kernel_orientation > 0) {
                    spdlog::info("[Application] Auto-detected panel orientation: {}° — "
                                 "applied at display init, saving to config",
                                 kernel_orientation);
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
                    m_splash_invalidation_suppression.end();
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
    // LayoutManager just resolved any --layout override. Republish
    // ui_is_portrait from it so XML visual decisions match the C++ ones; the
    // startup seed (theme_manager_init) and the rotation-probe refresh both ran
    // before this point and could only see detect_layout_type(). #1255.
    theme_manager_refresh_orientation();
    spdlog::info("[Application] Layout: {} ({})", layout_mgr.name(),
                 layout_mgr.is_standard() ? "default" : "override");
}

bool Application::register_widgets() {
    helix::ui::icon::register_widget();
    helix::ui::register_tile_rung_binding();
    ui_status_pill_register_widget();
    ui_switch_register();
    ui_card_register();
    setting_group_register();
    ui_temp_display_init();
    ui_ams_mini_status_init();
    ui_severity_card_register();
    ui_dialog_register();
    ui_bed_mesh_register();
    ui_gcode_viewer_register();
    ui_gradient_canvas_register();
    helix::ui::register_helix_sparkline_widget();

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

    // Start XML hot reloader if enabled. Default ON for native (non-release)
    // builds so editing XML during dev Just Works; OFF for cross-compiled
    // release targets. Env var HELIX_HOT_RELOAD={0,1} overrides either way.
    if (RuntimeConfig::hot_reload_enabled()) {
        m_hot_reloader = std::make_unique<helix::XmlHotReloader>();
        m_hot_reloader->set_after_reload_callback([](const std::string& component) {
            if (NavigationManager::is_destroyed())
                return;
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

    // Set initial language. When no pack is loaded for a language — which is
    // the normal case for English, whose pack is skipped entirely —
    // lv_translation_get() returns the tag itself, and since our tags ARE
    // English the UI is already correct without any registered pack.
    lv_translation_set_language(lang.c_str());

    // Load CJK runtime fonts if persisted language is CJK
    helix::system::CjkFontManager::instance().on_language_changed(lang);

    // Re-enable translation warnings for runtime (post-init warnings are actionable)
    helix::logging::set_suppress_translation_warnings(false);
    spdlog::info("[Application] Language set to '{}'", lang);

    return true;
}

bool Application::run_wizard() {
    bool wizard_required =
        (m_args.force_wizard || m_config->is_wizard_required()) && !m_args.skip_wizard;

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

        // Drop preset-mode so a wrong install-time full-seed is recoverable: clearing
        // the preset marker makes has_preset() false → the re-run is a FULL wizard
        // (identify + hardware pages reappear). Also clear the host so the user
        // re-enters the connection step. See install-time-detection design.
        m_config->clear_preset();
        m_config->set<std::string>(m_config->df() + "moonraker_host", "");

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

    // Touch-calibration force must work even when the first-run wizard is
    // pending. Without this, the wizard's step-0 auto-skip (already-calibrated
    // check) wins and the request is silently ignored — the standalone
    // overlay in apply_startup_cli_actions() is unreachable while m_wizard_active is
    // true. Pin the wizard to step 0 and disable the skip so users can
    // recalibrate from a stale-affine state. Mirror the three sources the
    // standalone-overlay handler accepts: --calibrate-touch CLI, the env
    // var, and the /input/force_calibration config option.
    const char* env_force_cal = std::getenv("HELIX_TOUCH_CALIBRATE");
    bool config_force_cal = m_config && m_config->get<bool>("/input/force_calibration", false);
    if (m_args.calibrate_touch || env_force_cal != nullptr || config_force_cal) {
        force_touch_calibration_step(true);
        initial_step = 0;
        spdlog::info("[Application] Forcing wizard touch-calibration step "
                     "(calibrate_touch={}, env={}, config={})",
                     m_args.calibrate_touch, env_force_cal ? "set" : "unset", config_force_cal);
    }

    // If step 1 was explicitly requested, force-show language chooser (for visual testing)
    if (m_args.wizard_step == 1) {
        force_language_chooser_step(true);
    }

    // initial_step is a raw CLI/config int (--wizard-step) — a genuine int seam.
    // Clamp to a valid StepId range so an out-of-range debug value lands on a real
    // step (the last one) instead of a blank wizard from a bogus enum cast.
    if (initial_step < 0 || initial_step >= helix::wizard::STEP_COUNT) {
        spdlog::warn("[Application] --wizard-step {} out of range [0,{}); clamping", initial_step,
                     helix::wizard::STEP_COUNT);
        initial_step = (initial_step < 0) ? 0 : helix::wizard::STEP_COUNT - 1;
    }
    // Map it to a StepId for the registry-driven wizard.
    ui_wizard_navigate_to_step(static_cast<helix::wizard::StepId>(initial_step));

    // Move keyboard above wizard
    lv_obj_t* keyboard = KeyboardManager::instance().get_instance();
    if (keyboard) {
        lv_obj_move_foreground(keyboard);
    }

    return true;
}

// Applies one-shot startup actions requested on the command line: force touch
// calibration (--calibrate-touch / env / config), the --release-notes update
// modal, and --select-file navigation. (Historically also launched panels/overlays
// via -p, now removed — that job belongs to helixctl; see docs/devel/HELIXCTL.md.)
void Application::apply_startup_cli_actions() {
    // Force touch calibration: --calibrate-touch flag, env var, OR config force_calibration option
    bool force_touch_cal = m_args.calibrate_touch;
    if (!force_touch_cal) {
        force_touch_cal = (std::getenv("HELIX_TOUCH_CALIBRATE") != nullptr);
    }
    if (!force_touch_cal && m_config) {
        force_touch_cal = m_config->get<bool>("/input/force_calibration", false);
    }

    if (force_touch_cal) {
        auto& overlay = helix::ui::get_touch_calibration_overlay();
        // Completion callback: clear config flag on success if it was set
        bool clear_config = m_config && m_config->get<bool>("/input/force_calibration", false);
        overlay.show(m_screen, [this, clear_config](bool success) {
            if (success && clear_config && m_config) {
                m_config->set<bool>("/input/force_calibration", false);
                m_config->save();
                spdlog::info("[Application] Cleared force_calibration config flag after success");
            }
        });
        spdlog::info("[Application] Opened touch calibration overlay (force={})", force_touch_cal);
    }

    // Handle --release-notes flag: fetch latest release notes and show in modal
    if (m_args.release_notes) {
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
        helix::nav::set_active(PanelId::PrintSelect);
        auto* print_panel =
            get_print_select_panel(get_printer_state(), m_session.moonraker()->api());
        if (print_panel) {
            print_panel->set_pending_file_selection(runtime_config->select_file);
        }
    }
}

void Application::restore_flush_callback() {
    if (m_original_flush_cb) {
        if (m_display) {
            m_display->restore_flush_cb(m_original_flush_cb);
        }
        m_original_flush_cb = nullptr;
    }
}

void Application::check_wifi_availability() {
    // Bring WiFi up at startup instead of waiting for a UI screen to construct
    // the manager. get_wifi_manager() creates the silent global instance, whose
    // constructor builds the backend (null when there is no hardware) and
    // start_async()es it — which is also where credentials are re-applied on
    // firmwares whose wpa_supplicant discards them.
    //
    // This used to be gated on is_wifi_expected(), which defaults to false and
    // is absent from settings.json on a Snapmaker U1 — so that device never
    // started WiFi at boot at all, and only ever connected once the user opened
    // a WiFi screen. Bringup is not a user-intent question.
    auto wifi = get_wifi_manager();

    // Link telemetry runs from boot, not only while a WiFi screen is open.
    helix::WifiLinkMonitor::instance().start();

    // wifi_expected keeps its original meaning: the user configured WiFi, so
    // tell them if the hardware has since disappeared.
    if (!m_config || !m_config->is_wifi_expected()) {
        return;
    }

    if (wifi && !wifi->has_hardware()) {
        NOTIFY_ERROR_MODAL(lv_tr("WiFi Unavailable"),
                           lv_tr("WiFi was configured but hardware is not available. "
                                 "Check system configuration."));
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
    // "HelixScreen Keeps Crashing" recovery dialog (#931). A separate safety
    // net wraps the initial-subscription dispatch path, but queued/observer/
    // timer callbacks inside lv_timer_handler are not yet guarded. Catch +
    // log + dump breadcrumbs + continue: the user gets a
    // toast and a usable app instead of a crash loop they can only escape
    // by reflashing the previous version. A streak counter breaks out if the
    // catch itself is in a tight retry loop.
    int exception_streak = 0;
    uint32_t streak_window_start = 0;
    static constexpr int RUNAWAY_THRESHOLD = 5;
    static constexpr uint32_t RUNAWAY_WINDOW_MS = 30000;

    // Re-install SIGUSR1 handler right before the main loop. SDL_Init or other
    // mid-startup library init (libdrm-ish, evdev) can reset signal handlers
    // to SIG_DFL — verified on Snapmaker U1 where the install at line ~429
    // didn't survive (SigCgt mask had USR1 bit clear post-init). Installing
    // here guarantees the handler is live for the entire run-loop lifetime.
    std::signal(SIGUSR1, [](int) { s_screenshot_requested.store(true); });

    // Main event loop
    while (lv_display_get_next(nullptr) && !app_quit_requested()) {
        try {
            uint32_t current_tick = DisplayManager::get_ticks();
            m_loop_handler.on_frame(current_tick);

            // Liveness signal. Placed at the top of the iteration and before any
            // of the work below, so it advances on every pass the loop actually
            // completes — including the backgrounded early-continue path further
            // down, which is a live loop and must not read as a hang.
            helix::MainLoopHeartbeat::beat();

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

            // SIGUSR1-triggered screenshot (remote debugging on touch-only devices).
            if (s_screenshot_requested.exchange(false)) {
                auto path = helix::save_screenshot();
                spdlog::info("[Application] SIGUSR1 screenshot saved: {}",
                             path.empty() ? "<failed>" : path.c_str());
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
                std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start)
                    .count();
            TelemetryManager::instance().record_frame_time(static_cast<uint32_t>(frame_us));

            // Task C handoff repaint: on the U1 DRM path the early fb0 splash owns
            // /dev/fb0 (the remote screen) and, once retired via SIGUSR1, no longer
            // repaints it. Force the real flush hook — which mirrors dirty rects
            // into fb0 — to paint one FULL frame BEFORE the splash is signaled to
            // exit, so the remote shows the complete UI rather than the splash's
            // frozen frame (or, on an idle UI that never dirties, black). Gated on a
            // remote sink being active so non-U1 devices skip the forced redraw;
            // ready_to_signal() makes it fire exactly once, on the frame the splash
            // is about to be retired. The mandatory order is: restore flush cb ->
            // full invalidate + lv_refr_now (mirror writes a full fb0 frame) -> THEN
            // check_and_signal (SIGUSR1 -> splash exits without clearing fb0).
            if (m_display && m_display->remote_screen_active() &&
                m_splash_manager.ready_to_signal()) {
                // If a suppressed-flush splash path was active (launcher passed
                // --splash-pid), lift suppression first so the repaint is not a
                // no-op; on the DRM watchdog path invalidation was never suppressed.
                // Ending it here also stops the post-signal handoff block below from
                // repainting a second time.
                m_splash_invalidation_suppression.end();
                restore_flush_callback(); // no-op on the DRM path (flush never swapped)
                if (lv_obj_t* screen = lv_screen_active()) {
                    lv_obj_update_layout(screen);
                    helix::ui::invalidate_all_recursive(screen);
                    lv_refr_now(nullptr);
                }
            }

            // Signal splash to exit when discovery completes (or timeout)
            m_splash_manager.check_and_signal();

            // Post-splash handoff: re-enable rendering and repaint
            // Display invalidation was suppressed to prevent framebuffer flicker
            // while both splash and main app were running simultaneously.
            if (m_splash_invalidation_suppression.active() &&
                m_splash_manager.needs_post_splash_refresh()) {
                m_splash_invalidation_suppression.end();
                restore_flush_callback();
                spdlog::info(
                    "[Application] Post-splash handoff: flush callback restored, painting UI");

                lv_obj_t* screen = lv_screen_active();
                if (screen) {
                    lv_obj_update_layout(screen);
                    helix::ui::invalidate_all_recursive(screen);
                    lv_refr_now(nullptr);
                }
                m_splash_manager.mark_refresh_done();
            }

            // Failsafe: if invalidation is still suppressed after hard deadline, force it back on.
            // Prevents permanent black screen if splash handoff fails for any reason.
            if (m_splash_invalidation_suppression.active() &&
                (current_tick - suppression_start_tick) >= INVALIDATION_FAILSAFE_MS) {
                m_splash_invalidation_suppression.end();
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
                helix::RefreshTiming timing = m_display->refresh_timing();
                // A running screensaver's refresh-period hold lowers the floor to pace its frames.
                timing.loop_min_sleep_ms =
                    helix::active_refresh_period_hold().loop_min_sleep_ms(timing.loop_min_sleep_ms);
                DisplayManager::delay(helix::main_loop_sleep_ms(
                    time_till_next, m_display->is_display_sleeping(), timing));
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
            const std::string type_name = crash_handler::current_exception_type_name();
            spdlog::error("[Application] Caught exception in main loop: {} ({})", e.what(),
                          type_name);
            crash_handler::breadcrumb::note("loop_catch", type_name.c_str());
            // Dump breadcrumbs so the next user log captures which observer/
            // callback path threw — root cause of #931 needs this trail.
            crash_handler::breadcrumb::dump_to_fd(STDERR_FILENO);
            try {
                TelemetryManager::instance().record_error("main_loop", "unhandled_exception",
                                                          e.what());
            } catch (...) {
                // Telemetry must never re-throw out of the catch handler.
            }

            uint32_t now_tick = DisplayManager::get_ticks();
            if (exception_streak == 0 || (now_tick - streak_window_start) > RUNAWAY_WINDOW_MS) {
                streak_window_start = now_tick;
                exception_streak = 1;
            } else {
                ++exception_streak;
            }
            if (exception_streak >= RUNAWAY_THRESHOLD) {
                spdlog::critical("[Application] Runaway exception streak ({} in {}ms) — "
                                 "exiting main loop cleanly to break the throw-catch tight loop",
                                 exception_streak, RUNAWAY_WINDOW_MS);
                break;
            }

            // Best-effort recovery toast. Wrap so a throw here doesn't escape.
            try {
                ToastManager::instance().show(
                    ToastSeverity::ERROR,
                    lv_tr("An internal error occurred. The app continues running. Please "
                          "send a debug bundle from Settings > Help & About if it "
                          "repeats."),
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

    // 1. Suspend the visible panel/overlay lifecycle (same hook the screensaver
    //    uses). on_deactivate() stops per-panel timers, camera streams and
    //    graph refreshes that would otherwise keep running against an LVGL
    //    thread Android has frozen. It runs FIRST, while the socket is still
    //    up and rendering is still enabled, so teardown that talks to Moonraker
    //    or touches widgets behaves exactly as it does on the sleep path.
    NavigationManager::instance().suspend_active();

    // 2. Disconnect WebSocket (stops all status updates and reconnect timer)
    if (m_session.moonraker()) {
        // Mark the disconnect as expected so the DISCONNECTED notification
        // (queued here, drained on resume) doesn't clear the overlay stack
        // and bounce the user to home (#1245).
        NavigationManager::instance().mark_disconnect_expected();
        m_session.moonraker()->client()->disconnect();
    }

    // 3. Mute sound
    SoundManager::instance().shutdown();

    // 4. Suppress rendering — save CPU/GPU
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
    if (m_session.moonraker() && m_session.moonraker()->client()) {
        m_last_force_reconnect = std::chrono::steady_clock::now();
        m_session.moonraker()->client()->force_reconnect();
    }

    // 4. Resume the visible panel/overlay lifecycle. Repainting alone only
    //    re-draws whatever the widgets already hold — on_activate() is what
    //    re-seeds subjects, re-binds observers, reloads content and restarts
    //    timers (that asymmetry is why a tab round-trip un-sticks a stale panel
    //    and a resume did not; prestonbrown/helixscreen#1245). It runs AFTER
    //    force_reconnect() so requests issued from on_activate() meet a socket
    //    that is already reconnecting rather than a definitively dead one, and
    //    BEFORE the repaint below so the forced frame paints the re-seeded UI
    //    instead of the stale one.
    NavigationManager::instance().resume_active();

    // 5. Reset LVGL's activity timestamp. Inactivity is measured off the tick,
    //    which keeps advancing while Android has us paused, so the first
    //    check_display_sleep() after resume would otherwise see the whole
    //    backgrounded interval as idle and drop straight back into sleep or the
    //    screensaver. Same idiom as DisplayManager::wake_display().
    lv_display_trigger_activity(nullptr);

    // 6. Force full display redraw — EGL surface may have been destroyed and
    //    recreated by Android while backgrounded.  Use invalidate_all_recursive
    //    (same as post-splash handoff) because partial-render mode won't
    //    propagate a single lv_obj_invalidate() to all descendants.
    lv_obj_t* screen = lv_screen_active();
    if (screen) {
        lv_obj_update_layout(screen);
        helix::ui::invalidate_all_recursive(screen);
        lv_refr_now(nullptr);
    }

    spdlog::info("[Application] Foreground resume complete");
}

#ifdef HELIX_ENABLE_SCREENSAVER
void Application::show_screensaver_migration_notice_if_pending() {
    auto* cfg = Config::get_instance();

    if (!cfg->exists("/display/screensaver_migration_notice_pending")) {
        return;
    }
    bool pending = cfg->get<bool>("/display/screensaver_migration_notice_pending", false);
    if (!pending)
        return;

    spdlog::info("[Application] Showing one-time screensaver migration notice");

    helix::ui::modal_alert(
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

void Application::process_notifications() {
    if (m_session.moonraker()) {
        m_session.moonraker()->process_notifications();
    }
}

void Application::check_timeouts() {
    uint32_t current_time = DisplayManager::get_ticks();
    if (current_time - m_last_timeout_check >= m_timeout_check_interval) {
        if (m_session.moonraker()) {
            m_session.moonraker()->process_timeouts();
        }
        m_last_timeout_check = current_time;
    }
}

// ============================================================================
// SOFT RESTART (printer switching)
// ============================================================================

void Application::shutdown() {
    // Guard against multiple calls (destructor + explicit shutdown)
    if (m_shutdown_complete) {
        return;
    }
    m_shutdown_complete = true;

    // Expire the callbacks this object deferred to the main thread before any of
    // the subsystems they touch are torn down below (#1165).
    m_async_lifetime.invalidate();
    helix::WifiLinkMonitor::instance().stop();

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

    // Stop remote control server first (before tearing down UI state)
#ifdef HELIX_ENABLE_REMOTE_CONTROL
    m_remote_control.stop();
#endif

    // Stop memory monitor
    helix::MemoryMonitor::instance().stop();

    spdlog::info("[Application] Shutting down...");

    m_session.teardown_printer_scope(helix::PrinterSession::TeardownScope::ProcessExit,
                                     m_display.get());

    // No code path can submit new HTTP work now. Stop the executors: drains the
    // currently-executing item and breaks promises on anything still queued.
    helix::http::HttpExecutor::stop_all();

    // Shutdown display (calls lv_deinit). All observer callbacks were removed above,
    // so widget deletion touches no observer linked list.
    m_display.reset();

    // Theme manager subjects (theme_changed_subject, swatch descriptions) are
    // file-scope statics not tracked by StaticSubjectRegistry, so they are torn down
    // by hand, and only HERE, after the display is gone.
    //
    // They must outlive m_display.reset(), because that is what runs lv_xml_deinit():
    // a component scope holding a <subject_expr> owns RAW lv_observer_t* pointers
    // (lv_xml_subject_expr_t::observers) attached to these subjects, and releases them
    // with lv_observer_remove(). Those are not ObserverGuards, so
    // ObserverGuard::invalidate_all() does not cover them; deinitialising the subjects
    // first frees every observer on them and the later scope teardown reads freed
    // memory.
    //
    // Running last is safe: lv_xml_deinit() detaches the <subject_expr> observers
    // before lv_deinit(), and lv_deinit() removes the object-bound ones, so these
    // subjects have no subscribers left by now.
    theme_manager_deinit();

    // Uninstall crash handler last — clean shutdown reached this point, so a
    // SIGBUS/SIGSEGV after this is the kernel's problem, not ours.
    crash_handler::uninstall();

    spdlog::info("[Application] Shutdown complete");
}
