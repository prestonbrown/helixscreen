// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_wifi.h"

#include "ui_error_reporting.h"
#include "ui_icon.h"
#include "ui_keyboard.h"
#include "ui_modal.h"
#include "ui_subject_registry.h"
#include "ui_theme.h"

#include "ethernet_manager.h"
#include "lvgl/lvgl.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardWifiStep> g_wizard_wifi_step;

WizardWifiStep* get_wizard_wifi_step() {
    if (!g_wizard_wifi_step) {
        g_wizard_wifi_step = std::make_unique<WizardWifiStep>();
    }
    return g_wizard_wifi_step.get();
}

void destroy_wizard_wifi_step() {
    g_wizard_wifi_step.reset();
}

// ============================================================================
// Helper Types (per-instance network item data)
// ============================================================================

/**
 * @brief Per-instance network item data for reactive UI updates
 */
struct NetworkItemData {
    WiFiNetwork network;
    lv_subject_t* ssid;
    lv_subject_t* signal_strength;
    lv_subject_t* is_secured;
    char ssid_buffer[64];
    WizardWifiStep* parent;  // Back-reference for callbacks

    NetworkItemData(const WiFiNetwork& net, WizardWifiStep* p) : network(net), parent(p) {
        ssid = new lv_subject_t();
        signal_strength = new lv_subject_t();
        is_secured = new lv_subject_t();

        strncpy(ssid_buffer, network.ssid.c_str(), sizeof(ssid_buffer) - 1);
        ssid_buffer[sizeof(ssid_buffer) - 1] = '\0';
        lv_subject_init_string(ssid, ssid_buffer, nullptr, sizeof(ssid_buffer), ssid_buffer);
        lv_subject_init_int(signal_strength, network.signal_strength);
        lv_subject_init_int(is_secured, network.is_secured ? 1 : 0);
    }

    ~NetworkItemData() {
        delete ssid;
        delete signal_strength;
        delete is_secured;
    }
};

// ============================================================================
// Helper: Constant Registration
// ============================================================================

struct WifiConstant {
    const char* name;
    const char* value;
};

static void register_wifi_constants_to_scope(lv_xml_component_scope_t* scope,
                                             const WifiConstant* constants) {
    if (!scope) return;
    for (int i = 0; constants[i].name != NULL; i++) {
        lv_xml_register_const(scope, constants[i].name, constants[i].value);
    }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardWifiStep::WizardWifiStep() {
    std::memset(wifi_status_buffer_, 0, sizeof(wifi_status_buffer_));
    std::memset(ethernet_status_buffer_, 0, sizeof(ethernet_status_buffer_));
    std::memset(wifi_password_modal_ssid_buffer_, 0, sizeof(wifi_password_modal_ssid_buffer_));
    std::memset(current_ssid_, 0, sizeof(current_ssid_));

    spdlog::debug("[{}] Instance created", get_name());
}

WizardWifiStep::~WizardWifiStep() {
    // NOTE: Do NOT call LVGL functions here - LVGL may be destroyed first
    // NOTE: Do NOT log here - spdlog may be destroyed first
    screen_root_ = nullptr;
    password_modal_ = nullptr;
    network_list_container_ = nullptr;
}

// ============================================================================
// Move Semantics
// ============================================================================

WizardWifiStep::WizardWifiStep(WizardWifiStep&& other) noexcept
    : screen_root_(other.screen_root_),
      password_modal_(other.password_modal_),
      network_list_container_(other.network_list_container_),
      wifi_enabled_(other.wifi_enabled_),
      wifi_status_(other.wifi_status_),
      ethernet_status_(other.ethernet_status_),
      wifi_scanning_(other.wifi_scanning_),
      wifi_password_modal_visible_(other.wifi_password_modal_visible_),
      wifi_password_modal_ssid_(other.wifi_password_modal_ssid_),
      wifi_connecting_(other.wifi_connecting_),
      wifi_manager_(std::move(other.wifi_manager_)),
      ethernet_manager_(std::move(other.ethernet_manager_)),
      current_secured_(other.current_secured_),
      wifi_item_bg_color_(other.wifi_item_bg_color_),
      wifi_item_text_color_(other.wifi_item_text_color_),
      subjects_initialized_(other.subjects_initialized_) {
    std::memcpy(wifi_status_buffer_, other.wifi_status_buffer_, sizeof(wifi_status_buffer_));
    std::memcpy(ethernet_status_buffer_, other.ethernet_status_buffer_, sizeof(ethernet_status_buffer_));
    std::memcpy(wifi_password_modal_ssid_buffer_, other.wifi_password_modal_ssid_buffer_, sizeof(wifi_password_modal_ssid_buffer_));
    std::memcpy(current_ssid_, other.current_ssid_, sizeof(current_ssid_));

    other.screen_root_ = nullptr;
    other.password_modal_ = nullptr;
    other.network_list_container_ = nullptr;
    other.subjects_initialized_ = false;
}

WizardWifiStep& WizardWifiStep::operator=(WizardWifiStep&& other) noexcept {
    if (this != &other) {
        screen_root_ = other.screen_root_;
        password_modal_ = other.password_modal_;
        network_list_container_ = other.network_list_container_;
        wifi_enabled_ = other.wifi_enabled_;
        wifi_status_ = other.wifi_status_;
        ethernet_status_ = other.ethernet_status_;
        wifi_scanning_ = other.wifi_scanning_;
        wifi_password_modal_visible_ = other.wifi_password_modal_visible_;
        wifi_password_modal_ssid_ = other.wifi_password_modal_ssid_;
        wifi_connecting_ = other.wifi_connecting_;
        wifi_manager_ = std::move(other.wifi_manager_);
        ethernet_manager_ = std::move(other.ethernet_manager_);
        current_secured_ = other.current_secured_;
        wifi_item_bg_color_ = other.wifi_item_bg_color_;
        wifi_item_text_color_ = other.wifi_item_text_color_;
        subjects_initialized_ = other.subjects_initialized_;

        std::memcpy(wifi_status_buffer_, other.wifi_status_buffer_, sizeof(wifi_status_buffer_));
        std::memcpy(ethernet_status_buffer_, other.ethernet_status_buffer_, sizeof(ethernet_status_buffer_));
        std::memcpy(wifi_password_modal_ssid_buffer_, other.wifi_password_modal_ssid_buffer_, sizeof(wifi_password_modal_ssid_buffer_));
        std::memcpy(current_ssid_, other.current_ssid_, sizeof(current_ssid_));

        other.screen_root_ = nullptr;
        other.password_modal_ = nullptr;
        other.network_list_container_ = nullptr;
        other.subjects_initialized_ = false;
    }
    return *this;
}

// ============================================================================
// Static Helper Functions
// ============================================================================

const char* WizardWifiStep::get_status_text(const char* status_name) {
    static char enum_key[64];
    snprintf(enum_key, sizeof(enum_key), "wifi_status.%s", status_name);

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("wizard_wifi_setup");
    const char* text = lv_xml_get_const(scope, enum_key);

    if (!text) {
        LOG_WARN_INTERNAL("Enum constant '{}' not found, using fallback", enum_key);
        return status_name;
    }

    spdlog::debug("[WiFi Screen] Enum '{}' = '{}'", enum_key, text);
    return text;
}

const char* WizardWifiStep::get_wifi_signal_icon(int signal_strength, bool is_secured) {
    if (signal_strength <= 25) {
        return is_secured ? "mat_wifi_strength_1_lock" : "mat_wifi_strength_1";
    } else if (signal_strength <= 50) {
        return is_secured ? "mat_wifi_strength_2_lock" : "mat_wifi_strength_2";
    } else if (signal_strength <= 75) {
        return is_secured ? "mat_wifi_strength_3_lock" : "mat_wifi_strength_3";
    } else {
        return is_secured ? "mat_wifi_strength_4_lock" : "mat_wifi_strength_4";
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

void WizardWifiStep::init_wifi_item_colors() {
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("wifi_network_item");
    if (scope) {
        bool use_dark_mode = ui_theme_is_dark_mode();

        const char* bg_str =
            lv_xml_get_const(scope, use_dark_mode ? "wifi_item_bg_dark" : "wifi_item_bg_light");
        wifi_item_bg_color_ = bg_str ? ui_theme_parse_color(bg_str) : lv_color_hex(0x262626);

        const char* text_str =
            lv_xml_get_const(scope, use_dark_mode ? "wifi_item_text_dark" : "wifi_item_text_light");
        wifi_item_text_color_ = text_str ? ui_theme_parse_color(text_str) : lv_color_hex(0xE3E3E3);

        spdlog::debug("[{}] Item colors loaded: bg={}, text={} ({})", get_name(),
                      bg_str ? bg_str : "default", text_str ? text_str : "default",
                      use_dark_mode ? "dark" : "light");
    } else {
        wifi_item_bg_color_ = lv_color_hex(0x262626);
        wifi_item_text_color_ = lv_color_hex(0xE3E3E3);
        LOG_WARN_INTERNAL("wifi_network_item component not registered - using defaults");
    }
}

void WizardWifiStep::apply_connected_network_highlight(lv_obj_t* item) {
    if (!item) return;

    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 4, LV_PART_MAIN);
    lv_color_t accent = ui_theme_get_color("primary_color");
    lv_obj_set_style_border_color(item, accent, LV_PART_MAIN);

    lv_obj_set_style_bg_color(item, wifi_item_bg_color_, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* ssid_label = lv_obj_find_by_name(item, "ssid_label");
    if (ssid_label) {
        lv_obj_set_style_text_color(ssid_label, wifi_item_text_color_, LV_PART_MAIN);
    }

    spdlog::trace("[{}] Applied connected network highlight", get_name());
}

void WizardWifiStep::update_wifi_status(const char* status) {
    if (!status) return;
    spdlog::debug("[{}] Updating WiFi status: {}", get_name(), status);
    lv_subject_copy_string(&wifi_status_, status);
}

void WizardWifiStep::update_ethernet_status() {
    if (!ethernet_manager_) {
        LOG_WARN_INTERNAL("Ethernet manager not initialized");
        lv_subject_copy_string(&ethernet_status_, "Unknown");
        return;
    }

    EthernetInfo info = ethernet_manager_->get_info();

    if (info.connected) {
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf), "Connected (%s)", info.ip_address.c_str());
        lv_subject_copy_string(&ethernet_status_, status_buf);
        spdlog::debug("[{}] Ethernet status: {}", get_name(), status_buf);
    } else {
        lv_subject_copy_string(&ethernet_status_, info.status.c_str());
        spdlog::debug("[{}] Ethernet status: {}", get_name(), info.status);
    }
}

void WizardWifiStep::populate_network_list(const std::vector<WiFiNetwork>& networks) {
    spdlog::debug("[{}] Populating network list with {} networks", get_name(), networks.size());

    if (!network_list_container_) {
        LOG_ERROR_INTERNAL("Network list container not found");
        return;
    }

    clear_network_list();

    // Sort by signal strength
    std::vector<WiFiNetwork> sorted_networks = networks;
    std::sort(sorted_networks.begin(), sorted_networks.end(),
              [](const WiFiNetwork& a, const WiFiNetwork& b) {
                  return a.signal_strength > b.signal_strength;
              });

    // Get connected network SSID
    std::string connected_ssid;
    if (wifi_manager_) {
        connected_ssid = wifi_manager_->get_connected_ssid();
        if (!connected_ssid.empty()) {
            spdlog::debug("[{}] Currently connected to: {}", get_name(), connected_ssid);
        }
    }

    // Create network items
    static int item_counter = 0;
    for (const auto& network : sorted_networks) {
        lv_obj_t* item =
            static_cast<lv_obj_t*>(lv_xml_create(network_list_container_, "wifi_network_item", nullptr));
        if (!item) {
            LOG_ERROR_INTERNAL("Failed to create network item for SSID: {}", network.ssid);
            continue;
        }

        char item_name[32];
        snprintf(item_name, sizeof(item_name), "network_item_%d", item_counter++);
        lv_obj_set_name(item, item_name);

        // Create per-instance data with back-reference to this step
        NetworkItemData* item_data = new NetworkItemData(network, this);

        // Bind SSID label
        lv_obj_t* ssid_label = lv_obj_find_by_name(item, "ssid_label");
        if (ssid_label) {
            lv_label_bind_text(ssid_label, item_data->ssid, nullptr);
        }

        // Set security type text
        lv_obj_t* security_label = lv_obj_find_by_name(item, "security_label");
        if (security_label) {
            if (network.is_secured) {
                lv_label_set_text(security_label, network.security_type.c_str());
            } else {
                lv_label_set_text(security_label, "");
            }
        }

        // Set signal icon
        lv_obj_t* signal_icon = lv_obj_find_by_name(item, "signal_icon");
        if (signal_icon) {
            const char* icon_name = get_wifi_signal_icon(network.signal_strength, network.is_secured);
            ui_icon_set_source(signal_icon, icon_name);
            spdlog::trace("[{}] Set signal icon '{}' for {}% ({})", get_name(), icon_name,
                          network.signal_strength, network.is_secured ? "secured" : "open");
        }

        // Highlight connected network
        bool is_connected = (!connected_ssid.empty() && network.ssid == connected_ssid);
        if (is_connected) {
            apply_connected_network_highlight(item);
            spdlog::debug("[{}] Highlighted connected network: {}", get_name(), network.ssid);
        }

        // Store data and register click
        lv_obj_set_user_data(item, item_data);
        lv_obj_add_event_cb(item, on_network_item_clicked_static, LV_EVENT_CLICKED, this);

        spdlog::debug("[{}] Added network: {} ({}%, {})", get_name(), network.ssid,
                      network.signal_strength, network.is_secured ? "secured" : "open");
    }

    spdlog::debug("[{}] Populated {} network items", get_name(), sorted_networks.size());
}

void WizardWifiStep::clear_network_list() {
    if (!network_list_container_) {
        spdlog::debug("[{}] clear_network_list: container is NULL", get_name());
        return;
    }

    spdlog::debug("[{}] Clearing network list", get_name());

    int32_t child_count = lv_obj_get_child_count(network_list_container_);
    spdlog::debug("[{}] Network list has {} children", get_name(), child_count);

    for (int32_t i = child_count - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(network_list_container_, i);
        if (!child) continue;

        const char* name = lv_obj_get_name(child);
        if (name && strncmp(name, "network_item_", 13) == 0) {
            spdlog::debug("[{}] Deleting network item: {}", get_name(), name);

            NetworkItemData* item_data = static_cast<NetworkItemData*>(lv_obj_get_user_data(child));
            lv_obj_delete(child);

            if (item_data) {
                delete item_data;
            }
        }
    }

    spdlog::debug("[{}] Network list cleared", get_name());
}

// ============================================================================
// Static Trampolines for LVGL Callbacks
// ============================================================================

void WizardWifiStep::on_wifi_toggle_changed_static(lv_event_t* e) {
    auto* self = static_cast<WizardWifiStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_wifi_toggle_changed(e);
    }
}

void WizardWifiStep::on_network_item_clicked_static(lv_event_t* e) {
    auto* self = static_cast<WizardWifiStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_network_item_clicked(e);
    }
}

void WizardWifiStep::on_modal_cancel_clicked_static(lv_event_t* e) {
    auto* self = static_cast<WizardWifiStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_modal_cancel_clicked();
    }
}

void WizardWifiStep::on_modal_connect_clicked_static(lv_event_t* e) {
    auto* self = static_cast<WizardWifiStep*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_modal_connect_clicked();
    }
}

// ============================================================================
// Event Handler Implementations
// ============================================================================

void WizardWifiStep::handle_wifi_toggle_changed(lv_event_t* e) {
    lv_obj_t* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle) return;

    bool checked = lv_obj_get_state(toggle) & LV_STATE_CHECKED;
    spdlog::debug("[{}] WiFi toggle changed: {}", get_name(), checked ? "ON" : "OFF");

    lv_subject_set_int(&wifi_enabled_, checked ? 1 : 0);

    if (checked) {
        update_wifi_status(get_status_text("enabled"));

        if (wifi_manager_) {
            wifi_manager_->set_enabled(true);
            lv_subject_set_int(&wifi_scanning_, 1);

            // Capture weak reference for async safety
            std::weak_ptr<WiFiManager> weak_mgr = wifi_manager_;
            WizardWifiStep* self = this;

            spdlog::debug("[{}] Starting network scan", get_name());
            wifi_manager_->start_scan([self, weak_mgr](const std::vector<WiFiNetwork>& networks) {
                spdlog::info("[{}] Scan callback with {} networks", self->get_name(), networks.size());

                // Check if manager still exists
                if (weak_mgr.expired()) {
                    spdlog::debug("[{}] WiFiManager destroyed, ignoring callback", self->get_name());
                    return;
                }

                lv_subject_set_int(&self->wifi_scanning_, 0);
                self->populate_network_list(networks);
            });
        } else {
            LOG_ERROR_INTERNAL("WiFi manager not initialized");
            NOTIFY_ERROR("WiFi unavailable");
        }
    } else {
        update_wifi_status(get_status_text("disabled"));
        lv_subject_set_int(&wifi_scanning_, 0);
        clear_network_list();

        if (wifi_manager_) {
            wifi_manager_->stop_scan();
            wifi_manager_->set_enabled(false);
        }
    }
}

void WizardWifiStep::handle_network_item_clicked(lv_event_t* e) {
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!item) return;

    NetworkItemData* item_data = static_cast<NetworkItemData*>(lv_obj_get_user_data(item));
    if (!item_data) {
        LOG_ERROR_INTERNAL("No network data found in clicked item");
        return;
    }

    const WiFiNetwork& network = item_data->network;
    spdlog::debug("[{}] Network clicked: {} ({}%)", get_name(), network.ssid, network.signal_strength);

    strncpy(current_ssid_, network.ssid.c_str(), sizeof(current_ssid_) - 1);
    current_ssid_[sizeof(current_ssid_) - 1] = '\0';
    current_secured_ = network.is_secured;

    char status_buf[128];
    snprintf(status_buf, sizeof(status_buf), "%s%s", get_status_text("connecting"), network.ssid.c_str());
    update_wifi_status(status_buf);

    if (network.is_secured) {
        show_password_modal(network.ssid.c_str());
    } else {
        // Connect to open network
        if (wifi_manager_) {
            WizardWifiStep* self = this;
            wifi_manager_->connect(network.ssid, "", [self](bool success, const std::string& error) {
                if (success) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%s%s", get_status_text("connected"), self->current_ssid_);
                    self->update_wifi_status(msg);
                    spdlog::info("[{}] Connected to {}", self->get_name(), self->current_ssid_);
                } else {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Failed to connect: %s", error.c_str());
                    self->update_wifi_status(msg);
                    NOTIFY_ERROR("Failed to connect to '{}': {}", self->current_ssid_, error);
                }
            });
        } else {
            LOG_ERROR_INTERNAL("WiFi manager not initialized");
            NOTIFY_ERROR("WiFi unavailable");
        }
    }
}

void WizardWifiStep::handle_modal_cancel_clicked() {
    spdlog::debug("[{}] Password modal cancel clicked", get_name());

    if (wifi_manager_) {
        wifi_manager_->disconnect();
        spdlog::info("[{}] Disconnecting from '{}'", get_name(), current_ssid_);
    }

    update_wifi_status(get_status_text("enabled"));
    hide_password_modal();
}

void WizardWifiStep::handle_modal_connect_clicked() {
    spdlog::debug("[{}] Password modal connect clicked", get_name());

    if (!password_modal_) {
        LOG_ERROR_INTERNAL("Password modal not found");
        return;
    }

    lv_obj_t* password_input = lv_obj_find_by_name(password_modal_, "password_input");
    if (!password_input) {
        LOG_ERROR_INTERNAL("Password input not found in modal");
        return;
    }

    const char* password = lv_textarea_get_text(password_input);
    if (!password || strlen(password) == 0) {
        lv_obj_t* modal_status = lv_obj_find_by_name(password_modal_, "modal_status");
        if (modal_status) {
            lv_label_set_text(modal_status, "Password cannot be empty");
            lv_obj_remove_flag(modal_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    spdlog::debug("[{}] Connecting to {} with password", get_name(), current_ssid_);

    lv_subject_set_int(&wifi_connecting_, 1);

    lv_obj_t* connect_btn = lv_obj_find_by_name(password_modal_, "modal_connect_btn");
    if (connect_btn) {
        lv_obj_add_state(connect_btn, LV_STATE_DISABLED);
    }

    char status_buf[128];
    snprintf(status_buf, sizeof(status_buf), "Connecting to %s...", current_ssid_);
    update_wifi_status(status_buf);

    if (wifi_manager_) {
        WizardWifiStep* self = this;
        wifi_manager_->connect(current_ssid_, password, [self](bool success, const std::string& error) {
            lv_subject_set_int(&self->wifi_connecting_, 0);

            lv_obj_t* connect_btn = lv_obj_find_by_name(self->password_modal_, "modal_connect_btn");
            if (connect_btn) {
                lv_obj_remove_state(connect_btn, LV_STATE_DISABLED);
            }

            if (success) {
                self->hide_password_modal();

                char msg[128];
                snprintf(msg, sizeof(msg), "%s%s", get_status_text("connected"), self->current_ssid_);
                self->update_wifi_status(msg);
                spdlog::info("[{}] Connected to {}", self->get_name(), self->current_ssid_);
            } else {
                lv_obj_t* modal_status = lv_obj_find_by_name(self->password_modal_, "modal_status");
                if (modal_status) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg), "Connection failed: %s", error.c_str());
                    lv_label_set_text(modal_status, error_msg);
                    lv_obj_remove_flag(modal_status, LV_OBJ_FLAG_HIDDEN);
                }

                self->update_wifi_status("Connection failed");
                NOTIFY_ERROR("Failed to connect to '{}': {}", self->current_ssid_, error);
            }
        });
    } else {
        LOG_ERROR_INTERNAL("WiFi manager not initialized");
        NOTIFY_ERROR("WiFi unavailable");
    }
}

// ============================================================================
// Subject Initialization
// ============================================================================

void WizardWifiStep::init_subjects() {
    spdlog::debug("[{}] Initializing subjects", get_name());

    UI_SUBJECT_INIT_AND_REGISTER_INT(wifi_enabled_, 0, "wifi_enabled");
    UI_SUBJECT_INIT_AND_REGISTER_INT(wifi_scanning_, 0, "wifi_scanning");
    UI_SUBJECT_INIT_AND_REGISTER_INT(wifi_password_modal_visible_, 0, "wifi_password_modal_visible");
    UI_SUBJECT_INIT_AND_REGISTER_INT(wifi_connecting_, 0, "wifi_connecting");

    UI_SUBJECT_INIT_AND_REGISTER_STRING(wifi_password_modal_ssid_, wifi_password_modal_ssid_buffer_, "", "wifi_password_modal_ssid");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(wifi_status_, wifi_status_buffer_, get_status_text("disabled"), "wifi_status");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(ethernet_status_, ethernet_status_buffer_, "Checking...", "ethernet_status");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

void WizardWifiStep::register_callbacks() {
    spdlog::debug("[{}] Registering event callbacks", get_name());

    lv_xml_register_event_cb(nullptr, "on_wifi_toggle_changed", on_wifi_toggle_changed_static);
    lv_xml_register_event_cb(nullptr, "on_network_item_clicked", on_network_item_clicked_static);
    lv_xml_register_event_cb(nullptr, "on_modal_cancel_clicked", on_modal_cancel_clicked_static);
    lv_xml_register_event_cb(nullptr, "on_modal_connect_clicked", on_modal_connect_clicked_static);

    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Responsive Constants Registration
// ============================================================================

void WizardWifiStep::register_responsive_constants() {
    spdlog::debug("[{}] Registering responsive constants", get_name());

    lv_display_t* display = lv_display_get_default();
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    const char* list_item_padding;
    const char* list_item_font;
    const char* size_label;

    static char list_item_height_buf[16];

    if (greater_res <= UI_BREAKPOINT_SMALL_MAX) {
        list_item_padding = "4";
        list_item_font = "montserrat_14";
        size_label = "SMALL";
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) {
        list_item_padding = "6";
        list_item_font = "montserrat_16";
        size_label = "MEDIUM";
    } else {
        list_item_padding = "8";
        list_item_font = lv_xml_get_const(NULL, "font_body");
        size_label = "LARGE";
    }

    const lv_font_t* item_font_ptr = lv_xml_get_font(NULL, list_item_font);
    if (item_font_ptr) {
        int32_t font_height = ui_theme_get_font_height(item_font_ptr);
        snprintf(list_item_height_buf, sizeof(list_item_height_buf), "%d", font_height);
        spdlog::debug("[{}] Calculated list_item_height={}px", get_name(), font_height);
    } else {
        const char* fallback_height = (greater_res <= UI_BREAKPOINT_SMALL_MAX) ? "60"
                                      : (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) ? "80"
                                                                                   : "100";
        snprintf(list_item_height_buf, sizeof(list_item_height_buf), "%s", fallback_height);
        LOG_WARN_INTERNAL("Failed to get font '{}', using fallback", list_item_font);
    }

    WifiConstant constants[] = {
        {"list_item_padding", list_item_padding},
        {"list_item_height", list_item_height_buf},
        {"list_item_font", list_item_font},
        {NULL, NULL}
    };

    lv_xml_component_scope_t* item_scope = lv_xml_component_get_scope("wifi_network_item");
    register_wifi_constants_to_scope(item_scope, constants);

    lv_xml_component_scope_t* wifi_setup_scope = lv_xml_component_get_scope("wizard_wifi_setup");
    register_wifi_constants_to_scope(wifi_setup_scope, constants);

    init_wifi_item_colors();

    spdlog::debug("[{}] Registered responsive constants ({})", get_name(), size_label);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardWifiStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating WiFi setup screen", get_name());

    if (!parent) {
        LOG_ERROR_INTERNAL("Cannot create WiFi screen: null parent");
        return nullptr;
    }

    // Register wifi_network_item component first
    static bool network_item_registered = false;
    if (!network_item_registered) {
        lv_xml_register_component_from_file("A:ui_xml/wifi_network_item.xml");
        network_item_registered = true;
        spdlog::debug("[{}] Registered wifi_network_item component", get_name());
    }

    screen_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_wifi_setup", nullptr));

    if (!screen_root_) {
        LOG_ERROR_INTERNAL("Failed to create wizard_wifi_setup from XML");
        return nullptr;
    }

    register_responsive_constants();

    network_list_container_ = lv_obj_find_by_name(screen_root_, "network_list_container");
    if (!network_list_container_) {
        LOG_ERROR_INTERNAL("Network list container not found in XML");
        return nullptr;
    }

    // Find WiFi toggle and attach callback with 'this' as user_data
    lv_obj_t* wifi_toggle = lv_obj_find_by_name(screen_root_, "wifi_toggle");
    if (wifi_toggle) {
        lv_obj_add_event_cb(wifi_toggle, on_wifi_toggle_changed_static, LV_EVENT_VALUE_CHANGED, this);
        spdlog::debug("[{}] WiFi toggle callback attached", get_name());
    }

    lv_obj_update_layout(screen_root_);

    spdlog::debug("[{}] WiFi screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// WiFi Manager Initialization
// ============================================================================

void WizardWifiStep::init_wifi_manager() {
    spdlog::debug("[{}] Initializing WiFi and Ethernet managers", get_name());

    wifi_manager_ = std::make_shared<WiFiManager>();
    wifi_manager_->init_self_reference(wifi_manager_);

    ethernet_manager_ = std::make_unique<EthernetManager>();

    update_ethernet_status();

    spdlog::debug("[{}] WiFi and Ethernet managers initialized", get_name());
}

// ============================================================================
// Password Modal
// ============================================================================

void WizardWifiStep::show_password_modal(const char* ssid) {
    if (!ssid) {
        LOG_ERROR_INTERNAL("Cannot show password modal: null SSID");
        return;
    }

    spdlog::debug("[{}] Showing password modal for SSID: {}", get_name(), ssid);

    ui_modal_keyboard_config_t kbd_config = {
        .auto_position = true
    };

    ui_modal_config_t config = {
        .position = {.use_alignment = true, .alignment = LV_ALIGN_CENTER},
        .backdrop_opa = 180,
        .keyboard = &kbd_config,
        .persistent = false,
        .on_close = nullptr
    };

    const char* attrs[] = {"ssid", ssid, NULL};
    password_modal_ = ui_modal_show("wifi_password_modal", &config, attrs);

    if (!password_modal_) {
        LOG_ERROR_INTERNAL("Failed to create password modal");
        return;
    }

    lv_subject_copy_string(&wifi_password_modal_ssid_, ssid);
    lv_subject_set_int(&wifi_password_modal_visible_, 1);

    lv_obj_t* password_input = lv_obj_find_by_name(password_modal_, "password_input");
    if (password_input) {
        lv_textarea_set_text(password_input, "");
        ui_modal_register_keyboard(password_modal_, password_input);

        lv_group_t* group = lv_group_get_default();
        if (group) {
            lv_group_focus_obj(password_input);
            spdlog::debug("[{}] Focused password input via group", get_name());
        }
    }

    lv_obj_t* cancel_btn = lv_obj_find_by_name(password_modal_, "modal_cancel_btn");
    if (cancel_btn) {
        lv_obj_add_event_cb(cancel_btn, on_modal_cancel_clicked_static, LV_EVENT_CLICKED, this);
    }

    lv_obj_t* connect_btn = lv_obj_find_by_name(password_modal_, "modal_connect_btn");
    if (connect_btn) {
        lv_obj_add_event_cb(connect_btn, on_modal_connect_clicked_static, LV_EVENT_CLICKED, this);
    }

    spdlog::info("[{}] Password modal shown for SSID: {}", get_name(), ssid);
}

void WizardWifiStep::hide_password_modal() {
    if (!password_modal_) return;

    spdlog::debug("[{}] Hiding password modal", get_name());

    lv_subject_set_int(&wifi_password_modal_visible_, 0);
    ui_modal_hide(password_modal_);
    password_modal_ = nullptr;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardWifiStep::cleanup() {
    spdlog::debug("[{}] Cleaning up WiFi screen", get_name());

    if (wifi_manager_) {
        spdlog::debug("[{}] Stopping scan", get_name());
        wifi_manager_->stop_scan();
    }

    spdlog::debug("[{}] Clearing network list", get_name());
    clear_network_list();

    spdlog::debug("[{}] Destroying WiFi manager", get_name());
    wifi_manager_.reset();

    spdlog::debug("[{}] Destroying Ethernet manager", get_name());
    ethernet_manager_.reset();

    screen_root_ = nullptr;
    password_modal_ = nullptr;
    network_list_container_ = nullptr;
    current_ssid_[0] = '\0';
    current_secured_ = false;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Deprecated Legacy API Wrappers
// ============================================================================

void ui_wizard_wifi_init_subjects() {
    get_wizard_wifi_step()->init_subjects();
}

void ui_wizard_wifi_register_callbacks() {
    get_wizard_wifi_step()->register_callbacks();
}

void ui_wizard_wifi_register_responsive_constants() {
    get_wizard_wifi_step()->register_responsive_constants();
}

lv_obj_t* ui_wizard_wifi_create(lv_obj_t* parent) {
    return get_wizard_wifi_step()->create(parent);
}

void ui_wizard_wifi_init_wifi_manager() {
    get_wizard_wifi_step()->init_wifi_manager();
}

void ui_wizard_wifi_cleanup() {
    get_wizard_wifi_step()->cleanup();
}

void ui_wizard_wifi_show_password_modal(const char* ssid) {
    get_wizard_wifi_step()->show_password_modal(ssid);
}

void ui_wizard_wifi_hide_password_modal() {
    get_wizard_wifi_step()->hide_password_modal();
}
