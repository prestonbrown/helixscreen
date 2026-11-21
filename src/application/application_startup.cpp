// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file application_startup.cpp
 * @brief Application startup phases after the display: widget and XML
 *        registration, translations, first-run wizard, CLI startup actions, and
 *        process, sound, monitoring and WiFi services.
 */

#include "ui_ams_mini_status.h"
#include "ui_bed_mesh.h"
#include "ui_card.h"
#include "ui_component_header_bar.h"
#include "ui_dialog.h"
#include "ui_filament_motion_glyph.h"
#include "ui_gcode_viewer.h"
#include "ui_gradient_canvas.h"
#include "ui_guided_device_flow.h"
#include "ui_humidity_drop.h"
#include "ui_icon.h"
#include "ui_ipv4_revert_retry.h"
#include "ui_keyboard_manager.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"
#include "ui_plate_glow.h"
#include "ui_severity_card.h"
#include "ui_status_pill.h"
#include "ui_switch.h"
#include "ui_temp_display.h"
#include "ui_tile_rung.h"
#include "ui_touch_calibration_overlay.h"
#include "ui_unit_art.h"
#include "ui_update_notify_modal.h"
#include "ui_wizard.h"
#include "ui_wizard_language_chooser.h"
#include "ui_wizard_touch_calibration.h"

#include "ams_backend.h"
#include "app_globals.h"
#include "application.h"
#include "audio_settings_manager.h"
#include "cjk_filename_glyphs.h"
#include "config.h"
#include "core/data_root_resolver.h"
#include "core/error_reporting.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "display_manager.h"
#include "helix_sparkline.h"
#include "lvgl_log_handler.h"
#include "main_loop_heartbeat.h"
#include "memory_monitor.h"
#include "memory_profiling.h"
#include "memory_utils.h"
#include "moonraker_api.h"
#include "moonraker_manager.h"
#include "printer_state.h"
#include "remote_screen_service.h"
#include "runtime_config.h"
#include "setting_group.h"
#include "sound_manager.h"
#include "system/crash_handler.h"
#include "system/crash_history.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "translation_loader.h"
#include "wifi_link_monitor.h"
#include "wifi_manager.h"
#include "wizard_config_paths.h"
#include "xml_hot_reloader.h"
#include "xml_registration.h"
#if HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif

#include "helix-xml/src/xml/lv_xml_translation.h"
#include "lvgl.h"

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "hv/json.hpp"

using namespace helix;

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
    helix::ui::gcode_viewer_register();
    ui_gradient_canvas_register();
    helix::ui::register_helix_sparkline_widget();
    helix::ui::register_unit_art_widget();
    helix::ui::register_humidity_drop_widget();
    helix::ui::register_filament_motion_glyph_widget();
    helix::ui::register_plate_glow_widget();

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

    // Guided flows must exist before any overlay builds: an action that names an unregistered
    // flow is refused rather than executed.
    helix::ui::register_builtin_guided_device_flows();

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

    // All language switching goes through lv_translation_set_language and the
    // per-locale XML files loaded below.

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

    helix::init_cjk_fonts(lang);

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
        lv_obj_move_to_index(keyboard, -1);
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
            lv_subject_set_string(checker.version_text_subject(), version_text);
            lv_subject_set_string(checker.release_notes_subject(), info->release_notes.c_str());
            lv_subject_set_int(checker.changelog_visible_subject(), 1);
            helix::ui::show_update_notification();
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

void Application::init_process_services() {
    // Initialize UpdateChecker before panel subjects (subjects must exist for XML binding)
    // On Android the checker still runs (so "Check for Updates" works), but
    // "Install Update" opens the store or release page the APK came from.
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
    // Before any backend exists: one built earlier sees none of its saved state.
    AmsBackend::init_persistent_state(user_config_dir);

    // Initialize TelemetryManager (opt-in, default OFF)
    // Note: record_session() is called after m_session.init_panel_subjects() so that
    // SettingsManager subjects are ready and the enabled state can be synced.
    // The identity lives in the state root, out of the install root an update
    // replaces and out of the Klipper config tree web UIs and backups expose.
    // Test mode keeps it in its own config dir, as it does the settings backups.
    TelemetryManager::instance().init(user_config_dir, get_runtime_config()->is_test_mode()
                                                           ? std::string{}
                                                           : app_get_state_root());
#if HELIX_ENABLE_SCREENSAVER
    TelemetryManager::instance().set_screensaver_usage(&ScreensaverManager::instance().usage());
#endif

    // First heap snapshot, before panel construction and XML load. Later
    // snapshots are diffed against this to narrow down which startup phase
    // burns allocator arena on small-RAM devices.
    TelemetryManager::instance().record_memory_snapshot("post_telemetry_init");
}

void Application::init_sound_services() {
    // Initialize SoundManager (audio feedback)
    // Backend detection (PWM, ALSA, SDL) is checked by PrinterCapabilitiesState
    // to show sound settings even without a Klipper beeper output_pin.
    SoundManager::instance().initialize();
    if (SoundManager::plays_startup_sound(get_runtime_config()->is_test_mode())) {
        SoundManager::instance().play("startup", SoundPriority::EVENT);
    }

    // Backend is now picked: seed the backend subjects so the Sound overlay's
    // device-row and Test Tracker bindings resolve correctly. Subjects init
    // before SoundManager, so the values are stale until this refresh.
    AudioSettingsManager::instance().refresh_backend_subjects();

    // Show sound settings immediately if a local backend exists,
    // without waiting for hardware discovery / Klipper connection.
    if (SoundManager::instance().has_backend()) {
        get_printer_state().capabilities_state().set_sound_backend_available(true);
    }
}

void Application::init_monitoring_services() {
    // Phase 14d: Built-in screen server, if the user shares the screen (#1825)
    helix::RemoteScreenService::instance().init(&m_display->remote_screen());
    helix::RemoteScreenService::instance().watch_print_state();

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
        uint32_t hang_ms = helix::MainLoopHangDetector::kDefaultThresholdMs;
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
        helix::HangMemoryContext memory;
        const auto sys = helix::get_system_memory_info();
        if (sys.total_kb > 0) {
            memory.mem_available_mb = static_cast<int64_t>(sys.available_mb());
        }
        memory.pressure = helix::MemoryMonitor::instance().pressure_level();
        memory.major_faults = helix::read_self_major_faults();
        TelemetryManager::instance().record_main_loop_hang(stalled_ms, memory);
        crash_handler::breadcrumb::note("main_loop", "hang");
    });

    // Drop LVGL's decoded-image cache on critical pressure. Printer images,
    // thumbnails, and XML-loaded PNGs live here as full ARGB8888 pixel buffers
    // (e.g. a 300x300 printer image is ~360KB decoded). Freeing them forces
    // the next draw to re-decode, which is cheap compared to an OOM kill.
    // Responder fires on the monitor thread — defer to UI thread via
    // queue_update: lv_image_cache_drop() reaches into the draw units
    // (LV_EVENT_INVALIDATE_AREA broadcast) which is not safe off the UI thread.
    helix::MemoryMonitor::instance().add_pressure_responder([](helix::MemoryPressureLevel level) {
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
    helix::MemoryMonitor::instance().add_pressure_responder([](helix::MemoryPressureLevel level) {
        if (level >= helix::MemoryPressureLevel::critical) {
            helix::ui::queue_update("Application::run", []() {
                crash_handler::breadcrumb::note("gcode_viewer", "pressure_clear");
                helix::ui::gcode_viewer_clear_all_active();
            });
        }
    });

    // Return the heap those responders freed to the OS, once per rise to
    // warning or critical. Freed heap stays in glibc's arenas otherwise, so RSS
    // barely moves and the box gains nothing. Registered after the two above so
    // its UI-queue hop runs behind theirs; the trim itself walks every arena
    // under its lock, so it runs on a worker.
    if (helix::heap_trim_supported()) {
        helix::MemoryMonitor::instance().add_pressure_responder(
            // Shared, not captured by value: the monitor copies its responder
            // list for every dispatch. Responders run only on the monitor
            // thread, so the state needs no lock.
            [state =
                 std::make_shared<helix::HeapReleaseState>()](helix::MemoryPressureLevel level) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
                if (!helix::should_release_heap_on_pressure(*state, level, now_ms))
                    return;
                helix::ui::queue_update("Application::heap_release", []() {
                    crash_handler::breadcrumb::note("heap", "pressure_trim");
                    helix::http::HttpExecutor::fast().submit([] { helix::release_free_heap(); });
                });
            });
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

    // An IPv4 change the user never kept is undone at boot.
    helix::settings::start_ipv4_startup_revert();

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
