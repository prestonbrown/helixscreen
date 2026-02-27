// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_wifi.h"

#include "ui_error_reporting.h"
#include "ui_icon.h"
#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "config.h"
#include "ethernet_manager.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>

using namespace helix;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardWifiStep> g_wizard_wifi_step;

WizardWifiStep* get_wizard_wifi_step() {
    if (!g_wizard_wifi_step) {
        g_wizard_wifi_step = std::make_unique<WizardWifiStep>();
        StaticPanelRegistry::instance().register_destroy("WizardWifiStep",
                                                         []() { g_wizard_wifi_step.reset(); });
    }
    return g_wizard_wifi_step.get();
}

// ============================================================================
// Helper Types (per-instance network item data)
// ============================================================================

/**
 * @brief Compute signal icon state from signal strength and security status
 *
 * @param signal_strength Signal strength (0-100%)
 * @param is_secured Whether network is password-protected
 * @return State 1-8: 1-4 unsecured (strength 1-4), 5-8 secured (strength 1-4)
 */
static int compute_signal_icon_state(int signal_strength, bool is_secured) {
    int base_state;
    if (signal_strength <= 25)
        base_state = 1;
    else if (signal_strength <= 50)
        base_state = 2;
    else if (signal_strength <= 75)
        base_state = 3;
    else
        base_state = 4;

    return is_secured ? base_state + 4 : base_state; // 1-4 unsecured, 5-8 secured
}

/**
 * @brief Per-instance network item data for reactive UI updates
 *
 * Subjects are stack-allocated (not heap) since lv_subject_t is a small struct (~32 bytes).
 * This eliminates manual memory management and potential leaks.
 * @note Named distinctly to avoid ODR conflicts with NetworkSettingsItemData
 */
struct WifiWizardNetworkItemData {
    WiFiNetwork network;
    lv_subject_t ssid;              // Stack-allocated subject
    lv_subject_t signal_strength;   // Stack-allocated subject
    lv_subject_t is_secured;        // Stack-allocated subject
    lv_subject_t signal_icon_state; // Combined state 1-8 for icon visibility binding
    char ssid_buffer[64];
    WizardWifiStep* parent; // Back-reference for callbacks

    WifiWizardNetworkItemData(const WiFiNetwork& net, WizardWifiStep* p) : network(net), parent(p) {
        strncpy(ssid_buffer, network.ssid.c_str(), sizeof(ssid_buffer) - 1);
        ssid_buffer[sizeof(ssid_buffer) - 1] = '\0';
        lv_subject_init_string(&ssid, ssid_buffer, nullptr, sizeof(ssid_buffer), ssid_buffer);
        lv_subject_init_int(&signal_strength, network.signal_strength);
        lv_subject_init_int(&is_secured, network.is_secured ? 1 : 0);

        // Compute combined icon state (1-8)
        int icon_state = compute_signal_icon_state(network.signal_strength, network.is_secured);
        lv_subject_init_int(&signal_icon_state, icon_state);
    }

    ~WifiWizardNetworkItemData() {
        // Deinit subjects before memory is freed
        lv_subject_deinit(&ssid);
        lv_subject_deinit(&signal_strength);
        lv_subject_deinit(&is_secured);
        lv_subject_deinit(&signal_icon_state);
    }
};

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardWifiStep::WizardWifiStep() {
    std::memset(wifi_status_buffer_, 0, sizeof(wifi_status_buffer_));
    std::memset(wifi_ip_buffer_, 0, sizeof(wifi_ip_buffer_));
    std::memset(wifi_mac_buffer_, 0, sizeof(wifi_mac_buffer_));
    std::memset(ethernet_status_buffer_, 0, sizeof(ethernet_status_buffer_));
    std::memset(ethernet_mac_buffer_, 0, sizeof(ethernet_mac_buffer_));
    std::memset(wifi_password_modal_ssid_buffer_, 0, sizeof(wifi_password_modal_ssid_buffer_));
    std::memset(current_ssid_, 0, sizeof(current_ssid_));

    spdlog::debug("[{}] Instance created", get_name());
}

WizardWifiStep::~WizardWifiStep() {
    // Release references to managers - shared WiFiManager continues running
    wifi_manager_.reset();
    ethernet_manager_.reset();

    // Deinitialize subjects BEFORE they're destroyed as member variables.
    // This disconnects any LVGL observers still bound to them, preventing
    // use-after-free when lv_deinit() later deletes widgets with bindings.
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Clear pointers (widgets still exist, owned by LVGL)
    screen_root_ = nullptr;
    password_modal_ = nullptr;
    network_list_container_ = nullptr;
}

// ============================================================================
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
    // Icon names from ui_icon_codepoints.h - font-based MDI icons
    if (signal_strength <= 25) {
        return is_secured ? "wifi_strength_1_lock" : "wifi_strength_1";
    } else if (signal_strength <= 50) {
        return is_secured ? "wifi_strength_2_lock" : "wifi_strength_2";
    } else if (signal_strength <= 75) {
        return is_secured ? "wifi_strength_3_lock" : "wifi_strength_3";
    } else {
        return is_secured ? "wifi_strength_4_lock" : "wifi_strength_4";
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

void WizardWifiStep::update_wifi_status(const char* status) {
    if (!status)
        return;
    spdlog::debug("[{}] Updating WiFi status: {}", get_name(), status);
    lv_subject_copy_string(&wifi_status_, status);
}

void WizardWifiStep::update_wifi_ip(const char* ip) {
    spdlog::debug("[{}] Updating WiFi IP: {}", get_name(), ip ? ip : "(none)");
    lv_subject_copy_string(&wifi_ip_, ip ? ip : "");

    // Update WiFi MAC when we have an IP (connected)
    if (ip && ip[0] != '\0' && wifi_manager_) {
        std::string mac = wifi_manager_->get_mac_address();
        spdlog::debug("[{}] WiFi MAC from backend: '{}' (len={})", get_name(), mac, mac.size());
        if (!mac.empty()) {
            char mac_buf[32];
            snprintf(mac_buf, sizeof(mac_buf), "MAC: %s", mac.c_str());
            lv_subject_copy_string(&wifi_mac_, mac_buf);
        }
    } else {
        lv_subject_copy_string(&wifi_mac_, "");
    }
}

void WizardWifiStep::update_ethernet_status() {
    if (!ethernet_manager_) {
        LOG_WARN_INTERNAL("Ethernet manager not initialized");
        lv_subject_copy_string(&ethernet_status_, "Unknown");
        lv_subject_copy_string(&ethernet_mac_, "");
        return;
    }

    EthernetInfo info = ethernet_manager_->get_info();

    if (info.connected) {
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf), lv_tr("Connected (%s)"), info.ip_address.c_str());
        lv_subject_copy_string(&ethernet_status_, status_buf);
        spdlog::debug("[{}] Ethernet status: {}", get_name(), status_buf);
    } else {
        lv_subject_copy_string(&ethernet_status_, info.status.c_str());
        spdlog::debug("[{}] Ethernet status: {}", get_name(), info.status);
    }

    // Always show MAC address if available
    if (!info.mac_address.empty()) {
        char mac_buf[32];
        snprintf(mac_buf, sizeof(mac_buf), "MAC: %s", info.mac_address.c_str());
        lv_subject_copy_string(&ethernet_mac_, mac_buf);
    } else {
        lv_subject_copy_string(&ethernet_mac_, "");
    }
}

void WizardWifiStep::populate_network_list(const std::vector<WiFiNetwork>& networks) {
    spdlog::debug("[{}] Populating network list with {} networks", get_name(), networks.size());

    if (!network_list_container_) {
        LOG_ERROR_INTERNAL("Network list container not found");
        return;
    }

    // Save scroll position before clearing - prevents jarring UX when list refreshes
    int32_t scroll_y = lv_obj_get_scroll_y(network_list_container_);
    spdlog::trace("[{}] Saving scroll position: {}px", get_name(), scroll_y);

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
        lv_obj_t* item = static_cast<lv_obj_t*>(
            lv_xml_create(network_list_container_, "wifi_network_item", nullptr));
        if (!item) {
            LOG_ERROR_INTERNAL("Failed to create network item for SSID: {}", network.ssid);
            continue;
        }

        char item_name[32];
        snprintf(item_name, sizeof(item_name), "network_item_%d", item_counter++);
        lv_obj_set_name(item, item_name);

        // Create per-instance data with back-reference to this step
        WifiWizardNetworkItemData* item_data = new WifiWizardNetworkItemData(network, this);

        // Bind SSID label to subject (LVGL auto-cleans observers when widget is deleted)
        lv_obj_t* ssid_label = lv_obj_find_by_name(item, "ssid_label");
        if (ssid_label) {
            lv_label_bind_text(ssid_label, &item_data->ssid, nullptr);
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

        // Bind signal icons - 8 icons in container, show only the one matching state
        // LVGL automatically removes observers when child widgets are deleted
        lv_obj_t* signal_icons = lv_obj_find_by_name(item, "signal_icons");
        if (signal_icons) {
            static const struct {
                const char* name;
                int state;
            } icon_bindings[] = {
                {"sig_1", 1},      {"sig_2", 2},      {"sig_3", 3},      {"sig_4", 4},
                {"sig_1_lock", 5}, {"sig_2_lock", 6}, {"sig_3_lock", 7}, {"sig_4_lock", 8},
            };

            int current_state = lv_subject_get_int(&item_data->signal_icon_state);

            for (const auto& binding : icon_bindings) {
                lv_obj_t* icon = lv_obj_find_by_name(signal_icons, binding.name);
                if (icon) {
                    // Bind visibility: hidden when state != ref_value
                    lv_obj_bind_flag_if_not_eq(icon, &item_data->signal_icon_state,
                                               LV_OBJ_FLAG_HIDDEN, binding.state);
                }
            }

            spdlog::trace("[{}] Bound signal icons for {}% ({}) -> state {}", get_name(),
                          network.signal_strength, network.is_secured ? "secured" : "open",
                          current_state);
        }

        // Mark connected network with LV_STATE_CHECKED (styling handled by XML)
        bool is_connected = (!connected_ssid.empty() && network.ssid == connected_ssid);
        if (is_connected) {
            lv_obj_add_state(item, LV_STATE_CHECKED);
            spdlog::debug("[{}] Marked connected network: {}", get_name(), network.ssid);

            // Update status/IP/MAC display — the initial is_connected() check at
            // init time can miss pre-existing connections due to NM query timing,
            // so we also update here when the scan reveals a connected network.
            std::string status_msg = std::string(get_status_text("connected")) + connected_ssid;
            update_wifi_status(status_msg.c_str());
            if (wifi_manager_) {
                std::string ip = wifi_manager_->get_ip_address();
                update_wifi_ip(ip.c_str());
            }
        }

        // Store network data for click handler (callback registered via XML event_cb)
        lv_obj_set_user_data(item, item_data);

        // Register DELETE handler for automatic cleanup when widget is deleted
        lv_obj_add_event_cb(item, network_item_delete_cb, LV_EVENT_DELETE, nullptr);

        spdlog::debug("[{}] Added network: {} ({}%, {})", get_name(), network.ssid,
                      network.signal_strength, network.is_secured ? "secured" : "open");
    }

    // Restore scroll position after repopulating
    // Need to update layout first so LVGL knows the new content size
    lv_obj_update_layout(network_list_container_);
    lv_obj_scroll_to_y(network_list_container_, scroll_y, LV_ANIM_OFF);
    spdlog::trace("[{}] Restored scroll position: {}px", get_name(), scroll_y);

    spdlog::debug("[{}] Populated {} network items", get_name(), sorted_networks.size());
}

void WizardWifiStep::clear_network_list() {
    if (!network_list_container_) {
        spdlog::debug("[{}] clear_network_list: container is NULL", get_name());
        return;
    }

    spdlog::debug("[{}] Clearing network list", get_name());

    int32_t child_count = static_cast<int32_t>(lv_obj_get_child_count(network_list_container_));
    spdlog::debug("[{}] Network list has {} children", get_name(), child_count);

    for (int32_t i = child_count - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(network_list_container_, i);
        if (!child)
            continue;

        const char* name = lv_obj_get_name(child);
        if (name && strncmp(name, "network_item_", 13) == 0) {
            spdlog::debug("[{}] Deleting network item: {}", get_name(), name);

            // Delete the widget - the DELETE handler will automatically clean up
            // WifiWizardNetworkItemData
            helix::ui::safe_delete(child);
        }
    }

    spdlog::debug("[{}] Network list cleared", get_name());
}

// ============================================================================
// Static Trampolines for LVGL Callbacks
// ============================================================================

void WizardWifiStep::network_item_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!obj)
        return;

    // Wrap raw pointer in unique_ptr for RAII cleanup
    // (destructor calls lv_subject_deinit on all subjects)
    std::unique_ptr<WifiWizardNetworkItemData> data(
        static_cast<WifiWizardNetworkItemData*>(lv_obj_get_user_data(obj)));
    lv_obj_set_user_data(obj, nullptr);

    // NOTE: Observers are auto-removed when LVGL deletes child widgets (before this callback).
    // Do NOT manually remove them - the observer pointers are already freed = use-after-free.
    // data automatically freed via ~unique_ptr()
}

void WizardWifiStep::on_wifi_toggle_changed_static(lv_event_t* e) {
    // Use global accessor pattern (XML event_cb doesn't provide user_data)
    WizardWifiStep* self = get_wizard_wifi_step();
    if (self) {
        self->handle_wifi_toggle_changed(e);
    }
}

void WizardWifiStep::on_network_item_clicked_static(lv_event_t* e) {
    // Network items use item user_data (WifiWizardNetworkItemData with parent pointer)
    // instead of event user_data, since XML event_cb can't pass instance context
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!item)
        return;

    WifiWizardNetworkItemData* item_data =
        static_cast<WifiWizardNetworkItemData*>(lv_obj_get_user_data(item));
    if (item_data && item_data->parent) {
        item_data->parent->handle_network_item_clicked(e);
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
    if (!toggle)
        return;

    // Don't process toggle if hardware unavailable
    if (lv_subject_get_int(&wifi_hardware_available_) == 0) {
        spdlog::debug("[{}] Ignoring toggle - WiFi hardware unavailable", get_name());
        return;
    }

    bool checked = lv_obj_get_state(toggle) & LV_STATE_CHECKED;
    spdlog::debug("[{}] WiFi toggle changed: {}", get_name(), checked ? "ON" : "OFF");

    lv_subject_set_int(&wifi_enabled_, checked ? 1 : 0);

    // Persist WiFi expectation
    if (auto* config = Config::get_instance()) {
        config->set_wifi_expected(checked);
        // Don't save yet - will be saved on wizard completion
    }

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
                spdlog::info("[{}] Scan callback with {} networks", self->get_name(),
                             networks.size());

                // Check if manager still exists
                if (weak_mgr.expired()) {
                    spdlog::trace("[{}] WiFiManager destroyed, ignoring callback",
                                  self->get_name());
                    return;
                }

                // Check if cleanup was called (step navigated away)
                if (self->cleanup_called_) {
                    spdlog::debug("[{}] Cleanup was called, ignoring stale scan callback",
                                  self->get_name());
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
        update_wifi_ip(""); // Clear IP when WiFi disabled
        lv_subject_set_int(&wifi_scanning_, 0);
        clear_network_list();

        if (wifi_manager_) {
            wifi_manager_->stop_scan();
            wifi_manager_->set_enabled(false);
        }
    }
}

void WizardWifiStep::handle_network_item_clicked(lv_event_t* e) {
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!item)
        return;

    WifiWizardNetworkItemData* item_data =
        static_cast<WifiWizardNetworkItemData*>(lv_obj_get_user_data(item));
    if (!item_data) {
        LOG_ERROR_INTERNAL("No network data found in clicked item");
        return;
    }

    const WiFiNetwork& network = item_data->network;
    spdlog::debug("[{}] Network clicked: {} ({}%)", get_name(), network.ssid,
                  network.signal_strength);

    strncpy(current_ssid_, network.ssid.c_str(), sizeof(current_ssid_) - 1);
    current_ssid_[sizeof(current_ssid_) - 1] = '\0';
    current_network_is_secured_ = network.is_secured;

    char status_buf[128];
    snprintf(status_buf, sizeof(status_buf), "%s%s", get_status_text("connecting"),
             network.ssid.c_str());
    update_wifi_status(status_buf);

    if (network.is_secured) {
        show_password_modal(network.ssid.c_str());
    } else {
        // Connect to open network
        if (wifi_manager_) {
            WizardWifiStep* self = this;
            wifi_manager_->connect(
                network.ssid, "", [self](bool success, const std::string& error) {
                    // Check if cleanup was called (step navigated away)
                    if (self->cleanup_called_) {
                        spdlog::debug("[{}] Cleanup was called, ignoring connect callback",
                                      self->get_name());
                        return;
                    }
                    if (success) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "%s%s", get_status_text("connected"),
                                 self->current_ssid_);
                        self->update_wifi_status(msg);
                        // Update IP address display
                        if (self->wifi_manager_) {
                            std::string ip = self->wifi_manager_->get_ip_address();
                            self->update_wifi_ip(ip.c_str());
                        }
                        spdlog::info("[{}] Connected to {}", self->get_name(), self->current_ssid_);
                    } else {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Failed to connect: %s", error.c_str());
                        self->update_wifi_status(msg);
                        self->update_wifi_ip(""); // Clear IP on failure
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
    update_wifi_ip(""); // Clear IP on disconnect
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
            lv_label_set_text(modal_status, lv_tr("Password cannot be empty"));
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
        wifi_manager_->connect(
            current_ssid_, password, [self](bool success, const std::string& error) {
                // Check if cleanup was called (step navigated away)
                if (self->cleanup_called_) {
                    spdlog::debug("[{}] Cleanup was called, ignoring connect callback",
                                  self->get_name());
                    return;
                }

                lv_subject_set_int(&self->wifi_connecting_, 0);

                lv_obj_t* connect_btn =
                    lv_obj_find_by_name(self->password_modal_, "modal_connect_btn");
                if (connect_btn) {
                    lv_obj_remove_state(connect_btn, LV_STATE_DISABLED);
                }

                if (success) {
                    self->hide_password_modal();

                    char msg[128];
                    snprintf(msg, sizeof(msg), "%s%s", get_status_text("connected"),
                             self->current_ssid_);
                    self->update_wifi_status(msg);
                    // Update IP address display
                    if (self->wifi_manager_) {
                        std::string ip = self->wifi_manager_->get_ip_address();
                        self->update_wifi_ip(ip.c_str());
                    }
                    spdlog::info("[{}] Connected to {}", self->get_name(), self->current_ssid_);
                } else {
                    lv_obj_t* modal_status =
                        lv_obj_find_by_name(self->password_modal_, "modal_status");
                    if (modal_status) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg), "Connection failed: %s",
                                 error.c_str());
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

    UI_MANAGED_SUBJECT_INT(wifi_enabled_, 0, "wifi_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(wifi_scanning_, 0, "wifi_scanning", subjects_);
    UI_MANAGED_SUBJECT_INT(wifi_connecting_, 0, "wifi_connecting", subjects_);
    UI_MANAGED_SUBJECT_INT(wifi_hardware_available_, 1, "wifi_hardware_available", subjects_);

    UI_MANAGED_SUBJECT_STRING(wifi_password_modal_ssid_, wifi_password_modal_ssid_buffer_, "",
                              "wifi_password_modal_ssid", subjects_);
    UI_MANAGED_SUBJECT_STRING(wifi_status_, wifi_status_buffer_, get_status_text("disabled"),
                              "wifi_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(wifi_ip_, wifi_ip_buffer_, "", "wifi_ip", subjects_);
    UI_MANAGED_SUBJECT_STRING(wifi_mac_, wifi_mac_buffer_, "", "wifi_mac", subjects_);
    UI_MANAGED_SUBJECT_STRING(ethernet_status_, ethernet_status_buffer_, "Checking...",
                              "ethernet_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(ethernet_mac_, ethernet_mac_buffer_, "", "ethernet_mac", subjects_);

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
    lv_xml_register_event_cb(nullptr, "on_wifi_password_cancel", on_modal_cancel_clicked_static);
    lv_xml_register_event_cb(nullptr, "on_wifi_password_connect", on_modal_connect_clicked_static);

    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardWifiStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating WiFi setup screen", get_name());

    // Reset cleanup flag when (re)creating the screen
    cleanup_called_ = false;

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

    network_list_container_ = lv_obj_find_by_name(screen_root_, "network_list_container");
    if (!network_list_container_) {
        LOG_ERROR_INTERNAL("Network list container not found in XML");
        return nullptr;
    }

    // WiFi toggle callback is attached via XML event_cb (uses global accessor pattern)

    lv_obj_update_layout(screen_root_);

    spdlog::debug("[{}] WiFi screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// WiFi Manager Initialization
// ============================================================================

void WizardWifiStep::init_wifi_manager() {
    spdlog::debug("[{}] Initializing WiFi and Ethernet managers", get_name());

    wifi_manager_ = get_wifi_manager();

    ethernet_manager_ = std::make_unique<EthernetManager>();

    update_ethernet_status();

    // Check WiFi hardware availability and update subject
    bool hw_available = wifi_manager_ && wifi_manager_->has_hardware();
    lv_subject_set_int(&wifi_hardware_available_, hw_available ? 1 : 0);

    if (!hw_available) {
        spdlog::info("[{}] WiFi hardware not available - controls disabled", get_name());
        update_wifi_status(lv_tr("WiFi control unavailable"));
        return;
    }

    // Detect actual WiFi state from system wpa_supplicant
    // Try to connect to existing wpa_supplicant and query state
    if (wifi_manager_->has_hardware()) {
        // Start the backend to connect to existing wpa_supplicant
        bool started = wifi_manager_->set_enabled(true);
        if (started && wifi_manager_->is_enabled()) {
            spdlog::info("[{}] WiFi backend connected to system wpa_supplicant", get_name());

            // Update toggle and subject to reflect actual state
            lv_subject_set_int(&wifi_enabled_, 1);

            // Update toggle visual state
            lv_obj_t* wifi_toggle = lv_obj_find_by_name(screen_root_, "wifi_toggle");
            if (wifi_toggle) {
                lv_obj_add_state(wifi_toggle, LV_STATE_CHECKED);
            }

            // Check if already connected
            if (wifi_manager_->is_connected()) {
                std::string ssid = wifi_manager_->get_connected_ssid();
                std::string ip = wifi_manager_->get_ip_address();
                spdlog::info("[{}] Already connected to '{}' with IP {}", get_name(), ssid, ip);

                // Update status and IP display
                std::string status_msg = std::string(lv_tr("Connected to ")) + ssid;
                update_wifi_status(status_msg.c_str());
                update_wifi_ip(ip.c_str());
            } else {
                update_wifi_status(get_status_text("enabled"));
            }

            // Start a scan to populate the network list
            lv_subject_set_int(&wifi_scanning_, 1);
            wifi_manager_->start_scan([this](const std::vector<WiFiNetwork>& networks) {
                // Check if cleanup was called (step navigated away)
                if (cleanup_called_) {
                    spdlog::debug("[{}] Cleanup was called, ignoring init scan callback",
                                  get_name());
                    return;
                }

                lv_subject_set_int(&wifi_scanning_, 0);
                if (!networks.empty()) {
                    // Use lv_async_call to update UI on main thread
                    // Store networks temporarily for the async call
                    cached_networks_ = networks;
                    helix::ui::async_call(
                        [](void* ctx) {
                            auto* self = static_cast<WizardWifiStep*>(ctx);
                            // Double-check cleanup in async callback
                            if (self->cleanup_called_) {
                                spdlog::debug("[{}] Cleanup called, skipping network list update",
                                              self->get_name());
                                return;
                            }
                            self->populate_network_list(self->cached_networks_);
                        },
                        this);
                }
            });
        } else {
            spdlog::debug("[{}] WiFi not available or failed to start", get_name());
        }
    }

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

    const char* attrs[] = {"ssid", ssid, NULL};
    password_modal_ = helix::ui::modal_show("wifi_password_modal", attrs);

    if (!password_modal_) {
        LOG_ERROR_INTERNAL("Failed to create password modal");
        return;
    }

    lv_subject_copy_string(&wifi_password_modal_ssid_, ssid);

    lv_obj_t* password_input = lv_obj_find_by_name(password_modal_, "password_input");
    if (password_input) {
        lv_textarea_set_text(password_input, "");
        helix::ui::modal_register_keyboard(password_modal_, password_input);

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
    if (!password_modal_)
        return;

    spdlog::debug("[{}] Hiding password modal", get_name());

    helix::ui::modal_hide(password_modal_);
    password_modal_ = nullptr;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardWifiStep::cleanup() {
    spdlog::debug("[{}] Cleaning up WiFi screen", get_name());

    // Mark as cleaned up FIRST to invalidate any pending async callbacks
    cleanup_called_ = true;

    if (wifi_manager_) {
        spdlog::debug("[{}] Stopping scan", get_name());
        wifi_manager_->stop_scan();
    }

    spdlog::debug("[{}] Clearing network list", get_name());
    clear_network_list();

    wifi_manager_.reset();
    ethernet_manager_.reset();

    screen_root_ = nullptr;
    password_modal_ = nullptr;
    network_list_container_ = nullptr;
    current_ssid_[0] = '\0';
    current_network_is_secured_ = false;

    spdlog::debug("[{}] Cleanup complete", get_name());
}
