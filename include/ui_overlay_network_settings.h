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
 *   2. show(parent_screen) runs init_subjects(), register_callbacks() and create()
 *      on first use, then pushes the overlay
 */

#pragma once

#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"
#include "network_tester.h"
#include "overlay_base.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "wifi_backend.h"

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
     * - on_network_settings_item_clicked
     * - on_network_settings_password_cancel
     * - on_network_settings_password_connect
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
    const char* xml_component() const override {
        return "network_settings_overlay";
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
    void on_deactivating(DeactivateReason reason) override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    //
    // === Public API ===
    //

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
    lv_obj_t* networks_list_ = nullptr;

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // WiFi subjects
    lv_subject_t wifi_hardware_available_{}; // 0=unavailable, 1=available
    lv_subject_t wifi_enabled_{};
    lv_subject_t wifi_connected_{};
    lv_subject_t wifi_only_24ghz_{};       // 1 if hardware only supports 2.4GHz
    lv_subject_t wifi_can_forget_{};       // 1 if the backend can forget a saved network
    lv_subject_t wifi_can_toggle_radio_{}; // 1 if the backend can move the radio
    lv_subject_t wifi_toggle_locked_{};    // 1 while the switch must not take a tap
    lv_subject_t wifi_can_set_country_{};  // 1 if the backend can set the regulatory country
    lv_subject_t wifi_country_index_{};    // country dropdown selection (0 = not set)
    lv_subject_t wifi_unresponsive_{};     // 1 while WiFi control stops answering (rows show
                                           // the last known state)
    lv_subject_t connected_ssid_{};
    lv_subject_t ip_address_{};
    lv_subject_t mac_address_{};
    lv_subject_t network_count_{};
    lv_subject_t wifi_scanning_{};

    // Ethernet subjects
    lv_subject_t eth_connected_{};
    lv_subject_t eth_ip_address_{};
    lv_subject_t eth_mac_address_{};

    // Network test subjects
    lv_subject_t any_network_connected_{}; // 1 if wifi OR ethernet connected
    lv_subject_t test_running_{};
    lv_subject_t test_gateway_status_{};
    lv_subject_t test_internet_status_{};

    // Password modal subjects
    lv_subject_t wifi_connecting_{};          // 0=idle, 1=connecting (toggles modal form)
    lv_subject_t wifi_password_modal_ssid_{}; // SSID displayed in password modal

    // Hidden-network modal subject
    lv_subject_t hidden_connecting_{}; // 0=idle, 1=connecting (toggles modal form)

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

    // Network test modal
    helix::ui::WidgetRef test_modal_;
    helix::ui::WidgetRef step_widget_;
    lv_subject_t test_complete_{}; // Controls close button enabled state

    // Hidden network modal (visibility controlled by Modal system)
    helix::ui::WidgetRef hidden_network_modal_;

    // Password modal for secured networks
    helix::ui::WidgetRef password_modal_;

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

    // Bumped (main thread) on toggle, connect start, forget and cleanup. A
    // status answer issued under an older value describes a state the user has
    // since replaced, so it is dropped.
    uint32_t status_generation_ = 0;
    // A Forget tap's status read is in flight: further taps issue none, so one
    // tap opens at most one confirm dialog. Cleared by the answer, and by
    // on_activate() since deactivation drops an answer still in flight.
    bool forget_read_pending_ = false;

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
    // While a radio change is in flight the switch shows the requested state and
    // stays locked until the radio answers, so a second tap cannot queue the
    // opposite change behind the first.
    void set_wlan_toggle_in_flight(bool in_flight);
    void handle_wlan_toggle_off_confirm();
    void handle_wlan_toggle_off_cancel();
    void handle_refresh_clicked();
    void handle_test_network_clicked();
    void handle_add_other_clicked();
    void handle_network_item_clicked(lv_event_t* e);
    void handle_network_settings_forget();
    void handle_country_changed(lv_event_t* e);
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
    // Reads the radio's country off the UI thread and applies it to the
    // country row: selection, visibility and the 5 GHz hint.
    void refresh_country();
    // Sets the 2.4 GHz-only indicator from the radio's band support, which a
    // scan can raise after the overlay opened.
    void refresh_band_capability();
    // What the closed country dropdown reads for selection @p index.
    void show_country_choice(int index, bool hint_5ghz);
    // Fills the country list in the current language and re-shows the choice.
    void fill_country_options();
    static void on_country_language_changed(lv_event_t* e);
    // Removes the dropdown's language handler, whose user_data is this object.
    void detach_country_dropdown();
    lv_obj_t* country_dropdown_ = nullptr;
    std::string country_button_text_;
    bool country_hint_5ghz_ = false; // 5 GHz radio left in the world domain

    /// Ethernet refresh coalescing: at most one daemon probe in flight, with
    /// one trailing refresh armed while one runs (a flapping link or a
    /// failed-join retry burst collapses to one round trip, not N).
    bool eth_refresh_in_flight_ = false;
    bool eth_refresh_trailing_ = false;
    void update_test_state(NetworkTester::TestState state, const NetworkTester::TestResult& result);
    // Caches the scan and rebuilds the rows once an async status read says
    // which network is connected: a status read can block for seconds on
    // wpa_supplicant, so none happens on the UI thread.
    void populate_network_list(const std::vector<WiFiNetwork>& networks);
    void build_network_list(const std::vector<WiFiNetwork>& networks,
                            const WifiBackend::ConnectionStatus& status);
    void apply_wifi_status(const WifiBackend::ConnectionStatus& status);
    void confirm_forget(const std::string& ssid);
    void clear_network_list();
    void show_placeholder(bool show);
    void update_signal_icons(lv_obj_t* item, int icon_state);

    void handle_network_test_close();

    // Hidden network modal callbacks
    void handle_hidden_cancel_clicked();
    void handle_hidden_connect_clicked();

    // Password modal methods
    void show_password_modal(const char* ssid);
    void hide_password_modal();

    void handle_password_cancel_clicked();
    void handle_password_connect_clicked();
};

// ============================================================================
// Global Instance Access
// ============================================================================

/// The process-wide NetworkSettingsOverlay, built on first use.
inline NetworkSettingsOverlay& get_network_settings_overlay() {
    return helix::lazy_global<NetworkSettingsOverlay>("NetworkSettingsOverlay");
}
