// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_connection.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_keyboard.h"
#include "ui_notification.h"
#include "ui_subject_registry.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "lvgl/lvgl.h"
#include "moonraker_client.h"
#include "wizard_validation.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>
#include <string>

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
    }
    return g_wizard_connection_step.get();
}

void destroy_wizard_connection_step() {
    g_wizard_connection_step.reset();
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

    spdlog::debug("[{}] Instance created", get_name());
}

WizardConnectionStep::~WizardConnectionStep() {
    // NOTE: Do NOT call LVGL functions here - LVGL may be destroyed first
    // NOTE: Do NOT log here - spdlog may be destroyed first
    screen_root_ = nullptr;
}

// ============================================================================
// Move Semantics
// ============================================================================

WizardConnectionStep::WizardConnectionStep(WizardConnectionStep&& other) noexcept
    : screen_root_(other.screen_root_),
      connection_ip_(other.connection_ip_),
      connection_port_(other.connection_port_),
      connection_status_icon_(other.connection_status_icon_),
      connection_status_text_(other.connection_status_text_),
      connection_testing_(other.connection_testing_),
      connection_validated_(other.connection_validated_),
      subjects_initialized_(other.subjects_initialized_),
      saved_ip_(std::move(other.saved_ip_)),
      saved_port_(std::move(other.saved_port_)) {
    // Move buffers
    std::memcpy(connection_ip_buffer_, other.connection_ip_buffer_, sizeof(connection_ip_buffer_));
    std::memcpy(connection_port_buffer_, other.connection_port_buffer_, sizeof(connection_port_buffer_));
    std::memcpy(connection_status_icon_buffer_, other.connection_status_icon_buffer_, sizeof(connection_status_icon_buffer_));
    std::memcpy(connection_status_text_buffer_, other.connection_status_text_buffer_, sizeof(connection_status_text_buffer_));

    // Null out other
    other.screen_root_ = nullptr;
    other.subjects_initialized_ = false;
    other.connection_validated_ = false;
}

WizardConnectionStep& WizardConnectionStep::operator=(WizardConnectionStep&& other) noexcept {
    if (this != &other) {
        screen_root_ = other.screen_root_;
        connection_ip_ = other.connection_ip_;
        connection_port_ = other.connection_port_;
        connection_status_icon_ = other.connection_status_icon_;
        connection_status_text_ = other.connection_status_text_;
        connection_testing_ = other.connection_testing_;
        connection_validated_ = other.connection_validated_;
        subjects_initialized_ = other.subjects_initialized_;
        saved_ip_ = std::move(other.saved_ip_);
        saved_port_ = std::move(other.saved_port_);

        // Move buffers
        std::memcpy(connection_ip_buffer_, other.connection_ip_buffer_, sizeof(connection_ip_buffer_));
        std::memcpy(connection_port_buffer_, other.connection_port_buffer_, sizeof(connection_port_buffer_));
        std::memcpy(connection_status_icon_buffer_, other.connection_status_icon_buffer_, sizeof(connection_status_icon_buffer_));
        std::memcpy(connection_status_text_buffer_, other.connection_status_text_buffer_, sizeof(connection_status_text_buffer_));

        // Null out other
        other.screen_root_ = nullptr;
        other.subjects_initialized_ = false;
        other.connection_validated_ = false;
    }
    return *this;
}

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardConnectionStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    // Load existing values from config if available
    Config* config = Config::get_instance();
    std::string default_ip = "";
    std::string default_port = "7125"; // Default Moonraker port

    try {
        std::string default_printer =
            config->get<std::string>("/default_printer", "default_printer");
        std::string printer_path = "/printers/" + default_printer;

        default_ip = config->get<std::string>(printer_path + "/moonraker_host", "");
        int port_num = config->get<int>(printer_path + "/moonraker_port", 7125);
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

    UI_SUBJECT_INIT_AND_REGISTER_STRING(connection_ip_, connection_ip_buffer_, connection_ip_buffer_, "connection_ip");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(connection_port_, connection_port_buffer_, connection_port_buffer_, "connection_port");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(connection_status_icon_, connection_status_icon_buffer_, "", "connection_status_icon");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(connection_status_text_, connection_status_text_buffer_, "", "connection_status_text");
    UI_SUBJECT_INIT_AND_REGISTER_INT(connection_testing_, 0, "connection_testing");

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
    const char* port_str = lv_subject_get_string(&connection_port_);

    spdlog::debug("[{}] Test connection clicked: {}:{}", get_name(), ip, port_str);

    // Clear previous validation state
    connection_validated_ = false;
    lv_subject_set_int(&connection_test_passed, 0);

    // Validate inputs
    if (!ip || strlen(ip) == 0) {
        lv_subject_copy_string(&connection_status_icon_, "");
        lv_subject_copy_string(&connection_status_text_, "Please enter an IP address or hostname");
        spdlog::warn("[{}] Empty IP address", get_name());
        return;
    }

    if (!is_valid_ip_or_hostname(ip)) {
        const char* error_icon = lv_xml_get_const(nullptr, "icon_xmark_circle");
        lv_subject_copy_string(&connection_status_icon_, error_icon ? error_icon : "");
        lv_subject_copy_string(&connection_status_text_, "Invalid IP address or hostname");
        spdlog::warn("[{}] Invalid IP/hostname: {}", get_name(), ip);
        return;
    }

    if (!is_valid_port(port_str)) {
        const char* error_icon = lv_xml_get_const(nullptr, "icon_xmark_circle");
        lv_subject_copy_string(&connection_status_icon_, error_icon ? error_icon : "");
        lv_subject_copy_string(&connection_status_text_, "Invalid port (must be 1-65535)");
        spdlog::warn("[{}] Invalid port: {}", get_name(), port_str);
        return;
    }

    // Get MoonrakerClient instance
    MoonrakerClient* client = get_moonraker_client();
    if (!client) {
        const char* error_icon = lv_xml_get_const(nullptr, "icon_xmark_circle");
        lv_subject_copy_string(&connection_status_icon_, error_icon ? error_icon : "");
        lv_subject_copy_string(&connection_status_text_, "Error: Moonraker client not initialized");
        lv_subject_set_int(&connection_testing_, 0);
        LOG_ERROR_INTERNAL("[{}] MoonrakerClient is nullptr", get_name());
        return;
    }

    // Disconnect any previous connection attempt
    client->disconnect();

    // Store IP/port for async callback
    saved_ip_ = ip;
    saved_port_ = port_str;

    // Set UI to testing state
    lv_subject_set_int(&connection_testing_, 1);
    const char* testing_icon = lv_xml_get_const(nullptr, "icon_question_circle");
    lv_subject_copy_string(&connection_status_icon_, testing_icon ? testing_icon : "");
    lv_subject_copy_string(&connection_status_text_, "Testing connection...");

    spdlog::debug("[{}] Starting connection test to {}:{}", get_name(), ip, port_str);

    // Set shorter timeout for wizard testing
    client->set_connection_timeout(5000);

    // Construct WebSocket URL
    std::string ws_url = "ws://" + std::string(ip) + ":" + std::string(port_str) + "/websocket";

    // Capture 'this' for async callbacks
    // NOTE: This is safe because the WizardConnectionStep instance outlives the connection test
    WizardConnectionStep* self = this;

    int result = client->connect(
        ws_url.c_str(),
        // On connected callback
        [self]() {
            self->on_connection_success();
        },
        // On disconnected callback
        [self]() {
            self->on_connection_failure();
        });

    // Disable automatic reconnection for wizard testing
    client->setReconnect(nullptr);

    if (result != 0) {
        spdlog::error("[{}] Failed to initiate connection: {}", get_name(), result);
        const char* error_icon = lv_xml_get_const(nullptr, "icon_xmark_circle");
        lv_subject_copy_string(&connection_status_icon_, error_icon ? error_icon : "");
        lv_subject_copy_string(&connection_status_text_, "Error starting connection test");
        lv_subject_set_int(&connection_testing_, 0);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void WizardConnectionStep::on_connection_success() {
    spdlog::info("[{}] Connection successful!", get_name());

    const char* check_icon = lv_xml_get_const(nullptr, "icon_check_circle");
    lv_subject_copy_string(&connection_status_icon_, check_icon ? check_icon : "");
    lv_subject_copy_string(&connection_status_text_, "Connection successful!");
    lv_subject_set_int(&connection_testing_, 0);
    connection_validated_ = true;
    lv_subject_set_int(&connection_test_passed, 1);

    // Save configuration
    Config* config = Config::get_instance();
    try {
        std::string default_printer =
            config->get<std::string>("/default_printer", "default_printer");
        std::string printer_path = "/printers/" + default_printer;

        config->set(printer_path + "/moonraker_host", saved_ip_);
        config->set(printer_path + "/moonraker_port", std::stoi(saved_port_));
        if (config->save()) {
            spdlog::debug("[{}] Saved configuration: {}:{}", get_name(), saved_ip_, saved_port_);
        } else {
            spdlog::error("[{}] Failed to save configuration!", get_name());
            NOTIFY_ERROR("Failed to save printer configuration");
        }
    } catch (const std::exception& e) {
        spdlog::error("[{}] Failed to save config: {}", get_name(), e.what());
        NOTIFY_ERROR("Error saving configuration: {}", e.what());
    }

    // Trigger hardware discovery
    MoonrakerClient* client = get_moonraker_client();
    if (client) {
        client->discover_printer([]() {
            spdlog::debug("[Wizard Connection] Hardware discovery complete!");
            MoonrakerClient* client = get_moonraker_client();
            if (client) {
                auto heaters = client->get_heaters();
                auto sensors = client->get_sensors();
                auto fans = client->get_fans();
                spdlog::debug("[Wizard Connection] Discovered {} heaters, {} sensors, {} fans",
                              heaters.size(), sensors.size(), fans.size());
            }
        });
    }
}

void WizardConnectionStep::on_connection_failure() {
    // Check if we're still in testing mode
    int testing_state = lv_subject_get_int(&connection_testing_);
    spdlog::debug("[{}] on_disconnected fired, connection_testing={}", get_name(), testing_state);

    if (testing_state == 1) {
        spdlog::error("[{}] Connection failed", get_name());

        const char* error_icon = lv_xml_get_const(nullptr, "icon_xmark_circle");
        lv_subject_copy_string(&connection_status_icon_, error_icon ? error_icon : "");
        lv_subject_copy_string(&connection_status_text_, "Connection failed. Check IP/port and try again.");
        lv_subject_set_int(&connection_testing_, 0);
        connection_validated_ = false;
        lv_subject_set_int(&connection_test_passed, 0);
    } else {
        spdlog::debug("[{}] Ignoring disconnect (not in testing mode)", get_name());
    }
}

void WizardConnectionStep::handle_ip_input_changed() {
    LVGL_SAFE_EVENT_CB_BEGIN("[Wizard Connection] handle_ip_input_changed");

    // Clear any previous status message
    const char* current_status = lv_subject_get_string(&connection_status_text_);
    if (current_status && strlen(current_status) > 0) {
        lv_subject_copy_string(&connection_status_icon_, "");
        lv_subject_copy_string(&connection_status_text_, "");
    }

    // Clear validation state
    connection_validated_ = false;
    lv_subject_set_int(&connection_test_passed, 0);

    LVGL_SAFE_EVENT_CB_END();
}

void WizardConnectionStep::handle_port_input_changed() {
    LVGL_SAFE_EVENT_CB_BEGIN("[Wizard Connection] handle_port_input_changed");

    // Clear any previous status message
    const char* current_status = lv_subject_get_string(&connection_status_text_);
    if (current_status && strlen(current_status) > 0) {
        lv_subject_copy_string(&connection_status_icon_, "");
        lv_subject_copy_string(&connection_status_text_, "");
    }

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

    // NOTE: We use static trampolines registered via lv_xml_register_event_cb
    // The actual event binding happens in create() where we have 'this' pointer
    lv_xml_register_event_cb(nullptr, "on_test_connection_clicked", on_test_connection_clicked_static);
    lv_xml_register_event_cb(nullptr, "on_ip_input_changed", on_ip_input_changed_static);
    lv_xml_register_event_cb(nullptr, "on_port_input_changed", on_port_input_changed_static);

    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardConnectionStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating connection screen", get_name());

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
        ui_keyboard_register_textarea(ip_input);
        spdlog::debug("[{}] IP input configured with keyboard", get_name());
    }

    lv_obj_t* port_input = lv_obj_find_by_name(screen_root_, "port_input");
    if (port_input) {
        const char* port_text = lv_subject_get_string(&connection_port_);
        if (port_text && strlen(port_text) > 0) {
            lv_textarea_set_text(port_input, port_text);
            spdlog::debug("[{}] Pre-filled port input: {}", get_name(), port_text);
        }
        lv_obj_add_event_cb(port_input, on_port_input_changed_static, LV_EVENT_VALUE_CHANGED, this);
        ui_keyboard_register_textarea(port_input);
        spdlog::debug("[{}] Port input configured with keyboard", get_name());
    }

    lv_obj_update_layout(screen_root_);

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardConnectionStep::cleanup() {
    spdlog::debug("[{}] Cleaning up connection screen", get_name());

    // If a connection test is in progress, cancel it
    if (lv_subject_get_int(&connection_testing_) == 1) {
        MoonrakerClient* client = get_moonraker_client();
        if (client) {
            client->disconnect();
        }
        lv_subject_set_int(&connection_testing_, 0);
    }

    // Clear status
    lv_subject_copy_string(&connection_status_icon_, "");
    lv_subject_copy_string(&connection_status_text_, "");

    // Reset UI references (wizard framework handles deletion)
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
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
    const char* port_str = lv_subject_get_string(const_cast<lv_subject_t*>(&connection_port_));

    if (!is_valid_ip_or_hostname(ip) || !is_valid_port(port_str)) {
        return false;
    }

    snprintf(buffer, size, "ws://%s:%s/websocket", ip, port_str);
    return true;
}

bool WizardConnectionStep::is_validated() const {
    return connection_validated_;
}

// ============================================================================
// Deprecated Legacy API Wrappers
// ============================================================================

void ui_wizard_connection_init_subjects() {
    get_wizard_connection_step()->init_subjects();
}

void ui_wizard_connection_register_callbacks() {
    get_wizard_connection_step()->register_callbacks();
}

lv_obj_t* ui_wizard_connection_create(lv_obj_t* parent) {
    return get_wizard_connection_step()->create(parent);
}

void ui_wizard_connection_cleanup() {
    get_wizard_connection_step()->cleanup();
}

bool ui_wizard_connection_get_url(char* buffer, size_t size) {
    return get_wizard_connection_step()->get_url(buffer, size);
}

bool ui_wizard_connection_is_validated() {
    return get_wizard_connection_step()->is_validated();
}
