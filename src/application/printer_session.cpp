// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_session.h"

#include "ui_ams_tool_text.h"
#include "ui_emergency_stop.h"
#include "ui_keyboard_manager.h"
#include "ui_language_refresh.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_notification.h"
#include "ui_notification_history.h"
#include "ui_notification_manager.h"
#include "ui_observer_guard.h"
#include "ui_panel_belt_tension.h"
#include "ui_panel_home.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_memory_stats.h"
#include "ui_panel_screws_tilt.h"
#include "ui_printer_status_icon.h"
#include "ui_probe_overlay.h"
#include "ui_settings_about.h"
#include "ui_spaghetti_detection_modal.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_wizard.h"

#include "abort_manager.h"
#include "active_print_media_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "cjk_font_manager.h"
#include "cli_args.h"
#include "config.h"
#include "detection_manager.h"
#include "discovery_steps.h"
#include "display_manager.h"
#include "filament_consumption_tracker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix_version.h"
#include "job_queue_state.h"
#include "json_utils.h"
#include "k2_stock_detection_source.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "light_button_config.h"
#include "memory_monitor.h"
#include "mock_performance_source.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_manager.h"
#include "moonraker_performance_source.h"
#include "page_scroll_auto_inject.h"
#include "panel_factory.h"
#include "pending_startup_warnings.h"
#include "performance_state.h"
#include "post_op_cooldown_manager.h"
#include "power_device_state.h"
#include "print_history_manager.h"
#include "printer_cache_registry.h"
#include "printer_discovery.h"
#include "printer_retarget.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "sensor_state.h"
#include "session_wiring.h"
#include "settings_manager.h"
#include "sound_manager.h"
#include "spoolman_active_spool_sync.h"
#include "spoolman_catalog_search.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "subject_initializer.h"
#include "system/afc_message_dedup.h"
#include "system/crash_handler.h"
#include "system/crash_history.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "temperature_history_manager.h"
#include "timelapse_state.h"
#include "u1_stock_detection_source.h"
#include "ui/ui_widget_helpers.h"
#include "upgrade_banner.h"
#if HELIX_HAS_PLUGINS
#include "plugin_dir_watcher.h"
#include "plugin_host.h"
#include "plugin_source_app.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix {

PrinterSession::PrinterSession(Config*& config, AsyncLifetimeGuard& async, lv_obj_t*& screen,
                               Host host)
    : m_config(config), m_async(async), m_host(std::move(host)),
      m_flow(config, async,
             {[this] { tear_down_printer_state(); }, [this] { rebuild(); },
              [] { helix::nav::set_active(PanelId::Home); }}),
      m_screen(screen), m_prompter(
                            async, [this] { return m_screen; },
                            [this] { return m_moonraker ? m_moonraker->api() : nullptr; }) {}

PrinterSession::~PrinterSession() = default;

void PrinterSession::reset_discovery_session() {
    m_hw_changes.reset();
    m_prompter.reset_for_new_connection();
}

void PrinterSession::switch_printer(const std::string& printer_id) {
    m_flow.switch_printer(printer_id);
}

void PrinterSession::request_switch(const std::string& printer_id) {
    m_flow.request_switch(printer_id);
}

void PrinterSession::add_printer_via_wizard() {
    m_flow.add_printer_via_wizard();
}

void PrinterSession::cancel_add_printer_wizard() {
    m_flow.cancel_add_printer_wizard();
}

bool PrinterSession::init_core_subjects() {
    m_subjects = std::make_unique<SubjectInitializer>();

    // Phase 1-3: Core subjects, PrinterState, AmsState
    // These must exist before MoonrakerManager::init() can create the API
    m_subjects->init_core_and_state();

    // Register the ams_current_tool_text formatter (a translated position
    // label, e.g. "Tool 1", or "---") now that AmsState's subjects are live.
    // The print status panel embeds <ams_current_tool> and binds
    // ams_current_tool_text — without this observer the lane label stays at
    // its default "---" until a user navigates into an AMS panel.
    helix::ui::init_ams_tool_text_observers();

    // Tool and extruder names are translated where the printer layer discovers
    // them; this re-renders them when the language changes.
    helix::ui::init_language_refresh();

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

bool PrinterSession::init_panel_subjects() {
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
        // K2: replaces the stock camera loop the installer disables. Probes
        // capability itself (K2 + /usr/bin/detection) and stays inert elsewhere.
        auto k2 = std::make_unique<helix::detection::K2StockDetectionSource>(&get_printer_state());
        k2->start();
        auto& dm = helix::detection::DetectionManager::instance();
        dm.register_source(std::move(u1));
        dm.register_source(std::move(k2));
        dm.init(get_moonraker_client(), &get_printer_state());
        dm.set_policy("u1_stock", static_cast<helix::detection::DetectionPolicy>(
                                      SettingsManager::instance().get_detection_policy_u1()));
        dm.set_presenter(
            [](const helix::detection::DetectionEvent& e, helix::detection::DetectionPolicy p) {
                helix::detection::present_detection(e, p);
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
#ifdef HELIX_ENABLE_MOCKS
    if (get_runtime_config()->should_mock_moonraker()) {
        helix::perf::PerformanceState::instance().set_source(
            std::make_unique<helix::perf::MockPerformanceSource>());
    } else
#endif
    {
        helix::perf::PerformanceState::instance().set_source(
            std::make_unique<helix::perf::MoonrakerPerformanceSource>(m_moonraker->api()));
    }
    spdlog::debug("[Application] PerformanceState initialized");

    spdlog::debug("[Application] Panel subjects initialized");
    helix::MemoryMonitor::log_now("after_panel_subjects_init");
    return true;
}

bool PrinterSession::init_ui() {
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
    helix::PendingStartupWarnings::instance().drain([](helix::PendingStartupWarnings::Severity sev,
                                                       const std::string& msg,
                                                       uint32_t duration_ms) {
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
        ToastManager::instance().show(toast_sev, msg.c_str(), duration_ms);
    });

    // Initialize overlay backdrop
    NavigationManager::instance().init_overlay_backdrop(m_screen);

    // Find navbar and content area
    lv_obj_t* navbar = helix::ui::find_required(m_app_layout, "navbar", "Application");
    lv_obj_t* content_area = helix::ui::find_required(m_app_layout, "content_area", "Application");

    if (!navbar || !content_area) {
        return false;
    }

    // Wire navigation
    NavigationManager::instance().wire_events(navbar);

    // Register printer switch/add callbacks so navbar badge menu can trigger actions
    NavigationManager::instance().set_printer_callbacks(
        [this](const std::string& printer_id) { request_switch(printer_id); },
        [this]() { add_printer_via_wizard(); });

    // Find panel container
    lv_obj_t* panel_container =
        helix::ui::find_required(content_area, "panel_container", "Application");
    if (!panel_container) {
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
    // print_status is created lazily by PrintStatusPanel::push_overlay()

    // Initialize keypad
    m_panels->init_keypad(m_screen);

    spdlog::info("[Application] UI created successfully");
    helix::MemoryMonitor::log_now("after_ui_created");
    return true;
}

bool PrinterSession::init_moonraker() {
    m_moonraker = std::make_unique<MoonrakerManager>();
    if (!m_moonraker->init(*get_runtime_config(), m_config)) {
        spdlog::error("[Application] Moonraker initialization failed");
        return false;
    }

    // API is now injected at panel construction in init_panel_subjects()
    // No need for deferred inject_api() call

    // Register MoonrakerManager globally
    set_moonraker_manager(m_moonraker.get());

    // Discovery callbacks on the client update the API's hardware_ and run
    // the discovery pipeline.
    setup_discovery_callbacks();

    // Create print history manager (shared cache for history panels and file status indicators)
    m_history_manager =
        std::make_unique<PrintHistoryManager>(m_moonraker->api(), get_moonraker_client());
    m_history_manager->hold_until_discovery();
    set_print_history_manager(m_history_manager.get());
    spdlog::debug("[Application] PrintHistoryManager created");

    // Create job queue state manager
    m_job_queue_state = std::make_unique<JobQueueState>(m_moonraker->api(), get_moonraker_client());
    m_job_queue_state->init_subjects();
    set_job_queue_state(m_job_queue_state.get());
    spdlog::debug("[Application] JobQueueState created");

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
    MemoryStatsOverlay::instance().init(m_screen, m_host.args.show_memory);

    spdlog::debug("[Application] Moonraker initialized");
    helix::MemoryMonitor::log_now("after_moonraker_init");
    return true;
}

#if HELIX_HAS_PLUGINS
void PrinterSession::init_plugins() {
    // The watcher and the driver hold references to the host, so they die
    // first; a printer switch rebuilds all of them against the new printer's
    // cache.
    m_plugin_watcher.reset();
    m_plugin_sync.reset();
    if (m_plugin_host) {
        m_plugin_host->unload_all();
        m_plugin_host.reset();
    }
    helix::plugin::PluginHost::Deps deps;
    deps.backend = helix::plugin::make_app_backend();
    deps.read_block = [this] { return m_config->get<json>("/plugins", json::object()); };
    deps.write_block = [this](const json& j) {
        m_config->set<json>("/plugins", j);
        m_config->save();
    };
    deps.settings_path = m_config->get_path();
    deps.helix_version = HELIX_VERSION;
    deps.memory_budget = helix::plugin::plugin_memory_budget(
        uint64_t{helix::get_system_memory_info().total_kb} * 1024);
    helix::plugin::register_plugin_event_callback();
    m_plugin_host = std::make_unique<helix::plugin::PluginHost>(std::move(deps));

    const char* dir = std::getenv("HELIX_PLUGIN_DIR");
    std::string cache;
    if (dir && *dir) {
        m_plugin_host->load_from(dir);
        // Developer mode: no sync driver runs against a local dir, so a poll
        // timer is the only thing that picks edits up while the app runs.
        if (RuntimeConfig::hot_reload_enabled())
            m_plugin_watcher =
                std::make_unique<helix::plugin::PluginDirWatcher>(*m_plugin_host, dir);
    } else {
        // Boot offline from the last sync: the per-printer cache is the plugin dir.
        cache = helix::plugin::plugin_cache_dir_for(m_config->get_active_printer_id());
        m_plugin_host->load_from(cache);
        // No driver without a Moonraker API (tests, early boot): the host still
        // runs the cached plugins, only the sync is missing.
        if (m_moonraker && m_moonraker->api()) {
            m_plugin_sync = std::make_unique<helix::plugin::PluginSyncDriver>(
                *m_plugin_host, helix::plugin::make_moonraker_source_deps(m_moonraker->api()),
                cache);
            m_plugin_sync->on_synced = [this](const helix::plugin::SyncResult& result) {
                on_plugin_sync(result);
            };
            // Discovery completion is queued behind this boot, but a driver
            // built once the connection is already up must not wait for the
            // next reconnect to hear about plugins.
            if (m_moonraker->api()->is_connected())
                m_plugin_sync->sync_now();
        }
    }
    m_known_plugin_ids.clear();
    for (const auto& info : m_plugin_host->plugins())
        m_known_plugin_ids.insert(info.dir_name);
    update_plugins_row_visibility();
}

void PrinterSession::on_plugin_sync(const helix::plugin::SyncResult& result) {
    std::vector<std::string> fresh;
    for (const auto& id : result.changed) {
        if (m_known_plugin_ids.count(id) == 0)
            fresh.push_back(id);
    }
    m_known_plugin_ids.clear();
    for (const auto& info : m_plugin_host->plugins())
        m_known_plugin_ids.insert(info.dir_name);
    update_plugins_row_visibility();

    // Only a folder the host can actually load is "new": a synced dir with no
    // manifest, or one the host rejected, cannot be enabled, so it never toasts.
    fresh = helix::plugin::loadable_plugin_ids(fresh, m_plugin_host->plugins());
    if (fresh.empty())
        return;
    if (fresh.size() == 1) {
        // Prefer the manifest's display name; an unreadable manifest falls back
        // to the directory name.
        std::string name = fresh.front();
        for (const auto& info : m_plugin_host->plugins()) {
            if (info.dir_name == fresh.front()) {
                if (info.manifest)
                    name = info.manifest->name;
                break;
            }
        }
        std::string msg = fmt::format(
            fmt::runtime(lv_tr("New plugin available: {}. Enable it in Settings > Plugins.")),
            name);
        ToastManager::instance().show(ToastSeverity::INFO, msg.c_str());
        return;
    }
    std::string msg = fmt::format(
        fmt::runtime(lv_tr("{} new plugins available. Enable them in Settings > Plugins.")),
        fresh.size());
    ToastManager::instance().show(ToastSeverity::INFO, msg.c_str());
}

void PrinterSession::update_plugins_row_visibility() {
    if (auto* subj = lv_xml_get_subject(nullptr, "settings_plugins_available"))
        lv_subject_set_int(subj, m_plugin_host && !m_plugin_host->plugins().empty() ? 1 : 0);
}
#endif

void PrinterSession::setup_discovery_callbacks() {
    IMoonrakerClient* client = m_moonraker->client();
    IMoonrakerAPI* api = m_moonraker->api();

    PrinterSession* app = this;

#if HELIX_HAS_PLUGINS
    // Moonraker pushes notify_filelist_changed for every file operation in
    // every root; only the plugin folder may cost a sync. The predicate is
    // pure and runs on the WebSocket thread, the sync request hops to the
    // main thread through the lifetime token (unregistered in both teardowns
    // before the driver dies).
    {
        auto token = m_async.token();
        client->register_method_callback(
            "notify_filelist_changed", "PluginSync", [token, app](const nlohmann::json& msg) {
                if (!helix::plugin::is_plugin_filelist_change(msg))
                    return;
                token.defer("PrinterSession::plugin_filelist_changed", [app]() {
                    if (app->m_plugin_sync)
                        app->m_plugin_sync->request_sync();
                });
            });
    }

    // Plugin subscriptions ride the app's union subscription. The provider is a free
    // function reading a process-wide registry, so it stays valid across plugin loads
    // and needs no state of the plugin host's.
    client->set_subscription_extras_provider(&helix::plugin::plugin_objects_union);
#endif

    helix::wire_discovery(*api, *client,
                          {m_hw_changes, [app] { return !app->m_host.shutdown_complete; },
                           // A new discovery cycle re-arms the once-per-connection targeted
                           // hardware-reconfig wizard guard, so a reconnect can re-offer it.
                           [app] { app->m_prompter.begin_discovery_cycle(); },
                           [app](helix::DiscoveryContext& ctx) {
#if HELIX_HAS_PLUGINS
                               // Every connect and reconnect re-syncs the plugin folder: it may
                               // have changed while the connection was down.
                               if (app->m_plugin_sync)
                                   app->m_plugin_sync->sync_now();
#endif
                               app->m_host.discovery_complete();
                               spdlog::info(
                                   "[Application] Moonraker discovery complete, splash can exit");
                               ctx.prompter = &app->m_prompter;
                               ctx.screen = app->m_screen;
                               helix::run_discovery_steps(helix::discovery_tail_steps(), ctx);
                           }});
}

bool PrinterSession::connect_moonraker() {
    // Boot and every rebuild connect through here, to the active printer.
    m_flow.set_connected_printer_id(m_config->get_active_printer_id());

    // Determine if we should connect
    std::string saved_host = m_config->get<std::string>(m_config->df() + "moonraker_host", "");
    bool has_cli_url = !m_host.args.moonraker_url.empty();
    // Always connect at boot when we have a host (fresh-install scaffold seeds
    // moonraker_host=127.0.0.1, so embedded devices can reach Moonraker without
    // user intervention). Connecting during the wizard is what lets auto-detection
    // run early — without this, the preset can't be applied until the connection
    // step's manual auto-probe, defeating the purpose of preset_mode skipping
    // hardware steps. The wizard's connection step still re-uses this connection
    // (or replaces it if the user changed the host).
    // In test mode, gate on m_host.wizard_active so unit/integration tests that
    // launch with --wizard don't race against fixture setup.
    bool should_connect = has_cli_url ||
                          (get_runtime_config()->test_mode && !m_host.wizard_active) ||
                          !saved_host.empty();

    if (!should_connect) {
        return true; // Not connecting is not an error
    }

    std::string moonraker_url;
    std::string http_base_url;

    if (has_cli_url) {
        moonraker_url = m_host.args.moonraker_url;
        std::string host_port = moonraker_url.substr(5);
        auto ws_pos = host_port.find("/websocket");
        if (ws_pos != std::string::npos) {
            host_port = host_port.substr(0, ws_pos);
        }
        http_base_url = "http://" + host_port;
    } else {
        moonraker_url = helix::active_printer_ws_url();
        http_base_url = helix::active_printer_http_url();
    }

    // Discovery callbacks are already registered (setup_discovery_callbacks in init_moonraker).
    // The display-wake reconnect callback is registered once in init_display(), not here —
    // connect_moonraker() re-runs on every printer switch and DisplayManager has no
    // unregister path.

    // The manager's connect sets the API's HTTP base URL; discovery starts on its own once
    // the socket is up.
    if (!helix::connect_printer(*m_moonraker, moonraker_url, http_base_url)) {
        return false;
    }

    // G-code response routing: action prompts, error and narration routers, layer tracking
    if (m_moonraker->client()) {
        m_routing.attach(m_moonraker->client(), m_moonraker->api());
    } else {
        spdlog::warn("[Application] Cannot init G-code response routing - no client");
    }

    // Start telemetry auto-send timer (periodic try_send)
    TelemetryManager::instance().start_auto_send();

    return true;
}

void PrinterSession::tear_down_printer_state() {
    spdlog::info("[Application] Tearing down printer state...");
    teardown_printer_scope(TeardownScope::PrinterSwitch);
    spdlog::info("[Application] Printer state torn down");
}

void PrinterSession::rebuild() {
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

    // ObserverGuard::invalidate_all() ran at the end of teardown. Guards in
    // surviving singletons hold freed observer pointers; when they get
    // reassigned (guard = observe_*()), reset() sees they predate the
    // invalidation and releases instead of calling lv_observer_remove().

    // 1. Reinitialize update queue BEFORE moonraker so background thread callbacks
    //    (hardware discovery, WebSocket messages) have a functioning queue.
    helix::ui::update_queue_init();

    // 2. Initialize core subjects (PrinterState, AmsState, etc.)
    if (!init_core_subjects()) {
        spdlog::error("[Application] Failed to reinitialize core subjects");
        show_init_error();
        return;
    }

    // 2b. Seed the active printer's display name from config
    get_printer_state().set_active_printer_name(m_config->get_active_printer_name());

    // 3. Initialize Moonraker (creates client + API + history managers)
    if (!init_moonraker()) {
        spdlog::error("[Application] Failed to reinitialize Moonraker");
        show_init_error();
        return;
    }

    // 4. Initialize panel subjects with API injection + post-init
    if (!init_panel_subjects()) {
        spdlog::error("[Application] Failed to reinitialize panel subjects");
        show_init_error();
        return;
    }

    // 5. Recreate UI (app_layout from XML, wire navigation)
    if (!init_ui()) {
        spdlog::error("[Application] Failed to reinitialize UI");
        show_init_error();
        return;
    }

    // 6. Run wizard if needed for new printer
    if (m_host.run_wizard()) {
        m_host.wizard_active = true;
        set_wizard_active(true);
    }

    // 7. Apply startup CLI actions (if any). Finalize the home panel here so
    //    its carousel + widget grid get built — HomePanel::setup() is
    //    deliberately minimal; finalize_setup() is what creates the visible
    //    content. Mirrors run()'s startup path; without it the home panel
    //    renders blank after a printer switch.
    if (!m_host.wizard_active) {
        m_host.startup_actions();
        get_global_home_panel().finalize_setup();
    }

    // 8. Reload plugins against the new printer's state
#if HELIX_HAS_PLUGINS
    init_plugins();
#endif

    // 9. Connect to new printer's Moonraker
    if (!connect_moonraker()) {
        spdlog::warn("[Application] Running without printer connection after switch");
    }

    // 10. Force full screen refresh
    lv_obj_update_layout(m_screen);
    helix::ui::invalidate_all_recursive(m_screen);
    lv_refr_now(nullptr);

    spdlog::info("[Application] Printer state initialized");
}

// The one ordered teardown behind both soft restart (PrinterSwitch: the process and LVGL
// stay alive, init_printer_state() rebuilds afterwards) and shutdown() (ProcessExit:
// lv_deinit() frees every widget and the process ends). Steps that differ are guarded by
// `exiting` and say why; everything else runs identically in both scopes. Subjects stay
// alive until StaticSubjectRegistry::deinit_all() so ObserverGuards can call
// lv_observer_remove() while destroying.
void PrinterSession::teardown_printer_scope(TeardownScope scope, DisplayManager* exit_display) {
    const bool exiting = scope == TeardownScope::ProcessExit;
    auto destroy_panels = [exiting] {
        if (exiting) {
            StaticPanelRegistry::instance().destroy_all();
        } else {
            helix::ui::destroy_static_panels();
        }
    };

    // A callback armed for the old printer's wizard must not fire against the next one.
    set_wizard_cancel_callback(nullptr);

    // The next printer's discovery is a first discovery with its own prompts to show.
    reset_discovery_session();

    // A switch freezes the UpdateQueue before the disconnect: work the WebSocket thread
    // enqueues from here on is buffered, and update_queue_shutdown() below discards the
    // buffer, so it never runs against the plugins, history managers and AMS backends
    // destroyed in between. Exit needs no freeze; update_queue_shutdown() gates the queue
    // off for good.
    std::optional<helix::ui::UpdateQueue::ScopedFreeze> queue_freeze;
    if (!exiting) {
        queue_freeze.emplace(helix::ui::UpdateQueue::instance(), "teardown_printer_scope");
    }

    // Disconnect the WebSocket client FIRST to stop background threads (mock simulation,
    // WebSocket I/O). Otherwise a notification delivered mid-teardown can trigger new API
    // requests (history fetch, metascan, webcam detection). The client object stays valid
    // for the unregister_method_callback() calls below.
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->disconnect();
    }

    // Clear SoundManager's client ref so the M300 sequencer thread won't call
    // gcode_script() on a dangling pointer (#714). Exit skips host recovery:
    // SoundManager::shutdown() runs below, so re-opening audio hardware would only be
    // torn down again.
    SoundManager::instance().set_moonraker_client(nullptr, /*host_recovery=*/!exiting);

    // Clear app_globals BEFORE destroying managers so destructors (e.g. PrintSelectPanel)
    // never reach destroyed objects.
    set_moonraker_manager(nullptr);
    set_moonraker_api(nullptr);
    set_moonraker_client(nullptr);
    set_job_queue_state(nullptr); // the object itself dies after deinit_all(), below
    set_print_history_manager(nullptr);
    set_temperature_history_manager(nullptr);

    // Deactivate overlays and clear navigation registries
    NavigationManager::instance().shutdown();

    // Detach page-scroll-buttons controllers (gutters + observers) while panel widgets are
    // still alive, before m_panels.reset() / destroy_all() tear down the containers they
    // point at.
    helix::ui::PageScrollAutoInject::instance().shutdown();

    UpdateChecker::instance().stop_auto_check();

    if (exiting) {
        // Process-scoped services: they persist across a printer switch.
        // The banner goes before UpdateChecker so its observers release cleanly (#705).
        m_host.upgrade_banner.shutdown();
        UpdateChecker::instance().shutdown();    // cancels pending checks
        TelemetryManager::instance().shutdown(); // persists queue, joins send thread
        helix::CrashHistory::instance().shutdown();
        AfcMessageDedup::instance().shutdown();
        // Before the client is destroyed: the M300 backend's sender lambda references it
        // and the sequencer thread must be stopped first (#714).
        SoundManager::instance().shutdown();
        PostOpCooldownManager::instance().shutdown(); // cancel pending cooldown timers
    }

    // Unload plugins before destroying what they depend on: plugin closers remove
    // printer-subject observers and Moonraker notify handlers, so they must run while the
    // subjects (deinit_all below) and the Moonraker client (m_moonraker.reset below) are
    // still alive.
#if HELIX_HAS_PLUGINS
    // The watcher and the driver hold host references, and the driver's filelist handler
    // must not outlive the driver it feeds.
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->unregister_method_callback("notify_filelist_changed", "PluginSync");
        // Before the plugin host goes: the registry's union stops being consulted, so
        // the refresh the unload-time clears schedule shrinks the subscription back to
        // app objects instead of growing it.
        m_moonraker->client()->set_subscription_extras_provider({});
    }
    m_plugin_watcher.reset();
    m_plugin_sync.reset();
    if (m_plugin_host) {
        m_plugin_host->unload_all();
        m_plugin_host.reset();
    }
#endif

    // History managers MUST be reset before moonraker (they use the client for
    // unregistration). JobQueueState is reset AFTER deinit_all() because it owns LVGL
    // subjects that panels still observe: destroying it early frees subject memory while
    // panel ObserverGuards still hold observer pointers into those lists.
    m_history_manager.reset();
    m_temp_history_manager.reset();

    // Unregister the connection-scoped method callbacks whose bodies reach panels or
    // subjects: StaticPanelRegistry::destroy_all() and StaticSubjectRegistry::deinit_all()
    // both run well before the client is released. external_spool_sync additionally
    // dereferences a raw IMoonrakerAPI* that the manager owns, straight from the WebSocket
    // thread.
    if (m_moonraker && m_moonraker->client()) {
        helix::TimelapseState::instance().detach(*m_moonraker->client());
        UpdateChecker::instance().detach(*m_moonraker->client());
        helix::settings::get_about_settings_overlay().detach_print_hours(*m_moonraker->client());
        helix::spoolman_sync::detach(*m_moonraker->client());
    }

    // Unsubscribe power device and sensor state
    if (m_moonraker && m_moonraker->api()) {
        helix::PowerDeviceState::instance().unsubscribe(*m_moonraker->api());
        helix::SensorState::instance().unsubscribe(*m_moonraker->api());
    }

    // Unregister the response handlers and drop the prompt system before moonraker is
    // destroyed.
    m_routing.detach_handlers(m_moonraker ? m_moonraker->client() : nullptr);

    // Stop AMS backend subscriptions BEFORE destroying MoonrakerClient: backends hold
    // SubscriptionGuards with raw client pointers and must unsubscribe while the client's
    // mutex is still alive.
    AmsState::instance().clear_backends();

    // Drain deferred UI callbacks BEFORE destroying panels. observe<int> and
    // observe<const char*> defer via ui_queue_update(), so queued callbacks may hold
    // `this` pointers to living panels; running them after m_panels.reset() is a
    // use-after-free.
    helix::ui::update_queue_shutdown();

    if (!exiting) {
        // Singletons that outlive this printer and hold API/client pointers or observe
        // PrinterState subjects about to be freed. LedAutoState drives LedController, so
        // it goes first.
        helix::led::LedAutoState::instance().deinit();
        helix::led::LedController::instance().deinit();
    }

    // Stop ALL LVGL animations before destroying panels: they hold widget pointers, and
    // completion callbacks fired by lv_anim_delete_all() would dereference freed objects
    // if the panels were already gone.
    lv_anim_delete_all();

    m_panels.reset();
    m_subjects.reset();

    if (exiting && exit_display) {
        // Guard for early exit paths like --help
        exit_display->restore_display_on_shutdown();
    }

    if (!exiting) {
        // The widgets this tracks are destroyed by the tree delete below; lv_deinit() does
        // that on exit.
        ModalStack::instance().clear();
    }

    // Stop the consumption tracker BEFORE destroying overlays. Overlay teardown can free
    // the tracker's PrinterState observer struct, and a later ObserverGuard::reset() then
    // dereferences freed memory (#927). Its self-registration with StaticSubjectRegistry
    // remains as a backstop and is a no-op once the observers are null.
    helix::FilamentConsumptionTracker::instance().stop();

    // Destroy ALL static panel/overlay globals (releases ObserverGuards, deinits local
    // subjects). LVGL must still be initialized so lv_observer_remove() can remove
    // unsubscribe_on_delete_cb from widget event lists. A switch frees the overlay roots
    // the panel destructors hand back (400-800KB each, parented to the screen, nothing
    // else deletes them); exit leaves them for lv_deinit(), because deleting widgets
    // inside this window reopens the crash it exists to avoid.
    destroy_panels();

    if (!exiting) {
        // Release global observer guards that observe subjects about to be freed.
        ui_notification_deinit();
        helix::deinit_active_print_media_manager();
    }

    // Deinit core singleton subjects (PrinterState, AmsState, SettingsManager, ...) BEFORE
    // lv_deinit(). lv_subject_deinit() calls lv_observer_remove() for each observer, which
    // removes unsubscribe_on_delete_cb from widget event lists, so widgets then delete
    // without firing stale unsubscribe callbacks on corrupted linked lists.
    StaticSubjectRegistry::instance().deinit_all();

    // Sweep any panel singleton a deinit callback lazily re-created on its way out (a
    // callback reaching through an auto-creating get_global_*_panel() getter builds a
    // replacement). Destroying it here, while LVGL and spdlog are up, keeps its destructor
    // off the static-destruction path. No-op when nothing resurrected.
    destroy_panels();

    // After deinit_all() so JobQueueState's registered cleanup lambda runs on a live
    // object; before m_moonraker.reset() so client unregistration works.
    m_job_queue_state.reset();

    if (exiting) {
        // Destroy runtime CJK fonts before LVGL shutdown
        helix::system::CjkFontManager::instance().shutdown();
    }

    // Invalidate all ObserverGuards so any reset() in surviving destructors releases
    // instead of calling lv_observer_remove() on freed observer pointers.
    // lv_subject_deinit() (via deinit_all() above) frees each observer it iterates, so
    // without this MoonrakerManager's ObserverGuard members would call
    // lv_observer_remove() on freed memory (lv_observer.c, lv_ll_remove).
    ObserverGuard::invalidate_all(exiting);

    // Tear down GcodeErrorRouter before MoonrakerClient: its dtor unregisters the live and
    // replay callbacks, both of which touch the client. Reset the router BEFORE the
    // presenter (the presenter must outlive it), and AmsErrorBridge, which also holds a
    // presenter reference, before that.
    m_routing.release_routers();

    // Destroy MoonrakerManager (its ObserverGuards now release without touching freed
    // observer memory thanks to invalidate_all() above).
    m_moonraker.reset();

    // The SpoolmanDB search answer belongs to that client; the next client's
    // connection generations start over and must not inherit it.
    helix::SpoolmanCatalogSearch::reset_cache();

    if (exiting) {
        // The caller finishes the exit: HTTP executors, display, theme manager.
        return;
    }

    KeyboardManager::instance().reset(); // widget pointers dangle after the tree delete

    // Delete the LVGL widget tree (panels already released their references). The display
    // stays alive, so no lv_deinit(). cancel_add_printer_wizard() reaches here inside a
    // queue_update() batch, where a synchronous lv_obj_del() would delete inside
    // UpdateQueue::process_pending() and can corrupt LVGL's global event list (#776/#190/#80);
    // m_app_layout is also the Home widget grid, where a relayout racing teardown could
    // iterate a freed container (#983). safe_delete_subtree() detaches the tree off-screen
    // synchronously (so the immediate init_printer_state() rebuild sees a clean m_screen),
    // forces LV_LAYOUT_NONE, and frees it asynchronously outside the batch.
    if (m_app_layout) {
        helix::ui::safe_delete_subtree(m_app_layout);
        m_app_layout = nullptr;
    }
    m_overlay_panels = {};
}

} // namespace helix
