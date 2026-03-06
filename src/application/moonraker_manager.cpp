// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file moonraker_manager.cpp
 * @brief Orchestrates MoonrakerClient lifecycle and WebSocket notification dispatch
 *
 * @pattern Manager with shared_ptr<atomic<bool>> alive flag for callback safety
 * @threading Queues notifications from WebSocket thread to main thread
 * @gotchas Set m_alive flag FIRST in shutdown before waiting on callbacks
 *
 * @see moonraker_client.cpp, printer_state.cpp
 */

#include "moonraker_manager.h"

#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "abort_manager.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "macro_modification_manager.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#ifdef HELIX_ENABLE_MOCKS
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#endif
#include "print_completion.h"
#include "print_start_collector.h"
#include "print_start_profile.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "sound_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <vector>

using namespace helix;

MoonrakerManager::MoonrakerManager() : m_startup_time(std::chrono::steady_clock::now()) {}

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

    if (!m_initialized) {
        return;
    }

    spdlog::debug("[MoonrakerManager] Shutting down...");

    // Stop print start collector first (before client is destroyed)
    if (m_print_start_collector) {
        m_print_start_collector->stop();
        m_print_start_collector.reset();
    }

    // Clean up macro analysis manager
    m_macro_analysis.reset();

    // Release observer guards without calling lv_observer_remove().
    // During shutdown, subjects may already be deinitialized (which frees observers).
    // Using release() avoids double-free of already-removed observers.
    m_print_start_observer.release();
    m_print_start_phase_observer.release();
    m_print_layer_fallback_observer.release();
    m_print_progress_fallback_observer.release();

    // Clear API before client (API uses client)
    m_api.reset();
    m_client.reset();

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
        m_api->set_http_base_url(http_base_url);
    }

    // Connect client - on_connected triggers printer discovery which subscribes to status updates
    // CRITICAL: Without discover_printer(), we never call printer.objects.subscribe,
    // so we never receive notify_status_update messages (print_stats, temperatures, etc.)
    MoonrakerClient* client = m_client.get();
    MoonrakerAPI* api = m_api.get();
    helix::MacroModificationManager* macro_mgr = m_macro_analysis.get();
    return m_client->connect(
        websocket_url.c_str(),
        [client, api, macro_mgr]() {
            // Connection established - start printer discovery
            // This queries printer capabilities and subscribes to status updates
            spdlog::info("[MoonrakerManager] Connected, starting printer discovery...");
            client->discover_printer([api, macro_mgr]() {
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
        []() {
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
                if (method_str.is_string() && method_str.get<std::string>() == "notify_status_update") {
                    auto& params = notification["params"];
                    if (params.is_array() && !params.empty()) {
                        get_printer_state().update_from_status(params[0]);
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
    if (runtime_config.should_mock_moonraker()) {
        double speedup = runtime_config.sim_speedup;
        spdlog::debug("[MoonrakerManager] Creating MOCK client (Voron 2.4, {}x speed)", speedup);
        auto mock = std::make_unique<MoonrakerClientMock>(
            MoonrakerClientMock::PrinterType::VORON_24, speedup);
        // Disable MMU if AMS is explicitly disabled via CLI or env var
        const char* mock_ams_env = std::getenv("HELIX_MOCK_AMS");
        bool ams_disabled = runtime_config.disable_mock_ams ||
                            (mock_ams_env && std::string(mock_ams_env) == "none");
        if (ams_disabled) {
            mock->set_mmu_enabled(false);
        }
        m_client = std::move(mock);
    } else {
#endif
        spdlog::debug("[MoonrakerManager] Creating REAL client");
        m_client = std::make_unique<MoonrakerClient>();
#ifdef HELIX_ENABLE_MOCKS
    }
#endif

    // Register with app_globals
    set_moonraker_client(m_client.get());

    // Initialize SoundManager with client for M300 audio feedback
    SoundManager::instance().set_moonraker_client(m_client.get());
}

void MoonrakerManager::configure_timeouts(Config* config) {
    if (!m_client || !config) {
        return;
    }

    uint32_t connection_timeout = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_connection_timeout_ms", 10000));
    uint32_t request_timeout = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_request_timeout_ms", 30000));
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

void MoonrakerManager::register_callbacks() {
    if (!m_client) {
        return;
    }

    // Register event handler for UI notifications
    m_client->register_event_handler([this](const MoonrakerEvent& evt) {
        const char* title = "Printer Error"; // Default title (never nullptr!)
        if (evt.type == MoonrakerEventType::CONNECTION_FAILED) {
            title = "Connection Failed";
        } else if (evt.type == MoonrakerEventType::KLIPPY_DISCONNECTED) {
            // Route through unified recovery dialog (same dialog as SHUTDOWN state)
            // Suppression checks are handled inside show_recovery_for()
            EmergencyStopOverlay::instance().show_recovery_for(RecoveryReason::DISCONNECTED);
            return;
        } else if (evt.type == MoonrakerEventType::RPC_ERROR) {
            title = "Request Failed";
        }

        // Suppress expected transient events during startup grace period
        auto now = std::chrono::steady_clock::now();
        bool within_grace_period =
            (now - m_startup_time) < AppConstants::Startup::NOTIFICATION_GRACE_PERIOD;

        if (evt.is_error) {
            // Suppress discovery failures during startup — Klipper may still be starting
            // on slow embedded devices (e.g. AD5M). Discovery will retry automatically.
            if (evt.type == MoonrakerEventType::DISCOVERY_FAILED && within_grace_period) {
                spdlog::info("[MoonrakerManager] Suppressing startup discovery failure: {}",
                             evt.message);
                return;
            }

            bool is_critical = (evt.type == MoonrakerEventType::CONNECTION_FAILED);
            if (is_critical) {
                NOTIFY_ERROR_MODAL(title, "{}", evt.message);
            } else {
                NOTIFY_ERROR_T(title, "{}", evt.message);
            }
        } else {
            // Suppress non-error toasts during wizard (first connection, not a "reconnection")
            if (is_wizard_active()) {
                spdlog::debug("[MoonrakerManager] Suppressing '{}' toast during wizard",
                              evt.message);
                return;
            }

            // Suppress "Klipper ready" toast during startup (expected at boot)
            if (evt.type == MoonrakerEventType::KLIPPY_READY && within_grace_period) {
                spdlog::info("[MoonrakerManager] Suppressing startup Klipper ready notification");
                return;
            }
            NOTIFY_WARNING("{}", evt.message);
        }
    });

    // Capture alive flag for destruction detection [L012]
    auto alive = m_alive;

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
        auto mock_api = std::make_unique<MoonrakerAPIMock>(*m_client, get_printer_state());

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

    // Store shared_ptr in a static for the lambda captures
    // This avoids the capturing lambda issue with ObserverGuard
    static std::weak_ptr<PrintStartCollector> s_collector;
    s_collector = m_print_start_collector;

    // Track previous state to detect TRANSITIONS to PRINTING, not just current state.
    // This prevents false triggers when the app starts while a print is already running.
    // (Similar pattern to print_start_navigation.cpp)
    //
    // Thread safety: These statics are safe because:
    // 1. init_print_start_collector() called once on main thread
    // 2. LVGL subject observers always fire on main thread (synchronous)
    static PrintJobState s_prev_print_state = PrintJobState::STANDBY;
    s_prev_print_state = static_cast<PrintJobState>(
        lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
    spdlog::debug("[MoonrakerManager] PRINT_START collector observer registered (initial state={})",
                  static_cast<int>(s_prev_print_state));

    // Capture print progress subject for mid-print detection
    static lv_subject_t* s_progress_subject = nullptr;
    s_progress_subject = get_printer_state().get_print_progress_subject();

    // Observer to start/stop collector based on print state
    m_print_start_observer = ObserverGuard(
        get_printer_state().get_print_state_enum_subject(),
        [](lv_observer_t*, lv_subject_t* subject) {
            auto collector = s_collector.lock();
            if (!collector)
                return;

            auto new_state = static_cast<PrintJobState>(lv_subject_get_int(subject));
            int current_progress = s_progress_subject ? lv_subject_get_int(s_progress_subject) : 0;

            // Use helper function for testable decision logic
            if (should_start_print_collector(s_prev_print_state, new_state, current_progress)) {
                if (!collector->is_active()) {
                    collector->reset();
                    collector->start();
                    collector->enable_fallbacks();
                    spdlog::info("[MoonrakerManager] PRINT_START collector started");
                }
            } else if (s_prev_print_state != PrintJobState::PRINTING &&
                       s_prev_print_state != PrintJobState::PAUSED &&
                       new_state == PrintJobState::PRINTING && current_progress > 0) {
                // Log when we skip due to mid-print detection
                spdlog::info("[MoonrakerManager] Skipping PRINT_START collector - mid-print ({}%)",
                             current_progress);
            } else if (new_state != PrintJobState::PRINTING && new_state != PrintJobState::PAUSED) {
                // No longer printing - stop collector if active
                if (collector->is_active()) {
                    collector->stop();
                    spdlog::info("[MoonrakerManager] PRINT_START collector stopped");
                }
            }

            s_prev_print_state = new_state;
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

    // Fallback observers
    m_print_layer_fallback_observer = ObserverGuard(
        get_printer_state().get_print_layer_current_subject(),
        [](lv_observer_t*, lv_subject_t*) {
            auto collector = s_collector.lock();
            if (collector && collector->is_active()) {
                collector->check_fallback_completion();
            }
        },
        nullptr);

    m_print_progress_fallback_observer = ObserverGuard(
        get_printer_state().get_print_progress_subject(),
        [](lv_observer_t*, lv_subject_t*) {
            auto collector = s_collector.lock();
            if (collector && collector->is_active()) {
                collector->check_fallback_completion();
            }
        },
        nullptr);

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
