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

#include "ui_overlay_timelapse_videos.h"

#include "ams_backend.h"
#include "ams_error_bridge.h"
#include "app_constants.h"
#include "asleep_status_wiring.h"
#include "asset_manager.h"
#include "cjk_filename_glyphs.h"
#include "config.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "detect_printer_cmd.h"
#include "discovery_steps.h"
#include "display_manager.h"
#include "env_knobs.h"
#include "env_refusal_notice.h"
#include "environment_config.h"
#include "fbdev_signal_clear.h"
#include "gcode_response_routing.h"
#include "hardware_fingerprint.h"
#include "hardware_role_registry.h"
#include "hardware_validator.h"
#include "helix_version.h"
#include "input_settings_manager.h"
#include "job_queue_state.h"
#include "keyboard_shortcuts.h"
#include "lan_client_auth_router.h"
#include "layout_manager.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "light_button_config.h"
#include "main_loop_wake.h"
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
#include "u1_stock_screen.h"
#ifdef HELIX_HAS_PWM_SOUND
#include "pwm_sound_backend.h"
#endif
#include "recovery_modal_presenter.h"
#include "refresh_period_hold.h"
#ifdef HELIX_ENABLE_REMOTE_CONTROL
#include "remote_control_server.h"
#endif
#include "audio_settings_manager.h"
#include "core/static_panel_registry.h"
#include "core/static_subject_registry.h"
#include "remote_screen_service.h"
#include "rotation_probe.h"
#include "rpc_error_correlation.h"
#include "screenshot.h"
#include "sensor_state.h"
#include "sound_manager.h"
#include "spoolman_active_spool_sync.h"
#include "subject_initializer.h"
#include "temp_graph_controller.h"
#include "temperature_history_manager.h"
#include "thermal_rate_model.h"
#include "timelapse_state.h"
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
#include "ui_filament_motion_glyph.h"
#include "ui_gcode_viewer.h"
#include "ui_gradient_canvas.h"
#include "ui_guided_device_flow.h"
#include "ui_humidity_drop.h"
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
#include "ui_plate_glow.h"
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
#include "ui_unit_art.h"
#include "ui_update_notify_modal.h"
#include "ui_utils.h"
#include "ui_wizard.h"
#include "ui_wizard_ams_identify.h"
#include "ui_wizard_language_chooser.h"
#include "ui_wizard_touch_calibration.h"
#include "ui_wizard_wifi.h"

#include "color_utils.h"
#include "core/observer_guard.h"
#include "preflight_validator.h"
#include "ui/ui_widget_helpers.h"

// Developer-only showcase panel (ENABLE_DEV_PANELS, excluded from release
// builds). Not wired into PanelFactory — kept as a live testbed for icon-font
// coverage.
#ifdef HELIX_ENABLE_DEV_PANELS
#include "ui_panel_glyphs.h"
#endif

#include "ui_ipv4_revert_retry.h"

#include "active_print_media_manager.h"
#include "android_asset_extractor.h"
#include "board_identity.h"
#include "core/data_root_resolver.h"
#include "display_settings_manager.h"
#include "glass_gate.h"
#include "helix_sparkline.h"
#include "setting_group.h"
#include "temperature_service.h"
#if HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif
#include "ui_perf_report.h"

#include "display_metrics.h"
#include "k2_stock_detection_source.h"
#include "led/ui_led_control_overlay.h"
#include "platform_info.h"
#include "printer_detector.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "system/config_trust.h"
#include "system/crash_handler.h"
#include "system/crash_history.h"
#include "system/crash_reporter.h"
#include "system/diagnostics.h"
#include "system/lvgl_thread_check.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "u1_stock_detection_source.h"
#include "upgrade_banner.h"
#include "wifi_link_monitor.h"
#include "wifi_manager.h"

// Backend headers
#include "abort_manager.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "detection_manager.h"
#include "filament_sensor_manager.h"
#include "gcode_file_modifier.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_translation.h"
#include "hv/hlog.h" // libhv logging - sync level with spdlog
#include "json_utils.h"
#include "logging_init.h"
#include "lvgl.h"
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
#include "print_health_log.h"
#if HELIX_HAS_PLUGINS
#include "plugin_dir_watcher.h"
#include "plugin_host.h"
#include "plugin_source_app.h"
#endif
#include "cleanup_hold.h"
#include "platform_table.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "sigterm_decision.h"
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

namespace {

// Android lifecycle: background/foreground state set from SDL event handler
std::atomic<bool> s_app_backgrounded{false};

// SIGUSR1: remote screenshot trigger. Handler is signal-safe; main loop polls.
std::atomic<bool> s_screenshot_requested{false};

bool s_safe_mode_active = false;

// 32 bits because 32-bit MIPS has no lock-free 64-bit atomic. Wraparound is harmless:
// callers only subtract two readings.
std::atomic<uint32_t> s_graceful_sigterm_ms{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

uint32_t monotonic_ms_signal_safe() {
    struct timespec ts {};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec) * 1000u + static_cast<uint32_t>(ts.tv_nsec / 1000000);
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
 * lost by skipping the explicit flush. The exception is plugin storage, which
 * is written up to 500 ms after a change: a SIGTERM inside that window drops
 * the plugin's latest helix.storage.set calls.
 *
 * Exception: while a heater is on that only this app will turn off, SIGTERM
 * takes the SIGINT path so teardown can send the off (sigterm_action()).
 *
 * Async-signal-safe: raw syscalls, lock-free atomics and _exit(2) only — no
 * spdlog, no allocation.
 */
void graceful_quit_signal_handler(int sig) {
    if (sig == SIGTERM) {
        const uint32_t now_ms = monotonic_ms_signal_safe();
        const uint32_t graceful_at = s_graceful_sigterm_ms.load(std::memory_order_relaxed);
        const helix::SigtermAction action =
            helix::sigterm_action(helix::CleanupHold::any_held_signal_safe(), graceful_at != 0,
                                  helix::ms_since_sigterm_stamp(now_ms, graceful_at));
        if (action == helix::SigtermAction::Ignore) {
            return;
        }
        if (action == helix::SigtermAction::GracefulQuit) {
            const int saved_errno = errno;
            s_graceful_sigterm_ms.store(helix::sigterm_stamp(now_ms), std::memory_order_relaxed);
            static const char held_msg[] =
                "[Application] SIGTERM with a cleanup owed: graceful quit\n";
            ssize_t written = write(STDERR_FILENO, held_msg, sizeof(held_msg) - 1);
            (void)written;
            app_request_quit_signal_safe();
            errno = saved_errno;
            return;
        }
        // A supervisor stop is not a crash. Because this path skips
        // Application::shutdown() — the only other place the crash-restart
        // marker is cleared — every SIGTERM used to leave its start timestamp
        // behind. On the Elegoo CC1, COSMOS's resonance-calibration macro
        // stops and starts the UI through gui-switcher (SIGTERM by pidfile);
        // three of those inside the 120s window tripped "Crash loop detected"
        // and HelixScreen refused to boot. unlink(2) is async-signal-safe;
        // the path was cached at startup so nothing is constructed here.
        helix::clear_crash_marker_signal_safe();
        // Blank the panel, or the last frame shows through a reboot's boot
        // screen. open/write/close only; a no-op unless fbdev registered it.
        helix::clear_framebuffer_signal_safe();
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

/// The enqueue hook while the display sleeps. Main-thread work queued during a pass is
/// drained in that pass, so only another thread's enqueue needs to wake the loop.
void notify_loop_from_other_thread() {
    if (!helix::ui::UpdateQueue::is_main_thread()) {
        helix::main_loop_wake::notify();
    }
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
    helix::android_configure_backup_dirs();
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
    // Stock-firmware U1: SSH did not come up although Root access is on.
    helix::u1::surface_root_ssh_down_from_hook();

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

    // --test aborts on the first LVGL call made off the LVGL thread, as the unit
    // tests do; HELIX_LVGL_THREAD_CHECK=report logs each call site and carries on.
    {
        using namespace helix::lvgl_thread_check;
        set_mode(mode_from_env(get_runtime_config()->is_test_mode() ? Mode::Abort : Mode::Off));
    }

    // Every path this process uses is chosen by a cascade with a user override
    // on every rung, so the log has to carry the resolved values: a support case
    // on an overridden box cannot be answered from the rules alone.
    helix::diagnostics::log_diagnostics();
    if (m_screen_size_env) {
        if (m_screen_size_env_valid) {
            spdlog::info("[Application] Screen size from HELIX_SCREEN_SIZE: {}x{}", m_screen_width,
                         m_screen_height);
        } else {
            spdlog::warn("[Application] Invalid HELIX_SCREEN_SIZE='{}' — use named size "
                         "(micro/tiny/small/medium/large/xlarge) or WxH (e.g. 480x400)",
                         *m_screen_size_env);
        }
    }
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

    init_process_services();

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

    init_sound_services();

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
    // kCrashLoopMaxCrashes) shows the recovery dialog. main_loop()'s own
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

        // Phases 14d and 15
        init_monitoring_services();

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
    static constexpr uint32_t kFbSelfhealIntervalMs = 10000; // 10 seconds

    // Liveness breadcrumb: emits every ~30 s so a crashed session's last moments
    // always show recent activity, even if the user was idle on one panel.
    uint32_t last_tick_crumb = start_time;
    static constexpr uint32_t kTickCrumbIntervalMs = 30000;
    uint64_t frame_counter = 0;

    // One line a minute during a print, so a report of a stalled print carries what the
    // UI was costing the board at the time.
    helix::attach_print_health_counters(m_display->display());
    helix::install_ui_perf_bundle_provider();
    helix::PrintHealthLog print_health(
        [this] { return helix::read_print_health_sample(m_display->display()); });

    // Heap snapshot refresh cadence — cheap, called from the main thread so the
    // crash signal handler can read the cached values without touching
    // non-async-signal-safe APIs (mallinfo, open, lv_mem_monitor).
    uint32_t last_heap_refresh = start_time;
    static constexpr uint32_t kHeapRefreshIntervalMs = 10000;
    crash_handler::refresh_heap_snapshot();

    m_splash_manager.arm_invalidation_failsafe(DisplayManager::get_ticks());

    // Configure main loop handler
    helix::application::MainLoopHandler::Config loop_config;
    loop_config.screenshot_enabled = m_args.screenshot_enabled;
    loop_config.screenshot_delay_ms = static_cast<uint32_t>(m_args.screenshot_delay_sec) * 1000U;
    loop_config.timeout_sec = m_args.timeout_sec;
    loop_config.benchmark_mode = helix::config::EnvironmentConfig::get_benchmark_mode();
    loop_config.benchmark_report_interval_ms = 5000;
    m_loop_handler.init(loop_config, start_time);

    // The glass gate: decided now from stored facts, or probed once after boot settles.
    helix::ui::glass_gate_start();

    // Show one-time post-migration notice before entering the main loop.
    // Splash handoff timing risk on target hardware (AD5M, Pi 3B) is theoretical —
    // verify on real hardware as part of Task 5 manual checks. If it's an issue,
    // move this call after the post-splash refresh block or defer via lv_async_call.
#if HELIX_ENABLE_SCREENSAVER
    show_screensaver_migration_notice_if_pending();
#endif

    // Top-level safety net: any std::exception thrown from a callback invoked
    // inside lv_timer_handler() (observers, queued UpdateQueue items, LVGL
    // animations, async calls) unwinds the entire stack out of main_loop into
    // main()'s catch and exits 134 — the watchdog interprets this as a crash
    // and after kCrashLoopMaxCrashes same-signature events triggers the
    // "HelixScreen Keeps Crashing" recovery dialog (#931). A separate safety
    // net wraps the initial-subscription dispatch path, but queued/observer/
    // timer callbacks inside lv_timer_handler are not yet guarded. Catch +
    // log + dump breadcrumbs + continue: the user gets a
    // toast and a usable app instead of a crash loop they can only escape
    // by reflashing the previous version. A streak counter breaks out if the
    // catch itself is in a tight retry loop.
    int exception_streak = 0;
    uint32_t streak_window_start = 0;
    static constexpr int kRunawayThreshold = 5;
    static constexpr uint32_t kRunawayWindowMs = 30000;

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

            // Asleep, queued work wakes the loop (the enqueue hook) and runs here, in the
            // pass that applied the status behind it, instead of on a polling timer.
            if (m_display->is_display_sleeping()) {
                helix::ui::UpdateQueue::instance().drain_until_empty();
            }

            // Check display sleep
            m_display->check_display_sleep();

            // Periodic full-screen invalidation on fbdev (self-heal kernel console bleed-through)
            if (needs_fb_self_heal &&
                (current_tick - last_fb_selfheal_tick) >= kFbSelfhealIntervalMs) {
                lv_obj_invalidate(lv_screen_active());
                last_fb_selfheal_tick = current_tick;
            }

            // Periodic liveness breadcrumb (counts frames so crash bundles know
            // whether the loop was spinning normally or stalled).
            ++frame_counter;
            if ((current_tick - last_tick_crumb) >= kTickCrumbIntervalMs) {
                crash_handler::breadcrumb::note("tick", "", static_cast<long>(frame_counter));
                last_tick_crumb = current_tick;
            }

            if (const auto line = print_health.tick(
                    current_tick,
                    lv_subject_get_int(
                        get_printer_state().print_state().get_print_active_subject()) != 0)) {
                spdlog::info("[PrintHealth] {}", *line);
            }

            // Refresh cached heap snapshot so any crash within the next window
            // reports recent memory state.
            if ((current_tick - last_heap_refresh) >= kHeapRefreshIntervalMs) {
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

            m_splash_manager.run_handoff_frame(m_display.get(), current_tick);

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
                const bool display_sleeping = m_display->is_display_sleeping();
                // Set from the sleep state on every pass, so no wake path can leave it slow.
                auto& update_queue = helix::ui::UpdateQueue::instance();
                update_queue.set_enqueue_hook(display_sleeping ? &notify_loop_from_other_thread
                                                               : nullptr);
                if (lv_timer_t* queue = update_queue.timer()) {
                    lv_timer_set_period(queue, helix::update_queue_period_ms(
                                                   display_sleeping, timing, LV_DEF_REFR_PERIOD));
                    if (display_sleeping) {
                        lv_timer_pause(queue);
                    } else {
                        lv_timer_resume(queue);
                    }
                }
                m_display->wait_for_work(
                    helix::main_loop_sleep_ms(time_till_next, display_sleeping, timing));
            } else {
                DisplayManager::delay(1);
            }
        } catch (const std::exception& e) {
            // A callback inside this iteration threw and was not caught
            // closer to the source. Pre-#931, this unwound through main()
            // and exited 134, triggering a watchdog crash loop. Now: log
            // type+what, dump the recent breadcrumb ring, record telemetry,
            // and continue. If catches pile up faster than kRunawayThreshold
            // / kRunawayWindowMs, exit cleanly so the watchdog sees a
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
            if (exception_streak == 0 || (now_tick - streak_window_start) > kRunawayWindowMs) {
                streak_window_start = now_tick;
                exception_streak = 1;
            } else {
                ++exception_streak;
            }
            if (exception_streak >= kRunawayThreshold) {
                spdlog::critical("[Application] Runaway exception streak ({} in {}ms) — "
                                 "exiting main loop cleanly to break the throw-catch tight loop",
                                 exception_streak, kRunawayWindowMs);
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
    lv_display_enable_invalidation(lv_display_get_default(), false);

    spdlog::info("[Application] Background pause complete");
}

void Application::on_enter_foreground() {
    if (!m_backgrounded)
        return;
    m_backgrounded = false;
    spdlog::info("[Application] Resuming from background");

    // 1. Re-enable rendering
    lv_display_enable_invalidation(lv_display_get_default(), true);

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
    lv_display_trigger_activity(lv_display_get_default());

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

#if HELIX_ENABLE_SCREENSAVER
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
