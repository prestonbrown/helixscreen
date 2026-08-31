// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_overlay_network_settings.h
 * @brief Network Settings overlay panel - WiFi and Ethernet configuration
 *
 * Manages reactive network settings overlay with:
 * - WiFi enable/disable toggle with connection status
 * - Ethernet status display (read-only)
 * - Network scanning and selection
 * - Connection status display (SSID, IP, MAC)
 * - Network connectivity testing (gateway + internet)
 * - Password entry modal for secured networks
 * - Hidden network configuration
 *
 * ## Architecture
 *
 * Fully reactive design - C++ updates subjects, XML handles all UI bindings.
 * Minimal direct widget manipulation (only network list population).
 *
 * ## Subject Bindings:
 *
 * WiFi subjects:
 * - wifi_hardware_available (int) - 0=unavailable, 1=available
 * - wifi_enabled (int) - 0=off, 1=on
 * - wifi_connected (int) - 0=disconnected, 1=connected
 * - wifi_only_24ghz (int) - 1 if hardware limited to 2.4GHz only
 * - connected_ssid (string) - Current network name
 * - ip_address (string) - e.g., "192.168.1.100"
 * - mac_address (string) - e.g., "50:41:1C:xx:xx:xx"
 * - network_count (string) - e.g., "(4)"
 * - wifi_scanning (int) - 0=idle, 1=scanning
 *
 * Ethernet subjects:
 * - eth_connected (int) - 0=disconnected, 1=connected
 * - eth_ip_address (string) - Ethernet IP address
 * - eth_mac_address (string) - Ethernet MAC address
 *
 * Test subjects:
 * - any_network_connected (int) - 1 if wifi OR ethernet connected
 * - test_running (int) - 0=idle, 1=running
 * - test_gateway_status (int) - 0=pending, 1=active, 2=success, 3=failed
 * - test_internet_status (int) - 0=pending, 1=active, 2=success, 3=failed
 *
 * ## Initialization Order (CRITICAL):
 *
 *   1. Register XML components (network_settings_overlay.xml, wifi_network_item.xml)
 *   2. init_subjects()
 *   3. register_callbacks()
 *   4. create(parent_screen)
 *   5. show() when ready to display
 */

#pragma once

#include "lvgl/lvgl.h"
#include "network_tester.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class WiFiManager;
}
class EthernetManager;
struct EthernetInfo;
struct WiFiNetwork;

/**
 * @class NetworkSettingsOverlay
 * @brief Reactive network settings overlay panel
 *
 * Manages WiFi and Ethernet configuration UI with reactive subject-based architecture.
 * Integrates with WiFiManager for scanning/connection, EthernetManager for status,
 * and NetworkTester for connectivity validation.
 *
 * Inherits from OverlayBase for lifecycle management (on_activate/on_deactivate).
 */
class NetworkSettingsOverlay : public OverlayBase {
  public:
    NetworkSettingsOverlay();
    ~NetworkSettingsOverlay();

    // Non-copyable
    NetworkSettingsOverlay(const NetworkSettingsOverlay&) = delete;
    NetworkSettingsOverlay& operator=(const NetworkSettingsOverlay&) = delete;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize reactive subjects
     *
     * Creates and registers subjects with defaults.
     * MUST be called BEFORE create() to ensure bindings work.
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks:
     * - on_wlan_toggle_changed
     * - on_refresh_clicked
     * - on_test_network_clicked
     * - on_add_other_clicked
     * - on_network_item_clicked
     * - on_network_settings_forget
     */
    void register_callbacks() override;

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent_screen Parent screen widget to attach overlay to
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent_screen) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Network Settings"
     */
    const char* get_name() const override {
        return "Network Settings";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Starts WiFi scanning, updates connection status.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Stops WiFi scanning, cancels network tests.
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    //
    // === Public API ===
    //

    /**
     * @brief Show overlay panel
     *
     * Pushes overlay onto navigation stack and registers with NavigationManager.
     * on_activate() will be called automatically after animation completes.
     */
    void show();

    /**
     * @brief Hide overlay panel
     *
     * Pops overlay from navigation stack via NavigationManager::go_back().
     * on_deactivate() will be called automatically before animation starts.
     */
    void hide();

    /**
     * @brief Check if overlay is created
     * @return true if overlay widget exists
     */
    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    // Note: is_visible() inherited from OverlayBase

  private:
    // Widget references (minimal - prefer subjects)
    // Note: overlay_root_ inherited from OverlayBase
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* networks_list_ = nullptr;

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // WiFi subjects
    lv_subject_t wifi_hardware_available_; // 0=unavailable, 1=available
    lv_subject_t wifi_enabled_;
    lv_subject_t wifi_connected_;
    lv_subject_t wifi_only_24ghz_; // 1 if hardware only supports 2.4GHz
    lv_subject_t connected_ssid_;
    lv_subject_t ip_address_;
    lv_subject_t mac_address_;
    lv_subject_t network_count_;
    lv_subject_t wifi_scanning_;

    // Ethernet subjects
    lv_subject_t eth_connected_;
    lv_subject_t eth_ip_address_;
    lv_subject_t eth_mac_address_;

    // Network test subjects
    lv_subject_t any_network_connected_; // 1 if wifi OR ethernet connected
    lv_subject_t test_running_;
    lv_subject_t test_gateway_status_;
    lv_subject_t test_internet_status_;

    // Password modal subjects
    lv_subject_t wifi_connecting_;          // 0=idle, 1=connecting (toggles modal form)
    lv_subject_t wifi_password_modal_ssid_; // SSID displayed in password modal

    // String buffers (subjects need stable char* pointers)
    char ssid_buffer_[64];
    char ip_buffer_[32];
    char mac_buffer_[32];
    char count_buffer_[16];
    char eth_ip_buffer_[32];
    char eth_mac_buffer_[32];
    char password_modal_ssid_buffer_[64];

    // Integration
    std::shared_ptr<helix::WiFiManager> wifi_manager_;
    std::unique_ptr<EthernetManager> ethernet_manager_;
    std::shared_ptr<NetworkTester> network_tester_;

    // State tracking
    // Note: subjects_initialized_, visible_, cleanup_called_ inherited from OverlayBase
    bool callbacks_registered_ = false;

    // Network test modal
    lv_obj_t* test_modal_ = nullptr;
    lv_obj_t* step_widget_ = nullptr;
    lv_subject_t test_complete_; // Controls close button enabled state

    // Hidden network modal (visibility controlled by Modal system)
    lv_obj_t* hidden_network_modal_ = nullptr;

    // Password modal for secured networks
    lv_obj_t* password_modal_ = nullptr;

    friend class NetworkSettingsOverlayTestAccess;

    // Current network selection for password modal
    char current_ssid_[64];
    bool current_network_is_secured_ = false;

    // SSID awaiting confirmation in the forget-network dialog; cleared once
    // the dialog resolves (confirm or cancel).
    std::string pending_forget_ssid_;

    // WLAN toggle switch awaiting confirmation before actually turning WiFi
    // off (Task 15/I2: no-strand gate on a live toggle, not just startup).
    // nullptr except while that confirmation dialog is up.
    lv_obj_t* pending_wlan_toggle_switch_ = nullptr;

    // Cached networks for async UI update
    std::vector<WiFiNetwork> cached_networks_;

    // Event handler implementations
    void handle_wlan_toggle_changed(lv_event_t* e);
    // Dispatches the radio on/off request (post no-strand confirmation, if one
    // was needed). Flips the UI optimistically and returns immediately; the
    // real outcome lands in finish_wlan_toggle().
    void apply_wlan_toggle(bool enabled);
    // Folds the radio's real state back into the subjects, settings and
    // network list, reverting the optimistic flip when they disagree.
    void finish_wlan_toggle(bool requested, bool success, bool actual);
    // Backstop for a dispatch whose completion never arrives (e.g. the
    // executor was stopped mid-flight): re-reads the radio and reconciles.
    void arm_wlan_toggle_backstop();
    void cancel_wlan_toggle_backstop();
    lv_timer_t* wlan_toggle_backstop_ = nullptr;
    bool wlan_toggle_requested_ = false;
    void handle_wlan_toggle_off_confirm();
    void handle_wlan_toggle_off_cancel();
    void handle_refresh_clicked();
    void handle_test_network_clicked();
    void handle_add_other_clicked();
    void handle_network_item_clicked(lv_event_t* e);
    void handle_network_settings_forget();
    void handle_network_forget_confirm();
    void handle_network_forget_cancel();

    // Helper functions
    void update_wifi_status();
    // Kicks off an async probe of the Ethernet backend and applies the
    // result on the UI thread via apply_ethernet_status(). Safe to call
    // from the UI thread — does not block.
    void update_ethernet_status();
    // Applies an EthernetInfo to the overlay subjects. UI-thread only.
    void apply_ethernet_status(const EthernetInfo& info);
    void update_any_network_connected();
    // Refresh BOTH transport rows after a WiFi transition (connect, forget).
    // On single-transport platforms (netd enforces this) a join or leave
    // downs/ups the OTHER transport too, so the ethernet row is as stale as
    // the wifi one until re-queried (prestonbrown/helixscreen#1398).
    void refresh_transport_status();

    /// Ethernet refresh coalescing: at most one daemon probe in flight, with
    /// one trailing refresh armed while one runs (a flapping link or a
    /// failed-join retry burst collapses to one round trip, not N).
    bool eth_refresh_in_flight_ = false;
    bool eth_refresh_trailing_ = false;
    void update_test_state(NetworkTester::TestState state, const NetworkTester::TestResult& result);
    void populate_network_list(const std::vector<WiFiNetwork>& networks);
    void clear_network_list();
    void show_placeholder(bool show);
    void update_signal_icons(lv_obj_t* item, int icon_state);

    // Static trampolines for LVGL callbacks
    static void on_wlan_toggle_changed(lv_event_t* e);
    static void on_refresh_clicked(lv_event_t* e);
    static void on_test_network_clicked(lv_event_t* e);
    static void on_add_other_clicked(lv_event_t* e);
    static void on_network_item_clicked(lv_event_t* e);
    // Scoped per [L039] to avoid colliding with other overlays' forget flows
    // (barcode scanner, label printer) in the flat XML callback namespace.
    static void on_network_settings_forget(lv_event_t* e);

    // Network test modal callbacks
    static void on_network_test_close(lv_event_t* e);
    void handle_network_test_close();

    // Hidden network modal callbacks
    static void on_hidden_cancel_clicked(lv_event_t* e);
    static void on_hidden_connect_clicked(lv_event_t* e);
    static void on_security_changed(lv_event_t* e);
    void handle_hidden_cancel_clicked();
    void handle_hidden_connect_clicked();
    void handle_security_changed(lv_event_t* e);

    // Password modal methods
    void show_password_modal(const char* ssid);
    void hide_password_modal();

    // Password modal callbacks
    /// Nulls whichever cached modal pointer matches the object being destroyed,
    /// including destruction this class does not drive itself (#1341).
    static void on_modal_deleted(lv_event_t* e);
    static void on_wifi_password_cancel(lv_event_t* e);
    static void on_wifi_password_connect(lv_event_t* e);
    void handle_password_cancel_clicked();
    void handle_password_connect_clicked();
};

// ============================================================================
// Global Instance Access
// ============================================================================

/**
 * @brief Get the global NetworkSettingsOverlay instance
 *
 * Creates the instance on first call. Singleton pattern.
 */
NetworkSettingsOverlay& get_network_settings_overlay();
