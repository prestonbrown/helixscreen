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
#include "environment_config.h"
#include "gcode_error_router.h"
#include "gcode_narration_router.h"
#include "hardware_fingerprint.h"
#include "hardware_role_registry.h"
#include "hardware_validator.h"
#include "helix_version.h"
#include "http_executor.h"
#include "job_queue_state.h"
#include "keyboard_shortcuts.h"
#include "lan_client_auth_router.h"
#include "layout_manager.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "moonraker_manager.h"
#include "page_scroll_auto_inject.h"
#include "panel_factory.h"
#include "panel_widget_manager.h"
#include "pending_startup_warnings.h"
#include "post_op_cooldown_manager.h"
#include "power_device_state.h"
#include "print_history_manager.h"
#include "printer_cache_registry.h"
#include "printer_recovery_service.h"
#include "recovery_modal_presenter.h"
#ifdef HELIX_ENABLE_REMOTE_CONTROL
#include "remote_control_server.h"
#endif
#include "audio_settings_manager.h"
#include "rpc_error_correlation.h"
#include "screenshot.h"
#include "sensor_state.h"
#include "sound_manager.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "streaming_policy.h"
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
#include "ui_settings_display_sound.h"
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

// Developer-only showcase panels (ENABLE_DEV_PANELS, excluded from release
// builds). Not wired into PanelFactory — kept as live testbeds for XML
// bindings, wizard step progress, the 3D G-code viewer and icon-font coverage.
#ifdef HELIX_ENABLE_DEV_PANELS
#include "ui_panel_gcode_test.h"
#include "ui_panel_glyphs.h"
#include "ui_panel_step_test.h"
#include "ui_panel_test.h"
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
#include "led/ui_led_control_overlay.h"
#include "platform_info.h"
#include "printer_detector.h"
#include "printer_image_manager.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "system/crash_handler.h"
#include "system/crash_history.h"
#include "system/crash_reporter.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "u1_stock_detection_source.h"
#include "upgrade_banner.h"
#include "wifi_manager.h"

// Backend headers
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "app_globals.h"
#include "detection_manager.h"
#include "filament_consumption_tracker.h"
#include "filament_sensor_manager.h"
#include "gcode_file_modifier.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_translation.h"
#include "hv/hlog.h" // libhv logging - sync level with spdlog
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
#include "plugin_manager.h"
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
// abi::__cxa_current_exception_type() — names the exception being handled
// without RTTI on the static type. See the catch blocks below.
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
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

#if HELIX_HAS_CAMERA
// Defined in src/ui/panel_widgets/camera_widget.cpp; that directory is not on
// application.cpp's include path, so forward-declare rather than including the
// header (same pattern as ui_settings_hardware.cpp).
namespace helix {
void open_standalone_camera_fullscreen(lv_obj_t* parent_screen);
}
#endif

// External globals for logging (defined in cli_args.cpp, populated by parse_cli_args)
extern std::string g_log_dest_cli;
extern std::string g_log_file_cli;
extern std::string g_log_level_cli;

namespace {

// Mangled type name of the exception currently being handled, for the catch-all
// loggers below.
//
// Replaces typeid(e).name(), which needs RTTI — the firmware builds -fno-rtti.
// __cxa_current_exception_type() is part of the Itanium ABI's exception-handling
// support, which is unaffected by -fno-rtti, and it reports the type that was
// thrown rather than the type of the handler's parameter. Null when no exception
// is in flight; inside a catch block it never is, but the callers do not depend
// on that.
const char* current_exception_type_name() {
    const std::type_info* ti = abi::__cxa_current_exception_type();
    return ti ? ti->name() : "<unknown>";
}

// Android lifecycle: background/foreground state set from SDL event handler
std::atomic<bool> s_app_backgrounded{false};

// SIGUSR1: remote screenshot trigger. Handler is signal-safe; main loop polls.
std::atomic<bool> s_screenshot_requested{false};

// Crash loop detection marker file. Routed through the writable config
// dir so it survives RO-rootfs platforms (Yocto squashfs etc.).
const std::string& crash_marker_path() {
    static const std::string p = helix::writable_path(".crash_restart_count");
    return p;
}

// Async-signal-safe copy of crash_marker_path(). The SIGTERM handler clears the
// marker with unlink(2) and may not build the path itself: std::string, the
// function-local static's guard, and std::filesystem::remove are all unsafe in
// a handler. The path is snapshotted here once on the main thread at startup;
// s_crash_marker_path_ready gates the handler until it is.
char s_crash_marker_path[PATH_MAX] = {0};
volatile sig_atomic_t s_crash_marker_path_ready = 0;

// GPU 3D crash-loop guard file (issues #966 / #1084 / #1085). The 3D GLES
// renderer writes this immediately before its first GPU draw and removes it
// after the first successful frame. If the driver hard-faults inside the draw
// the process dies with the file still present; finding it here at startup
// means the last session crashed in the GPU path, so we promote it to a
// persistent block. Routed through the writable config dir like the other
// markers so it survives RO-rootfs platforms.
const std::string& gpu_3d_guard_path() {
    static const std::string p = helix::writable_path("gpu_3d_guard");
    return p;
}

// GPU 2D blur crash-loop guard file. The DRM+EGL backdrop-blur path writes this
// immediately before its first Mali/EGL init and removes it once the pipeline is
// up. If the driver hard-faults inside that init (an in-driver SIGSEGV the
// reactive check_gl() guard cannot catch), the process dies with the file still
// present; finding it here at startup means the last session crashed initializing
// GPU blur, so we promote it to a persistent block. Routed through the writable
// config dir like the other markers so it survives RO-rootfs platforms.
const std::string& gpu_blur_guard_path() {
    static const std::string p = helix::writable_path("gpu_blur_guard");
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
        // A supervisor stop is not a crash. Because this path skips
        // Application::shutdown() — the only other place the crash-restart
        // marker is cleared — every SIGTERM used to leave its start timestamp
        // behind. On the Elegoo CC1, COSMOS's resonance-calibration macro
        // stops and starts the UI through gui-switcher (SIGTERM by pidfile);
        // three of those inside the 120s window tripped "Crash loop detected"
        // and HelixScreen refused to boot. unlink(2) is async-signal-safe;
        // the path was cached at startup so nothing is constructed here.
        helix::clear_crash_marker_signal_safe();
        static const char msg[] = "[Application] SIGTERM — fast exit\n";
        ssize_t n = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)n; // suppress unused-result warning
        _exit(0);
    }
    // SIGINT and any other caught signal: graceful path
    app_request_quit_signal_safe();
}

} // namespace

namespace helix {

bool cache_crash_marker_path_for_signal(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(s_crash_marker_path)) {
        spdlog::warn("[Application] Crash marker path not signal-cacheable ({} bytes): '{}'",
                     path.size(), path);
        s_crash_marker_path_ready = 0;
        return false;
    }
    std::memcpy(s_crash_marker_path, path.c_str(), path.size() + 1);
    s_crash_marker_path_ready = 1;
    return true;
}

void clear_crash_marker_signal_safe() {
    if (s_crash_marker_path_ready == 0) {
        return;
    }
    // ENOENT is the common case (no marker written, e.g. --test mode).
    (void)::unlink(s_crash_marker_path);
}

} // namespace helix

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
    if (const char* t = std::getenv("HELIX_CRASH_TEST"); t && *t && std::string(t) != "0") {
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

    // Snapshot the marker path for the SIGTERM handler. init_config() has run,
    // so writable_path() now resolves; the handler cannot construct this itself.
    helix::cache_crash_marker_path_for_signal(crash_marker_path());

    // Crash loop detection: track rapid restarts via marker file. Skipped in
    // test mode — automation (screenshot pipeline, helixctl-driven runs) relaunches
    // the binary rapidly by design, and this guard exists to protect users on a
    // real device from an infinite restart loop, never a dev running --test.
    if (!get_runtime_config()->is_test_mode()) {
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

    // Promote a surviving GPU 3D crash-loop guard to a persistent block. The
    // guard file only survives if the last session died inside the GPU driver
    // mid-draw (the renderer clears it after the first successful frame). Set
    // /display/gpu_3d_blocked so the gcode viewer uses the pure-CPU 2D path,
    // then remove the guard so a subsequent clean run can re-arm it.
    {
        std::error_code ec;
        if (std::filesystem::exists(gpu_3d_guard_path(), ec)) {
            spdlog::warn("[Application] GPU 3D crash-loop guard survived — last session likely "
                         "faulted inside the GPU driver; blocking 3D gcode preview");
            Config::get_instance()->set<bool>("/display/gpu_3d_blocked", true);
            Config::get_instance()->save();
            std::filesystem::remove(gpu_3d_guard_path(), ec);
        }
    }

    // Promote a surviving GPU blur crash-loop guard to a persistent block. The
    // guard file only survives if the last session died inside the Mali/EGL blur
    // init mid-setup (backdrop_blur.cpp clears it once the pipeline is up). Set
    // /display/gpu_blur_blocked so the backdrop uses the pure-CPU blur path, then
    // remove the guard so a subsequent clean run can re-arm it.
    {
        std::error_code ec;
        if (std::filesystem::exists(gpu_blur_guard_path(), ec)) {
            spdlog::warn("[Application] GPU blur crash-loop guard survived — last session likely "
                         "faulted inside the GPU driver; blocking GPU backdrop blur");
            Config::get_instance()->set<bool>("/display/gpu_blur_blocked", true);
            Config::get_instance()->save();
            std::filesystem::remove(gpu_blur_guard_path(), ec);
        }
    }

    // Phase 3: Initialize logging
    if (!init_logging()) {
        return 1;
    }

    spdlog::info("[Application] ========================");
    spdlog::info("[Application] HelixScreen {} ({})", helix_version(), helix_git_hash());
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

    // Backend is now picked: seed the audio-device-available subject so the
    // Display/Sound overlay's device-row binding resolves correctly. Subjects
    // init before SoundManager, so the value is stale until this refresh.
    AudioSettingsManager::instance().refresh_audio_device_available();

    // Show sound settings immediately if a local backend exists,
    // without waiting for hardware discovery / Klipper connection.
    if (SoundManager::instance().has_backend()) {
        get_printer_state().set_sound_backend_available(true);
    }

    // Initialize PostOpCooldownManager (unified filament operation cooldown)
    PostOpCooldownManager::instance().init();

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
                spdlog::info(
                    "[Application] Crash from post-update restart, suppressing crash dialog");
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
                    spdlog::info(
                        "[Application] Previous crash detected — showing crash report dialog");
                    auto* modal = new CrashReportModal();
                    modal->set_report(report);
                    modal->show_modal(lv_screen_active());
                }
            }
        }

        // Register wizard completion callback for add-printer recovery
        set_wizard_completion_callback([this]() {
            if (!m_wizard_previous_printer_id.empty()) {
                spdlog::info(
                    "[Application] Wizard completed — clearing add-printer recovery state");
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
                lv_tr("Safe Mode active. The printer connection is disabled because the app "
                      "kept crashing on startup. Open Settings to fix the issue, then reboot."),
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
            } else {
                rc.transport = helix::RemoteConfig::Transport::UnixSocket;
                rc.socket_path = helix::resolve_socket_path(m_args.remote_socket);
            }
            if (!helix::RemoteControlServer::instance().start(rc)) {
                spdlog::warn("[Application] Remote control server failed to start (non-fatal)");
            }
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
                    helix::ui::queue_update([]() {
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
                    helix::ui::queue_update([]() {
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
        const char* type_name = current_exception_type_name();
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

    // Resolve log file path: CLI > HELIX_LOG_FILE > config
    log_config.file_path = resolve_log_setting(g_log_file_cli, env_log_file,
                                               m_config->get<std::string>("/log_path", ""));

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
    // rather than in connect_moonraker(): that runs again on every printer
    // switch, and register_sleep_callback() only appends — there is no
    // unregister — so each switch would stack another copy and fire one extra
    // force_reconnect() per wake. init_display() runs once per process, and the
    // captured `this` owns m_display, so the callback list cannot outlive it.
    // m_moonraker is read lazily at wake time and need not exist yet.
#ifdef __ANDROID__
    m_display->register_sleep_callback([this](bool sleeping) {
        if (!sleeping && m_moonraker && m_moonraker->client()) {
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
            m_moonraker->client()->force_reconnect();
        }
    });
#endif

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
    spdlog::debug("[Application] Display DPI applied: {} (was {} before set)", effective_dpi,
                  pre_set_dpi);
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
    // LayoutManager just resolved any --layout override. Republish
    // ui_is_portrait from it so XML visual decisions match the C++ ones; the
    // startup seed (theme_manager_init) and the rotation-probe refresh both ran
    // before this point and could only see detect_layout_type(). #1255.
    theme_manager_refresh_orientation();
    spdlog::info("[Application] Layout: {} ({})", layout_mgr.name(),
                 layout_mgr.is_standard() ? "default" : "override");
}

bool Application::register_widgets() {
    ui_icon_register_widget();
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

bool Application::init_core_subjects() {
    m_subjects = std::make_unique<SubjectInitializer>();

    // Phase 1-3: Core subjects, PrinterState, AmsState
    // These must exist before MoonrakerManager::init() can create the API
    m_subjects->init_core_and_state();

    // Register the ams_current_tool_text formatter ("T<n>" / "---") now that
    // AmsState's subjects are live. The print status panel embeds
    // <ams_current_tool> and binds ams_current_tool_text — without this
    // observer the lane label stays at its default "---" until a user
    // navigates into an AMS panel, which is where the call used to live.
    helix::ui::init_ams_tool_text_observers();

    // Bring LedController up with no API yet so its `led_controllable` subject
    // is registered for XML before the home/print-status panels instantiate.
    // printer_discovery later re-runs init(api, client) to bind the API — init()
    // always overwrites api_/client_ + rebinds backend pointers, and the subject
    // init path is idempotent via version_subject_initialized_.
    helix::led::LedController::instance().init(nullptr, nullptr);

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

    // Spaghetti / failed-print detection
    // (see docs/devel/printers/SNAPMAKER_U1_SUPPORT.md, defect_detection)
    // Must happen after MoonrakerClient + PrinterState exist (above).
    {
        auto u1 = std::make_unique<helix::detection::U1StockSource>(&get_printer_state());
        u1->start();
        auto& dm = helix::detection::DetectionManager::instance();
        dm.register_source(std::move(u1));
        dm.init(get_moonraker_client(), &get_printer_state());
        dm.set_policy("u1_stock", static_cast<helix::detection::DetectionPolicy>(
                                      SettingsManager::instance().get_detection_policy_u1()));
        dm.set_presenter([](const helix::detection::DetectionEvent& e,
                            helix::detection::DetectionPolicy p) {
            using helix::detection::DetectionPolicy;
            if (!SettingsManager::instance().get_detection_enabled())
                return;
            if (p == DetectionPolicy::NotifyOnly) {
                ToastManager::instance().show(ToastSeverity::WARNING,
                                              lv_tr("Spaghetti detected — print paused"), 8000);
                return;
            }
            // DeferToSource: show the response modal. The modal self-deletes via its
            // on_hide() override (async_call(delete this)) — LVGL/ModalStack cleanup
            // never calls Modal::~Modal, so dropping this pointer is intentional and
            // does NOT leak.
            auto* modal = new SpaghettiDetectionModal();
            // TODO(detection): attach latest camera frame when a stream is active
            modal->set_detection(e.message, nullptr);
            modal->set_on_resume([] {
                get_moonraker_api()->job().resume_print([] {}, [](const MoonrakerError&) {});
            });
            modal->set_on_abort([] { helix::AbortManager::instance().start_abort(); });
            modal->set_on_tune([] {
                // Null callbacks, not empty lambdas: a non-null error_cb reads
                // as "this caller reports the failure itself", which would
                // suppress Klipper's `!!` broadcast for a rejected
                // DEFECT_DETECTION_CONFIG and leave the user with nothing.
                get_moonraker_client()->send_jsonrpc(
                    "printer.gcode.script",
                    nlohmann::json{{"script", "DEFECT_DETECTION_CONFIG NOODLE_SENSITIVITY=low"}},
                    nullptr, nullptr);
            });
            modal->show(lv_screen_active());
        });
    }

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

    // Initialize PerformanceState subjects and wire the data source.
    // Must happen after IMoonrakerAPI is up (m_moonraker->api() is valid here)
    // and before XML panels are created so subjects exist when bindings resolve.
    helix::perf::PerformanceState::instance().init_subjects();
    if (get_runtime_config()->should_mock_moonraker()) {
        helix::perf::PerformanceState::instance().set_source(
            std::make_unique<helix::perf::MockPerformanceSource>());
    } else {
        helix::perf::PerformanceState::instance().set_source(
            std::make_unique<helix::perf::MoonrakerPerformanceSource>(m_moonraker->api()));
    }
    spdlog::debug("[Application] PerformanceState initialized");

    spdlog::debug("[Application] Panel subjects initialized");
    helix::MemoryMonitor::log_now("after_panel_subjects_init");
    return true;
}

bool Application::init_ui() {
    // Create entire UI from XML. Timed because this builds all six panel
    // subtrees in one call — the other half of what per-panel deferral would
    // move off boot and onto the first navigation.
    auto layout_t0 = std::chrono::steady_clock::now();
    m_app_layout = static_cast<lv_obj_t*>(lv_xml_create(m_screen, "app_layout", nullptr));
    spdlog::debug(
        "[Application] app_layout XML create took {:.1f}ms",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - layout_t0)
            .count());
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
        NotificationHistory::instance().seed_test_data();
        // Update notification badge to show unread count and severity color
        helix::ui::notification_refresh_from_history();
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
#if HELIX_HAS_PLUGINS
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
            snprintf(toast_msg, sizeof(toast_msg), lv_tr("\"%s\" failed to load"),
                     errors[0].plugin_id.c_str());

            ToastManager::instance().show_with_action(
                ToastSeverity::WARNING, toast_msg, lv_tr("Disable"),
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
            snprintf(toast_msg, sizeof(toast_msg), lv_tr("%zu plugins failed to load"),
                     errors.size());

            ToastManager::instance().show_with_action(
                ToastSeverity::WARNING, toast_msg, lv_tr("Manage"),
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
#else
    spdlog::debug("[Application] Plugin system compiled out (HELIX_HAS_PLUGINS=0)");
    return true;
#endif
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
        NavigationManager::instance().set_active(PanelId::PrintSelect);
        auto* print_panel = get_print_select_panel(get_printer_state(), m_moonraker->api());
        if (print_panel) {
            print_panel->set_pending_file_selection(runtime_config->select_file);
        }
    }
}

#ifdef HELIX_ENABLE_REMOTE_CONTROL
namespace helix {

// Bring up a demo overlay/modal with representative sample data. These screens
// only appear in response to a real printer event (pre-print check, runout,
// active print) or configured state (lock PIN), so mock-mode navigation can't
// reach them — the remote-control `demo` command uses this to capture them for
// screenshots with the real widget lifecycle. Must run on the UI thread.
bool show_demo_overlay(const std::string& name) {
    lv_obj_t* screen = lv_screen_active();

    if (name == "preflight-check") {
        // Representative pre-print filament check: one matching tool, one color
        // mismatch (advisory), one empty required slot (the blocking case).
        helix::PreflightResult pf;
        helix::ToolCheck ok;
        ok.tool_index = 0;
        ok.intended_material = "PLA";
        ok.intended_color = 0x2E8B57;
        ok.mapped_slot = 0;
        ok.slot_present = true;
        ok.severity = helix::ToolCheck::Severity::Ok;
        helix::ToolCheck color;
        color.tool_index = 1;
        color.intended_material = "PLA";
        color.intended_color = 0xE23B3B;
        color.mapped_slot = 1;
        color.slot_present = true;
        color.color_ok = false;
        color.severity = helix::ToolCheck::Severity::ColorMismatch;
        helix::ToolCheck empty;
        empty.tool_index = 2;
        empty.intended_material = "PETG";
        empty.intended_color = 0xF5A623;
        empty.mapped_slot = -1;
        empty.slot_present = false;
        empty.severity = helix::ToolCheck::Severity::EmptySlot;
        pf.checks = {ok, color, empty};
        auto* modal = new helix::ui::PreflightCheckModal();
        modal->set_checks(pf);
        modal->show(screen);
        return true;
    }

    if (name == "color-mismatch") {
        // The SECOND gate on a Print tap, after the pre-flight empty-slot block:
        // the print-start pipeline warns when a tool resolves to no slot at all.
        // Only reachable from a real multi-tool file whose tools do not map, so
        // mock navigation cannot get here. Text mirrors the unresolved_tools
        // gate's dialog exactly — two unresolved tools, color name plus
        // material per row.
        std::string message = lv_tr("These tools have no matching filament loaded:");
        message += "\n\n";
        message += std::string("  ") + LV_SYMBOL_BULLET +
                   " T2: " + helix::describe_color(0xF5A623) + " (PETG)\n";
        message += std::string("  ") + LV_SYMBOL_BULLET +
                   " T3: " + helix::describe_color(0x2E8B57) + " (PLA)\n";
        message += "\n";
        message += lv_tr("Load the required filaments or start anyway?");
        static char demo_message[1024];
        snprintf(demo_message, sizeof(demo_message), "%s", message.c_str());
        helix::ui::modal_confirm(lv_tr("Color Mismatch"), demo_message, ModalSeverity::Warning,
                                 lv_tr("Start Anyway"), nullptr);
        return true;
    }

    if (name == "runout-modal") {
        auto* modal = new RunoutGuidanceModal();
        modal->set_autofeed_capable(false);
        modal->set_resume_blocked(false);
        modal->show(screen);
        return true;
    }

    if (name == "ams-loading-error") {
        // Worst case for the modal chrome budget (prestonbrown/helixscreen#1277):
        // a fault string long enough to drive content_container to its
        // #dialog_content_max cap, with the AFC diagram pinned BELOW it and
        // outside the scroll area. That combination overruns the 85% card cap on
        // a 480x272 panel and the button row falls off the bottom. Unreachable in
        // mock mode — AmsBackendMock never produces a recognised AFC fault — so
        // this is the only way to check the real layout instead of arithmetic on
        // a token table.
        // ams_loading_error_modal.xml is registered lazily by AmsPanel, which has
        // not necessarily run — register it here so the demo works from a cold start.
        // Idempotent: re-registering a component replaces the identical entry.
        lv_xml_register_component_from_file(
            helix::asset_component_uri("ui_xml/ams_loading_error_modal.xml").c_str());

        lv_subject_t* seg = lv_xml_get_subject(nullptr, "afc_fault_segment");
        if (seg != nullptr) {
            lv_subject_set_int(seg, static_cast<int>(PathSegment::HUB));
        }
        auto* modal = new helix::ui::AmsLoadingErrorModal();
        modal->show(screen,
                    "Filament did not reach the toolhead sensor after the "
                    "configured load length. The lane may be jammed at the hub, "
                    "the spool may have run out mid-load, or the bowden length "
                    "configured for this lane may not match the physical tube "
                    "run between the hub and the toolhead.",
                    "Check the filament path and try again. If the lane is clear, "
                    "verify the configured bowden length for this lane and confirm "
                    "the hub sensor triggers when filament passes it.",
                    []() {});
        return true;
    }

    if (name == "action-prompt-worst") {
        // Worst case for action_prompt_modal's chrome budget (#1277). This modal
        // carries MORE pinned chrome than ams_loading_error_modal: the AFC
        // diagram, a row_wrap button container that can spill to a second row,
        // and a footer divider + footer row that are hidden by default. All of
        // it sits below the scroll area, so it is the shape most likely to
        // overrun the 85% card cap. Unreachable in mock mode — it needs a live
        // Klipper `action:prompt_begin` — so this is the only way to measure it.
        lv_subject_t* seg = lv_xml_get_subject(nullptr, "afc_fault_segment");
        if (seg != nullptr) {
            lv_subject_set_int(seg, static_cast<int>(PathSegment::HUB));
        }
        helix::PromptData data;
        data.title = "Filament Runout Detected";
        data.severity = "error";
        data.text_lines = {
            "Lane 1 ran out of filament during the print.",
            "The toolhead has been parked and the print is paused.",
            "Load a new spool into lane 1, then choose how to continue.",
        };
        data.buttons = {
            {"Resume", "RESUME", "primary", "", false, -1},
            {"Retry Load", "AFC_LOAD LANE=1", "secondary", "", false, -1},
            {"Change Lane", "AFC_CHANGE_LANE", "secondary", "", false, -1},
            {"Cancel Print", "CANCEL_PRINT", "error", "", true, -1},
        };
        auto* modal = new helix::ui::ActionPromptModal();
        modal->show_prompt(screen, data);
        return true;
    }

    if (name == "action-prompt-many") {
        // A prompt whose buttons cannot share one row: a preheat macro offering
        // seven material presets. Each label is far wider than a seventh of the
        // card, so this is the case that must fall back to row_wrap instead of
        // being squeezed into equal-width cells. Unreachable in mock mode - it
        // needs a live Klipper `action:prompt_begin` - so this is the only way
        // to check the wrapped layout against a real 480x272 panel.
        helix::PromptData data;
        data.title = "Preheat for Load";
        data.text_lines = {"Preheat filament and choose a material."};
        data.buttons = {
            {"PLA 220/60", "SET_MATERIAL M=PLA", "primary", "", false, -1},
            {"PETG 240/80", "SET_MATERIAL M=PETG", "primary", "", false, -1},
            {"ABS 250/100", "SET_MATERIAL M=ABS", "primary", "", false, -1},
            {"ASA 260/100", "SET_MATERIAL M=ASA", "primary", "", false, -1},
            {"TPU 230/50", "SET_MATERIAL M=TPU", "primary", "", false, -1},
            {"PC 280/110", "SET_MATERIAL M=PC", "primary", "", false, -1},
            {"Nylon 260/80", "SET_MATERIAL M=NYLON", "primary", "", false, -1},
            {"Cancel", "", "error", "", true, -1},
        };
        auto* modal = new helix::ui::ActionPromptModal();
        modal->show_prompt(screen, data);
        return true;
    }

    if (name == "lock-screen") {
        helix::ui::LockScreenOverlay::instance().show();
        return true;
    }

    if (name == "print-status") {
        PrintStatusPanel::push_overlay(screen);
        return true;
    }

    if (name == "ams-error-toast") {
        // The widest thing the AMS error renderer ever has to lay out: the
        // longest suggestion any backend produces (96 chars) under a 44-char
        // message. Unreachable in mock mode — AmsBackendMock carries no print
        // gate — so this is the only way to check the two-line toast against a
        // real 480x272 panel instead of arithmetic on a font table.
        helix::ui::notify_ams_error(AmsErrorHelper::print_active(/*is_paused=*/true));
        return true;
    }

    if (name == "print-tune") {
        // show() is the real entry point (create() alone builds a hidden panel
        // that never gets pushed) — it wires api + printer state and pushes.
        get_print_tune_overlay().show(screen, get_moonraker_api(), get_printer_state());
        return true;
    }

    if (name == "ams") {
        // The filament panel's AMS row no-ops without a configured backend, so
        // reach the dedicated AMS management panel directly (mock provides the
        // backend in --test mode).
        navigate_to_ams_panel();
        return true;
    }

    if (name == "camera") {
#if HELIX_HAS_CAMERA
        // No-ops if no webcam is discovered yet; point at a live Moonraker with a
        // webcam (--moonraker ws://host:7125) for a real feed.
        open_standalone_camera_fullscreen(screen);
        return true;
#else
        spdlog::warn("[demo] camera viewer requested but HELIX_HAS_CAMERA is off");
        return false;
#endif
    }

    // Fallback: show any registered XML component that is a self-contained
    // modal. The cases above exist because they need state wired up first;
    // a plain dialog needs none, so screenshotting one should not require
    // adding an entry here. Unknown names return nullptr and fall through.
    if (Modal::show(name.c_str()) != nullptr) {
        spdlog::debug("[demo] shown as a plain modal component: {}", name);
        return true;
    }

    return false;
}

} // namespace helix
#endif // HELIX_ENABLE_REMOTE_CONTROL

void Application::reapply_hardware_roles() {
    m_async_lifetime.defer("Application::reapply_hardware_roles", [this]() {
        IMoonrakerAPI* api = m_moonraker ? m_moonraker->api() : nullptr;
        if (!api) {
            return;
        }
        const auto& fans = api->hardware().fans();
        const auto& heaters = api->hardware().heaters();
        // Re-resolve + persist fan roles, then rebind fan UI to the new mapping.
        // apply_roles, not init_fans: the wizard changed which fan plays which
        // role, not which fans exist, so the discovered list comes from what
        // discovery actually stored rather than being re-passed from here.
        auto roles = helix::FanRoleConfig::from_config(Config::get_instance(), fans);
        get_printer_state().apply_fan_roles(roles);
        // Heater roles persist back to config (no dedicated runtime fan-style consumer).
        helix::resolve_role_from_config(helix::HardwareRoleId::HotendHeater, Config::get_instance(),
                                        heaters, /*persist_autoheal=*/true);
        helix::resolve_role_from_config(helix::HardwareRoleId::BedHeater, Config::get_instance(),
                                        heaters, /*persist_autoheal=*/true);
    });
}

void Application::settle_deferred_hardware_setup() {
    Config* config = Config::get_instance();
    if (!helix::wizard_clear_hardware_setup_deferred(config)) {
        return;
    }
    if (!config->save()) {
        spdlog::warn("[Application] Failed to persist deferred hardware setup decision");
    }
}

void Application::launch_deferred_hardware_setup() {
    auto steps = std::move(m_pending_hardware_setup_steps);
    m_pending_hardware_setup_steps.clear();
    if (steps.empty()) {
        return;
    }
    spdlog::info("[Application] Launching deferred hardware setup ({} step(s))", steps.size());

    ui_wizard_register_event_callbacks();
    ui_wizard_container_register_responsive_constants();
    ui_wizard_init_subjects();
    // Back on the first targeted step has nothing to retreat to, so give it the
    // same dismiss semantics as the reconfig wizard.
    set_wizard_cancel_callback([]() {
        ui_wizard_complete_targeted();
        set_wizard_cancel_callback(nullptr);
    });
    Application* app = this;
    ui_wizard_create_targeted(m_screen, std::move(steps), [app]() {
        set_wizard_cancel_callback(nullptr);
        // ui_wizard_complete_targeted() deliberately skips the expected-hardware
        // population, so record the user's fresh picks here.
        ui_wizard_record_expected_hardware(Config::get_instance());
        app->reapply_hardware_roles();
    });
}

void Application::prompt_deferred_hardware_setup(std::vector<helix::wizard::StepId> steps) {
    // The steps to run are parked on the Application instance for the confirm
    // callback's timer to consume (launch_deferred_hardware_setup() reads them
    // from there). on_dismiss clears them too, so a backdrop tap or ESC cannot
    // strand the offer.
    m_pending_hardware_setup_steps = std::move(steps);
    spdlog::info("[Application] Offering deferred hardware setup ({} step(s))",
                 m_pending_hardware_setup_steps.size());

    helix::ui::modal_confirm(
        lv_tr("Printer hardware detected"),
        lv_tr("Your printer was offline during setup, so hardware options were skipped. "
              "Set them up now?"),
        ModalSeverity::Info, lv_tr("Set up"),
        [this] {
            // Settle first: the wizard tears itself down asynchronously, and a
            // crash mid-run must not leave the offer pending forever.
            settle_deferred_hardware_setup();
            // Build the wizard AFTER the modal's exit animation, not inside the
            // click that started it: the dialog's own close only marks the
            // backdrop exiting, so creating the full-screen wizard here would
            // put it underneath a still-fading backdrop. The pending step list
            // lives on the Application instance until the timer consumes it.
            lv_timer_t* launch = lv_timer_create(
                [](lv_timer_t* t) {
                    auto* self = static_cast<Application*>(lv_timer_get_user_data(t));
                    lv_timer_delete(t);
                    self->launch_deferred_hardware_setup();
                },
                300, this);
            lv_timer_set_repeat_count(launch, 1);
        },
        [this] {
            m_pending_hardware_setup_steps.clear();
            // Declining is final for this printer. The offer is for optional
            // role assignments the app already has working defaults for, the
            // snapshot was written regardless so nothing is flagged either way,
            // and re-asking on every boot is the exact nag the surrounding
            // reconfig-wizard code records declines to avoid. `--wizard` still
            // re-runs setup, and a saved role that later breaks still routes to
            // the targeted reconfig wizard on its own.
            settle_deferred_hardware_setup();
            spdlog::info("[Application] Deferred hardware setup declined");
        },
        lv_tr("Not now"), [this] { m_pending_hardware_setup_steps.clear(); },
        m_async_lifetime.token());
}

void Application::settle_type_mismatch_warning() {
    auto* cfg = Config::get_instance();
    cfg->set<std::string>(cfg->df() + helix::wizard::TYPE_MISMATCH_SHOWN_FOR,
                          cfg->get<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, ""));
    if (!cfg->save()) {
        spdlog::warn("[Application] Failed to persist type mismatch decision");
    }
}

void Application::maybe_warn_type_mismatch(const helix::PrinterDiscovery& hardware) {
    // The gates below all decline silently in normal operation. A debug bundle
    // is the only view we get of a reporter's run, so each one says why it
    // declined: without that there is no way to tell a 68%-confidence near-miss
    // apart from detection returning nothing at all (bundle TZT85MQ3).
    if (m_type_mismatch_shown) {
        spdlog::debug("[Application] Type mismatch check skipped: already prompted this session");
        return;
    }
    if (get_runtime_config()->should_mock_moonraker()) {
        // A mock printer's identity is whatever the persona declares, so a
        // mismatch against the saved type says nothing about real hardware.
        // Gate on the runtime-config predicate, not on HELIX_MOCK_PRINTER:
        // plain --test runs the mock client without that env var ever being
        // set, so the old getenv check let every --test launch open the
        // prompt against whatever type settings-test.json happened to carry.
        spdlog::debug("[Application] Type mismatch check skipped: mock printer");
        return;
    }
    if (Config::get_instance()->is_wizard_required() || is_wizard_active()) {
        spdlog::debug("[Application] Type mismatch check skipped: wizard required or active");
        return;
    }

    auto* cfg = Config::get_instance();
    const std::string stored = cfg->get<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "");

    // The saved type is a display name, so an entry renamed in the printer
    // database orphans every config written under the old one and detection
    // then contradicts a type that was never wrong. Resolve through the
    // database's alias list and heal the stored value, otherwise the stale
    // name keeps missing every other name-keyed lookup too (image, preset,
    // pre-print profile) long after this prompt is dismissed.
    const std::string saved = PrinterDetector::canonical_type_name(stored);
    if (saved != stored) {
        cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, saved);
        if (!cfg->save()) {
            spdlog::warn("[Application] Failed to persist renamed printer type '{}' -> '{}'",
                         stored, saved);
        }
    }

    // Canonicalised too: a dismissal recorded under the pre-rename name still
    // answers for the same printer.
    const std::string flag = PrinterDetector::canonical_type_name(
        cfg->get<std::string>(cfg->df() + helix::wizard::TYPE_MISMATCH_SHOWN_FOR, ""));

    auto detected = PrinterDetector::auto_detect(hardware);
    const auto decision = detected.detected()
                              ? PrinterDetector::classify_type_mismatch(saved, detected.type_name,
                                                                        detected.confidence, flag)
                              : PrinterDetector::MismatchDecision::NoDetection;
    if (decision != PrinterDetector::MismatchDecision::Warn) {
        // info, not debug: this runs once per discovery pass, and it is the line
        // that answers "why was there no prompt?" in a bundle.
        spdlog::info("[Application] No type mismatch prompt: detected '{}' at {}% (runner-up '{}' "
                     "at {}%), saved '{}', dismissed-for '{}', need >={}% - {}",
                     detected.type_name, detected.confidence, detected.runner_up_type_name,
                     detected.runner_up_confidence, saved, flag,
                     PrinterDetector::MISMATCH_MIN_CONFIDENCE,
                     PrinterDetector::mismatch_decision_name(decision));
        return;
    }

    // Session guard: one prompt per boot regardless of which button dismisses it.
    m_type_mismatch_shown = true;
    spdlog::info("[Application] Printer type mismatch: saved '{}' but detected '{}' ({}%)", saved,
                 detected.type_name, detected.confidence);

    // modal_confirm takes a plain const char* - compose the parameterized body
    // first (fmt::runtime: the format string is the translated handle, not a
    // compile-time literal).
    const std::string body =
        fmt::format(fmt::runtime(lv_tr("This printer looks like a {} ({}% confidence), but it is "
                                       "set up as a {}. A wrong type applies incorrect pre-print "
                                       "options and presets.")),
                    detected.type_name, detected.confidence, saved);

    helix::ui::modal_confirm(
        lv_tr("Printer type mismatch"), body.c_str(), ModalSeverity::Warning, lv_tr("Re-identify"),
        [this] {
            // Settle first: the wizard tears itself down asynchronously, and a
            // crash mid-run must not leave the prompt pending forever.
            settle_type_mismatch_warning();
            // Build the wizard AFTER the modal's exit animation, not inside the
            // click that started it: the dialog's own close only marks the
            // backdrop exiting, so creating the full-screen wizard here would
            // put it underneath a still-fading backdrop (same 300 ms one-shot
            // as launch_deferred_hardware_setup).
            lv_timer_t* launch = lv_timer_create(
                [](lv_timer_t* t) {
                    auto* self = static_cast<Application*>(lv_timer_get_user_data(t));
                    lv_timer_delete(t);
                    self->launch_type_reidentify_wizard();
                },
                300, this);
            lv_timer_set_repeat_count(launch, 1);
        },
        [this] {
            // Declining is final for this saved type. Keeping the type is a
            // deliberate choice (a heavily modified printer can legitimately
            // outvote a 70% heuristic), and the persisted flag stops the
            // prompt from re-appearing every boot. Re-identify remains
            // available via the full `--wizard` run.
            settle_type_mismatch_warning();
            spdlog::info("[Application] Type mismatch warning declined");
        },
        lv_tr("Keep current"), nullptr, m_async_lifetime.token());
}

void Application::launch_type_reidentify_wizard() {
    spdlog::info("[Application] Launching printer re-identify wizard");
    ui_wizard_register_event_callbacks();
    ui_wizard_container_register_responsive_constants();
    ui_wizard_init_subjects();
    // Back on the first targeted step has nothing to retreat to, so give it the
    // same dismiss semantics as the deferred hardware-setup session.
    set_wizard_cancel_callback([]() {
        ui_wizard_complete_targeted();
        set_wizard_cancel_callback(nullptr);
    });
    Application* app = this;
    ui_wizard_create_targeted(m_screen, {helix::wizard::StepId::PrinterIdentify}, [app]() {
        set_wizard_cancel_callback(nullptr);
        // The identify step's cleanup already persisted PRINTER_TYPE and applied
        // the new preset (ui_wizard_printer_identify.cpp cleanup). The preset
        // rewrote fan/heater role keys, so rebind the runtime mappings the same
        // way the deferred hardware-setup session does.
        app->reapply_hardware_roles();
    });
}

void Application::setup_discovery_callbacks() {
    IMoonrakerClient* client = m_moonraker->client();
    IMoonrakerAPI* api = m_moonraker->api();

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
            // A new discovery cycle is starting — re-arm the once-per-connection
            // targeted hardware-reconfig wizard guard so a reconnect can re-offer it.
            app->m_targeted_reconfig_shown = false;
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
        helix::ui::queue_update("Application::on_discovery_complete", [api, client, app, snapshot,
                                                                       status_snapshot]() {
            // Count invocations so crash bundles reveal whether we crashed on
            // the first discovery or on a reconnect-triggered re-run.
            static int s_discovery_complete_n = 0;
            long n = static_cast<long>(++s_discovery_complete_n);
            crash_handler::breadcrumb::note("disc", "cb_begin", n);

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
            crash_handler::breadcrumb::note("disc", "pre_api_hw",
                                            static_cast<long>(snapshot->macros().size()));
            api->hardware() = *snapshot;
            crash_handler::breadcrumb::note("disc", "post_api_hw", n);

            // Hardware-shape fingerprint: detect "reconnect with same hardware"
            // so user-facing side-effects (LED chip population, hardware
            // validation toasts, targeted reconfig wizard, telemetry snapshots)
            // can skip. See compute_hardware_fingerprint() in
            // hardware_fingerprint.h for rationale.
            // Computed from api->hardware() (post-copy) — *snapshot is moved
            // into set_hardware below and is empty after that point.
            const size_t new_fingerprint = helix::compute_hardware_fingerprint(api->hardware());
            const bool hw_changed = app->m_first_discovery_complete ||
                                    (new_fingerprint != app->m_last_hardware_fingerprint);
            app->m_last_hardware_fingerprint = new_fingerprint;
            app->m_first_discovery_complete = false;
            crash_handler::breadcrumb::note("disc", "hw_changed", hw_changed ? 1L : 0L);
            if (hw_changed) {
                spdlog::info("[Application] on_discovery_complete #{} — hardware shape changed "
                             "(fingerprint=0x{:x}), running full pipeline",
                             n, new_fingerprint);
            } else {
                spdlog::info("[Application] on_discovery_complete #{} — hardware shape unchanged "
                             "(fingerprint=0x{:x}), skipping user-facing side-effects",
                             n, new_fingerprint);
            }

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
            crash_handler::breadcrumb::note("disc", "pre_set_hw",
                                            static_cast<long>(snapshot->macros().size()));
            get_printer_state().set_hardware(std::move(*snapshot));
            crash_handler::breadcrumb::note("disc", "post_set_hw", n);
            const auto& fans = hw.fans();
            get_printer_state().init_fans(
                fans, helix::FanRoleConfig::from_config(Config::get_instance(), fans),
                hw.fan_max_power());
            crash_handler::breadcrumb::note("disc", "post_init_fans",
                                            static_cast<long>(hw.fans().size()));

            // Turn on the firmware's own z-offset persistence once per session,
            // only when idle. Some firmwares store the offset themselves but ship
            // with reload-at-print-start off, so adjustments made here would not
            // survive. Which printers need it, and what to send, lives in
            // include/z_offset_persistence.h. The print_active subject is not yet
            // applied from this discovery's status (see the reconfig-wizard gate
            // below), so consult status_snapshot directly to avoid injecting gcode
            // over a live print.
            {
                bool print_active =
                    lv_subject_get_int(get_printer_state().get_print_active_subject()) != 0;
                if (status_snapshot &&
                    helix::PrinterPrintState::status_indicates_active_print(*status_snapshot)) {
                    print_active = true;
                }
                const std::string enable_gcode =
                    helix::zoffset::persistence_enable_gcode(api->hardware());
                if (helix::zoffset::should_enable_persistence(!enable_gcode.empty(), print_active,
                                                              app->m_zoffset_persistence_enabled)) {
                    app->m_zoffset_persistence_enabled = true;
                    spdlog::info("[ZOffset] Enabling firmware z-offset persistence ({})",
                                 helix::zoffset::persistence_provider_name(api->hardware()));
                    // Fire-and-forget: callbacks are LOG-ONLY and capture nothing that
                    // can dangle, so the background response thread is lifetime-safe.
                    api->execute_gcode(
                        enable_gcode,
                        []() { spdlog::info("[ZOffset] Firmware z-offset persistence enabled"); },
                        [](const MoonrakerError& err) {
                            spdlog::warn("[ZOffset] Failed to enable z-offset persistence: {}",
                                         err.message);
                        },
                        0, /*silent=*/true, /*on_queued=*/nullptr,
                        /*caller_surfaces_errors=*/false);
                }
            }

            // Seed temperature graphs from Moonraker's cached history so they are
            // populated immediately instead of filling in live over several
            // minutes (#944). Fired after init_fans so heater/sensor subjects
            // exist. The RPC success callback runs on the WebSocket thread, so it
            // marshals the parsed store to the main thread before touching the
            // (main-thread-only) history manager. Re-runs naturally on reconnect
            // because discovery re-runs.
            client->get_temperature_store(
                [](const TemperatureStore& store) {
                    auto store_copy = std::make_shared<TemperatureStore>(store);
                    helix::ui::queue_update("Application::seed_temperature_store", [store_copy]() {
                        auto* mgr = get_temperature_history_manager();
                        if (mgr == nullptr) {
                            return;
                        }
                        using namespace std::chrono;
                        int64_t now_ms =
                            duration_cast<milliseconds>(system_clock::now().time_since_epoch())
                                .count();
                        mgr->seed_from_store(*store_copy, now_ms);
                        // Persistent graphs (home dashboard widget, filament mini
                        // graph) are built at startup before this seed arrives, so
                        // their construction-time backfill was empty. Re-backfill
                        // them now that history is available (#1124).
                        helix::TempGraphController::refresh_all_from_history();
                    });
                },
                [](const MoonrakerError& err) {
                    spdlog::debug("[Application] server.temperature_store seed failed: {}",
                                  err.message);
                });

            // Dispatch initial subscription status AFTER init_fans so fan/sensor subjects
            // exist when the status data is processed. The initial status is passed from the
            // discovery sequence rather than dispatched separately to guarantee ordering.
            // Flagged as a cached snapshot: it was captured on the background
            // thread when the subscribe response landed and has been carried
            // through the rest of discovery, so it can be seconds stale by the
            // time it lands here. Live WebSocket frames have been updating the
            // same state the whole time — this replay must not walk a liveness
            // signal (klippy state) backwards.
            if (!(*status_snapshot).empty()) {
                client->dispatch_status_update((*status_snapshot), /*from_cached_snapshot=*/true);
            }
            crash_handler::breadcrumb::note("disc", "post_status_dispatch", n);

            get_printer_state().set_klipper_version(hw.software_version());
            get_printer_state().set_moonraker_version(hw.moonraker_version());
            if (!hw.os_version().empty()) {
                get_printer_state().set_os_version(hw.os_version());
            }

            // Populate LED chips now that hardware is discovered.
            // Gated on hw_changed — LED chip topology doesn't change reconnect-to-
            // reconnect unless the hardware shape changed, and populate_led_chips
            // fires LED capability subjects that cascade into panel rebuilds.
            if (hw_changed) {
                get_global_settings_panel().populate_led_chips();
            }
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
            //
            // Gated on hw_changed — printer type detection is purely a function of the
            // hardware shape, so re-running on a reconnect with unchanged hardware would
            // produce the same result (and trigger config writes + subjects).
            // NOTE: use api->hardware() — snapshot was std::move'd into it above (#789).
            // Reading *snapshot here would pass an empty/moved-from PrinterDiscovery and
            // detection would fail with "0 sensors, 0 fans, hostname ''" (#802).
            if (hw_changed) {
                PrinterDetector::auto_detect_and_save(api->hardware(), Config::get_instance());
            }

            // Auto-heal + persist heater roles (batched single save, symmetry with fan roles
            // above). Ensures the validator sees resolved heater names so it does not emit a
            // toast every boot for a stale saved role that has a confident replacement.
            // Gated on hw_changed — healing is purely a function of heater hardware shape.
            if (hw_changed) {
                const auto& heaters = hw.heaters();
                auto* cfg = Config::get_instance();
                bool heater_changed = false;
                for (auto id :
                     {helix::HardwareRoleId::HotendHeater, helix::HardwareRoleId::BedHeater}) {
                    const auto* desc = helix::role_descriptor(id);
                    if (!desc)
                        continue;
                    const std::string key = cfg->df() + desc->config_key;
                    const std::string dflt = desc->canonical_default
                                                 ? std::string(desc->canonical_default)
                                                 : std::string();
                    const std::string saved = cfg->get<std::string>(key, dflt);
                    std::string healed = helix::resolve_role_from_config(id, cfg, heaters, false);
                    if (!healed.empty() && healed != saved) {
                        cfg->set<std::string>(key, healed);
                        heater_changed = true;
                    }
                }
                if (heater_changed && !cfg->save()) {
                    spdlog::warn("[Application] Failed to persist heater role heals");
                }
            }

            // Pay off a hardware snapshot the wizard deferred because Klipper was
            // down during setup (#1160). Everything discovered now was present all
            // along, so accepting it BEFORE validate() keeps this first successful
            // discovery clean instead of reporting every fan, filament sensor and
            // LED as newly appeared. The marker itself is cleared only once the
            // user answers the offer below, so a session killed before answering
            // still gets asked next boot.
            const bool hardware_setup_deferred =
                helix::wizard_hardware_setup_deferred(Config::get_instance());
            if (hardware_setup_deferred && !Config::get_instance()->is_wizard_required() &&
                !is_wizard_active()) {
                size_t accepted = HardwareValidator::acknowledge_discovered_hardware(
                    Config::get_instance(), api->hardware());
                spdlog::info("[Application] Deferred wizard hardware snapshot written "
                             "({} object(s) accepted)",
                             accepted);
            }

            // Hardware validation: check config expectations vs discovered hardware.
            // Now uses the post-preset config so preset-mapped fan/heater names are
            // checked against discovery, not the pre-preset scaffolded defaults.
            HardwareValidator validator;
            auto validation_result = validator.validate(Config::get_instance(), api->hardware());
            get_printer_state().set_hardware_validation_result(validation_result);

            // Gated on hw_changed — without this guard, any hardware issue toast
            // (e.g. "expected fan not found") would re-fire on every reconnect even
            // when nothing has changed. validate() still runs (cheap, caches result,
            // updates the validation_result subject for the UI); only the user-facing
            // notify is suppressed on unchanged hardware. The validator's session
            // snapshot below tracks state across runs regardless.
            if (validation_result.has_issues() && hw_changed &&
                !Config::get_instance()->is_wizard_required() && !is_wizard_active()) {
                validator.notify_user(validation_result);
            }

            // Route unresolved GUIDED hardware roles (a saved fan/heater role with no
            // confident live substitute) into the targeted reconfig wizard — only the
            // affected step(s), not the full first-run wizard. Skipped while the first-run
            // wizard is required/active, and gated to once-per-connection so it does not
            // relaunch on reconnect churn within a single session.
            //
            // Also gated on hw_changed: unresolved guided steps are purely a function of
            // (saved config, current hardware). If hardware is unchanged since the last
            // discovery, the result is identical and re-launching the wizard would just
            // harass the user. The wizard cancel callback persists the decline, so the
            // next discovery with the SAME hardware would also see no unresolved steps —
            // the hw_changed gate is defense-in-depth for the rare case where a prior
            // session was killed before the decline could be persisted.
            auto reconfig_steps = helix::unresolved_guided_steps(Config::get_instance(), hw);
            // Idle gate: NEVER launch the reconfig wizard over a live print.
            // The print_active subject is NOT yet updated from this discovery's
            // initial status — dispatch_status_update() above only QUEUES the status
            // (m_notification_queue); print_active_ is applied a tick later in
            // process_notifications, AFTER this queue_update returns. On a fresh
            // connection mid-print (app/panel restart during a print) the subject
            // still reads its initialized 0. Consult the just-arrived status directly
            // so a reconfig wizard never launches over a live print.
            bool print_active =
                lv_subject_get_int(get_printer_state().get_print_active_subject()) != 0;
            if (status_snapshot &&
                helix::PrinterPrintState::status_indicates_active_print(*status_snapshot)) {
                print_active = true;
            }
            if (hw_changed && !reconfig_steps.empty() && !print_active &&
                !Config::get_instance()->is_wizard_required() && !is_wizard_active() &&
                !app->m_targeted_reconfig_shown) {
                app->m_targeted_reconfig_shown = true;
                ui_wizard_register_event_callbacks();
                ui_wizard_container_register_responsive_constants();
                ui_wizard_init_subjects();
                // Cancel = dismiss the targeted session. Without this, Back on the first
                // targeted step is a no-op (no prior step to retreat to). Cancelling an
                // UNSATISFIABLE role (no candidate hardware exists) must also record the
                // decision — otherwise the wizard re-launches on every reconnect forever.
                // decline_unresolved_guided_roles() writes "" (declined) for each still-
                // Unresolved guided role with a present saved key, read against the CURRENT
                // discovered hardware (api->hardware()) at cancel time.
                set_wizard_cancel_callback([api]() {
                    // Record the cancel as a decline for each still-Unresolved guided
                    // role (writes "" + saves internally). reapply_hardware_roles() is
                    // intentionally NOT called here: ui_wizard_complete_targeted() fires
                    // the on_complete callback (registered in ui_wizard_create_targeted
                    // below), which already reapplies the roles. Calling it here too
                    // would be a redundant double reapply.
                    helix::decline_unresolved_guided_roles(Config::get_instance(), api->hardware());
                    ui_wizard_complete_targeted();
                    set_wizard_cancel_callback(nullptr);
                });
                ui_wizard_create_targeted(app->m_screen, reconfig_steps, [app]() {
                    set_wizard_cancel_callback(nullptr);
                    app->reapply_hardware_roles();
                });
            }

            // Offer the hardware steps the Klipper-down wizard could not show
            // (#1160). The user skipped them because their printer was broken,
            // not because they had nothing to choose. Gated exactly like the
            // reconfig wizard above (idle, no wizard running, once per session),
            // plus reconfig_steps.empty() so the two never stack — if a reconfig
            // session just launched, the offer waits for the next boot.
            if (hardware_setup_deferred && hw_changed && !print_active &&
                !Config::get_instance()->is_wizard_required() && !is_wizard_active() &&
                reconfig_steps.empty() && !app->m_hardware_setup_prompt_shown) {
                auto steps = ui_wizard_deferred_hardware_steps();
                app->m_hardware_setup_prompt_shown = true;
                if (steps.empty()) {
                    // Nothing left to ask about (a preset already answers these,
                    // or the printer has none of the optional hardware). Settle
                    // the debt silently rather than showing a dead-end dialog.
                    spdlog::info("[Application] Deferred hardware setup has no steps to offer; "
                                 "settling silently");
                    app->settle_deferred_hardware_setup();
                } else {
                    app->prompt_deferred_hardware_setup(std::move(steps));
                }
            }

            // Saved printer type vs detected hardware (bundle F2LNLQCC: a Voron
            // Trident saved as "FlashForge Adventurer 5M Pro" silently received
            // AD5M pre-print options, presets, and screws-tilt direction on every
            // boot — auto_detect_and_save self-guards on a saved type and never
            // re-checks). One actionable prompt per saved type. Gated exactly
            // like the reconfig wizard and deferred offer above — plus
            // reconfig_steps.empty() and !hardware_setup_deferred so the three
            // never stack in one discovery pass — and on the same hw_changed
            // gate: detection is purely a function of the hardware shape.
            if (hw_changed && !print_active && reconfig_steps.empty() && !hardware_setup_deferred &&
                !Config::get_instance()->is_wizard_required() && !is_wizard_active() &&
                !app->m_type_mismatch_shown) {
                app->maybe_warn_type_mismatch(api->hardware());
            }

            // Save session snapshot for next comparison (even if no issues)
            validator.save_session_snapshot(Config::get_instance(), api->hardware());
            crash_handler::breadcrumb::note("disc", "post_validate", n);

            // Record telemetry session event now that hardware data is available
            // (hardware_profile is deferred until after build volume is fetched below).
            // record_session always runs (counts sessions, including reconnects).
            // settings_snapshot + memory_snapshot are gated on hw_changed — they
            // capture a fingerprint of the current config + heap state for telemetry,
            // and would just re-record identical data on a reconnect with unchanged
            // hardware.
            TelemetryManager::instance().record_session();
            if (hw_changed) {
                TelemetryManager::instance().record_settings_snapshot();
                TelemetryManager::instance().record_memory_snapshot("session_start");
            }
            crash_handler::breadcrumb::note("disc", "post_telemetry", n);

            // Fetch safety limits and build volume from Klipper config (stepper ranges,
            // min_extrude_temp, max_temp, etc.) — runs for ALL discovery completions
            // (normal startup AND post-wizard) so we don't duplicate this in callers
            if (api) {
                IMoonrakerAPI* api_ptr = api;
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
                if (!backend || !backend->supports_per_tool_spool_assignment())
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
                    // Tool-changer auto-assign runs BEFORE the bypass gate, not
                    // after it. The gate passes only when
                    // active_spool_describes_bypass() is true, which is
                    // `no backend || any_bypass_active()` — and every backend that
                    // answers supports_per_tool_spool_assignment() (TOOL_CHANGER,
                    // SNAPMAKER) hardcodes is_bypass_active() to false. Downstream
                    // of the gate the assign therefore required "a backend exists"
                    // and "no backend exists" at once, so it never ran on a real
                    // changer. The two concerns are independent: the gate is about
                    // which slot owns the EXTERNAL spool record, this is about
                    // which spool is mounted on the active TOOL. Its own guards
                    // (per-tool support, assignments loaded, valid index, not
                    // already assigned) are what decide whether it acts.
                    try_assign_active_spool_to_tool(spool);

                    // An AMS slot assignment sets Moonraker's global active spool
                    // too, so mirroring it onto the bypass unconditionally used to
                    // overwrite the bypass with whichever lane was assigned last.
                    if (!AmsState::instance().active_spool_describes_bypass()) {
                        spdlog::debug("[Application] Active spool {} belongs to a lane, not the "
                                      "bypass — not syncing external spool",
                                      spool.id);
                        return;
                    }
                    // This record is the freshest view of the spool we will get
                    // — it arrives from the startup sync and from every
                    // notify_active_spool_set. Refresh the identity side
                    // channel from it (invalidate first: cache_identity() is
                    // insert-if-absent, so a stale entry would win otherwise).
                    SpoolmanManager::invalidate_identity(spool.id);
                    if (SpoolmanManager::cache_identity(spool)) {
                        // Tell the label consumers a name they could not resolve
                        // before is available now (#1264).
                        AmsState::instance().bump_slots_version();
                    }

                    SlotInfo slot;
                    slot.slot_index = -2;
                    slot.global_index = -2;
                    // apply_spool_to_slot() owns the whole identity copy,
                    // multi-colour included. Overwriting spool_name with
                    // display_name() here used to put "Polymaker PLA - Jet
                    // Black" on a field the lane_data schema, AFC and Happy
                    // Hare all read as the bare filament name.
                    apply_spool_to_slot(slot, spool);
                    AmsState::instance().set_external_spool_info(slot);
                    spdlog::info("[Application] External spool {}: {} (id={})", log_context,
                                 slot.spool_name, slot.spoolman_id);
                });
            };

            // Sync external spool from Moonraker's active Spoolman spool
            // This ensures the filament panel shows the correct spool on startup,
            // even if the active spool was changed via Spoolman's web UI or another client
            {
                IMoonrakerAPI* api_for_spool = api;
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
                IMoonrakerAPI* api_for_notify = api;
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
                            // Same global-vs-bypass confusion as the sync arm, with
                            // a worse blast radius: clearing an AMS lane makes
                            // commit_slot_edit post set_active_spool(0), which comes
                            // straight back as this notification. Taken at face
                            // value it erased the whole bypass record — one tap on a
                            // lane's "Clear Spool" and the user's bypass assignment
                            // was gone.
                            helix::ui::queue_update([]() {
                                auto& ams = AmsState::instance();
                                if (!ams.active_spool_describes_bypass()) {
                                    spdlog::debug("[Application] Active spool cleared for a lane, "
                                                  "not the bypass — keeping external spool");
                                    return;
                                }
                                spdlog::info("[Application] Active spool cleared via notification");
                                ams.clear_external_spool_info();
                            });
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
            IMoonrakerAPI* api_ptr_zoffset = api;
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

    // Discovery callbacks are already registered (setup_discovery_callbacks in init_moonraker).
    // The display-wake reconnect callback is registered once in init_display(), not here —
    // connect_moonraker() re-runs on every printer switch and DisplayManager has no
    // unregister path.

    // Set HTTP base URL for API
    IMoonrakerAPI* api = m_moonraker->api();
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
    IMoonrakerClient* client = m_moonraker->client();
    IMoonrakerAPI* api = m_moonraker->api();

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
            // Action-prompt buttons run firmware macros (IFS load/unload, color
            // changes, tool changes) that routinely exceed the 60s default
            // request timeout — heat + cut + multi-stage feed/retract/purge can
            // take minutes. Use the macro timeout so a slow-but-progressing
            // operation isn't falsely reported as failed/stalled (raza616's IFS
            // unload via the ZMOD COLOR macro timed out at exactly 60s).
            api->execute_gcode(
                gcode, []() { spdlog::debug("[ActionPrompt] Gcode executed successfully"); },
                [gcode](const MoonrakerError& err) {
                    spdlog::error("[ActionPrompt] Gcode execution failed: {}", err.message);
                    // The modal already closed on the button press, and this
                    // error_cb marks the call caller-handled so the `!!`
                    // GcodeError toast is suppressed for the same failure.
                    // Without this the user sees nothing at all.
                    helix::ui::report_action_prompt_gcode_failure(err.user_message());
                },
                IMoonrakerAPI::MACRO_TIMEOUT_MS);
        });
    }

    // Wire on_show callback to display modal (uses ui_queue_update() for thread safety)
    m_action_prompt_manager->set_on_show([this](const helix::PromptData& data) {
        spdlog::info("[ActionPrompt] Showing prompt: {}", data.title);
        // WebSocket callbacks run on background thread - must use ui_queue_update
        m_async_lifetime.defer("Application::action_prompt_show", [this, data]() {
            lv_obj_t* screen = lv_screen_active();
            if (m_action_prompt_modal && screen) {
                m_action_prompt_modal->show_prompt(screen, data);
            }
        });
    });

    // Wire on_close callback to hide modal
    m_action_prompt_manager->set_on_close([this]() {
        spdlog::info("[ActionPrompt] Closing prompt");
        m_async_lifetime.defer("Application::action_prompt_close", [this]() {
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
    if (const char* on = std::getenv("HELIX_AUTO_STRESS_PROMPT");
        on && *on && std::string(on) != "0") {
        int period_ms = 500;
        if (const char* p = std::getenv("HELIX_STRESS_PROMPT_MS")) {
            try {
                period_ms = std::max(100, std::stoi(p));
            } catch (...) {
            }
        }
        int delay_ms = 30000; // default 30s so user can navigate to Print Status first
        if (const char* d = std::getenv("HELIX_STRESS_START_DELAY_SEC")) {
            try {
                delay_ms = std::max(0, std::stoi(d)) * 1000;
            } catch (...) {
            }
        }
        spdlog::warn("[ActionPrompt] HELIX_AUTO_STRESS_PROMPT=1 — will drive show/hide every {}ms "
                     "after {}s delay (gated on is_showing)",
                     period_ms, delay_ms / 1000);
        // Gate transitions on ActionPromptManager state so we alternate
        // cleanly instead of stacking modals when the exit animation is
        // slower than our period. ActionPromptManager::is_showing() is the
        // canonical "is a prompt active right now" probe.
        struct StressCtx {
            ActionPromptManager* mgr;
            int period_ms;
        };
        auto* ctx = new StressCtx{prompt_mgr, period_ms};
        // One-shot kick-off timer; on fire it spawns the repeating stress
        // timer. Gives the operator time to navigate before modals start
        // hijacking the screen.
        auto* kickoff = lv_timer_create(
            [](lv_timer_t* t) {
                auto* c = static_cast<StressCtx*>(lv_timer_get_user_data(t));
                if (!c) {
                    lv_timer_delete(t);
                    return;
                }
                spdlog::warn("[ActionPrompt] Stress timer ARMED — show/hide cycle starting now");
                auto* repeat = lv_timer_create(
                    [](lv_timer_t* t2) {
                        auto* mgr = static_cast<ActionPromptManager*>(lv_timer_get_user_data(t2));
                        if (!mgr)
                            return;
                        if (ActionPromptManager::is_showing()) {
                            mgr->process_line("// action:prompt_end");
                        } else {
                            mgr->process_line("// action:prompt_begin StressTest");
                            mgr->process_line("// action:prompt_text Cluster A reproducer");
                            mgr->process_line("// action:prompt_button OK|ECHO_OK");
                            mgr->process_line("// action:prompt_show");
                        }
                    },
                    c->period_ms, c->mgr);
                (void)repeat;
                delete c;
                lv_timer_delete(t); // one-shot
            },
            std::max(1, delay_ms), ctx);
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

    // Recovery modal presenter: source-agnostic owner of the CRITICAL recovery
    // modal (AFC jam, CFS key840, etc.). Created before GcodeErrorRouter so the
    // presenter outlives the router if both are reset in the wrong order.
    // Not re-created on printer switch — the modal persists across reconnects.
    if (!m_recovery_presenter) {
        m_recovery_presenter = std::make_unique<helix::ui::RecoveryModalPresenter>(api);
    }

    // Klipper `!!` / `Error:` lines flow through GcodeErrorRouter, which
    // also replays the most recent gcode_store error when the WS (re)connects
    // (catches errors that fired while HelixScreen was offline). Lives as
    // a member so its dtor unregisters callbacks before MoonrakerClient dies.
    m_gcode_error_router =
        std::make_unique<helix::GcodeErrorRouter>(api, client, *m_recovery_presenter);

    // Narration router: maps `//` toolchange narration to the active AMS
    // backend's step model and drives the toolchange_step subject. Separate
    // handler key from the error router; ignores `!!` / `Error:` lines.
    m_gcode_narration_router = std::make_unique<helix::GcodeNarrationRouter>(api, client);

    // Firmware-brokered LAN pairing: on machines whose firmware asks the
    // printer's own screen to approve a slicer or phone app before letting it
    // in, HelixScreen is now that screen. Without this the request is
    // broadcast to nobody and pairing hangs. Inert on firmwares that never
    // send one — the notification is its own capability probe.
    m_lan_client_auth_router = std::make_unique<helix::LanClientAuthRouter>(client);

    // AMS error bridge: observes AmsState's action subject and surfaces
    // AmsAction::ERROR from STATUS-driven backends (IFS, QIDI, etc.) via the
    // recovery modal. Complements GcodeErrorRouter which handles `!!` lines.
    m_ams_error_bridge = std::make_unique<helix::AmsErrorBridge>(*m_recovery_presenter);
    m_ams_error_bridge->start();

    // Register layer tracking fallback via gcode responses.
    // Some slicers don't emit SET_PRINT_STATS_INFO, so Moonraker's print_stats.info
    // never updates current_layer. This parses gcode responses as a fallback.
    client->register_method_callback(
        "notify_gcode_response", "layer_tracker", [this](const nlohmann::json& msg) {
            if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
                return;
            }

            // Only track layers while printing or paused
            // RAW_PRINT_STATE_OK: layer tracking. There are no layers during a
            // preparing window, and admitting one would derive a layer number
            // from the pre-print block's own Z moves.
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
                // Clearing the flag here also stops the post-signal handoff block
                // below from repainting a second time.
                if (invalidation_suppressed) {
                    invalidation_suppressed = false;
                    lv_display_enable_invalidation(nullptr, true);
                }
                restore_flush_callback(); // no-op on the DRM path (flush never swapped)
                if (lv_obj_t* screen = lv_screen_active()) {
                    lv_obj_update_layout(screen);
                    invalidate_all_recursive(screen);
                    lv_refr_now(nullptr);
                }
            }

            // Signal splash to exit when discovery completes (or timeout)
            m_splash_manager.check_and_signal();

            // Post-splash handoff: re-enable rendering and repaint
            // Display invalidation was suppressed to prevent framebuffer flicker
            // while both splash and main app were running simultaneously.
            if (invalidation_suppressed && m_splash_manager.needs_post_splash_refresh()) {
                invalidation_suppressed = false;
                lv_display_enable_invalidation(nullptr, true);
                restore_flush_callback();
                spdlog::info(
                    "[Application] Post-splash handoff: flush callback restored, painting UI");

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
            const char* type_name = current_exception_type_name();
            spdlog::error("[Application] Caught exception in main loop: {} ({})", e.what(),
                          type_name);
            crash_handler::breadcrumb::note("loop_catch", type_name);
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
                    lv_tr("An internal error occurred. The app continues running — "
                          "please send a debug bundle from Settings > About if it repeats."),
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
    if (m_moonraker) {
        // Mark the disconnect as expected so the DISCONNECTED notification
        // (queued here, drained on resume) doesn't clear the overlay stack
        // and bounce the user to home (#1245).
        NavigationManager::instance().mark_disconnect_expected();
        m_moonraker->client()->disconnect();
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
    if (m_moonraker && m_moonraker->client()) {
        m_last_force_reconnect = std::chrono::steady_clock::now();
        m_moonraker->client()->force_reconnect();
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

namespace {

/**
 * @brief RAII latch for Application::m_soft_restart_in_progress.
 *
 * Every soft-restart path runs tear_down_printer_state() + init_printer_state(),
 * a 16-step rebuild that includes a Moonraker connect and can throw. Clearing the
 * re-entrancy flag by hand on each exit path leaves it stuck true whenever an exit
 * is not the one that was hand-coded, and a stuck flag makes every later printer
 * switch or add a silent no-op until the process restarts.
 */
class SoftRestartLatch {
  public:
    explicit SoftRestartLatch(bool& flag) : m_flag(flag) {
        m_flag = true;
    }
    ~SoftRestartLatch() {
        m_flag = false;
    }

    SoftRestartLatch(const SoftRestartLatch&) = delete;
    SoftRestartLatch& operator=(const SoftRestartLatch&) = delete;
    SoftRestartLatch(SoftRestartLatch&&) = delete;
    SoftRestartLatch& operator=(SoftRestartLatch&&) = delete;

  private:
    bool& m_flag;
};

} // namespace

void Application::switch_printer(const std::string& printer_id) {
    if (m_soft_restart_in_progress) {
        spdlog::warn("[Application] Ignoring switch_printer during active soft restart");
        return;
    }
    SoftRestartLatch soft_restart(m_soft_restart_in_progress);

    spdlog::info("[Application] Switching to printer '{}'...", printer_id);

    // Validate printer exists in config
    if (!m_config->set_active_printer(printer_id)) {
        spdlog::error("[Application] Failed to switch — unknown printer '{}'", printer_id);
        return;
    }
    m_config->save();

    // Per-printer state lives at /printers/<active>/… and is reached via Config::df().
    // The active printer just changed, so df() now points at the new printer — fire every
    // registered per-printer cache invalidator BEFORE teardown, while df() is already
    // correct, so nothing keeps serving the previous printer's values. PanelWidgetManager's
    // cached layouts (#804) were the first instance of this; the registry is what stops the
    // next one from being a latent bug nobody remembers to wire up.
    helix::PrinterCacheRegistry::instance().invalidate_all();

    tear_down_printer_state();
    init_printer_state();

    // Navigate to home
    NavigationManager::instance().set_active(PanelId::Home);

    // Show toast with the new printer name
    std::string printer_name =
        m_config->get<std::string>(m_config->df() + "printer_name", printer_id);
    std::string toast_msg = fmt::format(fmt::runtime(lv_tr("Connected to {}")), printer_name);
    ToastManager::instance().show(ToastSeverity::INFO, toast_msg.c_str());

    spdlog::info("[Application] Switched to printer '{}'", printer_id);
}

void Application::add_printer_via_wizard() {
    if (m_soft_restart_in_progress) {
        spdlog::warn("[Application] Ignoring add_printer_via_wizard during active soft restart");
        return;
    }
    SoftRestartLatch soft_restart(m_soft_restart_in_progress);

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

    // Same active-printer change as switch_printer(): Config::df() has moved to the new
    // entry, so every per-printer cache must be dropped before teardown.
    helix::PrinterCacheRegistry::instance().invalidate_all();

    // Soft restart and launch wizard for the new printer.
    // init_printer_state() calls run_wizard() internally when is_wizard_required() returns true
    // (which it does for the new empty printer entry with wizard_completed=false).
    // Do NOT call run_wizard() again here — that creates a duplicate wizard container.
    tear_down_printer_state();

    // Register cancel callback AFTER tear_down (which clears it) but BEFORE init (which runs
    // wizard)
    set_wizard_cancel_callback([this]() { cancel_add_printer_wizard(); });

    init_printer_state();
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
    m_async_lifetime.defer("Application::cancel_add_printer_wizard", [this]() {
        SoftRestartLatch soft_restart(m_soft_restart_in_progress);

        set_wizard_active(false);
        ui_wizard_deinit_subjects();

        // set_active_printer() above restored the previous printer, so Config::df() moved
        // again — drop every per-printer cache before teardown.
        helix::PrinterCacheRegistry::instance().invalidate_all();

        tear_down_printer_state();
        init_printer_state();
        NavigationManager::instance().set_active(PanelId::Home);
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

    // 10b. Deinit LedAutoState before LedController/subjects — it observes
    //      PrinterState subjects and drives LedController in apply_action().
    helix::led::LedAutoState::instance().deinit();

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

    // 17b. Tear down GcodeErrorRouter before MoonrakerClient — its dtor
    //      unregisters the notify_gcode_response handler and the
    //      gcode_store_replay connected observer; both touch the client.
    //      Reset the router BEFORE the presenter (presenter must outlive router).
    //      AmsErrorBridge also holds a reference to the presenter — reset it first.
    m_gcode_narration_router.reset();
    m_lan_client_auth_router.reset();
    m_gcode_error_router.reset();
    m_ams_error_bridge.reset();
    m_recovery_presenter.reset();

    // 18. Release MoonrakerManager
    m_moonraker.reset();

    // 19. Reset KeyboardManager (widget pointers become dangling after tree delete)
    KeyboardManager::instance().reset();

    // 20. Delete LVGL widget tree (panels already released references)
    //     DO NOT call lv_deinit() — display stays alive.
    //     On the cancel_add_printer_wizard() soft-restart path this runs inside a
    //     queue_update() batch, so a synchronous lv_obj_del() would delete inside
    //     UpdateQueue::process_pending() and can corrupt LVGL's global event list
    //     alongside other batched deletions (#776/#190/#80). m_app_layout is also
    //     the Home widget grid — a #983-prone structure where a relayout racing
    //     teardown could iterate a freed container. safe_delete_subtree() detaches
    //     the tree off-screen synchronously (so the immediate init_printer_state()
    //     rebuild sees a clean m_screen), forces LV_LAYOUT_NONE, and frees it on
    //     the async path outside the batch.
    if (m_app_layout) {
        helix::ui::safe_delete_subtree(m_app_layout);
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

    // 7. Apply startup CLI actions (if any). Finalize the home panel here so
    //    its carousel + widget grid get built — HomePanel::setup() is
    //    deliberately minimal; finalize_setup() is what creates the visible
    //    content. Mirrors run()'s startup path; without it the home panel
    //    renders blank after a printer switch.
    if (!m_wizard_active) {
        apply_startup_cli_actions();
        get_global_home_panel().finalize_setup();
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

    // Expire the callbacks this object deferred to the main thread before any of
    // the subsystems they touch are torn down below (#1165).
    m_async_lifetime.invalidate();

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
    helix::RemoteControlServer::instance().stop();
#endif

    // Stop memory monitor
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

    // Detach page-scroll-buttons controllers (removes gutters + observers) while
    // panel widgets are still alive — must run before m_panels.reset() /
    // StaticPanelRegistry::destroy_all() below tear down the containers it holds
    // pointers to.
    helix::ui::PageScrollAutoInject::instance().shutdown();

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

    // Sweep any panel singleton a deinit callback lazily re-created on its way
    // out (a callback reaching through an auto-creating get_global_*_panel()
    // getter builds a replacement instance). Destroying it here, while LVGL and
    // spdlog are still up, keeps its destructor off the static-destruction path
    // where those subsystems are already gone. No-op when nothing resurrected.
    StaticPanelRegistry::instance().destroy_all();

    // Destroy JobQueueState AFTER deinit_all() so its registered cleanup lambda runs
    // while the object is still alive. Must still be before m_moonraker.reset() so
    // client unregistration works. (Mirrors soft-restart path ordering.)
    m_job_queue_state.reset();

    // Destroy runtime CJK fonts before LVGL shutdown
    helix::system::CjkFontManager::instance().shutdown();

    // NOTE: theme_manager_deinit() is deliberately NOT called here. It has to run
    // after m_display.reset() — see the call below.

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

    // Tear down GcodeErrorRouter before MoonrakerClient (its dtor unregisters
    // the live + replay callbacks; both touch the client).
    // Reset the router BEFORE the presenter (presenter must outlive router).
    // AmsErrorBridge also holds a reference to the presenter — reset it first.
    m_gcode_narration_router.reset();
    m_lan_client_auth_router.reset();
    m_gcode_error_router.reset();
    m_ams_error_bridge.reset();
    m_recovery_presenter.reset();

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

    // Theme manager subjects (theme_changed_subject, swatch descriptions) are
    // file-scope statics not tracked by StaticSubjectRegistry, so they are torn
    // down by hand — and only HERE, after the display is gone.
    //
    // They must outlive m_display.reset(), because that is what runs
    // lv_xml_deinit(): a component scope holding a <subject_expr> owns RAW
    // lv_observer_t* pointers (lv_xml_subject_expr_t::observers) attached to
    // these subjects, and releases them with lv_observer_remove(). Those raw
    // pointers are not ObserverGuards, so ObserverGuard::invalidate_all() above
    // does not cover them. Deinitialising the subjects first frees every
    // observer on them (lv_subject_deinit -> lv_observer.c:468) and the later
    // scope teardown then reads freed memory — a heap-use-after-free that
    // aborted every single `ctl shutdown`.
    //
    // Running last is safe: lv_xml_deinit() detaches the <subject_expr>
    // observers before lv_deinit(), and lv_deinit() removes the object-bound
    // ones, so these subjects have no subscribers left by the time we get here.
    theme_manager_deinit();

    // Uninstall crash handler last — clean shutdown reached this point, so a
    // SIGBUS/SIGSEGV after this is the kernel's problem, not ours.
    crash_handler::uninstall();

    spdlog::info("[Application] Shutdown complete");
}
