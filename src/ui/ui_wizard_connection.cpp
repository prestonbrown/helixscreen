// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_connection.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_keyboard_manager.h"
#include "ui_notification.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
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
    // Signal async callbacks to abort — must be first! (prestonbrown/helixscreen#193)
    m_alive->store(false);

    // NOTE: Do NOT call LVGL functions here - LVGL may be destroyed first
    // NOTE: Do NOT log here - spdlog may be destroyed first
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

    // Load existing values from config if available
    Config* config = Config::get_instance();
    std::string default_ip = "127.0.0.1";
    std::string default_port = "7125"; // Default Moonraker port

    try {
        default_ip = config->get<std::string>(helix::wizard::MOONRAKER_HOST, "");
        int port_num = config->get<int>(helix::wizard::MOONRAKER_PORT, 7125);
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
    UI_SUBJECT_INIT_AND_REGISTER_STRING(mdns_status_, mdns_status_buffer_, "Scanning...",
                                        "mdns_status");

    // Set connection_test_passed to 0 (disabled) for this step
    lv_subject_set_int(&connection_test_passed, 0);

    // Reset validation state
    connection_validated_ = false;
    subjects_initialized_ = true;

    // Check if we have a saved configuration
    if (!default_ip.empty() && !default_port.empty()) {
        spdlog::debug("[{}] Have saved config, but needs validation", get_name());
    }

    spdlog::debug("[{}] Subjects initialized (IP: {}, Port: {})", get_name(),
                  default_ip.empty() ? "<empty>" : default_ip, default_port);
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
        set_status("icon_xmark_circle", StatusVariant::Danger, "Invalid IP address or hostname");
        spdlog::warn("[{}] Invalid IP/hostname: {}", get_name(), ip);
        return;
    }

    if (!is_valid_port(port_clean)) {
        set_status("icon_xmark_circle", StatusVariant::Danger, "Invalid port (must be 1-65535)");
        spdlog::warn("[{}] Invalid port: {}", get_name(), port_clean);
        return;
    }

    // Get MoonrakerClient instance
    MoonrakerClient* client = get_moonraker_client();
    if (!client) {
        set_status("icon_xmark_circle", StatusVariant::Danger,
                   "Error: Moonraker client not initialized");
        lv_subject_set_int(&connection_testing_, 0);
        LOG_ERROR_INTERNAL("[{}] MoonrakerClient is nullptr", get_name());
        return;
    }

    // Disconnect any previous connection attempt
    client->disconnect();

    // Increment generation to invalidate any pending callbacks from previous attempts
    uint64_t this_generation = ++connection_generation_;

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

    // Capture generation counter to detect stale callbacks
    // If cleanup_called_ or generation changes, callback will be ignored
    WizardConnectionStep* self = this;

    int result = client->connect(
        ws_url.c_str(),
        // On connected callback - check generation before proceeding
        [self, this_generation]() {
            if (self->is_stale() || !self->is_current_generation(this_generation)) {
                spdlog::debug("[Wizard Connection] Ignoring stale success callback");
                return;
            }
            self->on_connection_success();
        },
        // On disconnected callback - check generation before proceeding
        [self, this_generation]() {
            if (self->is_stale() || !self->is_current_generation(this_generation)) {
                spdlog::debug("[Wizard Connection] Ignoring stale failure callback");
                return;
            }
            self->on_connection_failure();
        });

    // Disable automatic reconnection for wizard testing
    client->setReconnect(nullptr);

    if (result != 0) {
        spdlog::error("[{}] Failed to initiate connection: {}", get_name(), result);
        set_status("icon_xmark_circle", StatusVariant::Danger, "Error starting connection test");
        lv_subject_set_int(&connection_testing_, 0);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void WizardConnectionStep::on_connection_success() {
    // NOTE: This is called from WebSocket thread - only do thread-safe operations here
    spdlog::info("[Wizard Connection] Connection successful!");

    // Defer ALL operations (including config) to main thread
    helix::ui::async_call(
        [](void* ctx) {
            auto* self = static_cast<WizardConnectionStep*>(ctx);

            if (self->is_stale()) {
                spdlog::debug("[Wizard Connection] Cleanup called, skipping connection success UI");
                return;
            }

            // Get saved values under lock
            std::string ip, port;
            {
                std::lock_guard<std::mutex> lock(self->saved_values_mutex_);
                ip = self->saved_ip_;
                port = self->saved_port_;
            }

            // NOW safe to access config (on main thread)
            Config* config = Config::get_instance();
            try {
                config->set(helix::wizard::MOONRAKER_HOST, ip);
                config->set(helix::wizard::MOONRAKER_PORT, std::stoi(port));
                if (config->save()) {
                    spdlog::debug("[Wizard Connection] Saved configuration: {}:{}", ip, port);
                } else {
                    spdlog::error("[Wizard Connection] Failed to save configuration!");
                }
            } catch (const std::exception& e) {
                spdlog::error("[Wizard Connection] Failed to save config: {}", e.what());
            }

            // Show "discovering" status - spinner shows via XML binding
            lv_subject_set_int(&self->connection_discovering_, 1);
            self->set_status(nullptr, StatusVariant::None,
                             lv_tr("Connected! Discovering printer..."));
            lv_subject_set_int(&self->connection_testing_, 0);

            // Set HTTP base URL so discovery can make HTTP calls
            MoonrakerAPI* api = get_moonraker_api();
            if (api) {
                std::string http_url = "http://" + ip + ":" + port;
                api->set_http_base_url(http_url);
            }

            // Trigger hardware discovery - only enable Next when this completes
            MoonrakerClient* client = get_moonraker_client();
            if (client) {
                // Capture generation for discovery callback
                uint64_t discover_gen = self->connection_generation_.load();

                client->discover_printer(
                    // Success callback
                    [self, discover_gen]() {
                        // Check if still valid before queueing UI update
                        if (self->is_stale() || !self->is_current_generation(discover_gen)) {
                            spdlog::debug("[Wizard Connection] Ignoring stale discovery callback");
                            return;
                        }

                        spdlog::info("[Wizard Connection] Hardware discovery complete!");

                        // Defer discovery UI update to main thread
                        helix::ui::async_call(
                            [](void* ctx2) {
                                auto* self2 = static_cast<WizardConnectionStep*>(ctx2);

                                if (self2->is_stale()) {
                                    spdlog::debug("[Wizard Connection] Cleanup called, skipping "
                                                  "discovery UI update");
                                    return;
                                }

                                MoonrakerClient* client = get_moonraker_client();
                                MoonrakerAPI* api = get_moonraker_api();
                                if (api) {
                                    const auto& heaters = api->hardware().heaters();
                                    const auto& sensors = api->hardware().sensors();
                                    const auto& fans = api->hardware().fans();
                                    spdlog::info("[Wizard Connection] Discovered {} heaters, {} "
                                                 "sensors, {} fans",
                                                 heaters.size(), sensors.size(), fans.size());
                                    spdlog::info("[Wizard Connection] Hostname: '{}'",
                                                 api->hardware().hostname());

                                    // Initialize subsystems (AMS, filament sensors, macros)
                                    // so they're available for later wizard steps
                                    helix::init_subsystems_from_hardware(api->hardware(), api,
                                                                         client);
                                }

                                // NOW enable Next button - discovery is complete
                                lv_subject_set_int(&self2->connection_discovering_, 0);
                                self2->set_status("icon_check_circle", StatusVariant::Success,
                                                  lv_tr("Connection successful!"));
                                self2->connection_validated_ = true;
                                lv_subject_set_int(&connection_test_passed, 1);
                            },
                            self);
                    },
                    // Error callback - discovery failed (e.g., Klippy not connected)
                    [self, discover_gen](const std::string& reason) {
                        // Check if still valid before queueing UI update
                        if (self->is_stale() || !self->is_current_generation(discover_gen)) {
                            spdlog::debug(
                                "[Wizard Connection] Ignoring stale discovery error callback");
                            return;
                        }

                        spdlog::warn("[Wizard Connection] Discovery failed: {}", reason);

                        // Defer error UI update to main thread
                        helix::ui::async_call(
                            [](void* ctx2) {
                                auto* self2 = static_cast<WizardConnectionStep*>(ctx2);

                                if (self2->is_stale()) {
                                    spdlog::debug("[Wizard Connection] Cleanup called, skipping "
                                                  "discovery error UI update");
                                    return;
                                }

                                // Reset discovering state and show error
                                lv_subject_set_int(&self2->connection_discovering_, 0);
                                self2->set_status(
                                    "icon_triangle_exclamation", StatusVariant::Warning,
                                    "Moonraker connected, but Klipper is not running. "
                                    "Start Klipper and retry.");

                                // Keep test button enabled for retry
                                lv_subject_set_int(&self2->connection_testing_, 0);
                                self2->connection_validated_ = false;
                                lv_subject_set_int(&connection_test_passed, 0);
                            },
                            self);
                    });
            } else {
                // No client available - still show success but warn
                lv_subject_set_int(&self->connection_discovering_, 0);
                self->set_status("icon_check_circle", StatusVariant::Success,
                                 lv_tr("Connected (no discovery)"));
                self->connection_validated_ = true;
                lv_subject_set_int(&connection_test_passed, 1);
            }
        },
        this);
}

void WizardConnectionStep::on_connection_failure() {
    // NOTE: This is called from WebSocket thread - only do thread-safe operations here
    spdlog::debug("[Wizard Connection] on_disconnected fired");

    // Defer LVGL operations to main thread
    helix::ui::async_call(
        [](void* ctx) {
            auto* self = static_cast<WizardConnectionStep*>(ctx);

            if (self->is_stale()) {
                spdlog::debug("[Wizard Connection] Cleanup called, skipping connection failure UI");
                return;
            }

            // Check if we're still in testing mode (must check on main thread)
            int testing_state = lv_subject_get_int(&self->connection_testing_);
            spdlog::debug("[Wizard Connection] Connection failure, testing_state={}",
                          testing_state);

            if (testing_state == 1) {
                spdlog::error("[Wizard Connection] Connection failed");

                self->set_status("icon_xmark_circle", StatusVariant::Danger,
                                 "Connection failed. Check IP/port and try again.");
                lv_subject_set_int(&self->connection_testing_, 0);
                self->connection_validated_ = false;
                lv_subject_set_int(&connection_test_passed, 0);
            } else {
                spdlog::debug("[Wizard Connection] Ignoring disconnect (not in testing mode)");
            }
        },
        this);
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
        self->attempt_auto_probe();
    }
}

void WizardConnectionStep::attempt_auto_probe() {
    // Get the IP/port from subjects - may be from config or default
    const char* ip = lv_subject_get_string(&connection_ip_);
    std::string port_clean = sanitize_port(lv_subject_get_string(&connection_port_));

    // If IP is empty, use localhost as default probe target
    std::string probe_ip = (ip && strlen(ip) > 0) ? ip : "127.0.0.1";
    std::string probe_port = !port_clean.empty() ? port_clean : "7125";

    spdlog::debug("[{}] Starting auto-probe to {}:{}", get_name(), probe_ip, probe_port);

    // Mark as attempted (prevents re-probe on re-entry)
    auto_probe_attempted_ = true;
    auto_probe_state_.store(AutoProbeState::IN_PROGRESS);

    // Increment generation to invalidate any stale callbacks
    uint64_t this_generation = ++connection_generation_;

    // Clear timer reference (it's already fired)
    auto_probe_timer_ = nullptr;

    // Get MoonrakerClient
    MoonrakerClient* client = get_moonraker_client();
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

    WizardConnectionStep* self = this;
    int result = client->connect(
        ws_url.c_str(),
        // Check generation before invoking callback
        [self, this_generation]() {
            if (self->is_stale() || !self->is_current_generation(this_generation)) {
                spdlog::debug("[Wizard Connection] Ignoring stale auto-probe success");
                return;
            }
            self->on_auto_probe_success();
        },
        [self, this_generation]() {
            if (self->is_stale() || !self->is_current_generation(this_generation)) {
                spdlog::debug("[Wizard Connection] Ignoring stale auto-probe failure");
                return;
            }
            self->on_auto_probe_failure();
        });

    // Disable auto-reconnect for probe
    client->setReconnect(nullptr);

    if (result != 0) {
        spdlog::debug("[{}] Auto-probe: Failed to initiate connection", get_name());
        auto_probe_state_.store(AutoProbeState::FAILED);
        lv_subject_set_int(&connection_testing_, 0);
        // Silent failure - reset to help text
        set_status(nullptr, StatusVariant::None,
                   lv_tr("Connection must be tested successfully to continue"));
    }
}

void WizardConnectionStep::on_auto_probe_success() {
    // NOTE: This is called from WebSocket thread - only do thread-safe operations here

    // Verify we're still in auto-probe mode (atomic read)
    if (auto_probe_state_.load() != AutoProbeState::IN_PROGRESS) {
        spdlog::debug("[Wizard Connection] Ignoring auto-probe success (state changed)");
        return;
    }

    // Get saved values under lock for logging
    std::string ip_copy, port_copy;
    {
        std::lock_guard<std::mutex> lock(saved_values_mutex_);
        ip_copy = saved_ip_;
        port_copy = saved_port_;
    }

    spdlog::info("[Wizard Connection] Auto-probe successful! Connected to {}:{}", ip_copy,
                 port_copy);

    auto_probe_state_.store(AutoProbeState::SUCCEEDED);

    // Defer ALL operations (including config) to main thread
    helix::ui::async_call(
        [](void* ctx) {
            auto* self = static_cast<WizardConnectionStep*>(ctx);

            if (self->is_stale()) {
                spdlog::debug("[Wizard Connection] Cleanup called, skipping auto-probe UI update");
                return;
            }

            // Get saved values under lock
            std::string ip, port;
            {
                std::lock_guard<std::mutex> lock(self->saved_values_mutex_);
                ip = self->saved_ip_;
                port = self->saved_port_;
            }

            // NOW safe to access config (on main thread)
            Config* config = Config::get_instance();
            try {
                config->set(helix::wizard::MOONRAKER_HOST, ip);
                config->set(helix::wizard::MOONRAKER_PORT, std::stoi(port));
                if (config->save()) {
                    spdlog::debug("[Wizard Connection] Auto-probe: Saved configuration");
                }
            } catch (const std::exception& e) {
                spdlog::error("[Wizard Connection] Auto-probe: Failed to save config: {}",
                              e.what());
            }

            // Update subjects with the successful connection target
            lv_subject_copy_string(&self->connection_ip_, ip.c_str());
            lv_subject_copy_string(&self->connection_port_, port.c_str());

            // Hide help text on successful auto-probe
            if (self->screen_root_) {
                lv_obj_t* help_text = lv_obj_find_by_name(self->screen_root_, "help_text");
                if (help_text) {
                    lv_obj_add_flag(help_text, LV_OBJ_FLAG_HIDDEN);
                }
            }

            // Show "discovering" status - spinner shows via XML binding
            lv_subject_set_int(&self->connection_discovering_, 1);
            self->set_status(nullptr, StatusVariant::None, lv_tr("Connected, discovering..."));

            // Clear testing state
            lv_subject_set_int(&self->connection_testing_, 0);

            // Set HTTP base URL so discovery can make HTTP calls
            MoonrakerAPI* api = get_moonraker_api();
            if (api) {
                std::string http_url = "http://" + ip + ":" + port;
                api->set_http_base_url(http_url);
            }

            // Trigger hardware discovery - only enable Next when this completes
            MoonrakerClient* client = get_moonraker_client();
            if (client) {
                // Capture generation for discovery callback
                uint64_t discover_gen = self->connection_generation_.load();

                client->discover_printer(
                    // Success callback
                    [self, discover_gen]() {
                        // Check if still valid before queueing UI update
                        if (self->is_stale() || !self->is_current_generation(discover_gen)) {
                            spdlog::debug("[Wizard Connection] Ignoring stale discovery callback");
                            return;
                        }

                        spdlog::info("[Wizard Connection] Auto-probe: Hardware discovery complete");

                        // Defer discovery completion UI update to main thread
                        helix::ui::async_call(
                            [](void* ctx2) {
                                auto* self2 = static_cast<WizardConnectionStep*>(ctx2);

                                if (self2->is_stale()) {
                                    spdlog::debug("[Wizard Connection] Cleanup called, skipping "
                                                  "discovery UI update");
                                    return;
                                }

                                MoonrakerClient* client = get_moonraker_client();
                                MoonrakerAPI* api = get_moonraker_api();
                                if (api) {
                                    spdlog::info("[Wizard Connection] Hostname: '{}'",
                                                 api->hardware().hostname());

                                    // Initialize subsystems (AMS, filament sensors, macros)
                                    helix::init_subsystems_from_hardware(api->hardware(), api,
                                                                         client);
                                }

                                // NOW enable Next button - discovery is complete
                                lv_subject_set_int(&self2->connection_discovering_, 0);
                                self2->set_status("icon_check_circle", StatusVariant::Success,
                                                  lv_tr("Connection successful!"));
                                self2->connection_validated_ = true;
                                lv_subject_set_int(&connection_test_passed, 1);
                            },
                            self);
                    },
                    // Error callback - discovery failed (e.g., Klippy not connected)
                    [self, discover_gen](const std::string& reason) {
                        // Check if still valid before queueing UI update
                        if (self->is_stale() || !self->is_current_generation(discover_gen)) {
                            spdlog::debug(
                                "[Wizard Connection] Ignoring stale discovery error callback");
                            return;
                        }

                        spdlog::warn("[Wizard Connection] Auto-probe: Discovery failed: {}",
                                     reason);

                        // Defer error UI update to main thread
                        helix::ui::async_call(
                            [](void* ctx2) {
                                auto* self2 = static_cast<WizardConnectionStep*>(ctx2);

                                if (self2->is_stale()) {
                                    spdlog::debug("[Wizard Connection] Cleanup called, skipping "
                                                  "discovery error UI update");
                                    return;
                                }

                                // Reset discovering state and show error
                                lv_subject_set_int(&self2->connection_discovering_, 0);
                                self2->set_status(
                                    "icon_triangle_exclamation", StatusVariant::Warning,
                                    "Moonraker connected, but Klipper is not running. "
                                    "Start Klipper and retry.");

                                // Keep test button enabled for retry
                                lv_subject_set_int(&self2->connection_testing_, 0);
                                self2->connection_validated_ = false;
                                lv_subject_set_int(&connection_test_passed, 0);
                            },
                            self);
                    });
            } else {
                // No client - still show success
                lv_subject_set_int(&self->connection_discovering_, 0);
                self->set_status("icon_check_circle", StatusVariant::Success,
                                 lv_tr("Connection successful!"));
                self->connection_validated_ = true;
                lv_subject_set_int(&connection_test_passed, 1);
            }
        },
        this);
}

void WizardConnectionStep::on_auto_probe_failure() {
    // NOTE: This is called from WebSocket thread - only do thread-safe operations here

    // Verify we're still in auto-probe mode (atomic read)
    if (auto_probe_state_.load() != AutoProbeState::IN_PROGRESS) {
        spdlog::debug("[Wizard Connection] Ignoring auto-probe failure (state changed)");
        return;
    }

    spdlog::debug("[Wizard Connection] Auto-probe: No printer at localhost (silent failure)");

    auto_probe_state_.store(AutoProbeState::FAILED);

    // Defer LVGL operations to main thread
    helix::ui::async_call(
        [](void* ctx) {
            auto* self = static_cast<WizardConnectionStep*>(ctx);

            if (self->is_stale()) {
                spdlog::debug("[Wizard Connection] Cleanup called, skipping auto-probe failure UI");
                return;
            }

            // Silent failure - reset to help text
            self->set_status(nullptr, StatusVariant::None,
                             lv_tr("Connection must be tested successfully to continue"));
            lv_subject_set_int(&self->connection_testing_, 0);

            // Leave fields empty - user will enter manually
        },
        this);
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
        MoonrakerClient* client = get_moonraker_client();
        if (client) {
            client->disconnect();
        }
        lv_subject_set_int(&connection_testing_, 0);
    }

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
        MoonrakerClient* client = get_moonraker_client();
        if (client) {
            client->disconnect();
        }
        lv_subject_set_int(&connection_testing_, 0);
    }

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

    // Reset cleanup guard for fresh screen (atomic store with release semantics)
    cleanup_called_.store(false, std::memory_order_release);

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

    // Find input fields and attach change handlers + keyboard support
    lv_obj_t* ip_input = lv_obj_find_by_name(screen_root_, "ip_input");
    if (ip_input) {
        const char* ip_text = lv_subject_get_string(&connection_ip_);
        if (ip_text && strlen(ip_text) > 0) {
            lv_textarea_set_text(ip_input, ip_text);
            spdlog::debug("[{}] Pre-filled IP input: {}", get_name(), ip_text);
        }
        lv_obj_add_event_cb(ip_input, on_ip_input_changed_static, LV_EVENT_VALUE_CHANGED, this);
        KeyboardManager::instance().register_textarea(ip_input);
        spdlog::debug("[{}] IP input configured with keyboard", get_name());
    }

    lv_obj_t* port_input = lv_obj_find_by_name(screen_root_, "port_input");
    if (port_input) {
        // Note: NOT using lv_textarea_set_accepted_chars() here because it conflicts
        // with bind_text two-way binding — set_text adds chars one-by-one, each fires
        // VALUE_CHANGED, and the observer cascade truncates the text. Port sanitization
        // is handled by sanitize_port() at all read sites instead.
        const char* port_text = lv_subject_get_string(&connection_port_);
        if (port_text && strlen(port_text) > 0) {
            lv_textarea_set_text(port_input, port_text);
            spdlog::debug("[{}] Pre-filled port input: {}", get_name(), port_text);
        }
        lv_obj_add_event_cb(port_input, on_port_input_changed_static, LV_EVENT_VALUE_CHANGED, this);
        KeyboardManager::instance().register_textarea(port_input);
        spdlog::debug("[{}] Port input configured with keyboard", get_name());
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
    auto alive = m_alive;
    mdns_discovery_->start_discovery([this, alive](const std::vector<DiscoveredPrinter>& printers) {
        if (!alive->load()) {
            return;
        }
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

    // Mark cleanup as called to guard async callbacks (atomic store with release semantics)
    cleanup_called_.store(true, std::memory_order_release);

    // Stop mDNS discovery
    if (mdns_discovery_) {
        spdlog::debug("[{}] Stopping mDNS discovery", get_name());
        mdns_discovery_->stop_discovery();
    }

    // Cancel any pending auto-probe timer
    // Guard against LVGL shutdown - timer may already be destroyed
    if (auto_probe_timer_ && lv_is_initialized()) {
        lv_timer_delete(auto_probe_timer_);
        auto_probe_timer_ = nullptr;
    }

    // If a connection test or auto-probe is in progress, cancel it
    if (lv_subject_get_int(&connection_testing_) == 1 ||
        auto_probe_state_.load() == AutoProbeState::IN_PROGRESS) {
        MoonrakerClient* client = get_moonraker_client();
        if (client) {
            client->disconnect();
        }
        lv_subject_set_int(&connection_testing_, 0);
    }

    // Reset auto-probe state (but NOT auto_probe_attempted_ - that persists)
    auto_probe_state_.store(AutoProbeState::IDLE);

    // Clear status (must be before screen_root_ is cleared)
    set_status(nullptr, StatusVariant::None, "");

    // Reset UI references (wizard framework handles deletion)
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// mDNS Discovery Handlers
// ============================================================================

void WizardConnectionStep::on_printers_discovered(const std::vector<DiscoveredPrinter>& printers) {
    // NOTE: This callback comes from the mDNS discovery thread via ui_async_call
    // but MdnsDiscovery already handles thread marshaling, so we're on main thread here

    if (is_stale()) {
        spdlog::debug("[Wizard Connection] Ignoring mDNS update (cleanup called)");
        return;
    }

    discovered_printers_ = printers;

    // Update status text
    if (printers.empty()) {
        lv_subject_copy_string(&mdns_status_, lv_tr("No printers found"));
    } else if (printers.size() == 1) {
        lv_subject_copy_string(&mdns_status_, lv_tr("Found 1 printer"));
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "Found %zu printers", printers.size());
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
    if (!self || self->is_stale()) {
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
