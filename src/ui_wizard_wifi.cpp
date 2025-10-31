/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "ui_wizard_wifi.h"
#include "wifi_manager.h"
#include "ethernet_manager.h"
#include "ui_keyboard.h"
#include "lvgl/lvgl.h"
#include <spdlog/spdlog.h>
#include <memory>
#include <cstring>
#include <algorithm>

// ============================================================================
// Static Data & Subjects
// ============================================================================

// Subject declarations (module scope)
static lv_subject_t wifi_enabled;
static lv_subject_t wifi_status;
static lv_subject_t ethernet_status;
static lv_subject_t wifi_scanning;  // 0=not scanning, 1=scanning

// String buffers (must be persistent)
static char wifi_status_buffer[64];
static char ethernet_status_buffer[64];

// WiFi screen instance
static lv_obj_t* wifi_screen_root = nullptr;
static lv_obj_t* password_modal = nullptr;
static lv_obj_t* network_list_container = nullptr;

// WiFiManager instance
// Using shared_ptr instead of unique_ptr for async callback safety
static std::shared_ptr<WiFiManager> wifi_manager = nullptr;

// EthernetManager instance
static std::unique_ptr<EthernetManager> ethernet_manager = nullptr;

// Current network selection (for password modal)
static char current_ssid[64] = "";
static bool current_secured = false;

// Per-instance network item data (reactive approach)
// Each network item gets its own subject set for reactive UI updates
struct NetworkItemData {
    WiFiNetwork network;           // Network info for click handler
    lv_subject_t* ssid;           // Subject for SSID label binding
    lv_subject_t* signal_strength;// Subject for signal icon color
    lv_subject_t* is_secured;     // Subject for lock icon visibility
    char ssid_buffer[64];         // Persistent buffer for SSID subject

    NetworkItemData(const WiFiNetwork& net) : network(net) {
        ssid = new lv_subject_t();
        signal_strength = new lv_subject_t();
        is_secured = new lv_subject_t();

        // Initialize subjects
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
// Forward Declarations
// ============================================================================

static void on_wifi_toggle_changed(lv_event_t* e);
static void on_network_item_clicked(lv_event_t* e);
static void on_modal_cancel_clicked(lv_event_t* e);
static void on_modal_connect_clicked(lv_event_t* e);

static void update_wifi_status(const char* status);
static void update_ethernet_status();
static void populate_network_list(const std::vector<WiFiNetwork>& networks);
static void clear_network_list();

// ============================================================================
// Public API Implementation
// ============================================================================

void ui_wizard_wifi_init_subjects() {
    spdlog::debug("[WiFi Screen] Initializing subjects");

    // Initialize subjects with defaults
    lv_subject_init_int(&wifi_enabled, 0);   // WiFi off by default
    lv_subject_init_int(&wifi_scanning, 0);  // Not scanning by default

    lv_subject_init_string(&wifi_status, wifi_status_buffer, nullptr,
                           sizeof(wifi_status_buffer), "WiFi Disabled");

    lv_subject_init_string(&ethernet_status, ethernet_status_buffer, nullptr,
                           sizeof(ethernet_status_buffer), "Checking...");

    // Register subjects globally
    lv_xml_register_subject(nullptr, "wifi_enabled", &wifi_enabled);
    lv_xml_register_subject(nullptr, "wifi_status", &wifi_status);
    lv_xml_register_subject(nullptr, "ethernet_status", &ethernet_status);
    lv_xml_register_subject(nullptr, "wifi_scanning", &wifi_scanning);

    spdlog::info("[WiFi Screen] Subjects initialized");
}

void ui_wizard_wifi_register_callbacks() {
    spdlog::debug("[WiFi Screen] Registering event callbacks");

    lv_xml_register_event_cb(nullptr, "on_wifi_toggle_changed", on_wifi_toggle_changed);
    lv_xml_register_event_cb(nullptr, "on_network_item_clicked", on_network_item_clicked);
    lv_xml_register_event_cb(nullptr, "on_modal_cancel_clicked", on_modal_cancel_clicked);
    lv_xml_register_event_cb(nullptr, "on_modal_connect_clicked", on_modal_connect_clicked);

    spdlog::info("[WiFi Screen] Callbacks registered");
}

void ui_wizard_wifi_register_responsive_constants() {
    spdlog::debug("[WiFi Screen] Registering responsive constants");

    // Detect screen size
    int width = lv_display_get_horizontal_resolution(lv_display_get_default());

    // Calculate responsive values
    const char* card_height;
    const char* ethernet_height;
    const char* toggle_height;
    const char* title_font;
    const char* status_font;
    const char* help_font;
    const char* network_title_font;
    const char* network_item_height;
    const char* network_icon_size;

    if (width < 600) {  // TINY (480x320)
        card_height = "80";
        ethernet_height = "70";
        toggle_height = "32";  // size="medium" at TINY: 24px + 2*2px pad = 28px
        title_font = "montserrat_14";
        status_font = "montserrat_12";
        help_font = "montserrat_12";
        network_title_font = "montserrat_14";
        network_item_height = "60";
        network_icon_size = "20";
        spdlog::info("[WiFi Screen] Size: TINY ({}px)", width);
    } else if (width < 900) {  // SMALL (800x480)
        card_height = "120";
        ethernet_height = "100";
        toggle_height = "48";  // size="medium" at SMALL: 40px + 2*3px pad = 46px
        title_font = "montserrat_20";
        status_font = "montserrat_14";
        help_font = "montserrat_16";
        network_title_font = "montserrat_16";
        network_item_height = "80";
        network_icon_size = "24";
        spdlog::info("[WiFi Screen] Size: SMALL ({}px)", width);
    } else {  // LARGE (1024x600+)
        card_height = "140";
        ethernet_height = "120";
        toggle_height = "64";  // size="medium" switch (56px) + breathing room
        title_font = "montserrat_24";
        status_font = "montserrat_16";
        help_font = "montserrat_18";
        network_title_font = "montserrat_18";
        network_item_height = "100";
        network_icon_size = "32";
        spdlog::info("[WiFi Screen] Size: LARGE ({}px)", width);
    }

    // Get globals scope
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");

    // Register constants BEFORE creating WiFi screen
    lv_xml_register_const(scope, "wifi_card_height", card_height);
    lv_xml_register_const(scope, "wifi_ethernet_height", ethernet_height);
    lv_xml_register_const(scope, "wifi_toggle_height", toggle_height);
    lv_xml_register_const(scope, "wifi_title_font", title_font);
    lv_xml_register_const(scope, "wifi_status_font", status_font);
    lv_xml_register_const(scope, "wifi_help_font", help_font);
    lv_xml_register_const(scope, "wifi_network_title_font", network_title_font);
    lv_xml_register_const(scope, "network_item_height", network_item_height);
    lv_xml_register_const(scope, "network_icon_size", network_icon_size);

    spdlog::debug("[WiFi Screen] Registered constants: card={}px, ethernet={}px, toggle={}px, network_item={}px",
                  card_height, ethernet_height, toggle_height, network_item_height);
}

lv_obj_t* ui_wizard_wifi_create(lv_obj_t* parent) {
    spdlog::debug("[WiFi Screen] Creating WiFi setup screen");

    if (!parent) {
        spdlog::error("[WiFi Screen] Cannot create: null parent");
        return nullptr;
    }

    // Create WiFi screen from XML
    wifi_screen_root = (lv_obj_t*)lv_xml_create(parent, "wizard_wifi_setup", nullptr);

    if (!wifi_screen_root) {
        spdlog::error("[WiFi Screen] Failed to create wizard_wifi_setup from XML");
        return nullptr;
    }

    // Find password modal (hidden by default in XML)
    password_modal = lv_obj_find_by_name(wifi_screen_root, "wifi_password_modal");
    if (!password_modal) {
        spdlog::warn("[WiFi Screen] Password modal not found in XML");
    }

    // Find network list container
    network_list_container = lv_obj_find_by_name(wifi_screen_root, "network_list_container");
    if (!network_list_container) {
        spdlog::error("[WiFi Screen] Network list container not found in XML");
        return nullptr;
    }

    // Update layout to ensure SIZE_CONTENT calculates correctly
    lv_obj_update_layout(wifi_screen_root);

    spdlog::info("[WiFi Screen] WiFi screen created successfully");
    return wifi_screen_root;
}

void ui_wizard_wifi_init_wifi_manager() {
    spdlog::debug("[WiFi Screen] Initializing WiFi and Ethernet managers");

    // Create WiFiManager instance as shared_ptr for async callback safety
    wifi_manager = std::make_shared<WiFiManager>();

    // Initialize self-reference for safe async callback handling
    wifi_manager->init_self_reference(wifi_manager);

    // Create EthernetManager instance
    ethernet_manager = std::make_unique<EthernetManager>();

    // Update ethernet status
    update_ethernet_status();

    spdlog::info("[WiFi Screen] WiFi and Ethernet managers initialized");
}

void ui_wizard_wifi_cleanup() {
    spdlog::debug("[WiFi Screen] Cleaning up WiFi screen");

    // Stop WiFi scanning and disable if enabled
    if (wifi_manager) {
        spdlog::debug("[WiFi Screen] Stopping scan and disabling WiFi");
        wifi_manager->stop_scan();
        if (lv_subject_get_int(&wifi_enabled) == 1) {
            wifi_manager->set_enabled(false);
        }
    }

    // Clear network list BEFORE destroying wifi_manager
    // This ensures network data is freed while manager still exists
    spdlog::debug("[WiFi Screen] Clearing network list");
    clear_network_list();

    // Destroy WiFi manager (weak_ptr in async callbacks safely handles this)
    spdlog::debug("[WiFi Screen] Destroying WiFi manager");
    wifi_manager.reset();

    // Destroy Ethernet manager
    spdlog::debug("[WiFi Screen] Destroying Ethernet manager");
    ethernet_manager.reset();

    // Reset UI references
    wifi_screen_root = nullptr;
    password_modal = nullptr;
    network_list_container = nullptr;
    current_ssid[0] = '\0';
    current_secured = false;

    spdlog::info("[WiFi Screen] Cleanup complete");
}

void ui_wizard_wifi_show_password_modal(const char* ssid) {
    if (!password_modal) {
        spdlog::error("[WiFi Screen] Cannot show password modal: modal not found");
        return;
    }

    if (!ssid) {
        spdlog::error("[WiFi Screen] Cannot show password modal: null SSID");
        return;
    }

    spdlog::debug("[WiFi Screen] Showing password modal for SSID: {}", ssid);

    // Update modal title with SSID
    lv_obj_t* modal_ssid_label = lv_obj_find_by_name(password_modal, "modal_ssid");
    if (modal_ssid_label) {
        lv_label_set_text(modal_ssid_label, ssid);
    }

    // Clear password input
    lv_obj_t* password_input = lv_obj_find_by_name(password_modal, "password_input");
    if (password_input) {
        lv_textarea_set_text(password_input, "");
        // Register textarea with keyboard for input
        ui_keyboard_register_textarea(password_input);
    }

    // Hide status message
    lv_obj_t* modal_status = lv_obj_find_by_name(password_modal, "modal_status");
    if (modal_status) {
        lv_obj_add_flag(modal_status, LV_OBJ_FLAG_HIDDEN);
    }

    // Show modal
    lv_obj_remove_flag(password_modal, LV_OBJ_FLAG_HIDDEN);
}

void ui_wizard_wifi_hide_password_modal() {
    if (!password_modal) {
        return;
    }

    spdlog::debug("[WiFi Screen] Hiding password modal");
    lv_obj_add_flag(password_modal, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_wifi_toggle_changed(lv_event_t* e) {
    lv_obj_t* toggle = (lv_obj_t*)lv_event_get_target(e);
    if (!toggle) {
        return;
    }

    // Get toggle state (ui_switch stores checked state)
    bool checked = lv_obj_get_state(toggle) & LV_STATE_CHECKED;

    spdlog::debug("[WiFi Screen] WiFi toggle changed: {}", checked ? "ON" : "OFF");

    // Update wifi_enabled subject
    lv_subject_set_int(&wifi_enabled, checked ? 1 : 0);

    if (checked) {
        // Enable WiFi
        update_wifi_status("Enabling WiFi...");

        if (wifi_manager) {
            wifi_manager->set_enabled(true);
            update_wifi_status("WiFi Enabled");

            // Show scanning indicator
            lv_subject_set_int(&wifi_scanning, 1);

            // Start network scanning
            spdlog::debug("[WiFi Screen] About to call start_scan with callback");
            wifi_manager->start_scan([](const std::vector<WiFiNetwork>& networks) {
                spdlog::info("[WiFi Screen] *** SCAN CALLBACK INVOKED with {} networks ***", networks.size());

                // Hide scanning indicator
                lv_subject_set_int(&wifi_scanning, 0);

                // Populate network list
                populate_network_list(networks);
            });
            spdlog::debug("[WiFi Screen] start_scan call completed");
        } else {
            spdlog::error("[WiFi Screen] WiFi manager not initialized");
            update_wifi_status("WiFi Error");
        }
    } else {
        // Disable WiFi
        update_wifi_status("WiFi Disabled");
        lv_subject_set_int(&wifi_scanning, 0);  // Stop scanning indicator
        clear_network_list();

        if (wifi_manager) {
            wifi_manager->stop_scan();
            wifi_manager->set_enabled(false);
        }
    }
}

static void on_network_item_clicked(lv_event_t* e) {
    lv_obj_t* item = (lv_obj_t*)lv_event_get_target(e);
    if (!item) {
        return;
    }

    // Get network item data from user_data
    NetworkItemData* item_data = (NetworkItemData*)lv_obj_get_user_data(item);
    if (!item_data) {
        spdlog::error("[WiFi Screen] No network data found in clicked item");
        return;
    }

    const WiFiNetwork& network = item_data->network;
    spdlog::debug("[WiFi Screen] Network clicked: {} ({}%)", network.ssid, network.signal_strength);

    // Store current network selection
    strncpy(current_ssid, network.ssid.c_str(), sizeof(current_ssid) - 1);
    current_ssid[sizeof(current_ssid) - 1] = '\0';
    current_secured = network.is_secured;

    // Update WiFi status
    char status_buf[128];
    snprintf(status_buf, sizeof(status_buf), "Connecting to %s...", network.ssid.c_str());
    update_wifi_status(status_buf);

    if (network.is_secured) {
        // Show password modal for secured networks
        ui_wizard_wifi_show_password_modal(network.ssid.c_str());
    } else {
        // Connect directly to open network
        if (wifi_manager) {
            wifi_manager->connect(network.ssid, "", [](bool success, const std::string& error) {
                if (success) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Connected to %s", current_ssid);
                    update_wifi_status(msg);
                    spdlog::info("[WiFi Screen] Connected to {}", current_ssid);
                } else {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Failed to connect: %s", error.c_str());
                    update_wifi_status(msg);
                    spdlog::error("[WiFi Screen] Connection failed: {}", error);
                }
            });
        } else {
            spdlog::error("[WiFi Screen] WiFi manager not initialized");
            update_wifi_status("WiFi Error");
        }
    }
}

static void on_modal_cancel_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[WiFi Screen] Password modal cancel clicked");
    ui_wizard_wifi_hide_password_modal();
}

static void on_modal_connect_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[WiFi Screen] Password modal connect clicked");

    if (!password_modal) {
        spdlog::error("[WiFi Screen] Password modal not found");
        return;
    }

    // Get password from textarea
    lv_obj_t* password_input = lv_obj_find_by_name(password_modal, "password_input");
    if (!password_input) {
        spdlog::error("[WiFi Screen] Password input not found in modal");
        return;
    }

    const char* password = lv_textarea_get_text(password_input);
    if (!password || strlen(password) == 0) {
        // Show error in modal
        lv_obj_t* modal_status = lv_obj_find_by_name(password_modal, "modal_status");
        if (modal_status) {
            lv_label_set_text(modal_status, "Password cannot be empty");
            lv_obj_remove_flag(modal_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    spdlog::debug("[WiFi Screen] Attempting to connect to {} with password", current_ssid);

    // Update WiFi status
    char status_buf[128];
    snprintf(status_buf, sizeof(status_buf), "Connecting to %s...", current_ssid);
    update_wifi_status(status_buf);

    // Attempt connection
    if (wifi_manager) {
        wifi_manager->connect(current_ssid, password, [](bool success, const std::string& error) {
            if (success) {
                // Connection successful - hide modal and update status
                ui_wizard_wifi_hide_password_modal();

                char msg[128];
                snprintf(msg, sizeof(msg), "Connected to %s", current_ssid);
                update_wifi_status(msg);
                spdlog::info("[WiFi Screen] Connected to {}", current_ssid);
            } else {
                // Connection failed - show error in modal
                lv_obj_t* modal_status = lv_obj_find_by_name(password_modal, "modal_status");
                if (modal_status) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg), "Connection failed: %s", error.c_str());
                    lv_label_set_text(modal_status, error_msg);
                    lv_obj_remove_flag(modal_status, LV_OBJ_FLAG_HIDDEN);
                }

                update_wifi_status("Connection failed");
                spdlog::error("[WiFi Screen] Connection failed: {}", error);
            }
        });
    } else {
        spdlog::error("[WiFi Screen] WiFi manager not initialized");
        update_wifi_status("WiFi Error");
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

static void update_wifi_status(const char* status) {
    if (!status) {
        return;
    }

    spdlog::debug("[WiFi Screen] Updating WiFi status: {}", status);
    lv_subject_copy_string(&wifi_status, status);
}

static void update_ethernet_status() {
    if (!ethernet_manager) {
        spdlog::warn("[WiFi Screen] Ethernet manager not initialized");
        lv_subject_copy_string(&ethernet_status, "Unknown");
        return;
    }

    EthernetInfo info = ethernet_manager->get_info();

    if (info.connected) {
        // Format: "Connected (192.168.1.100)"
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf), "Connected (%s)", info.ip_address.c_str());
        lv_subject_copy_string(&ethernet_status, status_buf);
        spdlog::debug("[WiFi Screen] Ethernet status: {}", status_buf);
    } else {
        // Show status message (e.g., "No cable", "No connection", etc.)
        lv_subject_copy_string(&ethernet_status, info.status.c_str());
        spdlog::debug("[WiFi Screen] Ethernet status: {}", info.status);
    }
}

static void populate_network_list(const std::vector<WiFiNetwork>& networks) {
    spdlog::debug("[WiFi Screen] Populating network list with {} networks", networks.size());

    if (!network_list_container) {
        spdlog::error("[WiFi Screen] Network list container not found");
        return;
    }

    // Register wifi_network_item component if not already registered
    static bool component_registered = false;
    if (!component_registered) {
        lv_xml_register_component_from_file("A:ui_xml/wifi_network_item.xml");
        component_registered = true;
    }

    // Clear existing network items
    clear_network_list();

    // Sort networks by signal strength (descending)
    std::vector<WiFiNetwork> sorted_networks = networks;
    std::sort(sorted_networks.begin(), sorted_networks.end(),
              [](const WiFiNetwork& a, const WiFiNetwork& b) {
                  return a.signal_strength > b.signal_strength;
              });

    // Create network items
    for (const auto& network : sorted_networks) {
        // Create network item from component
        lv_obj_t* item = (lv_obj_t*)lv_xml_create(network_list_container, "wifi_network_item", nullptr);
        if (!item) {
            spdlog::error("[WiFi Screen] Failed to create network item for SSID: {}", network.ssid);
            continue;
        }

        // Set unique name for tracking (network_item_0, network_item_1, etc.)
        static int item_counter = 0;
        char item_name[32];
        snprintf(item_name, sizeof(item_name), "network_item_%d", item_counter++);
        lv_obj_set_name(item, item_name);

        // Create per-instance subjects and bind to widgets (reactive approach)
        NetworkItemData* item_data = new NetworkItemData(network);

        // Find widgets within the component
        lv_obj_t* ssid_label = lv_obj_find_by_name(item, "ssid_label");
        lv_obj_t* security_label = lv_obj_find_by_name(item, "security_label");
        lv_obj_t* lock_icon = lv_obj_find_by_name(item, "lock_icon");
        lv_obj_t* signal_icon = lv_obj_find_by_name(item, "signal_icon");

        // Bind SSID label to subject
        if (ssid_label) {
            lv_label_bind_text(ssid_label, item_data->ssid, nullptr);
        }

        // Set security type text directly (not reactive - rarely changes)
        if (security_label) {
            lv_label_set_text(security_label, network.security_type.c_str());
        }

        // Bind lock icon visibility to is_secured subject
        // Hidden when is_secured == 0 (open network)
        if (lock_icon) {
            lv_obj_bind_flag_if_eq(lock_icon, item_data->is_secured, LV_OBJ_FLAG_HIDDEN, 0);
        }

        // Signal icon color based on strength (theme handles colors)
        // Could be implemented with state changes if color-coding is needed
        (void)signal_icon;  // Suppress unused warning

        // Store NetworkItemData in user_data for click handler and cleanup
        lv_obj_set_user_data(item, item_data);

        // Register click event
        lv_obj_add_event_cb(item, on_network_item_clicked, LV_EVENT_CLICKED, nullptr);

        spdlog::debug("[WiFi Screen] Added network: {} ({}%, {})",
                     network.ssid, network.signal_strength,
                     network.is_secured ? "secured" : "open");
    }

    spdlog::info("[WiFi Screen] Populated {} network items", sorted_networks.size());
}

static void clear_network_list() {
    if (!network_list_container) {
        spdlog::debug("[WiFi Screen] clear_network_list: container is NULL, skipping");
        return;
    }

    spdlog::debug("[WiFi Screen] Clearing network list");

    // Find all network items and delete them
    // Keep title and placeholder (they're part of the XML template)
    int32_t child_count = lv_obj_get_child_count(network_list_container);
    spdlog::debug("[WiFi Screen] Network list has {} children", child_count);

    for (int32_t i = child_count - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(network_list_container, i);
        if (!child) {
            spdlog::warn("[WiFi Screen] Child at index {} is NULL", i);
            continue;
        }

        const char* name = lv_obj_get_name(child);

        // Only delete network items, keep title and placeholder
        if (name && strncmp(name, "network_item_", 13) == 0) {
            spdlog::debug("[WiFi Screen] Deleting network item: {}", name);

            // Get user data before deleting widget
            NetworkItemData* item_data = (NetworkItemData*)lv_obj_get_user_data(child);

            // Delete the widget FIRST (this removes event handlers and subject bindings)
            lv_obj_delete(child);

            // Now safe to free NetworkItemData (subjects no longer referenced by LVGL)
            if (item_data) {
                delete item_data;  // Destructor cleans up subjects
            }
        }
    }

    spdlog::debug("[WiFi Screen] Network list cleared");
}
