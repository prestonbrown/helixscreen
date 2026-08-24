// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file moonraker_manager.cpp
 * @brief Orchestrates MoonrakerClient lifecycle and WebSocket notification dispatch
 *
 * @pattern Manager with shared_ptr<atomic<bool>> alive flag for callback safety
 * @threading Queues notifications from WebSocket thread to main thread
 * @gotchas Destroy m_client FIRST in shutdown — it waits for in-flight callbacks
 *
 * @see moonraker_client.cpp, printer_state.cpp
 */

#include "moonraker_manager.h"

#include "ui_change_host_modal.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "abort_manager.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_client.h"
#include "macro_modification_manager.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_event_routing.h"
#include "power_device_state.h"
#include "sensor_state.h"
#include "unit_conversions.h"
#ifdef HELIX_ENABLE_MOCKS
#include "ams_backend_mock.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#endif
#include "print_collector_arming.h"
#include "print_completion.h"
#include "print_start_collector.h"
#include "print_start_profile.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "sound_manager.h"
#include "spoolman_manager.h"
#include "tool_state.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstdlib>
#include <vector>

using namespace helix;

MoonrakerManager::MoonrakerManager() {}

MoonrakerManager::~MoonrakerManager() {
    shutdown();
}

bool MoonrakerManager::init(const RuntimeConfig& runtime_config, Config* config) {
    if (m_initialized) {
        spdlog::warn("[MoonrakerManager] Already initialized");
        return false;
    }

    spdlog::debug("[MoonrakerManager] Initializing...");

    // Create client (mock or real)
    create_client(runtime_config);

    // HELIX_MOCK_PRINTER is authoritative over any persisted printer type.
    // PrinterDetector::auto_detect_and_save() (which runs later, after the
    // discovery handshake) short-circuits when a printer type is already
    // saved in config — so a stale "Voron 2.4" from a previous run would
    // silently win over the env var. Clearing the saved type here (before
    // detection runs) forces auto-detection to re-resolve from the mock's
    // reported identity on every launch. Strictly env-gated: zero behavior
    // change when HELIX_MOCK_PRINTER is unset.
    if (config && std::getenv("HELIX_MOCK_PRINTER")) {
        const std::string mock_printer = std::getenv("HELIX_MOCK_PRINTER");
        const std::string type_path = config->df() + helix::wizard::PRINTER_TYPE;
        // The k1 persona's detection identity is not complete enough to clear
        // the auto-save bar (shared chamber sensor + generic volume score it
        // as a Qidi at 73%), so name the capture machine directly: the
        // persona's printer type is part of what the env var declares.
        if (mock_printer == "k1") {
            config->set<std::string>(type_path, "Creality K1C");
            config->save();
            spdlog::info("[MoonrakerManager] HELIX_MOCK_PRINTER=k1 — saved printer type "
                         "'Creality K1C' (persona identity doesn't clear the detection bar)");
        } else {
            const std::string prev = config->get<std::string>(type_path, "");
            if (!prev.empty()) {
                config->set<std::string>(type_path, "");
                config->save();
                spdlog::info("[MoonrakerManager] HELIX_MOCK_PRINTER set — cleared saved "
                             "printer type '{}' so mock identity re-detects this launch",
                             prev);
            }
        }
    }

    // Configure timeouts from config file
    if (config) {
        configure_timeouts(config);
    }

    // Register callbacks for notifications and state changes
    register_callbacks();

    // Create API (mock or real)
    create_api(runtime_config);

    m_initialized = true;
    spdlog::info("[MoonrakerManager] Initialized (not connected yet)");

    return true;
}

void MoonrakerManager::shutdown() {
    // Signal to async callbacks that we're being destroyed [L012]
    // Must happen FIRST before any cleanup
    m_alive->store(false);
    // Same signal for the generation-guarded event handler: any bg_cb already
    // queued for the main thread sees the bumped generation and skips instead of
    // running present_event() against a half-torn-down manager.
    lifetime_.invalidate();

    if (!m_initialized) {
        return;
    }

    spdlog::debug("[MoonrakerManager] Shutting down...");

    // Stop print start collector first (before client is destroyed)
    if (m_print_start_collector) {
        m_print_start_collector->stop();
        m_print_start_collector.reset();
    }

    // Release observer guards without calling lv_observer_remove().
    // During shutdown, subjects may already be deinitialized (which frees observers).
    // Using release() avoids double-free of already-removed observers.
    m_print_start_observer.release();
    m_print_start_phase_observer.release();
    m_preparing_epoch_observer.release();
    m_print_bed_target_fallback_observer.release();
    m_print_ext_target_fallback_observer.release();
    for (auto& guard : m_print_position_observers) {
        guard.release();
    }
    m_print_layer_observer.release();
    m_print_duration_observer.release();

    // Destroy client FIRST: its destructor waits for in-flight libhv callbacks
    // to finish. connect() lambdas hold raw pointers to m_api and m_macro_analysis,
    // so those must outlive the client to avoid use-after-free (#628).
    m_client.reset();

    // Safe now — no callbacks can fire after client destruction.
    // Note: m_api holds a MoonrakerClient& that is now dangling, but
    // ~MoonrakerAPI() only joins HTTP threads and deinits subjects.
    m_macro_analysis.reset();
    m_api.reset();

    // Clear notification queue
    {
        std::lock_guard<std::mutex> lock(m_notification_mutex);
        while (!m_notification_queue.empty()) {
            m_notification_queue.pop();
        }
    }

    m_initialized = false;
    spdlog::info("[MoonrakerManager] Shutdown complete");
}

int MoonrakerManager::connect(const std::string& websocket_url, const std::string& http_base_url) {
    if (!m_initialized || !m_client) {
        spdlog::error("[MoonrakerManager] Cannot connect - not initialized");
        return -1;
    }

    spdlog::info("[MoonrakerManager] Connecting to {} ...", websocket_url);

    // Set HTTP base URL for API
    if (m_api) {
        std::string effective_http_base = http_base_url;
#ifdef HELIX_ENABLE_MOCKS
        // Under --test the configured host is a real printer address the mock
        // never talks to, so file downloads would hang or fail. Point them at
        // the loopback mock instead.
        if (m_mock_http && m_mock_http->running()) {
            effective_http_base = m_mock_http->base_url();
            spdlog::info("[MoonrakerManager] Mock mode — HTTP file base URL redirected to {}",
                         effective_http_base);
        }
#endif
        m_api->set_http_base_url(effective_http_base);
    }

    // Connect client - on_connected triggers printer discovery which subscribes to status updates
    // CRITICAL: Without discover_printer(), we never call printer.objects.subscribe,
    // so we never receive notify_status_update messages (print_stats, temperatures, etc.)
    IMoonrakerClient* client = m_client.get();
    MoonrakerAPI* api = m_api.get();
    helix::MacroModificationManager* macro_mgr = m_macro_analysis.get();
    // Raw pointers remain valid because shutdown() destroys client first,
    // waiting for in-flight callbacks before destroying api/macro_analysis.
    // The alive flag provides early-out for callbacks queued during shutdown (#435, #628).
    auto alive = m_alive;
    return m_client->connect(
        websocket_url.c_str(),
        [client, api, macro_mgr, alive]() {
            if (!alive->load())
                return;
            // Connection established - start printer discovery
            // This queries printer capabilities and subscribes to status updates
            spdlog::info("[MoonrakerManager] Connected, starting printer discovery...");
            client->discover_printer([api, macro_mgr, alive]() {
                if (!alive->load())
                    return;
                spdlog::info("[MoonrakerManager] Printer discovery complete");

                // Clean up any stale .helix_temp files from previous sessions
                // (These are temp files created when modifying G-code for prints)
                helix::cleanup_stale_helix_temp_files(api);

                // Safety limits + build volume now fetched in
                // Application::setup_discovery_callbacks() on_discovery_complete,
                // so all discovery paths (startup + post-wizard) share one call.

                // Trigger macro analysis after discovery
                if (macro_mgr) {
                    spdlog::debug("[MoonrakerManager] Triggering PRINT_START macro analysis");
                    macro_mgr->check_and_notify();
                }
            });
        },
        [alive]() {
            if (!alive->load())
                return;
            // Disconnected - state changes are handled via notification queue
        });
}

void MoonrakerManager::process_notifications() {
    std::lock_guard<std::mutex> lock(m_notification_mutex);

    while (!m_notification_queue.empty()) {
        json notification = std::move(m_notification_queue.front());
        m_notification_queue.pop();

        // Check for connection state change (queued from state_change_callback)
        if (notification.contains("_connection_state")) {
            int new_state = notification["new_state"].get<int>();
            static const char* messages[] = {
                "Disconnected",     // DISCONNECTED
                "Connecting...",    // CONNECTING
                "Connected",        // CONNECTED
                "Reconnecting...",  // RECONNECTING
                "Connection Failed" // FAILED
            };
            spdlog::trace("[MoonrakerManager] Processing connection state change: {}",
                          messages[new_state]);
            get_printer_state().set_printer_connection_state(new_state, messages[new_state]);

            // Subscribe to power device events as soon as WebSocket connects.
            // Power devices are Moonraker-managed and don't need Klipper.
            // Safe to call multiple times — insert is a no-op if handler already registered.
            if (new_state == static_cast<int>(ConnectionState::CONNECTED) && m_api) {
                helix::PowerDeviceState::instance().subscribe(*m_api);
                helix::SensorState::instance().subscribe(*m_api);
            }

            // Auto-close Connection Failed modal when connection is restored
            // (Disconnect modal is now handled by unified recovery dialog in EmergencyStopOverlay)
            if (new_state == static_cast<int>(ConnectionState::CONNECTED)) {
                lv_obj_t* modal = helix::ui::modal_get_top();
                if (modal) {
                    lv_obj_t* title_label = lv_obj_find_by_name(modal, "dialog_title");
                    if (title_label) {
                        const char* title = lv_label_get_text(title_label);
                        if (title && strcmp(title, "Connection Failed") == 0) {
                            spdlog::info("[MoonrakerManager] Auto-closing '{}' modal on reconnect",
                                         title);
                            helix::ui::modal_hide(modal);
                        }
                    }
                }
            }
        } else {
            // Regular Moonraker notification — extract status and update directly
            if (notification.contains("method") && notification.contains("params")) {
                const auto& method_str = notification["method"];
                if (method_str.is_string() &&
                    method_str.get<std::string>() == "notify_status_update") {
                    auto& params = notification["params"];
                    if (params.is_array() && !params.empty()) {
                        // params[1] is Klipper's eventtime, and the marker says
                        // whether this is a replay of an earlier snapshot rather
                        // than current traffic. PrinterState needs both to keep a
                        // stale klippy state from overwriting a live one; this is
                        // the production status path (PrinterState::
                        // update_from_notification is not wired up here).
                        const double eventtime = (params.size() > 1 && params[1].is_number())
                                                     ? params[1].get<double>()
                                                     : 0.0;
                        const bool from_cached_snapshot =
                            notification.value(helix::CACHED_SNAPSHOT_MARKER, false);
                        get_printer_state().update_from_status(params[0], eventtime,
                                                               from_cached_snapshot);
                        helix::ToolState::instance().update_from_status(params[0]);
                    }
                }
            }
        }
    }
}

void MoonrakerManager::process_timeouts() {
    if (m_client) {
        m_client->process_timeouts();
    }
}

size_t MoonrakerManager::pending_notification_count() const {
    std::lock_guard<std::mutex> lock(m_notification_mutex);
    return m_notification_queue.size();
}

void MoonrakerManager::create_client(const RuntimeConfig& runtime_config) {
    spdlog::debug("[MoonrakerManager] Creating Moonraker client...");

#ifdef HELIX_ENABLE_MOCKS
    MoonrakerClientMock* mock_client = nullptr;
#endif

#if defined(ESP_PLATFORM)
    // Embedded firmware: platform-provided client over esp_websocket_client.
    // HELIX_ENABLE_MOCKS is never defined for ESP32 targets, so there is no
    // separate mock arm to consider here.
    spdlog::debug("[MoonrakerManager] Creating platform (ESP32) client");
    m_client = helix::create_platform_moonraker_client();
#else
#ifdef HELIX_ENABLE_MOCKS
    if (runtime_config.should_mock_moonraker()) {
        double speedup = runtime_config.sim_speedup;
        // HELIX_MOCK_PRINTER=voron_24|voron_trident|k1|ad5m|generic_corexy|
        // generic_bedslinger|multi_extruder — defaults to Voron 2.4. K2 and
        // CC1 don't have dedicated mock types yet; they fall through to the
        // default with a warning.
        const char* type_env = std::getenv("HELIX_MOCK_PRINTER");
        auto type = MoonrakerClientMock::PrinterType::VORON_24;
        const char* type_name = "Voron 2.4";
        if (type_env) {
            std::string t(type_env);
            if (t == "multi_extruder") {
                type = MoonrakerClientMock::PrinterType::MULTI_EXTRUDER;
                type_name = "Multi-Extruder";
            } else if (t == "voron_trident") {
                type = MoonrakerClientMock::PrinterType::VORON_TRIDENT;
                type_name = "Voron Trident";
            } else if (t == "k1") {
                type = MoonrakerClientMock::PrinterType::CREALITY_K1;
                type_name = "Creality K1";
            } else if (t == "ad5m") {
                type = MoonrakerClientMock::PrinterType::FLASHFORGE_AD5M;
                type_name = "Flashforge AD5M";
            } else if (t == "generic_corexy") {
                type = MoonrakerClientMock::PrinterType::GENERIC_COREXY;
                type_name = "Generic CoreXY";
            } else if (t == "generic_bedslinger") {
                type = MoonrakerClientMock::PrinterType::GENERIC_BEDSLINGER;
                type_name = "Generic Bedslinger";
            } else if (t != "voron_24") {
                spdlog::warn("[MoonrakerManager] HELIX_MOCK_PRINTER='{}' not recognised "
                             "— falling back to Voron 2.4. Valid: voron_24, voron_trident, "
                             "k1, ad5m, generic_corexy, generic_bedslinger, multi_extruder.",
                             t);
            }
        }
        spdlog::info("[MoonrakerManager] Creating MOCK client ({}, {}x speed)", type_name, speedup);
        auto mock = std::make_unique<MoonrakerClientMock>(type, speedup);

        // HELIX_MOCK_AUTO_PRINT=1 — boot straight into an active mock print so
        // print-gated features (e.g. adaptive bed mesh) are exercisable under
        // --test without driving the UI through a print-start flow. Sets the
        // existing mock_auto_start_print flag the mock consumes on connect().
        // Additive + env-gated: default --test behavior is unchanged.
        const char* auto_print_env = std::getenv("HELIX_MOCK_AUTO_PRINT");
        if (auto_print_env && auto_print_env[0] && std::string(auto_print_env) != "0") {
            get_runtime_config()->mock_auto_start_print = true;
            if (!get_runtime_config()->gcode_test_file) {
                get_runtime_config()->gcode_test_file = RuntimeConfig::get_default_test_file_path();
            }
            spdlog::info("[MoonrakerManager] HELIX_MOCK_AUTO_PRINT set — mock will "
                         "auto-start a print on connect");
        }

        // HELIX_MOCK_REPLAY=<script.json> — replay a captured print-start
        // sequence (klippy + app log extraction) through the mock's real
        // dispatch paths so the full observer chain runs against captured
        // data. Combine with --sim-speed to fast-forward; position/gcode
        // events fire at capture timing divided by the speedup.
        const char* replay_env = std::getenv("HELIX_MOCK_REPLAY");
        if (replay_env && replay_env[0]) {
            if (mock->arm_event_replay(replay_env)) {
                spdlog::info("[MoonrakerManager] HELIX_MOCK_REPLAY armed — capture replay "
                             "starts on connect");
            }
        }

        // Disable MMU if AMS is explicitly disabled via CLI or env var
        const char* mock_ams_env = std::getenv("HELIX_MOCK_AMS");
        bool ams_disabled = runtime_config.disable_mock_ams ||
                            (mock_ams_env && std::string(mock_ams_env) == "none");
        if (ams_disabled) {
            mock->set_mmu_enabled(false);
        }
        // Remember the concrete types while we still have them. This is the one
        // place that knows what m_client actually is, so recording it here beats
        // recovering it later with a dynamic_cast (the firmware builds
        // -fno-rtti, and the desktop code must not diverge).
        mock_client = mock.get();
        m_concrete_client = mock.get();
        m_client = std::move(mock);

        // Thumbnails do not travel over the WebSocket the mock client answers —
        // download_thumbnail() issues a real HTTP GET on an HttpExecutor worker.
        // Without something listening, --test never exercises the cold-fetch
        // pipeline at all (download → decode → prescale → evict). Stand up a
        // loopback server so it does. Best-effort: a failure here just leaves
        // the configured host in place, exactly as before.
        m_mock_http = std::make_unique<helix::MockHttpFileServer>();
        if (!m_mock_http->start()) {
            m_mock_http.reset();
        }
    } else {
#endif
        spdlog::debug("[MoonrakerManager] Creating REAL client");
        auto real_client = std::make_unique<MoonrakerClient>();
#ifdef HELIX_ENABLE_MOCKS
        m_concrete_client = real_client.get();
#endif
        m_client = std::move(real_client);
#ifdef HELIX_ENABLE_MOCKS
    }
#endif
#endif // defined(ESP_PLATFORM)

    // Register with app_globals
    set_moonraker_client(m_client.get());
#ifdef HELIX_ENABLE_MOCKS
    // Publish the mock under its concrete type as well, so consumers that need
    // the mock-only API (AmsBackend's simulated-tool subscription) can reach it
    // without downcasting the interface. Null on a real-client run.
    set_moonraker_client_mock(mock_client);
#endif

    // Initialize SoundManager with client for M300 audio feedback
    SoundManager::instance().set_moonraker_client(m_client.get());

#ifdef HELIX_ENABLE_MOCKS
    // Mock-to-mock wiring: if an AmsBackendMock was already created during the
    // earlier AmsState init phase (which happens before this method runs),
    // hook it up to the freshly created moonraker mock so it follows the
    // simulator's active-gcode-tool. Production AMS backends ignore this —
    // they read printer.mmu.tool / toolchanger.tool_number directly from
    // Klipper. See post-1.0 issue #958 for the architecturally-correct fix.
    if (mock_client) {
        auto* backend = AmsState::instance().get_backend();
        // as_mock() is AmsBackend's RTTI-free stand-in for a dynamic_cast: it
        // returns null on every production backend and `this` on the mock.
        if (auto* ams_mock = backend ? backend->as_mock() : nullptr) {
            mock_client->add_active_gcode_tool_observer([ams_mock](int tool, uint32_t color) {
                ams_mock->on_simulated_gcode_tool_changed(tool, color);
            });
            spdlog::info("[MoonrakerManager] Mock AMS backend subscribed to simulator's "
                         "active-gcode-tool notifications");
        }
    }
#endif
}

void MoonrakerManager::configure_timeouts(Config* config) {
    if (!m_client || !config) {
        return;
    }

    uint32_t connection_timeout = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_connection_timeout_ms", 10000));
    uint32_t request_timeout = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_request_timeout_ms",
                         MoonrakerRequestTracker::DEFAULT_REQUEST_TIMEOUT_MS));
    uint32_t keepalive_interval = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_keepalive_interval_ms", 10000));
    uint32_t reconnect_min_delay = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_reconnect_min_delay_ms", 200));
    uint32_t reconnect_max_delay = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_reconnect_max_delay_ms", 2000));

    m_client->configure_timeouts(connection_timeout, request_timeout, keepalive_interval,
                                 reconnect_min_delay, reconnect_max_delay);

    spdlog::debug("[MoonrakerManager] Timeouts: connection={}ms, request={}ms, keepalive={}ms",
                  connection_timeout, request_timeout, keepalive_interval);
}

void MoonrakerManager::present_event(const MoonrakerEvent& evt) {
    // MAIN THREAD ONLY — reached through lifetime_.bg_cb() in register_callbacks().
    // Every lv_tr() below depends on that.
    const auto decision = helix::decide_moonraker_event(evt.type, evt.is_error, is_wizard_active(),
                                                        !ModalStack::instance().empty());

    switch (decision.route) {
    case helix::MoonrakerEventRoute::Ignore:
        switch (decision.suppressed_because) {
        case helix::MoonrakerEventSuppression::DiscoveryDeferred:
            spdlog::info("[MoonrakerManager] Suppressing deferred discovery: {}", evt.message);
            break;
        case helix::MoonrakerEventSuppression::Wizard:
            spdlog::debug("[MoonrakerManager] Suppressing '{}' toast during wizard", evt.message);
            break;
        case helix::MoonrakerEventSuppression::KlippyReady:
            spdlog::debug(
                "[MoonrakerManager] Suppressing Klipper ready notification (recovery UI owns the "
                "signal)");
            break;
        case helix::MoonrakerEventSuppression::None:
            break;
        }
        return;

    case helix::MoonrakerEventRoute::RecoveryDisconnected:
        // Unified recovery dialog (same dialog as SHUTDOWN state).
        EmergencyStopOverlay::instance().show_recovery_for(RecoveryReason::DISCONNECTED);
        return;

    case helix::MoonrakerEventRoute::RecoveryShutdown:
        EmergencyStopOverlay::instance().show_recovery_for(RecoveryReason::SHUTDOWN);
        return;

    case helix::MoonrakerEventRoute::ConnectionFailedModal: {
        // Not NOTIFY_ERROR_MODAL: that builds an OK-only alert, and OK on an
        // unreachable-address error returns the user to the same dead end. This
        // prompt carries a "Change Address" action straight to the host setting.
        const char* title = lv_tr(decision.title_tag);
        spdlog::error("[CRITICAL] {}: {}", title, evt.message);
        helix::ui::show_connection_failed_modal(title, evt.message);
        return;
    }

    case helix::MoonrakerEventRoute::ErrorToast: {
        const char* title = lv_tr(decision.title_tag);
        NOTIFY_ERROR_T(title, "{}", evt.message);
        return;
    }

    case helix::MoonrakerEventRoute::WarningToast:
        NOTIFY_WARNING("{}", evt.message);
        return;
    }
}

void MoonrakerManager::register_callbacks() {
    if (!m_client) {
        return;
    }

    // Capture alive flag for destruction detection [L012]
    auto alive = m_alive;

    // Register event handler for UI notifications.
    //
    // This fires on whatever thread raised the event: MoonrakerClient::emit_event()
    // invokes it synchronously from on_ws_close, the health-check timer, and
    // set_connection_state — i.e. the libhv event-loop thread. Everything the
    // handler goes on to do is LVGL-facing (translation lookup, toasts, modals),
    // so the entire body is marshalled to the main thread in ONE hop here rather
    // than each sink marshalling itself. Doing it per-sink is what left lv_tr()
    // running off-thread: lv_translation_get() reads the file-scope selected_lang
    // that lv_translation_set_language() frees and replaces, so a language change
    // overlapping an error event was a read of freed memory (#1219).
    // bg_cb() defers the WHOLE call to the main thread behind a generation guard,
    // which is what makes present_event()'s lv_tr() safe. It also decays the
    // MoonrakerEvent& into a value, so the body never sees a reference that died
    // with the raising thread's stack frame.
    m_client->register_event_handler(lifetime_.bg_cb(
        "MoonrakerManager::event", [this](const MoonrakerEvent& evt) { present_event(evt); }));

    // Set up state change callback to queue updates for main thread
    // CRITICAL: This runs on Moonraker thread, NOT main thread
    m_client->set_state_change_callback(
        [this, alive](ConnectionState old_state, ConnectionState new_state) {
            if (!alive->load())
                return;

            spdlog::trace("[MoonrakerManager] State change: {} -> {} (queueing)",
                          static_cast<int>(old_state), static_cast<int>(new_state));

            std::lock_guard<std::mutex> lock(m_notification_mutex);
            json state_change;
            state_change["_connection_state"] = true;
            state_change["old_state"] = static_cast<int>(old_state);
            state_change["new_state"] = static_cast<int>(new_state);
            m_notification_queue.push(state_change);
        });

    // Register notification callback to queue updates for main thread
    m_client->register_notify_update([this, alive](const json& notification) {
        if (!alive->load())
            return;

        std::lock_guard<std::mutex> lock(m_notification_mutex);
        m_notification_queue.push(notification);
    });
}

void MoonrakerManager::create_api(const RuntimeConfig& runtime_config) {
    spdlog::debug("[MoonrakerManager] Creating Moonraker API...");

#ifdef HELIX_ENABLE_MOCKS
    if (runtime_config.should_use_test_files()) {
        spdlog::debug("[MoonrakerManager] Creating MOCK API (local file transfers)");
        // MoonrakerAPIMock (and its sub-mocks) predate the IMoonrakerClient
        // split and still take a concrete helix::MoonrakerClient&.
        // create_client() records the concrete pointer as it builds it, so no
        // downcast is needed to get back to it here.
        assert(m_concrete_client && "create_client() must run before create_api()");
        auto mock_api = std::make_unique<MoonrakerAPIMock>(*m_concrete_client, get_printer_state());

        // Check HELIX_MOCK_SPOOLMAN env var
        const char* spoolman_env = std::getenv("HELIX_MOCK_SPOOLMAN");
        if (spoolman_env &&
            (std::string(spoolman_env) == "0" || std::string(spoolman_env) == "off")) {
            mock_api->spoolman_mock().set_mock_spoolman_enabled(false);
            spdlog::info("[MoonrakerManager] Mock Spoolman disabled via HELIX_MOCK_SPOOLMAN=0");
        }

        m_api = std::move(mock_api);
    } else {
#endif
        m_api = std::make_unique<MoonrakerAPI>(*m_client, get_printer_state());
#ifdef HELIX_ENABLE_MOCKS
    }
#endif

    // Register with app_globals
    set_moonraker_api(m_api.get());

    // Set API for AmsState Spoolman integration
    AmsState::instance().set_moonraker_api(m_api.get());

    // Set API for FilamentSensorManager bypass runout arming
    FilamentSensorManager::instance().set_moonraker_api(m_api.get());

    // Set API for SpoolmanManager weight polling
    SpoolmanManager::instance().set_api(m_api.get());

    // Note: EmergencyStopOverlay::init() and create() are called from Application
    // after both MoonrakerManager and SubjectInitializer are ready
}

void MoonrakerManager::init_print_start_collector() {
    if (!m_client) {
        spdlog::warn("[MoonrakerManager] Cannot init print_start_collector - no client");
        return;
    }

    // Create collector
    m_print_start_collector = std::make_shared<PrintStartCollector>(*m_client, get_printer_state());

    // Load print start profile based on detected printer type
    std::string printer_type = get_printer_state().get_printer_type();
    if (!printer_type.empty()) {
        std::string profile_name = PrinterDetector::get_print_start_profile(printer_type);
        if (!profile_name.empty()) {
            auto profile = PrintStartProfile::load(profile_name);
            m_print_start_collector->set_profile(profile);
            spdlog::debug("[MoonrakerManager] Loaded print start profile '{}' for printer '{}'",
                          profile_name, printer_type);
        } else {
            spdlog::debug(
                "[MoonrakerManager] No print start profile for printer '{}', using default",
                printer_type);
        }
    }

    // Bed-mesh presence verdicts feed the collector: a mesh cleared during
    // nozzle cleaning marks the start of leveling work that Creality-class
    // firmware does not echo to gcode_response. The observer fires on the
    // WebSocket thread; the weak_ptr keeps it safe across printer switches
    // (init_print_start_collector re-runs and replaces the collector).
    // Mesh probe-area bounds ride the same updates — they anchor the
    // position classifier's zones, and reading them here runs on the same
    // thread, ordered after the update_bed_mesh() write they come from.
    if (m_api) {
        std::weak_ptr<PrintStartCollector> collector_ref = m_print_start_collector;
        MoonrakerAPI* api = m_api.get();
        m_api->set_bed_mesh_presence_observer([api, collector_ref](bool present) {
            if (auto collector = collector_ref.lock()) {
                collector->note_bed_mesh_presence(present);
                if (auto* mesh = api->advanced().get_active_bed_mesh()) {
                    if (mesh->mesh_max[0] > mesh->mesh_min[0] &&
                        mesh->mesh_max[1] > mesh->mesh_min[1]) {
                        collector->note_mesh_bounds(mesh->mesh_min[0], mesh->mesh_max[0],
                                                    mesh->mesh_min[1], mesh->mesh_max[1]);
                    }
                }
            }
        });
    }

    // Store shared_ptr in a static for the lambda captures
    // This avoids the capturing lambda issue with ObserverGuard
    static std::weak_ptr<PrintStartCollector> s_collector;
    s_collector = m_print_start_collector;

    // Track previous state to detect TRANSITIONS to PRINTING, not just current state.
    // This prevents false triggers when the app starts while a print is already running.
    // (Similar pattern to print_start_navigation.cpp)
    //
    // Thread safety: this static is safe because LVGL subject observers always
    // fire on the main thread, synchronously.
    //
    // Lifetime: this function re-runs on EVERY printer switch, so the arming
    // state must be re-established here. It previously lived in two separate
    // statics; the initial-transition flag could not be reassigned (a
    // function-local static initializes once per process), so after the first
    // print the mid-print-join suppression was permanently off and switching to
    // a printer already partway through a job drew a "Preparing..." overlay over
    // a running print.
    static helix::PrintCollectorArming s_arming;
    s_arming.reset();
    // RAW_PRINT_STATE_OK: the collector MEASURES the pre-print window, so every
    // state it reacts to must be the printer's own. On the lifecycle its
    // completion signal would be the very state it is waiting to observe.
    s_arming.note_transition(static_cast<PrintJobState>(
        lv_subject_get_int(get_printer_state().get_print_state_enum_subject())));
    spdlog::debug("[MoonrakerManager] PRINT_START collector observer registered (initial state={})",
                  static_cast<int>(s_arming.prev_state()));

    // Capture print progress + duration subjects for mid-print detection.
    // Progress alone is unreliable on initial-state attach because the state
    // observer fires synchronously before virtual_sdcard / display_status are
    // processed in the same tick. print_duration is the load-bearing signal —
    // 0 at normal print start, >0 when joining a print already in progress.
    static lv_subject_t* s_progress_subject = nullptr;
    static lv_subject_t* s_print_duration_subject = nullptr;
    static lv_subject_t* s_layer_current_subject = nullptr;
    s_progress_subject = get_printer_state().get_print_progress_subject();
    s_print_duration_subject = get_printer_state().get_print_duration_subject();
    s_layer_current_subject = get_printer_state().get_print_layer_current_subject();

    // Observer to start/stop collector based on print state
    m_print_start_observer = ObserverGuard(
        // RAW_PRINT_STATE_OK: collector arming - see note_transition() above.
        get_printer_state().get_print_state_enum_subject(),
        [](lv_observer_t*, lv_subject_t* subject) {
            auto collector = s_collector.lock();
            if (!collector)
                return;

            // PRINT_STATE_CAST_OK: `subject` IS print_state_enum - LVGL hands the
            // observer the subject it registered on, so the pairing cannot drift.
            auto new_state = static_cast<PrintJobState>(lv_subject_get_int(subject));
            int current_progress = s_progress_subject ? lv_subject_get_int(s_progress_subject) : 0;
            int current_print_duration =
                s_print_duration_subject ? lv_subject_get_int(s_print_duration_subject) : 0;

            // Diagnostic for the stuck "Starting Print..." overlay: a mid-print
            // restart of the collector (prev state left the PRINTING/PAUSED pair,
            // e.g. during AFC error recovery, then returned to PRINTING) re-enters
            // the pre-print phase at high progress. Logging the full transition
            // with progress + duration captures the exact sequence on the next
            // field occurrence so the persistence path can be confirmed.
            spdlog::info("[MoonrakerManager] print_state transition: {} -> {} (progress={}%, "
                         "print_duration={}s, initial={}, collector_active={})",
                         static_cast<int>(s_arming.prev_state()), static_cast<int>(new_state),
                         current_progress, current_print_duration, s_arming.is_initial_transition(),
                         collector->is_active());

            // Use helper function for testable decision logic
            if (should_start_print_collector(s_arming.prev_state(), new_state, current_progress,
                                             s_arming.is_initial_transition(),
                                             current_print_duration)) {
                if (!collector->is_active()) {
                    collector->reset();
                    collector->start();
                    collector->enable_fallbacks();
                    spdlog::info("[MoonrakerManager] PRINT_START collector started");
                }
                s_arming.consume_initial_transition();
                // RAW_PRINT_STATE_OK: this whole dispatch - the collector
                // measures the pre-print window, so it arms and completes on the
                // PRINTER's own transitions. On the lifecycle its completion
                // signal would be the very state it is waiting to observe.
                // RAW_PRINT_STATE_OK: Moonraker confirming the print is running
                // is the collector's authoritative completion signal.
            } else if (new_state == PrintJobState::PRINTING && collector->is_active()) {
                // Authoritative signal: Moonraker confirms print is running.
                // This is the hard cutoff — if the collector is still active when
                // Klipper reports PRINTING, the pre-print phase is definitively over.
                spdlog::info("[MoonrakerManager] Authoritative: print_stats.state=printing, "
                             "completing pre-print phase");
                collector->complete_from_external_signal("Moonraker state=printing");
            } else if (s_arming.is_initial_transition() &&
                       // RAW_PRINT_STATE_OK: mid-print detection at startup.
                       s_arming.prev_state() != PrintJobState::PRINTING &&
                       s_arming.prev_state() != PrintJobState::PAUSED &&
                       new_state == PrintJobState::PRINTING && current_progress > 0) {
                // Log when we skip due to mid-print detection (app startup only)
                spdlog::info("[MoonrakerManager] Skipping PRINT_START collector - mid-print ({}%)",
                             current_progress);
                s_arming.consume_initial_transition();
            } else if (should_stop_print_collector(new_state,
                                                   get_printer_state().has_preparing_job())) {
                // No longer printing - stop collector if active. A live
                // preparing job means this is the transient hop INTO a print we
                // initiated, not the end of one.
                if (collector->is_active()) {
                    collector->stop();
                    spdlog::info("[MoonrakerManager] PRINT_START collector stopped");
                }
            }

            s_arming.note_transition(new_state);
        },
        nullptr);

    // Arm the collector when WE commit to a print, not only when the printer
    // reports one. A host-side pre-start block runs before the job is handed
    // over, so waiting for the printer edge leaves the whole window untracked:
    // the overlay shows a generic "Preparing Print..." and no phase ever
    // advances, because nothing is parsing gcode responses yet.
    m_preparing_epoch_observer = ObserverGuard(
        get_printer_state().get_preparing_epoch_subject(),
        [](lv_observer_t*, lv_subject_t* subject) {
            auto collector = s_collector.lock();
            if (!collector) {
                return;
            }
            if (lv_subject_get_int(subject) <= 0) {
                // Retirement. The print-state observer cannot cover this: it
                // only fires when print_state_enum CHANGES, and a job that dies
                // before the printer accepts it never moves the wire off
                // standby. Leaving the collector armed means the next G-code the
                // user runs by hand is parsed as a pre-print phase, re-raising
                // the "Preparing Print" overlay over whatever they are doing.
                // RAW_PRINT_STATE_OK: collector teardown mirrors its arming.
                const auto job_state = static_cast<helix::PrintJobState>(
                    lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
                if (collector->is_active() && should_stop_collector_on_retirement(job_state)) {
                    collector->stop();
                    spdlog::info("[MoonrakerManager] PRINT_START collector stopped (retired "
                                 "without a print)");
                }
                return;
            }
            if (collector->is_active()) {
                return; // already tracking
            }
            collector->reset();
            collector->start();
            collector->enable_fallbacks();
            spdlog::info("[MoonrakerManager] PRINT_START collector started (commit)");
        },
        nullptr);

    // Observer for print start phase completion
    m_print_start_phase_observer = ObserverGuard(
        get_printer_state().get_print_start_phase_subject(),
        [](lv_observer_t*, lv_subject_t* subject) {
            auto collector = s_collector.lock();
            if (!collector)
                return;

            auto phase = static_cast<PrintStartPhase>(lv_subject_get_int(subject));
            if (phase == PrintStartPhase::COMPLETE) {
                if (collector->is_active()) {
                    collector->stop();
                    spdlog::info(
                        "[MoonrakerManager] PRINT_START collector stopped (phase=COMPLETE)");
                }
            }
        },
        nullptr);

    // Real-first-layer signal (primary): the pre-print → printing hand-off is
    // gated on print_stats.info.current_layer >= 1, NOT raw extrusion. On the
    // Snapmaker U1 (and any firmware that purges / auto-feeds during the
    // print_stats.state=printing window) print_duration goes positive while the
    // nozzle is still heating and the toolhead is still homing, so a
    // print_duration-based shortcut dropped the Preparing/Homing phase minutes
    // early. current_layer is firmware-agnostic and only reaches 1 at the
    // genuine first layer. See should_complete_preprint().
    m_print_layer_observer = ObserverGuard(
        get_printer_state().get_print_layer_current_subject(),
        [](lv_observer_t*, lv_subject_t* subject) {
            auto collector = s_collector.lock();
            if (!collector || !collector->is_active())
                return;
            int current_layer = lv_subject_get_int(subject);
            int print_duration =
                s_print_duration_subject ? lv_subject_get_int(s_print_duration_subject) : 0;
            // Discriminate on the STICKY printer capability, NOT the racy
            // per-print has_real_layer_data (cleared async by
            // reset_for_new_print() after the collector starts — see
            // should_complete_preprint()).
            bool printer_reports_layers = get_printer_state().printer_reports_layers();
            // Arm the layer-1 edge: latch once we observe current_layer < 1 for
            // THIS print, so a stale positive carried over from the previous
            // print (reset_for_new_print() runs async, after the collector goes
            // active) can't complete the new pre-print phase instantly.
            collector->note_current_layer(current_layer);
            if (should_complete_preprint(printer_reports_layers, current_layer, print_duration,
                                         collector->has_seen_layer_zero(),
                                         collector->has_seen_layer_advance())) {
                spdlog::info("[MoonrakerManager] Authoritative: first real layer detected "
                             "(current_layer={}, printer_reports_layers={}, seen_zero={}, "
                             "advanced={}), completing pre-print phase",
                             current_layer, printer_reports_layers,
                             collector->has_seen_layer_zero(), collector->has_seen_layer_advance());
                collector->complete_from_external_signal("first layer");
            }
        },
        nullptr);

    // Fallback for printers that have NEVER reported any layer field this
    // session (printer_reports_layers sticky-false): when print_duration goes
    // positive the current_layer subject only carries a progress-derived
    // estimate, so the layer observer above can't be trusted.
    // should_complete_preprint() routes those printers back to the original
    // first-extrusion behavior so they still leave Preparing. When
    // printer_reports_layers is true this observer is a no-op — the layer
    // observer owns completion and the print_duration fallback NEVER applies
    // (the U1 premature-completion regression was this fallback firing during a
    // layer-reporting printer's pre-print purge).
    m_print_duration_observer = ObserverGuard(
        get_printer_state().get_print_duration_subject(),
        [](lv_observer_t*, lv_subject_t* subject) {
            auto collector = s_collector.lock();
            if (!collector || !collector->is_active())
                return;
            int print_duration = lv_subject_get_int(subject);
            int current_layer =
                s_layer_current_subject ? lv_subject_get_int(s_layer_current_subject) : 0;
            bool printer_reports_layers = get_printer_state().printer_reports_layers();
            collector->note_current_layer(current_layer);
            // Prime/purge phase nudge (layer-reporting printers only): on the
            // U1 the initial prime line ("G1 X110 E15") extrudes silently — no
            // gcode_response, and PRINT_PREEXTRUDING only fires for a 2nd tool
            // mid-print. print_duration going 0->positive while current_layer is
            // still < 1 is the one observable "priming has begun" signal. Show
            // "Priming..." WITHOUT completing — completion stays gated on the
            // genuine current_layer 0->1 edge below / in the layer observer.
            // Skipped for non-reporting printers: there, print_duration>0 IS the
            // completion signal (handled by should_complete_preprint), so a
            // PURGING nudge would just be immediately replaced by COMPLETE.
            if (printer_reports_layers && print_duration > 0 && current_layer < 1) {
                collector->note_priming();
            }
            if (should_complete_preprint(printer_reports_layers, current_layer, print_duration,
                                         collector->has_seen_layer_zero(),
                                         collector->has_seen_layer_advance())) {
                spdlog::info("[MoonrakerManager] Authoritative: pre-print complete via "
                             "print_duration fallback (print_duration={}s, "
                             "printer_reports_layers={}), completing pre-print phase",
                             print_duration, printer_reports_layers);
                collector->complete_from_external_signal("first extrusion (fallback)");
            }
        },
        nullptr);

    // Fallback observers: trigger check_fallback_completion() when temperature targets change.
    // Proactive heating phase detection fires immediately when heater targets change (without
    // these, proactive detection only runs from the 5-second ETA timer).
    auto fallback_cb = [](lv_observer_t*, lv_subject_t*) {
        auto collector = s_collector.lock();
        if (collector && collector->is_active()) {
            collector->check_fallback_completion();
        }
    };
    m_print_bed_target_fallback_observer = ObserverGuard(
        get_printer_state().get_bed_target_subject(m_print_bed_target_fallback_lifetime),
        fallback_cb, nullptr);
    m_print_bed_target_fallback_observer.set_alive_token(m_print_bed_target_fallback_lifetime);
    m_print_ext_target_fallback_observer = ObserverGuard(
        get_printer_state().get_active_extruder_target_subject(), fallback_cb, nullptr);

    // Toolhead position feeds the collector's silent-window inference
    // ("Probing Z..." / "Checking Bed Mesh..." / sweep → bed mesh). The
    // subjects fire on the main thread (queued status updates), only while
    // the toolhead moves; the collector gates on its profile and drops
    // duplicates. One guard per axis — each callback reads the coherent
    // triple, and near-identical repeat calls are deduped downstream.
    auto position_cb = [](lv_observer_t*, lv_subject_t*) {
        auto collector = s_collector.lock();
        if (collector && collector->is_active()) {
            auto& state = get_printer_state();
            collector->note_position_sample(
                static_cast<float>(
                    helix::units::from_centimm(lv_subject_get_int(state.get_position_x_subject()))),
                static_cast<float>(
                    helix::units::from_centimm(lv_subject_get_int(state.get_position_y_subject()))),
                static_cast<float>(helix::units::from_centimm(
                    lv_subject_get_int(state.get_position_z_subject()))));
        }
    };
    m_print_position_observers[0] =
        ObserverGuard(get_printer_state().get_position_x_subject(), position_cb, nullptr);
    m_print_position_observers[1] =
        ObserverGuard(get_printer_state().get_position_y_subject(), position_cb, nullptr);
    m_print_position_observers[2] =
        ObserverGuard(get_printer_state().get_position_z_subject(), position_cb, nullptr);

    spdlog::debug("[MoonrakerManager] Print start collector initialized");
}

void MoonrakerManager::init_macro_analysis(Config* config) {
    if (!m_api) {
        spdlog::warn("[MoonrakerManager] Cannot init macro_analysis - no API");
        return;
    }

    m_macro_analysis = std::make_unique<helix::MacroModificationManager>(config, m_api.get());
    spdlog::debug("[MoonrakerManager] Macro modification manager initialized");
}

helix::MacroModificationManager* MoonrakerManager::macro_analysis() const {
    return m_macro_analysis.get();
}
