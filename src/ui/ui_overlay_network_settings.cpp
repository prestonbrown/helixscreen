// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_network_settings.h"

#include "ui_callback_helpers.h"
#include "ui_effects.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_step_progress.h"
#include "ui_subject_registry.h"
#include "ui_timer_guard.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "config.h"
#include "data_root_resolver.h"
#include "ethernet_manager.h"
#include "log_redact.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "network_tester.h"
#include "static_panel_registry.h"
#include "system_settings_manager.h"
#include "wifi_manager.h"
#include "wifi_radio_toggle.h"
#include "wifi_ui_utils.h"

#include <lvgl/lvgl.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace helix;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<NetworkSettingsOverlay> g_network_settings_overlay;

NetworkSettingsOverlay& get_network_settings_overlay() {
    if (!g_network_settings_overlay) {
        g_network_settings_overlay = std::make_unique<NetworkSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "NetworkSettingsOverlay", []() { g_network_settings_overlay.reset(); });
    }
    return *g_network_settings_overlay;
}

// ============================================================================
// Helper Types
// ============================================================================

/**
 * @brief Per-instance network item data for click handling
 *
 * Also owns the per-row subjects that drive the band badge: a list row is built
 * from scan data, so there is no global subject to bind to and the row must
 * carry its own (same pattern as WifiWizardNetworkItemData).
 *
 * @note Named distinctly to avoid ODR conflicts with WifiWizardNetworkSettingsItemData
 */
struct NetworkSettingsItemData {
    std::string ssid;
    bool is_secured;
    char band_buffer[16];
    lv_subject_t band_text;    ///< String subject bound to band_label's text
    lv_subject_t band_visible; ///< 1 when the badge should be shown, else 0

    NetworkSettingsItemData(const std::string& ssid_, bool secured, const std::string& band)
        : ssid(ssid_), is_secured(secured) {
        std::snprintf(band_buffer, sizeof(band_buffer), "%s", band.c_str());
        lv_subject_init_string(&band_text, band_buffer, nullptr, sizeof(band_buffer), band_buffer);
        lv_subject_init_int(&band_visible, band.empty() ? 0 : 1);
    }

    ~NetworkSettingsItemData() {
        lv_subject_deinit(&band_text);
        lv_subject_deinit(&band_visible);
    }
};

/**
 * @brief DELETE event handler for network list items
 * Automatically frees NetworkSettingsItemData when widget is deleted
 */
static void network_item_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    NetworkSettingsItemData* data =
        static_cast<NetworkSettingsItemData*>(lv_obj_get_user_data(obj));
    if (data) {
        // Use unique_ptr for RAII cleanup
        std::unique_ptr<NetworkSettingsItemData> auto_delete(data);
        lv_obj_set_user_data(obj, nullptr);
        // data automatically freed when unique_ptr goes out of scope
    }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

NetworkSettingsOverlay::NetworkSettingsOverlay() {
    std::memset(ssid_buffer_, 0, sizeof(ssid_buffer_));
    std::memset(ip_buffer_, 0, sizeof(ip_buffer_));
    std::memset(mac_buffer_, 0, sizeof(mac_buffer_));
    std::memset(count_buffer_, 0, sizeof(count_buffer_));
    std::memset(eth_ip_buffer_, 0, sizeof(eth_ip_buffer_));
    std::memset(eth_mac_buffer_, 0, sizeof(eth_mac_buffer_));
    std::memset(current_ssid_, 0, sizeof(current_ssid_));
    std::memset(password_modal_ssid_buffer_, 0, sizeof(password_modal_ssid_buffer_));

    spdlog::debug("[NetworkSettingsOverlay] Instance created");
}

NetworkSettingsOverlay::~NetworkSettingsOverlay() {
    // cleanup() cancels this too, but a teardown that skips cleanup() would
    // otherwise leave the backstop armed on a freed `this` (#1173).
    // lv_timer_cancel_safe() self-guards on lv_is_initialized().
    cancel_wlan_toggle_backstop();

    // Clean up managers FIRST - they have background threads
    // NOTE: wifi_manager_ is the global singleton - do NOT reset it
    ethernet_manager_.reset();
    network_tester_.reset();

    // Modal dialogs: use helix::ui::modal_hide() - NOT lv_obj_del()!
    if (lv_is_initialized()) {
        // StaticPanelRegistry::destroy_all() runs before lv_deinit(), and
        // modal_hide() below only starts an exit ANIMATION - the modal tree is
        // deleted after this destructor has returned and freed `this`. Uninstall
        // the DELETE hooks while the object is still valid; on_modal_deleted()
        // writes through the pointer it is handed. A cached pointer the hook has
        // already nulled means that tree died first, so nothing is left to
        // uninstall.
        for (lv_obj_t* watched :
             {password_modal_, hidden_network_modal_, test_modal_, step_widget_}) {
            if (watched) {
                lv_obj_remove_event_cb_with_user_data(watched, on_modal_deleted, this);
            }
        }

        if (hidden_network_modal_) {
            helix::ui::modal_hide(hidden_network_modal_);
            hidden_network_modal_ = nullptr;
        }
        if (test_modal_) {
            helix::ui::modal_hide(test_modal_);
            test_modal_ = nullptr;
            step_widget_ = nullptr;
        }
        if (password_modal_) {
            helix::ui::modal_hide(password_modal_);
            password_modal_ = nullptr;
        }
    }

    // Deinitialize subjects to disconnect observers
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Clear widget pointers (owned by LVGL)
    overlay_root_ = nullptr;
    parent_screen_ = nullptr;
    networks_list_ = nullptr;
    // NOTE: Do NOT use spdlog here - it may be destroyed during exit
}

// ============================================================================
// Subject Initialization
// ============================================================================

void NetworkSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[NetworkSettingsOverlay] Subjects already initialized");
        return;
    }

    spdlog::debug("[NetworkSettingsOverlay] Initializing subjects");

    // WiFi subjects
    UI_MANAGED_SUBJECT_INT(wifi_hardware_available_, 1, "wifi_hardware_available", subjects_);
    UI_MANAGED_SUBJECT_INT(wifi_enabled_, 0, "wifi_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(wifi_connected_, 0, "wifi_connected", subjects_);
    UI_MANAGED_SUBJECT_INT(wifi_only_24ghz_, 1, "wifi_only_24ghz",
                           subjects_); // Default: assume 2.4GHz only
    UI_MANAGED_SUBJECT_INT(wifi_scanning_, 0, "wifi_scanning", subjects_);

    // WiFi string subjects
    UI_MANAGED_SUBJECT_STRING(connected_ssid_, ssid_buffer_, "", "connected_ssid", subjects_);
    UI_MANAGED_SUBJECT_STRING(ip_address_, ip_buffer_, "", "ip_address", subjects_);
    UI_MANAGED_SUBJECT_STRING(mac_address_, mac_buffer_, "", "mac_address", subjects_);
    UI_MANAGED_SUBJECT_STRING(network_count_, count_buffer_, "(0)", "network_count", subjects_);

    // Ethernet subjects
    UI_MANAGED_SUBJECT_INT(eth_connected_, 0, "eth_connected", subjects_);
    UI_MANAGED_SUBJECT_STRING(eth_ip_address_, eth_ip_buffer_, "", "eth_ip_address", subjects_);
    UI_MANAGED_SUBJECT_STRING(eth_mac_address_, eth_mac_buffer_, "", "eth_mac_address", subjects_);

    // Network test subjects
    UI_MANAGED_SUBJECT_INT(any_network_connected_, 0, "any_network_connected", subjects_);
    UI_MANAGED_SUBJECT_INT(test_running_, 0, "test_running", subjects_);
    UI_MANAGED_SUBJECT_INT(test_gateway_status_, 0, "test_gateway_status", subjects_);
    UI_MANAGED_SUBJECT_INT(test_internet_status_, 0, "test_internet_status", subjects_);

    // Network test modal subject (controls close button enabled state)
    UI_MANAGED_SUBJECT_INT(test_complete_, 0, "test_complete", subjects_);

    // Password modal subjects
    UI_MANAGED_SUBJECT_INT(wifi_connecting_, 0, "wifi_connecting", subjects_);
    UI_MANAGED_SUBJECT_STRING(wifi_password_modal_ssid_, password_modal_ssid_buffer_, "",
                              "wifi_password_modal_ssid", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[NetworkSettingsOverlay] Subjects initialized");
}

// ============================================================================
// Callback Registration
// ============================================================================

void NetworkSettingsOverlay::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[NetworkSettingsOverlay] Callbacks already registered");
        return;
    }

    spdlog::debug("[NetworkSettingsOverlay] Registering event callbacks");

    register_xml_callbacks({
        {"on_wlan_toggle_changed", on_wlan_toggle_changed},
        {"on_refresh_clicked", on_refresh_clicked},
        {"on_test_network_clicked", on_test_network_clicked},
        {"on_add_other_clicked", on_add_other_clicked},
        {"on_network_item_clicked", on_network_item_clicked},
        {"on_network_settings_forget", on_network_settings_forget},
        // Network test modal
        {"on_network_test_close", on_network_test_close},
        // Hidden network modal
        {"on_hidden_cancel_clicked", on_hidden_cancel_clicked},
        {"on_hidden_connect_clicked", on_hidden_connect_clicked},
        {"on_security_changed", on_security_changed},
        // Password modal
        {"on_wifi_password_cancel", on_wifi_password_cancel},
        {"on_wifi_password_connect", on_wifi_password_connect},
    });

    callbacks_registered_ = true;
    spdlog::debug("[NetworkSettingsOverlay] Event callbacks registered");
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* NetworkSettingsOverlay::create(lv_obj_t* parent_screen) {
    if (!parent_screen) {
        spdlog::error("[NetworkSettingsOverlay] Cannot create: null parent_screen");
        return nullptr;
    }

    spdlog::debug("[NetworkSettingsOverlay] Creating overlay from XML");

    parent_screen_ = parent_screen;

    // Reset cleanup flag when (re)creating
    cleanup_called_ = false;

    // Register wifi_network_item component first
    static bool network_item_registered = false;
    if (!network_item_registered) {
        lv_xml_register_component_from_file(
            helix::asset_component_uri("ui_xml/wifi_network_item.xml").c_str());
        network_item_registered = true;
        spdlog::debug("[NetworkSettingsOverlay] Registered wifi_network_item component");
    }

    // Create overlay from XML
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen, "network_settings_overlay", nullptr));

    if (!overlay_root_) {
        spdlog::error("[NetworkSettingsOverlay] Failed to create from XML");
        return nullptr;
    }

    // Get reference to networks_list for population
    networks_list_ = lv_obj_find_by_name(overlay_root_, "networks_list");
    if (!networks_list_) {
        spdlog::error("[NetworkSettingsOverlay] networks_list not found in XML");
        return nullptr;
    }

    // Note: Back button is wired via header_bar.xml default callback (on_header_back_clicked)

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Initialize WiFi manager - use global singleton
    if (!wifi_manager_) {
        wifi_manager_ = get_wifi_manager(); // Uses global singleton (already has self-reference)
        spdlog::debug("[NetworkSettingsOverlay] WiFiManager obtained from global singleton");

        // Check WiFi hardware availability and update subject
        bool hw_available = wifi_manager_->has_hardware();
        lv_subject_set_int(&wifi_hardware_available_, hw_available ? 1 : 0);
        if (!hw_available) {
            spdlog::info(
                "[NetworkSettingsOverlay] WiFi hardware not available - controls disabled");
        }
    }

    // Initialize Ethernet manager
    if (!ethernet_manager_) {
        ethernet_manager_ = std::make_unique<EthernetManager>();
        spdlog::debug("[NetworkSettingsOverlay] EthernetManager initialized");
    }

    // Initialize NetworkTester
    if (!network_tester_) {
        network_tester_ = std::make_shared<NetworkTester>();
        network_tester_->init_self_reference(network_tester_);
        spdlog::debug("[NetworkSettingsOverlay] NetworkTester initialized");
    }

    // Update initial connection status
    update_wifi_status();
    update_ethernet_status();
    update_any_network_connected();

    spdlog::info("[NetworkSettingsOverlay] Overlay created successfully");
    return overlay_root_;
}

// ============================================================================
// Show/Hide
// ============================================================================

void NetworkSettingsOverlay::show() {
    if (!overlay_root_) {
        spdlog::error("[NetworkSettingsOverlay] Cannot show: overlay not created");
        return;
    }

    spdlog::debug("[NetworkSettingsOverlay] Showing overlay");

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);

    spdlog::info("[NetworkSettingsOverlay] Overlay shown");
}

void NetworkSettingsOverlay::hide() {
    if (!overlay_root_) {
        return;
    }

    spdlog::debug("[NetworkSettingsOverlay] Hiding overlay");

    // Pop from navigation stack - on_deactivate() will be called by NavigationManager
    NavigationManager::instance().go_back();

    spdlog::info("[NetworkSettingsOverlay] Overlay hidden");
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void NetworkSettingsOverlay::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[NetworkSettingsOverlay] on_activate()");

    // Update connection status
    update_wifi_status();
    update_ethernet_status();
    update_any_network_connected();

    // Update band capability indicator (show "Only 2.4GHz" if 5GHz not supported)
    if (wifi_manager_) {
        bool only_24ghz = !wifi_manager_->supports_5ghz();
        lv_subject_set_int(&wifi_only_24ghz_, only_24ghz ? 1 : 0);
        spdlog::debug("[NetworkSettingsOverlay] WiFi band capability: {}",
                      only_24ghz ? "2.4GHz only" : "2.4GHz + 5GHz");
    }

    // Start scanning if WiFi enabled
    if (wifi_manager_ && wifi_manager_->is_enabled()) {
        lv_subject_set_int(&wifi_scanning_, 1);

        std::weak_ptr<WiFiManager> weak_mgr = wifi_manager_;
        auto token = lifetime_.token();

        wifi_manager_->start_scan(
            [this, token, weak_mgr](const std::vector<WiFiNetwork>& networks) {
                if (token.expired() || weak_mgr.expired())
                    return;
                token.defer([this, networks]() {
                    lv_subject_set_int(&wifi_scanning_, 0);
                    populate_network_list(networks);
                });
            });
    }
}

void NetworkSettingsOverlay::on_deactivate() {
    spdlog::debug("[NetworkSettingsOverlay] on_deactivate()");

    // Stop scanning
    if (wifi_manager_) {
        wifi_manager_->stop_scan();
        lv_subject_set_int(&wifi_scanning_, 0);
    }

    // Cancel any running tests
    if (network_tester_ && network_tester_->is_running()) {
        network_tester_->cancel();
        lv_subject_set_int(&test_running_, 0);
    }

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// Cleanup
// ============================================================================

void NetworkSettingsOverlay::cleanup() {
    spdlog::debug("[NetworkSettingsOverlay] Cleaning up");

    // Unregister from NavigationManager before cleaning up
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Call base class to set cleanup_called_ flag
    OverlayBase::cleanup();

    cancel_wlan_toggle_backstop();

    if (wifi_manager_) {
        wifi_manager_->stop_scan();
    }

    if (network_tester_ && network_tester_->is_running()) {
        network_tester_->cancel();
    }

    clear_network_list();

    // NOTE: wifi_manager_ is the global singleton - do NOT reset it
    ethernet_manager_.reset();
    network_tester_.reset();

    parent_screen_ = nullptr;
    networks_list_ = nullptr;

    current_ssid_[0] = '\0';
    current_network_is_secured_ = false;

    spdlog::debug("[NetworkSettingsOverlay] Cleanup complete");
}

// ============================================================================
// Helper Functions
// ============================================================================

void NetworkSettingsOverlay::update_wifi_status() {
    if (!wifi_manager_) {
        spdlog::debug("[NetworkSettingsOverlay] Cannot update WiFi status: no WiFiManager");
        return;
    }

    bool enabled = wifi_manager_->is_enabled();
    bool connected = wifi_manager_->is_connected();

    lv_subject_set_int(&wifi_enabled_, enabled ? 1 : 0);
    lv_subject_set_int(&wifi_connected_, connected ? 1 : 0);

    if (connected) {
        std::string ssid = wifi_manager_->get_connected_ssid();
        std::string ip = wifi_manager_->get_ip_address();
        std::string mac = wifi_manager_->get_mac_address();

        strncpy(ssid_buffer_, ssid.c_str(), sizeof(ssid_buffer_) - 1);
        ssid_buffer_[sizeof(ssid_buffer_) - 1] = '\0';
        lv_subject_notify(&connected_ssid_);

        strncpy(ip_buffer_, ip.c_str(), sizeof(ip_buffer_) - 1);
        ip_buffer_[sizeof(ip_buffer_) - 1] = '\0';
        lv_subject_notify(&ip_address_);

        strncpy(mac_buffer_, mac.c_str(), sizeof(mac_buffer_) - 1);
        mac_buffer_[sizeof(mac_buffer_) - 1] = '\0';
        lv_subject_notify(&mac_address_);

        spdlog::debug("[NetworkSettingsOverlay] WiFi connected: {} ({})", helix::redact::ssid(ssid),
                      ip);
    } else {
        ssid_buffer_[0] = '\0';
        ip_buffer_[0] = '\0';
        mac_buffer_[0] = '\0';
        lv_subject_notify(&connected_ssid_);
        lv_subject_notify(&ip_address_);
        lv_subject_notify(&mac_address_);
    }
}

void NetworkSettingsOverlay::update_ethernet_status() {
    if (!ethernet_manager_) {
        spdlog::debug("[NetworkSettingsOverlay] Cannot update Ethernet status: no EthernetManager");
        return;
    }

    // Kick off an async probe — the callback fires on the UI thread via
    // tok.defer() so it's safe to touch subjects there. No bare expired()
    // check on the bg thread: defer's own guard skips when the owner is
    // gone, and dropping the bare check silences the bg_tok_expired_check
    // detector for this site (3XNZQB2R audit).
    auto tok = lifetime_.token();
    ethernet_manager_->get_info_async([this, tok](const EthernetInfo& info) {
        tok.defer("NetworkSettingsOverlay::apply_ethernet_status",
                  [this, info]() { apply_ethernet_status(info); });
    });
}

void NetworkSettingsOverlay::apply_ethernet_status(const EthernetInfo& info) {
    lv_subject_set_int(&eth_connected_, info.connected ? 1 : 0);

    if (info.connected) {
        strncpy(eth_ip_buffer_, info.ip_address.c_str(), sizeof(eth_ip_buffer_) - 1);
        eth_ip_buffer_[sizeof(eth_ip_buffer_) - 1] = '\0';
        lv_subject_notify(&eth_ip_address_);

        strncpy(eth_mac_buffer_, info.mac_address.c_str(), sizeof(eth_mac_buffer_) - 1);
        eth_mac_buffer_[sizeof(eth_mac_buffer_) - 1] = '\0';
        lv_subject_notify(&eth_mac_address_);

        spdlog::debug("[NetworkSettingsOverlay] Ethernet connected: {}", info.ip_address);
    } else {
        eth_ip_buffer_[0] = '\0';
        eth_mac_buffer_[0] = '\0';
        lv_subject_notify(&eth_ip_address_);
        lv_subject_notify(&eth_mac_address_);
        spdlog::debug("[NetworkSettingsOverlay] Ethernet not connected: {}", info.status);
    }
    update_any_network_connected();
}

void NetworkSettingsOverlay::update_any_network_connected() {
    bool wifi_conn = (lv_subject_get_int(&wifi_connected_) == 1);
    bool eth_conn = (lv_subject_get_int(&eth_connected_) == 1);
    lv_subject_set_int(&any_network_connected_, (wifi_conn || eth_conn) ? 1 : 0);
}

void NetworkSettingsOverlay::update_test_state(NetworkTester::TestState state,
                                               const NetworkTester::TestResult& result) {
    spdlog::debug("[NetworkSettingsOverlay] Test state: {}", static_cast<int>(state));

    switch (state) {
    case NetworkTester::TestState::IDLE:
        lv_subject_set_int(&test_running_, 0);
        lv_subject_set_int(&test_gateway_status_, 0);
        lv_subject_set_int(&test_internet_status_, 0);
        break;

    case NetworkTester::TestState::TESTING_GATEWAY:
        lv_subject_set_int(&test_running_, 1);
        lv_subject_set_int(&test_gateway_status_, 1);  // active
        lv_subject_set_int(&test_internet_status_, 0); // pending
        break;

    case NetworkTester::TestState::TESTING_INTERNET:
        lv_subject_set_int(&test_running_, 1);
        lv_subject_set_int(&test_gateway_status_, result.gateway_ok ? 2 : 3); // success/failed
        lv_subject_set_int(&test_internet_status_, 1);                        // active
        break;

    case NetworkTester::TestState::COMPLETED:
        lv_subject_set_int(&test_running_, 0);
        lv_subject_set_int(&test_gateway_status_, result.gateway_ok ? 2 : 3);
        lv_subject_set_int(&test_internet_status_, result.internet_ok ? 2 : 3);
        spdlog::info("[NetworkSettingsOverlay] Test complete - Gateway: {}, Internet: {}",
                     result.gateway_ok ? "OK" : "FAIL", result.internet_ok ? "OK" : "FAIL");
        break;

    case NetworkTester::TestState::FAILED:
        lv_subject_set_int(&test_running_, 0);
        lv_subject_set_int(&test_gateway_status_, 3);  // failed
        lv_subject_set_int(&test_internet_status_, 3); // failed
        spdlog::warn("[NetworkSettingsOverlay] Test failed: {}", result.error_message);
        break;
    }
}

void NetworkSettingsOverlay::populate_network_list(const std::vector<WiFiNetwork>& networks) {
    if (!networks_list_) {
        spdlog::error("[NetworkSettingsOverlay] Cannot populate: networks_list is null");
        return;
    }

    spdlog::debug("[NetworkSettingsOverlay] Populating network list with {} networks",
                  networks.size());

    // Save scroll position before clearing
    int32_t scroll_y = lv_obj_get_scroll_y(networks_list_);

    clear_network_list();

    // Update count
    snprintf(count_buffer_, sizeof(count_buffer_), "(%zu)", networks.size());
    lv_subject_notify(&network_count_);

    // Show/hide placeholder
    show_placeholder(networks.empty());

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
    }

    // Band badges only earn their pixels when the scan actually spans bands —
    // on a 2.4GHz-only radio every row would read "2.4G" (helixscreen#1189).
    const bool show_bands = helix::ui::wifi::wifi_scan_spans_multiple_bands(sorted_networks);

    // Create network items
    static int item_counter = 0;
    for (const auto& network : sorted_networks) {
        lv_obj_t* item =
            static_cast<lv_obj_t*>(lv_xml_create(networks_list_, "wifi_network_item", nullptr));
        if (!item) {
            spdlog::error("[NetworkSettingsOverlay] Failed to create network item for SSID: {}",
                          helix::redact::ssid(network.ssid));
            continue;
        }

        char item_name[32];
        snprintf(item_name, sizeof(item_name), "network_item_%d", item_counter++);
        lv_obj_set_name(item, item_name);

        // Set SSID
        lv_obj_t* ssid_label = lv_obj_find_by_name(item, "ssid_label");
        if (ssid_label) {
            lv_label_set_text(ssid_label, network.ssid.c_str());
        }

        // Set security label
        lv_obj_t* security_label = lv_obj_find_by_name(item, "security_label");
        if (security_label) {
            if (network.is_secured) {
                lv_label_set_text(security_label, network.security_type.c_str());
            } else {
                lv_label_set_text(security_label, "");
            }
        }

        // Update signal icons
        int icon_state = helix::ui::wifi::wifi_compute_signal_icon_state(network.signal_strength,
                                                                         network.is_secured);
        update_signal_icons(item, icon_state);

        // Mark connected network with LV_STATE_CHECKED
        bool is_connected = (!connected_ssid.empty() && network.ssid == connected_ssid);
        if (is_connected) {
            lv_obj_add_state(item, LV_STATE_CHECKED);
            spdlog::debug("[NetworkSettingsOverlay] Marked connected network: {}",
                          helix::redact::ssid(network.ssid));
        }

        // Store network data for click handler; also owns the band subjects
        std::string band_text =
            show_bands ? helix::ui::wifi::wifi_format_band_label(network.band_mask) : std::string();
        auto* data = new NetworkSettingsItemData(network.ssid, network.is_secured, band_text);
        lv_obj_set_user_data(item, data);

        // Bind the band badge to this row's own subjects
        lv_obj_t* band_label = lv_obj_find_by_name(item, "band_label");
        if (band_label) {
            lv_label_bind_text(band_label, &data->band_text, nullptr);
            lv_obj_bind_flag_if_eq(band_label, &data->band_visible, LV_OBJ_FLAG_HIDDEN, 0);
        }

        // Register DELETE handler for automatic cleanup
        lv_obj_add_event_cb(item, network_item_delete_cb, LV_EVENT_DELETE, nullptr);

        spdlog::debug("[NetworkSettingsOverlay] Added network: {} ({}%, {}, band {})",
                      helix::redact::ssid(network.ssid), network.signal_strength,
                      network.is_secured ? "secured" : "open",
                      helix::ui::wifi::wifi_format_band_label(network.band_mask));
    }

    // Restore scroll position
    lv_obj_update_layout(networks_list_);
    lv_obj_scroll_to_y(networks_list_, scroll_y, LV_ANIM_OFF);

    spdlog::debug("[NetworkSettingsOverlay] Populated {} network items", sorted_networks.size());
}

void NetworkSettingsOverlay::clear_network_list() {
    if (!networks_list_) {
        return;
    }

    spdlog::debug("[NetworkSettingsOverlay] Clearing network list");

    // Cancel any in-progress press/scroll on this list. safe_delete_deferred_raw
    // below reparents each child to lv_layer_top() before async-delete; the reparent
    // detaches from networks_list_ immediately, but indev's cached scroll/press
    // target still points at the (soon-to-be-freed) child. Without this reset,
    // a SCROLL_THROW_BEGIN dispatched between reparent and async-delete dereferences
    // freed widget memory — SIGBUS in libc with MALLOC_PERTURB_ poison (#850).
    if (lv_indev_t* indev = lv_indev_active()) {
        lv_indev_reset(indev, networks_list_);
    }

    // Freeze queue to prevent background thread from enqueueing callbacks
    // targeting children we're about to delete
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    int32_t child_count = static_cast<int32_t>(lv_obj_get_child_count(networks_list_));
    for (int32_t i = child_count - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(networks_list_, i);
        if (!child)
            continue;

        const char* name = lv_obj_get_name(child);
        if (name && strncmp(name, "network_item_", 13) == 0) {
            // Remove clickable flag to prevent stale indev click events —
            // LVGL's indev may have cached this item as the pressed target.
            lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);

            // Defer deletion to next tick — safe_delete_deferred hides immediately
            // and deletes via lv_obj_delete_async, preventing event list corruption
            // when called from within process_pending() (token.defer callbacks).
            helix::ui::safe_delete_deferred_raw(child);
        }
    }

    spdlog::debug("[NetworkSettingsOverlay] Network list cleared");
}

void NetworkSettingsOverlay::show_placeholder(bool show) {
    if (!networks_list_) {
        return;
    }

    lv_obj_t* placeholder = lv_obj_find_by_name(networks_list_, "no_networks_placeholder");
    if (placeholder) {
        if (show) {
            lv_obj_remove_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void NetworkSettingsOverlay::update_signal_icons(lv_obj_t* item, int icon_state) {
    if (!item) {
        return;
    }

    lv_obj_t* signal_icons = lv_obj_find_by_name(item, "signal_icons");
    if (!signal_icons) {
        return;
    }

    // Icon names and their corresponding states
    static const struct {
        const char* name;
        int state;
    } icon_bindings[] = {
        {"sig_1", 1},      {"sig_2", 2},      {"sig_3", 3},      {"sig_4", 4},
        {"sig_1_lock", 5}, {"sig_2_lock", 6}, {"sig_3_lock", 7}, {"sig_4_lock", 8},
    };

    // Show only the icon matching current state
    for (const auto& binding : icon_bindings) {
        lv_obj_t* icon = lv_obj_find_by_name(signal_icons, binding.name);
        if (icon) {
            if (binding.state == icon_state) {
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ============================================================================
// Event Handler Implementations
// ============================================================================

void NetworkSettingsOverlay::handle_wlan_toggle_changed(lv_event_t* e) {
    lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!sw)
        return;

    // Don't process toggle if hardware unavailable
    if (lv_subject_get_int(&wifi_hardware_available_) == 0) {
        spdlog::debug("[NetworkSettingsOverlay] Ignoring toggle - WiFi hardware unavailable");
        return;
    }

    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    spdlog::info("[NetworkSettingsOverlay] WiFi toggle: {}", enabled ? "ON" : "OFF");

    if (!wifi_manager_) {
        spdlog::error("[NetworkSettingsOverlay] WiFiManager not initialized");
        return;
    }

    // Turning WiFi off on a device with no other network path is a one-way
    // door FROM THIS SCREEN — once the radio is off there is no wired
    // fallback to reach it and re-enable it remotely. This is the live-toggle
    // twin of the startup no-strand guard (Task 15/CC1 incident): that guard
    // only covers the backend reasserting a stored "off" on boot, not the
    // user flipping the switch right here. Warn and let the user back out
    // before actually applying it.
    if (!enabled && !wifi_manager_->has_non_wifi_fallback()) {
        spdlog::info("[NetworkSettingsOverlay] WiFi-off toggle with no non-WiFi network path — "
                     "confirming before applying");
        pending_wlan_toggle_switch_ = sw;
        std::string msg =
            lv_tr("This device has no wired network connection. If you turn WiFi off, it can "
                  "only be turned back on from this screen.");
        helix::ui::modal_show_confirmation(
            lv_tr("Turn Off WiFi?"), msg.c_str(), ModalSeverity::Warning, lv_tr("Turn Off"),
            on_wlan_toggle_off_confirm, on_wlan_toggle_off_cancel, nullptr);
        return;
    }

    apply_wlan_toggle(enabled);
}

void NetworkSettingsOverlay::handle_wlan_toggle_off_confirm() {
    helix::ui::modal_hide(helix::ui::modal_get_top());

    lv_obj_t* sw = pending_wlan_toggle_switch_;
    pending_wlan_toggle_switch_ = nullptr;
    if (!sw)
        return;

    spdlog::info("[NetworkSettingsOverlay] WiFi-off confirmed despite no wired fallback");
    apply_wlan_toggle(false);
}

void NetworkSettingsOverlay::handle_wlan_toggle_off_cancel() {
    helix::ui::modal_hide(helix::ui::modal_get_top());

    lv_obj_t* sw = pending_wlan_toggle_switch_;
    pending_wlan_toggle_switch_ = nullptr;
    if (sw) {
        // Nothing was ever applied to the backend — put the switch back to
        // reflect the radio's actual (still-on) state.
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
}

void NetworkSettingsOverlay::apply_wlan_toggle(bool enabled) {
    if (!wifi_manager_) {
        spdlog::error("[NetworkSettingsOverlay] WiFiManager not initialized");
        return;
    }

    // Optimistic: the switch has already moved under the user's finger, so
    // move the model with it. Driving the radio can park the caller for tens
    // of seconds (two wpa_ctrl commands per direction, each retrying the send
    // for up to 5s and then waiting up to 10s for a reply), and doing that on
    // the LVGL thread froze the whole UI mid-animation. Reality is folded back
    // in by finish_wlan_toggle().
    wlan_toggle_requested_ = enabled;
    lv_subject_set_int(&wifi_enabled_, enabled ? 1 : 0);

    if (enabled) {
        // Spinner runs from the tap until the radio is actually up and the
        // first scan lands.
        lv_subject_set_int(&wifi_scanning_, 1);
    } else {
        // Off is immediate feedback: nothing here waits on the radio.
        wifi_manager_->stop_scan();
        lv_subject_set_int(&wifi_scanning_, 0);
        clear_network_list();
        show_placeholder(true);
    }

    arm_wlan_toggle_backstop();

    auto token = lifetime_.token();
    wifi_manager_->set_enabled_async(enabled, token, [this, enabled](bool success, bool actual) {
        // Runs on the UI thread — set_enabled_async() delivers through the
        // token, so `this` is already known-good here.
        cancel_wlan_toggle_backstop();
        finish_wlan_toggle(enabled, success, actual);
    });
}

void NetworkSettingsOverlay::finish_wlan_toggle(bool requested, bool success, bool actual) {
    auto outcome = helix::wifi::reconcile_radio_toggle(requested, success, actual);

    if (outcome.reverted) {
        spdlog::warn("[NetworkSettingsOverlay] WiFi {} did not take — radio is {}",
                     requested ? "enable" : "disable", actual ? "on" : "off");
        if (outcome.silent_revert) {
            // The backend reported success, so WiFiManager raised no error
            // toast; without this the switch would jump back with no
            // explanation at all.
            NOTIFY_WARNING(lv_tr("WiFi radio did not change state"));
        }
    }

    // Persist and reflect what actually happened, not what was requested —
    // the radio change can fail (e.g. rfkill write denied), and persisting the
    // requested value regardless would feed a false "off" (or "on") back into
    // the startup reassert and the UI, the exact class of state lie this
    // whole branch exists to eliminate.
    lv_subject_set_int(&wifi_enabled_, outcome.enabled ? 1 : 0);

    if (outcome.enabled) {
        lv_subject_set_int(&wifi_scanning_, 1);

        std::weak_ptr<WiFiManager> weak_mgr = wifi_manager_;
        auto token = lifetime_.token();

        wifi_manager_->start_scan(
            [this, token, weak_mgr](const std::vector<WiFiNetwork>& networks) {
                if (token.expired() || weak_mgr.expired())
                    return;
                token.defer([this, networks]() {
                    lv_subject_set_int(&wifi_scanning_, 0);
                    populate_network_list(networks);
                });
            });
    } else {
        if (wifi_manager_) {
            wifi_manager_->stop_scan();
        }
        lv_subject_set_int(&wifi_scanning_, 0);
        clear_network_list();
        show_placeholder(true);

        // Update connection status
        lv_subject_set_int(&wifi_connected_, 0);
        ssid_buffer_[0] = '\0';
        ip_buffer_[0] = '\0';
        mac_buffer_[0] = '\0';
        lv_subject_notify(&connected_ssid_);
        lv_subject_notify(&ip_address_);
        lv_subject_notify(&mac_address_);
    }

    // Both writes below hit settings.json. Deferred to here rather than run on
    // the click so the LVGL thread never does a synchronous disk write while
    // the user is still touching the switch. set_wifi_enabled() saves, so the
    // expectation is staged first and the pair costs one write.
    if (auto* config = Config::get_instance()) {
        config->set_wifi_expected(outcome.enabled);
    }
    SystemSettingsManager::instance().set_wifi_enabled(outcome.enabled);

    // Update combined network status
    update_any_network_connected();
}

void NetworkSettingsOverlay::arm_wlan_toggle_backstop() {
    cancel_wlan_toggle_backstop();

    // Last resort only. The completion normally arrives well inside this
    // window; this covers a dispatch that never reports at all (HttpExecutor
    // stopped mid-flight breaks the promise without running the work), which
    // would otherwise leave the switch stuck showing a state the radio never
    // reached.
    wlan_toggle_backstop_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<NetworkSettingsOverlay*>(lv_timer_get_user_data(t));
            if (!self)
                return;
            self->cancel_wlan_toggle_backstop();
            if (!self->wifi_manager_)
                return;
            spdlog::warn("[NetworkSettingsOverlay] WiFi toggle never reported — "
                         "reconciling against the radio");
            const bool actual = self->wifi_manager_->is_enabled();
            self->finish_wlan_toggle(self->wlan_toggle_requested_, /*success=*/false, actual);
        },
        45000, this);
    lv_timer_set_repeat_count(wlan_toggle_backstop_, 1);
}

void NetworkSettingsOverlay::cancel_wlan_toggle_backstop() {
    if (wlan_toggle_backstop_) {
        helix::ui::lv_timer_cancel_safe(wlan_toggle_backstop_);
        wlan_toggle_backstop_ = nullptr;
    }
}

void NetworkSettingsOverlay::handle_refresh_clicked() {
    spdlog::debug("[NetworkSettingsOverlay] Refresh clicked");

    // Don't refresh if hardware unavailable
    if (lv_subject_get_int(&wifi_hardware_available_) == 0) {
        spdlog::debug("[NetworkSettingsOverlay] Ignoring refresh - WiFi hardware unavailable");
        return;
    }

    if (!wifi_manager_ || !wifi_manager_->is_enabled()) {
        spdlog::warn("[NetworkSettingsOverlay] Cannot refresh: WiFi not enabled");
        return;
    }

    lv_subject_set_int(&wifi_scanning_, 1);

    std::weak_ptr<WiFiManager> weak_mgr = wifi_manager_;
    auto token = lifetime_.token();

    wifi_manager_->start_scan([this, token, weak_mgr](const std::vector<WiFiNetwork>& networks) {
        if (token.expired() || weak_mgr.expired())
            return;
        token.defer([this, networks]() {
            lv_subject_set_int(&wifi_scanning_, 0);
            populate_network_list(networks);
        });
    });
}

void NetworkSettingsOverlay::handle_test_network_clicked() {
    spdlog::debug("[NetworkSettingsOverlay] Test network clicked");

    if (!network_tester_) {
        spdlog::error("[NetworkSettingsOverlay] NetworkTester not initialized");
        return;
    }

    // Check if any network is connected (wifi OR ethernet)
    bool any_connected = (lv_subject_get_int(&any_network_connected_) == 1);
    if (!any_connected) {
        spdlog::warn("[NetworkSettingsOverlay] Cannot test: no network connected");
        return;
    }

    // Reset test complete flag (disables close button)
    lv_subject_set_int(&test_complete_, 0);

    test_modal_ = helix::ui::modal_show("network_test_modal");
    if (!test_modal_) {
        spdlog::error("[NetworkSettingsOverlay] Failed to show network test modal");
        return;
    }

    // Find the step_container and create the step widget
    lv_obj_add_event_cb(test_modal_, on_modal_deleted, LV_EVENT_DELETE, this);

    lv_obj_t* step_container = lv_obj_find_by_name(test_modal_, "step_container");
    if (!step_container) {
        spdlog::error("[NetworkSettingsOverlay] step_container not found in modal");
        helix::ui::modal_hide(test_modal_);
        test_modal_ = nullptr;
        return;
    }

    // Create vertical step progress widget with 3 steps:
    // 1. Local connection - network config found (gateway IP known)
    // 2. Gateway - can reach router (gateway ping succeeds)
    // 3. Internet access - can reach internet (8.8.8.8 ping)
    ui_step_t steps[] = {{"Local connection", StepState::Pending},
                         {"Gateway", StepState::Pending},
                         {"Internet access", StepState::Pending}};

    step_widget_ = ui_step_progress_create(step_container, steps, 3, false, "network_test");
    if (!step_widget_) {
        spdlog::error("[NetworkSettingsOverlay] Failed to create step widget");
        helix::ui::modal_hide(test_modal_);
        test_modal_ = nullptr;
        return;
    }

    spdlog::debug("[NetworkSettingsOverlay] Network test modal shown, starting test");

    // Start the first step (Local connection)
    // Watched in its own right, not just via test_modal_: it is a grandchild of
    // the modal, and step_container can also be repopulated independently.
    lv_obj_add_event_cb(step_widget_, on_modal_deleted, LV_EVENT_DELETE, this);

    ui_step_progress_set_current(step_widget_, 0);

    // Reset test status
    lv_subject_set_int(&test_gateway_status_, 0);
    lv_subject_set_int(&test_internet_status_, 0);
    lv_subject_set_int(&test_running_, 1);

    auto token = lifetime_.token();

    network_tester_->start_test(
        [this, token](NetworkTester::TestState state, const NetworkTester::TestResult& result) {
            if (token.expired())
                return;
            token.defer([this, state, result]() {
                update_test_state(state, result);

                // Update step widget based on state (3 steps: Local, Gateway, Internet)
                if (step_widget_) {
                    switch (state) {
                    case NetworkTester::TestState::TESTING_GATEWAY:
                        // Gateway IP found (local connection OK), now pinging gateway
                        ui_step_progress_set_completed(step_widget_, 0);
                        ui_step_progress_set_current(step_widget_, 1);
                        break;
                    case NetworkTester::TestState::TESTING_INTERNET:
                        // Gateway ping succeeded, now testing internet
                        ui_step_progress_set_completed(step_widget_, 0);
                        if (result.gateway_ok) {
                            ui_step_progress_set_completed(step_widget_, 1);
                        }
                        ui_step_progress_set_current(step_widget_, 2);
                        break;
                    case NetworkTester::TestState::COMPLETED:
                    case NetworkTester::TestState::FAILED:
                        // Mark all steps based on results
                        ui_step_progress_set_completed(step_widget_, 0);
                        if (result.gateway_ok) {
                            ui_step_progress_set_completed(step_widget_, 1);
                        }
                        if (result.internet_ok) {
                            ui_step_progress_set_completed(step_widget_, 2);
                        }
                        // Enable close button
                        lv_subject_set_int(&test_complete_, 1);
                        break;
                    default:
                        break;
                    }
                }
            });
        });
}

void NetworkSettingsOverlay::handle_add_other_clicked() {
    spdlog::debug("[NetworkSettingsOverlay] Add Hidden Network clicked");

    // Don't add network if hardware unavailable
    if (lv_subject_get_int(&wifi_hardware_available_) == 0) {
        spdlog::debug("[NetworkSettingsOverlay] Ignoring add other - WiFi hardware unavailable");
        return;
    }

    // Create modal if not already created
    if (!hidden_network_modal_) {
        hidden_network_modal_ = helix::ui::modal_show("hidden_network_modal");
        if (!hidden_network_modal_) {
            spdlog::error("[NetworkSettingsOverlay] Failed to show hidden network modal");
            return;
        }

        // Register soft keyboard for text inputs
        lv_obj_add_event_cb(hidden_network_modal_, on_modal_deleted, LV_EVENT_DELETE, this);

        lv_obj_t* ssid_input = lv_obj_find_by_name(hidden_network_modal_, "ssid_input");
        if (ssid_input) {
            helix::ui::modal_register_keyboard(hidden_network_modal_, ssid_input);
            // Focus SSID input so user can start typing immediately
            lv_group_t* group = lv_group_get_default();
            if (group) {
                lv_group_focus_obj(ssid_input);
            }
        }

        lv_obj_t* password_input = lv_obj_find_by_name(hidden_network_modal_, "password_input");
        if (password_input) {
            helix::ui::modal_register_keyboard(hidden_network_modal_, password_input);
        }
    }

    spdlog::debug("[NetworkSettingsOverlay] Hidden network modal shown");
}

void NetworkSettingsOverlay::handle_network_test_close() {
    spdlog::debug("[NetworkSettingsOverlay] Network test close clicked");

    // Cancel any running test
    if (network_tester_ && network_tester_->is_running()) {
        network_tester_->cancel();
        lv_subject_set_int(&test_running_, 0);
    }

    // Hide the modal
    if (test_modal_) {
        helix::ui::modal_hide(test_modal_);
        test_modal_ = nullptr;
        step_widget_ = nullptr;
    }

    // Reset test complete flag
    lv_subject_set_int(&test_complete_, 0);
}

void NetworkSettingsOverlay::handle_hidden_cancel_clicked() {
    spdlog::debug("[NetworkSettingsOverlay] Hidden network cancel clicked");

    if (hidden_network_modal_) {
        helix::ui::modal_hide(hidden_network_modal_);
        hidden_network_modal_ = nullptr;
    }
}

void NetworkSettingsOverlay::handle_hidden_connect_clicked() {
    spdlog::debug("[NetworkSettingsOverlay] Hidden network connect clicked");

    if (!hidden_network_modal_) {
        spdlog::error("[NetworkSettingsOverlay] No hidden network modal");
        return;
    }

    // Get SSID from input
    lv_obj_t* ssid_input = lv_obj_find_by_name(hidden_network_modal_, "ssid_input");
    if (!ssid_input) {
        spdlog::error("[NetworkSettingsOverlay] SSID input not found");
        return;
    }

    const char* ssid = lv_textarea_get_text(ssid_input);
    if (!ssid || strlen(ssid) == 0) {
        spdlog::warn("[NetworkSettingsOverlay] SSID is empty");
        lv_obj_t* error_label = lv_obj_find_by_name(hidden_network_modal_, "error_label");
        if (error_label) {
            lv_label_set_text(error_label, lv_tr("Network name cannot be empty"));
            lv_obj_remove_flag(error_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Get security type from dropdown
    lv_obj_t* security_dropdown = lv_obj_find_by_name(hidden_network_modal_, "security_dropdown");
    uint32_t security_idx = 0;
    if (security_dropdown) {
        security_idx = lv_dropdown_get_selected(security_dropdown);
    }

    // Get password if security is not "None" (index 0)
    std::string password;
    if (security_idx > 0) {
        lv_obj_t* password_input = lv_obj_find_by_name(hidden_network_modal_, "password_input");
        if (password_input) {
            const char* pwd = lv_textarea_get_text(password_input);
            if (pwd && strlen(pwd) > 0) {
                password = pwd;
            }
        }
    }

    if (security_idx > 0 && password.empty()) {
        lv_obj_t* error_label = lv_obj_find_by_name(hidden_network_modal_, "error_label");
        if (error_label) {
            lv_label_set_text(error_label, lv_tr("Password is required for secured networks"));
            lv_obj_remove_flag(error_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (!wifi_manager_) {
        spdlog::error("[NetworkSettingsOverlay] WiFiManager not initialized");
        return;
    }

    spdlog::info("[NetworkSettingsOverlay] Connecting to hidden network: {} (security: {})",
                 helix::redact::ssid(ssid), security_idx);

    lv_subject_t* connecting_subject = lv_xml_get_subject(nullptr, "hidden_connecting");
    if (connecting_subject) {
        lv_subject_set_int(connecting_subject, 1);
    }

    std::string ssid_str(ssid);
    auto token = lifetime_.token();

    wifi_manager_->connect(
        ssid_str, password, [this, token, ssid_str](bool success, const std::string& error) {
            if (token.expired())
                return;
            token.defer([this, success, ssid_str, error]() {
                lv_subject_t* subj = lv_xml_get_subject(nullptr, "hidden_connecting");
                if (subj) {
                    lv_subject_set_int(subj, 0);
                }

                if (success) {
                    spdlog::info("[NetworkSettingsOverlay] Connected to hidden network: {}",
                                 helix::redact::ssid(ssid_str));
                    handle_hidden_cancel_clicked();
                    update_wifi_status();
                    update_any_network_connected();

                    if (wifi_manager_) {
                        auto scan_token = lifetime_.token();
                        wifi_manager_->start_scan(
                            [this, scan_token](const std::vector<WiFiNetwork>& networks) {
                                if (scan_token.expired())
                                    return;
                                scan_token.defer(
                                    [this, networks]() { populate_network_list(networks); });
                            });
                    }
                } else {
                    spdlog::error("[NetworkSettingsOverlay] Hidden network connection failed: {}",
                                  error);

                    if (hidden_network_modal_) {
                        lv_obj_t* error_label =
                            lv_obj_find_by_name(hidden_network_modal_, "error_label");
                        if (error_label) {
                            lv_label_set_text(error_label,
                                              lv_tr("Connection failed. Check credentials."));
                            lv_obj_remove_flag(error_label, LV_OBJ_FLAG_HIDDEN);
                        }
                    }
                }
            });
        });
}

void NetworkSettingsOverlay::handle_security_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!dropdown) {
        return;
    }

    uint32_t selected = lv_dropdown_get_selected(dropdown);
    spdlog::debug("[NetworkSettingsOverlay] Security changed to index: {}", selected);

    // Update hidden_security subject (0=None hides password field)
    lv_subject_t* security_subject = lv_xml_get_subject(nullptr, "hidden_security");
    if (security_subject) {
        lv_subject_set_int(security_subject, static_cast<int>(selected));
    }
}

void NetworkSettingsOverlay::handle_network_item_clicked(lv_event_t* e) {
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!item)
        return;

    NetworkSettingsItemData* item_data =
        static_cast<NetworkSettingsItemData*>(lv_obj_get_user_data(item));
    if (!item_data) {
        spdlog::error("[NetworkSettingsOverlay] No network data found in clicked item");
        return;
    }

    spdlog::info("[NetworkSettingsOverlay] Network clicked: {} ({})",
                 helix::redact::ssid(item_data->ssid), item_data->is_secured ? "secured" : "open");

    strncpy(current_ssid_, item_data->ssid.c_str(), sizeof(current_ssid_) - 1);
    current_ssid_[sizeof(current_ssid_) - 1] = '\0';
    current_network_is_secured_ = item_data->is_secured;

    if (item_data->is_secured) {
        // Show password modal for secured networks
        show_password_modal(item_data->ssid.c_str());
    } else {
        // Connect to open network
        if (!wifi_manager_) {
            spdlog::error("[NetworkSettingsOverlay] WiFiManager not initialized");
            return;
        }

        auto token = lifetime_.token();
        wifi_manager_->connect(
            item_data->ssid, "", [this, token](bool success, const std::string& error) {
                if (token.expired())
                    return;
                token.defer([this, success, error]() {
                    if (success) {
                        spdlog::info("[NetworkSettingsOverlay] Connected to {}",
                                     helix::redact::ssid(current_ssid_));
                        update_wifi_status();
                        update_any_network_connected();
                    } else {
                        spdlog::error("[NetworkSettingsOverlay] Failed to connect: {}", error);
                    }
                });
            });
    }
}

void NetworkSettingsOverlay::handle_network_settings_forget() {
    if (!wifi_manager_) {
        spdlog::error("[NetworkSettingsOverlay] WiFiManager not initialized");
        return;
    }

    // Fresh read, not the cached connected_ssid_ subject — the confirmation
    // dialog acts on whatever is actually associated right now.
    std::string ssid = wifi_manager_->get_connected_ssid();
    if (ssid.empty()) {
        spdlog::debug("[NetworkSettingsOverlay] Forget clicked with no connected network");
        return;
    }

    pending_forget_ssid_ = ssid;

    spdlog::debug("[NetworkSettingsOverlay] Confirming forget for: {}", helix::redact::ssid(ssid));

    std::string msg =
        fmt::format(lv_tr("Forget network '{}'? You will need the password to reconnect."), ssid);
    helix::ui::modal_show_confirmation(
        lv_tr("Forget Network?"), msg.c_str(), ModalSeverity::Warning, lv_tr("Forget"),
        on_network_forget_confirm, on_network_forget_cancel, nullptr);
}

void NetworkSettingsOverlay::handle_network_forget_confirm() {
    helix::ui::modal_hide(helix::ui::modal_get_top());

    std::string ssid = pending_forget_ssid_;
    pending_forget_ssid_.clear();

    if (ssid.empty() || !wifi_manager_) {
        return;
    }

    spdlog::info("[NetworkSettingsOverlay] Forgetting network: {}", helix::redact::ssid(ssid));

    auto token = lifetime_.token();
    wifi_manager_->forget(ssid, [this, token, ssid](bool success, const std::string& error) {
        if (token.expired())
            return;
        token.defer([this, success, ssid, error]() {
            if (success) {
                spdlog::info("[NetworkSettingsOverlay] Forgot network: {}",
                             helix::redact::ssid(ssid));
                ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Network forgotten"),
                                              2000);
                update_wifi_status();
                update_any_network_connected();

                // Refresh the scan list so a stale "connected" checkmark clears.
                if (wifi_manager_ && wifi_manager_->is_enabled()) {
                    auto scan_token = lifetime_.token();
                    wifi_manager_->start_scan([this, scan_token](
                                                  const std::vector<WiFiNetwork>& networks) {
                        if (scan_token.expired())
                            return;
                        scan_token.defer([this, networks]() { populate_network_list(networks); });
                    });
                }
            } else if (!error.empty()) {
                // WiFiManager::forget() already toasts a genuine failure itself
                // (NOTIFY_ERROR) and stays silent for NETWORK_NOT_FOUND — nothing
                // was there to forget isn't a user-facing error either way, so
                // there's nothing left to surface here.
                spdlog::debug("[NetworkSettingsOverlay] Forget did not remove a saved entry: {}",
                              error);
            }
        });
    });
}

void NetworkSettingsOverlay::handle_network_forget_cancel() {
    helix::ui::modal_hide(helix::ui::modal_get_top());
    pending_forget_ssid_.clear();
}

// ============================================================================
// Static Trampolines for LVGL Callbacks
// ============================================================================

void NetworkSettingsOverlay::on_wlan_toggle_changed(lv_event_t* e) {
    auto& self = get_network_settings_overlay();
    self.handle_wlan_toggle_changed(e);
}

void NetworkSettingsOverlay::on_refresh_clicked(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_refresh_clicked();
}

void NetworkSettingsOverlay::on_test_network_clicked(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_test_network_clicked();
}

void NetworkSettingsOverlay::on_add_other_clicked(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_add_other_clicked();
}

void NetworkSettingsOverlay::on_network_item_clicked(lv_event_t* e) {
    auto& self = get_network_settings_overlay();
    self.handle_network_item_clicked(e);
}

void NetworkSettingsOverlay::on_network_settings_forget(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_network_settings_forget();
}

void NetworkSettingsOverlay::on_network_forget_confirm(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_network_forget_confirm();
}

void NetworkSettingsOverlay::on_network_forget_cancel(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_network_forget_cancel();
}

void NetworkSettingsOverlay::on_wlan_toggle_off_confirm(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_wlan_toggle_off_confirm();
}

void NetworkSettingsOverlay::on_wlan_toggle_off_cancel(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_wlan_toggle_off_cancel();
}

void NetworkSettingsOverlay::on_network_test_close(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_network_test_close();
}

void NetworkSettingsOverlay::on_hidden_cancel_clicked(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_hidden_cancel_clicked();
}

void NetworkSettingsOverlay::on_hidden_connect_clicked(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_hidden_connect_clicked();
}

void NetworkSettingsOverlay::on_security_changed(lv_event_t* e) {
    auto& self = get_network_settings_overlay();
    self.handle_security_changed(e);
}

// ============================================================================
// Password Modal Implementation
// ============================================================================

void NetworkSettingsOverlay::show_password_modal(const char* ssid) {
    spdlog::debug("[NetworkSettingsOverlay] Showing password modal for: {}",
                  helix::redact::ssid(ssid));

    // Update SSID subject for modal display
    strncpy(password_modal_ssid_buffer_, ssid, sizeof(password_modal_ssid_buffer_) - 1);
    password_modal_ssid_buffer_[sizeof(password_modal_ssid_buffer_) - 1] = '\0';
    lv_subject_notify(&wifi_password_modal_ssid_);

    // Reset connecting state
    lv_subject_set_int(&wifi_connecting_, 0);

    // Show modal
    password_modal_ = helix::ui::modal_show("wifi_password_modal");
    if (!password_modal_) {
        spdlog::error("[NetworkSettingsOverlay] Failed to show password modal");
        return;
    }

    // Drop the cached pointer if anything other than us destroys this modal.
    lv_obj_add_event_cb(password_modal_, on_modal_deleted, LV_EVENT_DELETE, this);

    // Clear password input and register keyboard
    lv_obj_t* password_input = lv_obj_find_by_name(password_modal_, "password_input");
    if (password_input) {
        lv_textarea_set_text(password_input, "");
        helix::ui::modal_register_keyboard(password_modal_, password_input);

        // Focus the input
        lv_group_t* group = lv_group_get_default();
        if (group) {
            lv_group_focus_obj(password_input);
        }
    }

    // Hide any previous error message
    lv_obj_t* modal_status = lv_obj_find_by_name(password_modal_, "modal_status");
    if (modal_status) {
        lv_obj_add_flag(modal_status, LV_OBJ_FLAG_HIDDEN);
    }

    spdlog::debug("[NetworkSettingsOverlay] Password modal shown");
}

/**
 * @brief Drop a cached modal pointer the moment that modal object dies
 *
 * This class nulls each pointer for the dismissals it drives itself, but the
 * modal system can also close a modal (back gesture, ModalStack unwinding), and
 * those paths left a dangling non-null pointer behind. The connect-result
 * callbacks null-check the pointer and then walk it with lv_obj_find_by_name(),
 * which on a freed object reads whatever the allocator left there - a hard
 * SIGBUS on MIPS, silent corruption elsewhere (#1341). The lifetime token those
 * callbacks carry does not help: it guards the overlay, which is a singleton
 * that outlives every modal.
 *
 * Same lesson as e7aba0d42: watch the object's OWN delete event, not just the
 * paths you expect to destroy it. Every cached pointer gets it, because the
 * ones that had not crashed yet differ only in how often they are exercised.
 *
 * KEEP THIS HANDLER A PURE ASSIGNMENT. obj_delete_core() sends DELETE to the
 * object first and only then recurses into its children, so on a test_modal_
 * deletion this runs for the modal while step_widget_ is still non-null and
 * already inside its ancestor's teardown. Nulling a pointer is safe there;
 * touching the widget it names would not be.
 */
void NetworkSettingsOverlay::on_modal_deleted(lv_event_t* e) {
    auto* self = static_cast<NetworkSettingsOverlay*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    lv_obj_t* dying = lv_event_get_target_obj(e);
    // Compare by identity: a late DELETE from a previous instance of the same
    // modal must not clear the pointer to the one now on screen.
    for (lv_obj_t** cached : {&self->password_modal_, &self->hidden_network_modal_,
                              &self->test_modal_, &self->step_widget_}) {
        if (*cached && *cached == dying) {
            *cached = nullptr;
            return;
        }
    }
}

void NetworkSettingsOverlay::hide_password_modal() {
    if (password_modal_) {
        helix::ui::modal_hide(password_modal_);
        password_modal_ = nullptr;
    }
    lv_subject_set_int(&wifi_connecting_, 0);
}

void NetworkSettingsOverlay::handle_password_cancel_clicked() {
    spdlog::debug("[NetworkSettingsOverlay] Password cancel clicked");
    hide_password_modal();
}

void NetworkSettingsOverlay::handle_password_connect_clicked() {
    spdlog::debug("[NetworkSettingsOverlay] Password connect clicked");

    if (!password_modal_) {
        spdlog::error("[NetworkSettingsOverlay] No password modal");
        return;
    }

    // Get password from input
    lv_obj_t* password_input = lv_obj_find_by_name(password_modal_, "password_input");
    if (!password_input) {
        spdlog::error("[NetworkSettingsOverlay] Password input not found");
        return;
    }

    const char* password = lv_textarea_get_text(password_input);
    if (!password || strlen(password) == 0) {
        // Show error in modal
        lv_obj_t* modal_status = lv_obj_find_by_name(password_modal_, "modal_status");
        if (modal_status) {
            lv_label_set_text(modal_status, lv_tr("Password cannot be empty"));
            lv_obj_remove_flag(modal_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (!wifi_manager_) {
        spdlog::error("[NetworkSettingsOverlay] WiFiManager not initialized");
        return;
    }

    // Show connecting state (hides form, shows spinner)
    lv_subject_set_int(&wifi_connecting_, 1);

    spdlog::info("[NetworkSettingsOverlay] Connecting to secured network: {}",
                 helix::redact::ssid(current_ssid_));

    // Capture password for lambda
    std::string pwd(password);
    std::string ssid(current_ssid_);
    auto token = lifetime_.token();

    wifi_manager_->connect(ssid, pwd, [this, token, ssid](bool success, const std::string& error) {
        if (token.expired())
            return;
        token.defer([this, success, ssid, error]() {
            // Reset connecting state
            lv_subject_set_int(&wifi_connecting_, 0);

            if (success) {
                spdlog::info("[NetworkSettingsOverlay] Connected to {}", helix::redact::ssid(ssid));
                hide_password_modal();
                update_wifi_status();
                update_any_network_connected();

                // Refresh network list to show checkmark on connected network
                if (wifi_manager_) {
                    auto scan_token = lifetime_.token();
                    wifi_manager_->start_scan([this, scan_token](
                                                  const std::vector<WiFiNetwork>& networks) {
                        if (scan_token.expired())
                            return;
                        scan_token.defer([this, networks]() { populate_network_list(networks); });
                    });
                }
            } else {
                spdlog::error("[NetworkSettingsOverlay] Connection failed: {}", error);

                // Show error in modal — use backend detail if available
                if (password_modal_) {
                    lv_obj_t* modal_status = lv_obj_find_by_name(password_modal_, "modal_status");
                    if (modal_status) {
                        // Permission errors get backend message; auth failures get generic
                        auto lower_err = error;
                        std::transform(lower_err.begin(), lower_err.end(), lower_err.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        bool is_permission =
                            lower_err.find("permission denied") != std::string::npos;
                        if (is_permission) {
                            lv_label_set_text(modal_status, error.c_str());
                        } else {
                            lv_label_set_text(modal_status,
                                              lv_tr("Connection failed. Check password."));
                        }
                        lv_obj_remove_flag(modal_status, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
        });
    });
}

void NetworkSettingsOverlay::on_wifi_password_cancel(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_password_cancel_clicked();
}

void NetworkSettingsOverlay::on_wifi_password_connect(lv_event_t* e) {
    (void)e;
    auto& self = get_network_settings_overlay();
    self.handle_password_connect_clicked();
}
