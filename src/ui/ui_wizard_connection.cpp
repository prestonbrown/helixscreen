// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_connection.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_notification.h"
#include "ui_subject_registry.h"
#include "ui_timer_guard.h"
#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "platform_info.h"
#include "printer_discovery.h"
#include "runtime_config.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "wizard_config_paths.h"
#include "wizard_validation.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>
#include <string>

using namespace helix;

// ============================================================================
// External Subject (defined in ui_wizard.cpp)
// ============================================================================

// Controls wizard Next button globally - shared across wizard steps
extern lv_subject_t connection_test_passed;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardConnectionStep> g_wizard_connection_step;

WizardConnectionStep* get_wizard_connection_step() {
    if (!g_wizard_connection_step) {
        g_wizard_connection_step = std::make_unique<WizardConnectionStep>();
        StaticPanelRegistry::instance().register_destroy(
            "WizardConnectionStep", []() { g_wizard_connection_step.reset(); });
    }
    return g_wizard_connection_step.get();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardConnectionStep::WizardConnectionStep() {
    // Zero-initialize buffers
    std::memset(connection_ip_buffer_, 0, sizeof(connection_ip_buffer_));
    std::memset(connection_port_buffer_, 0, sizeof(connection_port_buffer_));
    std::memset(connection_status_icon_buffer_, 0, sizeof(connection_status_icon_buffer_));
    std::memset(connection_status_text_buffer_, 0, sizeof(connection_status_text_buffer_));
    std::memset(mdns_status_buffer_, 0, sizeof(mdns_status_buffer_));

    // NOTE: mDNS discovery is created lazily in create() to avoid spawning
    // background threads in test fixtures where should_mock_mdns() is true

    spdlog::debug("[{}] Instance created", get_name());
}

WizardConnectionStep::~WizardConnectionStep() {
    // Expire all outstanding lifetime tokens — replaces m_alive (#193)
    lifetime_.invalidate();

    // NOTE: Do NOT call LVGL functions here - LVGL may be destroyed first
    // NOTE: Do NOT log here - spdlog may be destroyed first

    // Exception to the NOTE above: shutdown runs StaticPanelRegistry::destroy_all()
    // BEFORE lv_deinit(), so a watchdog armed at this point is still in LVGL's timer
    // list holding a freed `this`. Both cancels go through lv_timer_cancel_safe(),
    // which no-ops when lv_is_initialized() is false, so they are safe either way.
    // The auto-probe timer needs this too: cleanup() cancels it, but a teardown
    // path that destroys the step without calling cleanup() first leaves the
    // one-shot armed on a freed `this` (the UAF auto_probe_timer_cb documents).
    cancel_auto_probe_timer();
    cancel_discovery_watchdog();

    screen_root_ = nullptr;
}

void WizardConnectionStep::set_mdns_discovery(std::unique_ptr<IMdnsDiscovery> discovery) {
    mdns_discovery_ = std::move(discovery);
}

// ============================================================================
// ============================================================================
// Subject Initialization
// ============================================================================

void WizardConnectionStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    // Seed the buffers ONLY on first init. On revisits we want to preserve
    // whatever the user already typed (or the value most recently written via
    // the bind_text two-way sync). Otherwise navigating away and back wipes
    // unsaved input. The buffer IS the subject's storage — preserving the
    // buffer preserves the user's in-progress entry.
    if (!subjects_initialized_) {
        Config* config = Config::get_instance();
        std::string default_ip = helix::is_android_platform() ? "" : "127.0.0.1";
        std::string default_port = "7125"; // Default Moonraker port

        try {
            // A factory reset or --wizard re-run persists an empty host string;
            // Config::get returns it verbatim, so resolve an empty stored host back
            // to the localhost default rather than leaving the field blank (users
            // report 127.0.0.1 not pre-filled).
            std::string stored_ip =
                config->get<std::string>(config->df() + helix::wizard::MOONRAKER_HOST, default_ip);
            default_ip = resolve_moonraker_host_default(stored_ip, helix::is_android_platform());
            int port_num = config->get<int>(config->df() + helix::wizard::MOONRAKER_PORT, 7125);
            default_port = std::to_string(port_num);

            spdlog::debug("[{}] Loaded from config: {}:{}", get_name(), default_ip, default_port);
        } catch (const std::exception& e) {
            spdlog::debug("[{}] No existing config, using defaults: {}", get_name(), e.what());
        }

        // Initialize with values from config or defaults
        strncpy(connection_ip_buffer_, default_ip.c_str(), sizeof(connection_ip_buffer_) - 1);
        connection_ip_buffer_[sizeof(connection_ip_buffer_) - 1] = '\0';

        strncpy(connection_port_buffer_, default_port.c_str(), sizeof(connection_port_buffer_) - 1);
        connection_port_buffer_[sizeof(connection_port_buffer_) - 1] = '\0';
    } else {
        spdlog::debug("[{}] Re-init: preserving buffer (IP='{}', Port='{}')", get_name(),
                      connection_ip_buffer_, connection_port_buffer_);
    }

    UI_SUBJECT_INIT_AND_REGISTER_STRING(connection_ip_, connection_ip_buffer_,
                                        connection_ip_buffer_, "connection_ip");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(connection_port_, connection_port_buffer_,
                                        connection_port_buffer_, "connection_port");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(connection_status_icon_, connection_status_icon_buffer_, "",
                                        "connection_status_icon");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(connection_status_text_, connection_status_text_buffer_, "",
                                        "connection_status_text");
    UI_SUBJECT_INIT_AND_REGISTER_INT(connection_testing_, 0, "connection_testing");
    UI_SUBJECT_INIT_AND_REGISTER_INT(connection_discovering_, 0, "connection_discovering");

    // mDNS discovery subjects
    UI_SUBJECT_INIT_AND_REGISTER_STRING(mdns_status_, mdns_status_buffer_, lv_tr("Scanning..."),
                                        "mdns_status");

    // Set connection_test_passed to 0 (disabled) for this step
    lv_subject_set_int(&connection_test_passed, 0);

    // Reset validation state
    connection_validated_ = false;
    subjects_initialized_ = true;

    spdlog::debug("[{}] Subjects initialized (IP: '{}', Port: '{}')", get_name(),
                  connection_ip_buffer_, connection_port_buffer_);
}

// ============================================================================
// Static Trampolines for LVGL Callbacks
// ============================================================================

void WizardConnectionStep::on_test_connection_clicked_static(lv_event_t* e) {
    auto* self = static_cast<WizardConnectionStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_test_connection_clicked();
    }
}

void WizardConnectionStep::on_ip_input_changed_static(lv_event_t* e) {
    auto* self = static_cast<WizardConnectionStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_ip_input_changed();
    }
}

void WizardConnectionStep::on_port_input_changed_static(lv_event_t* e) {
    auto* self = static_cast<WizardConnectionStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_port_input_changed();
    }
}

// ============================================================================
// Event Handler Implementations
// ============================================================================

void WizardConnectionStep::handle_test_connection_clicked() {
    LVGL_SAFE_EVENT_CB_BEGIN("[Wizard Connection] handle_test_connection_clicked");

    // Get values from subjects
    const char* ip = lv_subject_get_string(&connection_ip_);
    std::string port_clean = sanitize_port(lv_subject_get_string(&connection_port_));

    // Auto-fill default Moonraker port when empty
    if (port_clean.empty()) {
        port_clean = "7125";
        lv_subject_copy_string(&connection_port_, "7125");
        if (screen_root_) {
            lv_obj_t* port_input = lv_obj_find_by_name(screen_root_, "port_input");
            if (port_input) {
                lv_textarea_set_text(port_input, "7125");
            }
        }
        spdlog::debug("[{}] Port empty, auto-filled default: 7125", get_name());
    }

    spdlog::debug("[{}] Test connection clicked: {}:{}", get_name(), ip, port_clean);

    // Clear previous validation state
    connection_validated_ = false;
    lv_subject_set_int(&connection_test_passed, 0);

    // Validate inputs
    if (!ip || strlen(ip) == 0) {
        set_status(nullptr, StatusVariant::None, "Please enter an IP address or hostname");
        spdlog::warn("[{}] Empty IP address", get_name());
        return;
    }

    if (!is_valid_ip_or_hostname(ip)) {
        set_status("icon_close_circle", StatusVariant::Danger, "Invalid IP address or hostname");
        spdlog::warn("[{}] Invalid IP/hostname: {}", get_name(), ip);
        return;
    }

    if (!is_valid_port(port_clean)) {
        set_status("icon_close_circle", StatusVariant::Danger, "Invalid port (must be 1-65535)");
        spdlog::warn("[{}] Invalid port: {}", get_name(), port_clean);
        return;
    }

    // Get MoonrakerClient instance
    IMoonrakerClient* client = get_moonraker_client();
    if (!client) {
        set_status("icon_close_circle", StatusVariant::Danger,
                   "Error: Moonraker client not initialized");
        lv_subject_set_int(&connection_testing_, 0);
        LOG_ERROR_INTERNAL("[{}] MoonrakerClient is nullptr", get_name());
        return;
    }

    // Disconnect any previous connection attempt
    client->disconnect();

    // Invalidate pending callbacks from previous attempts, then grab fresh token
    lifetime_.invalidate();
    auto tok = lifetime_.token();

    // The previous attempt's discovery is abandoned along with its callbacks;
    // its watchdog must not survive to unblock Next behind this new attempt.
    cancel_discovery_watchdog();

    // Store IP/port for async callback (thread-safe)
    {
        std::lock_guard<std::mutex> lock(saved_values_mutex_);
        saved_ip_ = ip;
        saved_port_ = port_clean;
    }

    // Set UI to testing state
    lv_subject_set_int(&connection_testing_, 1);
    set_status("icon_question_circle", StatusVariant::None, "Testing connection...");

    spdlog::debug("[{}] Starting connection test to {}:{}", get_name(), ip, port_clean);

    // Set shorter timeout for wizard testing
    client->set_connection_timeout(5000);

    // Construct WebSocket URL
    std::string ws_url = "ws://" + std::string(ip) + ":" + port_clean + "/websocket";

    int result = client->connect(
        ws_url.c_str(),
        // On connected callback
        [this, tok]() {
            tok.defer("WizardConnectionStep::dispatch_connection_success",
                      [this, tok]() { on_connection_success(tok); });
        },
        // On disconnected callback
        [this, tok]() {
            tok.defer("WizardConnectionStep::dispatch_connection_failure",
                      [this, tok]() { on_connection_failure(tok); });
        });

    // Disable automatic reconnection for wizard testing
    client->set_auto_reconnect(false);

    if (result != 0) {
        spdlog::error("[{}] Failed to initiate connection: {}", get_name(), result);
        set_status("icon_close_circle", StatusVariant::Danger, "Error starting connection test");
        lv_subject_set_int(&connection_testing_, 0);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void WizardConnectionStep::on_connection_success(const helix::LifetimeToken& tok) {
    // Already on main thread — caller dispatches via tok.defer (see handle_test).
    // The inner discover_printer cbs DO fire on bg, so we keep those defers.
    spdlog::info("[Wizard Connection] Connection successful!");

    // Get saved values under lock
    std::string ip, port;
    {
        std::lock_guard<std::mutex> lock(saved_values_mutex_);
        ip = saved_ip_;
        port = saved_port_;
    }

    // Persist config (safe on main thread)
    Config* config = Config::get_instance();
    try {
        config->set(config->df() + helix::wizard::MOONRAKER_HOST, ip);
        config->set(config->df() + helix::wizard::MOONRAKER_PORT, std::stoi(port));
        if (config->save()) {
            spdlog::debug("[Wizard Connection] Saved configuration: {}:{}", ip, port);
        } else {
            spdlog::error("[Wizard Connection] Failed to save configuration!");
        }
    } catch (const std::exception& e) {
        spdlog::error("[Wizard Connection] Failed to save config: {}", e.what());
    }

    // Show "discovering" status - spinner shows via XML binding
    lv_subject_set_int(&connection_discovering_, 1);
    set_status(nullptr, StatusVariant::None, lv_tr("Connected! Discovering printer..."));
    lv_subject_set_int(&connection_testing_, 0);

    // Set HTTP base URL so discovery can make HTTP calls
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        std::string http_url = "http://" + ip + ":" + port;
        api->set_http_base_url(http_url);
    }

    // Trigger hardware discovery - only enable Next when this completes
    IMoonrakerClient* client = get_moonraker_client();
    if (client) {
        // discover_printer() can return without ever invoking either callback
        // (stale generation / superseded sequence), which stranded the spinner
        // and Next forever (#1161). Bound the wait from here.
        start_discovery_watchdog();

        client->discover_printer(
            // Success callback (fires on background thread)
            [this, tok]() {
                spdlog::info("[Wizard Connection] Hardware discovery complete!");

                tok.defer("WizardConnectionStep::discovery_success", [this]() {
                    cancel_discovery_watchdog();

                    IMoonrakerAPI* api = get_moonraker_api();
                    IMoonrakerClient* client = get_moonraker_client();
                    if (api) {
                        const auto& heaters = api->hardware().heaters();
                        const auto& sensors = api->hardware().sensors();
                        const auto& fans = api->hardware().fans();
                        spdlog::info("[Wizard Connection] Discovered {} heaters, {} "
                                     "sensors, {} fans",
                                     heaters.size(), sensors.size(), fans.size());
                        spdlog::info("[Wizard Connection] Hostname: '{}'",
                                     api->hardware().hostname());

                        helix::init_subsystems_from_hardware(api->hardware(), api, client);
                    }

                    // NOW enable Next button - discovery is complete
                    lv_subject_set_int(&connection_discovering_, 0);
                    set_status("icon_check_circle", StatusVariant::Success,
                               lv_tr("Connection successful!"));
                    connection_validated_ = true;
                    lv_subject_set_int(&connection_test_passed, 1);
                });
            },
            // Error callback - discovery failed (e.g., Klippy not connected)
            [this, tok](const std::string& reason) {
                spdlog::warn("[Wizard Connection] Discovery failed: {}", reason);

                tok.defer("WizardConnectionStep::discovery_error",
                          [this]() { allow_continue_without_klipper(); });
            });
    } else {
        // No client available - still show success but warn
        lv_subject_set_int(&connection_discovering_, 0);
        set_status("icon_check_circle", StatusVariant::Success, lv_tr("Connected (no discovery)"));
        connection_validated_ = true;
        lv_subject_set_int(&connection_test_passed, 1);
    }
}

void WizardConnectionStep::on_connection_failure(const helix::LifetimeToken& /*tok*/) {
    // Already on main thread — caller dispatches via tok.defer (see handle_test).
    spdlog::debug("[Wizard Connection] on_disconnected fired");

    int testing_state = lv_subject_get_int(&connection_testing_);
    spdlog::debug("[Wizard Connection] Connection failure, testing_state={}", testing_state);

    if (testing_state == 1) {
        spdlog::error("[Wizard Connection] Connection failed");

        set_status("icon_close_circle", StatusVariant::Danger,
                   lv_tr("Connection failed. Check IP/port and try again."));
        lv_subject_set_int(&connection_testing_, 0);
        connection_validated_ = false;
        lv_subject_set_int(&connection_test_passed, 0);
    } else {
        spdlog::debug("[Wizard Connection] Ignoring disconnect (not in testing mode)");
    }
}

// ============================================================================
// Auto-Probe Methods
// ============================================================================

bool WizardConnectionStep::should_auto_probe() const {
    // Don't probe if already attempted this session
    if (auto_probe_attempted_) {
        return false;
    }

    // Don't probe if already testing a connection
    if (lv_subject_get_int(const_cast<lv_subject_t*>(&connection_testing_)) == 1) {
        return false;
    }

    // Don't probe if already validated
    if (connection_validated_) {
        return false;
    }

    // Probe both when:
    // 1. IP is empty (no saved config) - will probe 127.0.0.1
    // 2. IP is set but not validated yet - will test the saved config
    return true;
}

void WizardConnectionStep::auto_probe_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<WizardConnectionStep*>(lv_timer_get_user_data(timer));
    if (self) {
        // The timer is one-shot — LVGL deletes it immediately after this callback
        // returns. Null our member pointer FIRST so any path through
        // attempt_auto_probe() (including early-returns) can't leave a dangling
        // pointer that cleanup() later tries to cancel. ASAN-confirmed UAF
        // (heap-use-after-free in lv_timer_set_cb via lv_timer_cancel_safe) when
        // attempt_auto_probe early-returned on empty probe_ip.
        self->auto_probe_timer_ = nullptr;
        self->attempt_auto_probe();
    }
}

void WizardConnectionStep::attempt_auto_probe() {
    // Get the IP/port from subjects - may be from config or default
    const char* ip = lv_subject_get_string(&connection_ip_);
    std::string port_clean = sanitize_port(lv_subject_get_string(&connection_port_));

    std::string probe_ip = (ip && strlen(ip) > 0) ? ip : "";
    if (probe_ip.empty()) {
        spdlog::debug("[{}] Auto-probe: No IP configured, skipping", get_name());
        auto_probe_state_.store(AutoProbeState::FAILED);
        return;
    }
    std::string probe_port = !port_clean.empty() ? port_clean : "7125";

    spdlog::debug("[{}] Starting auto-probe to {}:{}", get_name(), probe_ip, probe_port);

    // Mark as attempted (prevents re-probe on re-entry)
    auto_probe_attempted_ = true;
    auto_probe_state_.store(AutoProbeState::IN_PROGRESS);

    // Invalidate pending callbacks from any previous attempt, grab fresh token
    lifetime_.invalidate();
    auto tok = lifetime_.token();
    cancel_discovery_watchdog();

    // Clear timer reference (it's already fired)
    auto_probe_timer_ = nullptr;

    // Get MoonrakerClient
    IMoonrakerClient* client = get_moonraker_client();
    if (!client) {
        spdlog::warn("[{}] Auto-probe: MoonrakerClient not available", get_name());
        auto_probe_state_.store(AutoProbeState::FAILED);
        return;
    }

    // Disconnect any previous connection
    client->disconnect();

    // Store probe target for callbacks (thread-safe)
    {
        std::lock_guard<std::mutex> lock(saved_values_mutex_);
        saved_ip_ = probe_ip;
        saved_port_ = probe_port;
    }

    // Show subtle probing indicator
    set_status("icon_question_circle", StatusVariant::None, "Testing connection...");

    // Set testing state (reuses existing subject for button disable)
    lv_subject_set_int(&connection_testing_, 1);

    // Set short timeout for auto-probe (3 seconds - faster than manual test)
    client->set_connection_timeout(3000);

    // Construct WebSocket URL
    std::string ws_url = "ws://" + probe_ip + ":" + probe_port + "/websocket";

    int result = client->connect(
        ws_url.c_str(),
        [this, tok]() {
            tok.defer("WizardConnectionStep::dispatch_auto_probe_success",
                      [this, tok]() { on_auto_probe_success(tok); });
        },
        [this, tok]() {
            tok.defer("WizardConnectionStep::dispatch_auto_probe_failure",
                      [this, tok]() { on_auto_probe_failure(tok); });
        });

    // Disable auto-reconnect for probe
    client->set_auto_reconnect(false);

    if (result != 0) {
        spdlog::debug("[{}] Auto-probe: Failed to initiate connection", get_name());
        auto_probe_state_.store(AutoProbeState::FAILED);
        lv_subject_set_int(&connection_testing_, 0);
        // Silent failure - reset to help text
        set_status(nullptr, StatusVariant::None,
                   lv_tr("Connection must be tested successfully to continue"));
    }
}

void WizardConnectionStep::on_auto_probe_success(const helix::LifetimeToken& tok) {
    // Already on main thread — caller dispatches via tok.defer (see auto_probe).
    // The inner discover_printer cbs DO fire on bg, so we keep those defers.

    // Verify we're still in auto-probe mode (atomic read)
    if (auto_probe_state_.load() != AutoProbeState::IN_PROGRESS) {
        spdlog::debug("[Wizard Connection] Ignoring auto-probe success (state changed)");
        return;
    }

    // Get saved values under lock
    std::string ip, port;
    {
        std::lock_guard<std::mutex> lock(saved_values_mutex_);
        ip = saved_ip_;
        port = saved_port_;
    }

    spdlog::info("[Wizard Connection] Auto-probe successful! Connected to {}:{}", ip, port);

    auto_probe_state_.store(AutoProbeState::SUCCEEDED);

    // Persist config (safe on main thread)
    Config* config = Config::get_instance();
    try {
        config->set(config->df() + helix::wizard::MOONRAKER_HOST, ip);
        config->set(config->df() + helix::wizard::MOONRAKER_PORT, std::stoi(port));
        if (config->save()) {
            spdlog::debug("[Wizard Connection] Auto-probe: Saved configuration");
        }
    } catch (const std::exception& e) {
        spdlog::error("[Wizard Connection] Auto-probe: Failed to save config: {}", e.what());
    }

    // Update subjects with the successful connection target
    lv_subject_copy_string(&connection_ip_, ip.c_str());
    lv_subject_copy_string(&connection_port_, port.c_str());

    // Hide help text on successful auto-probe
    if (screen_root_) {
        lv_obj_t* help_text = lv_obj_find_by_name(screen_root_, "help_text");
        if (help_text) {
            lv_obj_add_flag(help_text, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Show "discovering" status - spinner shows via XML binding
    lv_subject_set_int(&connection_discovering_, 1);
    set_status(nullptr, StatusVariant::None, lv_tr("Connected, discovering..."));

    // Clear testing state
    lv_subject_set_int(&connection_testing_, 0);

    // Set HTTP base URL so discovery can make HTTP calls
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        std::string http_url = "http://" + ip + ":" + port;
        api->set_http_base_url(http_url);
    }

    // Trigger hardware discovery - only enable Next when this completes
    IMoonrakerClient* client = get_moonraker_client();
    if (client) {
        // Same unbounded-spinner hazard as the manual test path (#1161).
        start_discovery_watchdog();

        client->discover_printer(
            // Success callback (fires on background thread)
            [this, tok]() {
                spdlog::info("[Wizard Connection] Auto-probe: Hardware discovery complete");

                tok.defer("WizardConnectionStep::auto_probe_discovery_success", [this]() {
                    cancel_discovery_watchdog();

                    IMoonrakerAPI* api = get_moonraker_api();
                    IMoonrakerClient* client = get_moonraker_client();
                    if (api) {
                        spdlog::info("[Wizard Connection] Hostname: '{}'",
                                     api->hardware().hostname());
                        helix::init_subsystems_from_hardware(api->hardware(), api, client);
                    }

                    // NOW enable Next button - discovery is complete
                    lv_subject_set_int(&connection_discovering_, 0);
                    set_status("icon_check_circle", StatusVariant::Success,
                               lv_tr("Connection successful!"));
                    connection_validated_ = true;
                    lv_subject_set_int(&connection_test_passed, 1);
                });
            },
            // Error callback - discovery failed (e.g., Klippy not connected)
            [this, tok](const std::string& reason) {
                spdlog::warn("[Wizard Connection] Auto-probe: Discovery failed: {}", reason);

                tok.defer("WizardConnectionStep::auto_probe_discovery_error",
                          [this]() { allow_continue_without_klipper(); });
            });
    } else {
        // No client - still show success
        lv_subject_set_int(&connection_discovering_, 0);
        set_status("icon_check_circle", StatusVariant::Success, lv_tr("Connection successful!"));
        connection_validated_ = true;
        lv_subject_set_int(&connection_test_passed, 1);
    }
}

void WizardConnectionStep::on_auto_probe_failure(const helix::LifetimeToken& /*tok*/) {
    // Already on main thread — caller dispatches via tok.defer (see auto_probe).

    // Verify we're still in auto-probe mode (atomic read)
    if (auto_probe_state_.load() != AutoProbeState::IN_PROGRESS) {
        spdlog::debug("[Wizard Connection] Ignoring auto-probe failure (state changed)");
        return;
    }

    spdlog::debug("[Wizard Connection] Auto-probe: No printer at localhost (silent failure)");

    auto_probe_state_.store(AutoProbeState::FAILED);

    set_status(nullptr, StatusVariant::None,
               lv_tr("Connection must be tested successfully to continue"));
    lv_subject_set_int(&connection_testing_, 0);
}

// ============================================================================
// Input Change Handlers
// ============================================================================

void WizardConnectionStep::handle_ip_input_changed() {
    LVGL_SAFE_EVENT_CB_BEGIN("[Wizard Connection] handle_ip_input_changed");

    // If auto-probe is in progress, cancel it
    if (auto_probe_state_.load() == AutoProbeState::IN_PROGRESS) {
        spdlog::debug("[{}] User input during auto-probe, cancelling", get_name());
        auto_probe_state_.store(AutoProbeState::FAILED); // Mark as failed to ignore callbacks
        IMoonrakerClient* client = get_moonraker_client();
        if (client) {
            client->disconnect();
        }
        lv_subject_set_int(&connection_testing_, 0);
    }

    // The user retargeted the connection, so any discovery still in flight is
    // moot — its watchdog must not re-open the gate we clear just below.
    cancel_discovery_watchdog();
    lv_subject_set_int(&connection_discovering_, 0);

    // Reset to help text (user needs to test again after changing input)
    set_status(nullptr, StatusVariant::None,
               lv_tr("Connection must be tested successfully to continue"));

    // Clear validation state
    connection_validated_ = false;
    lv_subject_set_int(&connection_test_passed, 0);

    LVGL_SAFE_EVENT_CB_END();
}

void WizardConnectionStep::handle_port_input_changed() {
    LVGL_SAFE_EVENT_CB_BEGIN("[Wizard Connection] handle_port_input_changed");

    // If auto-probe is in progress, cancel it
    if (auto_probe_state_.load() == AutoProbeState::IN_PROGRESS) {
        spdlog::debug("[{}] User input during auto-probe, cancelling", get_name());
        auto_probe_state_.store(AutoProbeState::FAILED); // Mark as failed to ignore callbacks
        IMoonrakerClient* client = get_moonraker_client();
        if (client) {
            client->disconnect();
        }
        lv_subject_set_int(&connection_testing_, 0);
    }

    // The user retargeted the connection, so any discovery still in flight is
    // moot — its watchdog must not re-open the gate we clear just below.
    cancel_discovery_watchdog();
    lv_subject_set_int(&connection_discovering_, 0);

    // Reset to help text (user needs to test again after changing input)
    set_status(nullptr, StatusVariant::None,
               lv_tr("Connection must be tested successfully to continue"));

    // Clear validation state
    connection_validated_ = false;
    lv_subject_set_int(&connection_test_passed, 0);

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardConnectionStep::register_callbacks() {
    spdlog::debug("[{}] Registering event callbacks", get_name());

    // Static trampolines — actual event binding happens in create() where we have 'this' pointer
    register_xml_callbacks({
        {"on_test_connection_clicked", on_test_connection_clicked_static},
        {"on_ip_input_changed", on_ip_input_changed_static},
        {"on_port_input_changed", on_port_input_changed_static},
        {"on_printer_selected", on_printer_selected_cb},
    });

    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardConnectionStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating connection screen", get_name());

    // No explicit reset needed — lifetime tokens are captured fresh at each
    // connection/auto-probe attempt. The mDNS token is captured below in create().

    if (!parent) {
        LOG_ERROR_INTERNAL("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    // Create from XML
    screen_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_connection", nullptr));

    if (!screen_root_) {
        LOG_ERROR_INTERNAL("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    // Find and configure test button - pass 'this' as user_data
    lv_obj_t* test_btn = lv_obj_find_by_name(screen_root_, "btn_test_connection");
    if (test_btn) {
        lv_obj_add_event_cb(test_btn, on_test_connection_clicked_static, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}] Test button callback attached", get_name());
    } else {
        LOG_ERROR_INTERNAL("[{}] Test button not found in XML", get_name());
    }

    // Find input fields and attach change handlers + keyboard support.
    // Pre-fill is driven by the underlying buffer, NOT by lv_subject_get_string,
    // because the subject's value can be transiently empty during XML creation
    // (bind_text observer-on-add ordering). The buffer is the source of truth:
    // it's seeded from config/defaults on first init and preserved across
    // step revisits by the subjects_initialized_ guard in init_subjects().
    lv_obj_t* ip_input = lv_obj_find_by_name(screen_root_, "ip_input");
    if (ip_input) {
        if (connection_ip_buffer_[0] != '\0') {
            lv_textarea_set_text(ip_input, connection_ip_buffer_);
            // Keep subject in sync so any observers see the same value.
            lv_subject_copy_string(&connection_ip_, connection_ip_buffer_);
            spdlog::debug("[{}] Pre-filled IP input: {}", get_name(), connection_ip_buffer_);
        }
        lv_obj_add_event_cb(ip_input, on_ip_input_changed_static, LV_EVENT_VALUE_CHANGED, this);
        spdlog::debug("[{}] IP input configured", get_name());
    }

    lv_obj_t* port_input = lv_obj_find_by_name(screen_root_, "port_input");
    if (port_input) {
        // Note: NOT using lv_textarea_set_accepted_chars() here because it conflicts
        // with bind_text two-way binding — set_text adds chars one-by-one, each fires
        // VALUE_CHANGED, and the observer cascade truncates the text. Port sanitization
        // is handled by sanitize_port() at all read sites instead.
        if (connection_port_buffer_[0] != '\0') {
            lv_textarea_set_text(port_input, connection_port_buffer_);
            lv_subject_copy_string(&connection_port_, connection_port_buffer_);
            spdlog::debug("[{}] Pre-filled port input: {}", get_name(), connection_port_buffer_);
        }
        lv_obj_add_event_cb(port_input, on_port_input_changed_static, LV_EVENT_VALUE_CHANGED, this);
        spdlog::debug("[{}] Port input configured", get_name());
    }

    lv_obj_update_layout(screen_root_);

    // Set initial dropdown text (bind_options doesn't work for dropdowns)
    lv_obj_t* printer_dropdown = lv_obj_find_by_name(screen_root_, "printer_dropdown");
    if (printer_dropdown) {
        lv_dropdown_set_options(printer_dropdown, lv_tr("Searching..."));
    }

    // Schedule auto-probe if appropriate (empty config, first visit)
    if (should_auto_probe()) {
        spdlog::debug("[{}] Scheduling auto-probe for localhost", get_name());
        auto_probe_timer_ = lv_timer_create(auto_probe_timer_cb, 100, this);
        lv_timer_set_repeat_count(auto_probe_timer_, 1); // One-shot timer
    }

    // Lazy-create mDNS discovery if none was injected
    if (!mdns_discovery_) {
        mdns_discovery_ = std::make_unique<MdnsDiscovery>();
    }
    spdlog::debug("[{}] Starting mDNS discovery", get_name());
    auto mdns_tok = lifetime_.token();
    mdns_discovery_->start_discovery(
        [this, mdns_tok](const std::vector<DiscoveredPrinter>& printers) {
            if (mdns_tok.expired())
                return;
            on_printers_discovered(printers);
        });

    // Set initial help text (bind_text only fires on changes, not initial value)
    set_status(nullptr, StatusVariant::None,
               lv_tr("Connection must be tested successfully to continue"));

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardConnectionStep::cleanup() {
    spdlog::debug("[{}] Cleaning up connection screen", get_name());

    // Expire all outstanding lifetime tokens — any pending async callbacks
    // (connection test, auto-probe, discovery, mDNS) will be silently skipped
    lifetime_.invalidate();

    // Stop mDNS discovery
    if (mdns_discovery_) {
        spdlog::debug("[{}] Stopping mDNS discovery", get_name());
        mdns_discovery_->stop_discovery();
    }

    cancel_auto_probe_timer();

    // Same for the discovery watchdog — it holds `this` and outliving the step
    // would fire into a torn-down screen.
    cancel_discovery_watchdog();

    // If a connection test or auto-probe is in progress, cancel it
    if (lv_subject_get_int(&connection_testing_) == 1 ||
        auto_probe_state_.load() == AutoProbeState::IN_PROGRESS) {
        IMoonrakerClient* client = get_moonraker_client();
        if (client) {
            client->disconnect();
        }
        lv_subject_set_int(&connection_testing_, 0);
    }

    // Reset auto-probe state (but NOT auto_probe_attempted_ - that persists)
    auto_probe_state_.store(AutoProbeState::IDLE);

    // Stop the connection_spinner's rotation animation before lv_obj_clean()
    // tears it down. LVGL's spinner is an arc with a continuous anim; if the
    // anim timer fires mid-lv_obj_clean, the arc event chain
    // (lv_arc_event → lv_event_send → update_obj_state → lv_malloc_zeroed)
    // can touch freed style memory. Seen on back-nav 3→2 in DM626HYE (#843).
    if (screen_root_ && lv_is_initialized()) {
        lv_obj_t* spinner = lv_obj_find_by_name(screen_root_, "connection_spinner");
        if (spinner) {
            lv_anim_delete(spinner, nullptr);
        }
    }

    // Skip set_status() here — widgets are about to be deleted by lv_obj_clean()
    // in ui_wizard_load_screen(). Changing label text during cleanup triggers
    // lv_obj_invalidate → blur tree walk which can hit freed objects (#792).
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// mDNS Discovery Handlers
// ============================================================================

void WizardConnectionStep::on_printers_discovered(const std::vector<DiscoveredPrinter>& printers) {
    // NOTE: This callback comes from the mDNS discovery thread via ui_queue_update()
    // but MdnsDiscovery already handles thread marshaling, so we're on main thread here

    discovered_printers_ = printers;

    // Update status text
    if (printers.empty()) {
        lv_subject_copy_string(&mdns_status_, lv_tr("No printers found"));
    } else if (printers.size() == 1) {
        lv_subject_copy_string(&mdns_status_, lv_tr("Found 1 printer"));
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), lv_tr("Found %zu printers"), printers.size());
        lv_subject_copy_string(&mdns_status_, buf);
    }

    // Update dropdown options (newline-separated for LVGL dropdown)
    std::string options;
    if (printers.empty()) {
        options = lv_tr("No printers found");
    } else {
        for (const auto& p : printers) {
            if (!options.empty()) {
                options += "\n";
            }
            options += p.name + " (" + p.ip_address + ")";
        }
    }

    // Set dropdown options directly (bind_options doesn't work for dropdowns)
    if (screen_root_) {
        lv_obj_t* dropdown = lv_obj_find_by_name(screen_root_, "printer_dropdown");
        if (dropdown) {
            lv_dropdown_set_options(dropdown, options.c_str());
        }
    }

    spdlog::debug("[Wizard Connection] mDNS update: {} printers discovered", printers.size());
}

void WizardConnectionStep::on_printer_selected_cb(lv_event_t* e) {
    auto* self = get_wizard_connection_step();
    if (!self) {
        return;
    }

    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    uint32_t selected = lv_dropdown_get_selected(dropdown);

    if (selected < self->discovered_printers_.size()) {
        const auto& printer = self->discovered_printers_[selected];

        // Update IP input
        lv_subject_copy_string(&self->connection_ip_, printer.ip_address.c_str());

        // Update port input
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", printer.port);
        lv_subject_copy_string(&self->connection_port_, port_str);

        // Also update the text areas directly so user sees the change
        if (self->screen_root_) {
            lv_obj_t* ip_input = lv_obj_find_by_name(self->screen_root_, "ip_input");
            if (ip_input) {
                lv_textarea_set_text(ip_input, printer.ip_address.c_str());
            }
            lv_obj_t* port_input = lv_obj_find_by_name(self->screen_root_, "port_input");
            if (port_input) {
                lv_textarea_set_text(port_input, port_str);
            }
        }

        // Clear any previous validation (user still needs to test)
        self->connection_validated_ = false;
        lv_subject_set_int(&connection_test_passed, 0);

        // Reset to help text (user still needs to test)
        self->set_status(nullptr, StatusVariant::None,
                         lv_tr("Connection must be tested successfully to continue"));

        spdlog::info("[Wizard Connection] Selected printer: {} at {}:{}", printer.name,
                     printer.ip_address, printer.port);
    }
}

// ============================================================================
// Status Helper
// ============================================================================

void WizardConnectionStep::set_status(const char* icon_name, StatusVariant variant,
                                      const char* text) {
    if (!screen_root_) {
        return;
    }

    // Find and update icon
    lv_obj_t* icon_label = lv_obj_find_by_name(screen_root_, "connection_status_icon");
    if (icon_label) {
        // Get icon codepoint
        const char* icon_text = icon_name ? lv_xml_get_const(nullptr, icon_name) : "";
        lv_label_set_text(icon_label, icon_text ? icon_text : "");

        // Set color based on variant
        lv_color_t color;
        switch (variant) {
        case StatusVariant::Success:
            color = theme_manager_get_color("success");
            break;
        case StatusVariant::Warning:
            color = theme_manager_get_color("warning");
            break;
        case StatusVariant::Danger:
            color = theme_manager_get_color("danger");
            break;
        case StatusVariant::None:
        default:
            color = theme_manager_get_color("text_muted");
            break;
        }
        lv_obj_set_style_text_color(icon_label, color, LV_PART_MAIN);
    }

    // Find and update text
    lv_obj_t* text_label = lv_obj_find_by_name(screen_root_, "connection_status_text");
    if (text_label) {
        lv_label_set_text(text_label, text ? text : "");
    }
}

void WizardConnectionStep::unblock_after_incomplete_discovery(const char* message) {
    cancel_discovery_watchdog();

    lv_subject_set_int(&connection_discovering_, 0);
    lv_subject_set_int(&connection_testing_, 0);

    set_status("icon_triangle_exclamation", StatusVariant::Warning, message);

    // Not a full validation: discovery never ran, so hardware lists are empty.
    connection_validated_ = false;
    lv_subject_set_int(&connection_test_passed, 1);
}

void WizardConnectionStep::allow_continue_without_klipper() {
    // Moonraker answered on this address, so the connection itself is good and
    // is already persisted — only Klipper is unusable. Report that plainly and
    // unblock Next: the app's own error surfaces show the actual Klipper fault,
    // which is far more useful than a wizard the user cannot leave.
    unblock_after_incomplete_discovery(
        lv_tr("Connected to Moonraker, but Klipper is not running. You can continue "
              "— HelixScreen will show the Klipper error, and hardware detection "
              "stays limited until Klipper starts."));
}

// ============================================================================
// Discovery Watchdog (#1161)
// ============================================================================

void WizardConnectionStep::start_discovery_watchdog() {
    cancel_discovery_watchdog();

    if (!lv_is_initialized()) {
        return;
    }

    discovery_in_flight_ = true;
    discovery_watchdog_timer_ =
        lv_timer_create(discovery_watchdog_timer_cb, discovery_watchdog_ms_, this);
    lv_timer_set_repeat_count(discovery_watchdog_timer_, 1); // One-shot timer

    spdlog::debug("[{}] Discovery watchdog armed ({} ms)", get_name(), discovery_watchdog_ms_);
}

void WizardConnectionStep::arm_auto_probe_timer_for_test() {
    cancel_auto_probe_timer();
    auto_probe_timer_ = lv_timer_create(auto_probe_timer_cb, 100, this);
    lv_timer_set_repeat_count(auto_probe_timer_, 1);
}

void WizardConnectionStep::cancel_auto_probe_timer() {
    // Neuter rather than delete: cleanup() can run from inside lv_timer_handler
    // (via a click event), where deleting a timer corrupts LVGL's timer list.
    // lv_timer_cancel_safe() also no-ops when LVGL is already gone.
    if (auto_probe_timer_ && lv_is_initialized()) {
        helix::ui::lv_timer_cancel_safe(auto_probe_timer_);
    }
    auto_probe_timer_ = nullptr;
}

void WizardConnectionStep::cancel_discovery_watchdog() {
    discovery_in_flight_ = false;

    // Same reasoning as the auto-probe timer: neuter rather than delete so a
    // cancel from inside lv_timer_handler cannot corrupt the timer list.
    if (discovery_watchdog_timer_ && lv_is_initialized()) {
        helix::ui::lv_timer_cancel_safe(discovery_watchdog_timer_);
    }
    discovery_watchdog_timer_ = nullptr;
}

void WizardConnectionStep::discovery_watchdog_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<WizardConnectionStep*>(lv_timer_get_user_data(timer));
    if (self) {
        // One-shot — LVGL deletes the timer as soon as this returns, so drop our
        // reference first or cancel_discovery_watchdog() neuters freed memory
        // (the UAF auto_probe_timer_cb documents).
        self->discovery_watchdog_timer_ = nullptr;
        self->discovery_watchdog_expired();
    }
}

void WizardConnectionStep::discovery_watchdog_expired() {
    if (!discovery_in_flight_) {
        // Discovery already settled (or the step moved on) and cancelled us.
        return;
    }

    spdlog::warn("[{}] Hardware discovery produced neither result nor error within {} ms — "
                 "unblocking the wizard",
                 get_name(), discovery_watchdog_ms_);

    // Deliberately vaguer than the Klipper-down wording: a silent discovery
    // tells us nothing about *why* it went quiet.
    unblock_after_incomplete_discovery(
        lv_tr("Connected to Moonraker, but printer discovery did not finish. You can "
              "continue — hardware detection stays limited until the printer responds."));
}

// ============================================================================
// Utility Functions
// ============================================================================

bool WizardConnectionStep::get_url(char* buffer, size_t size) const {
    if (!buffer || size == 0) {
        return false;
    }

    // Cast away const for LVGL API (subject is not modified by get_string)
    const char* ip = lv_subject_get_string(const_cast<lv_subject_t*>(&connection_ip_));
    std::string port_clean =
        sanitize_port(lv_subject_get_string(const_cast<lv_subject_t*>(&connection_port_)));

    if (!is_valid_ip_or_hostname(ip) || !is_valid_port(port_clean)) {
        return false;
    }

    snprintf(buffer, size, "ws://%s:%s/websocket", ip, port_clean.c_str());
    return true;
}

bool WizardConnectionStep::is_validated() const {
    return connection_validated_;
}
