// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "platform_info.h"
#include "subject_managed_panel.h"
#include "wizard_step.h"

#include <memory>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class WiFiManager;
}
class EthernetManager;
struct WiFiNetwork;

/**
 * @file ui_wizard_wifi.h
 * @brief Wizard WiFi setup step - network configuration and connection
 *
 * Handles WiFi configuration during first-run wizard:
 * - WiFi on/off toggle
 * - Network scanning and selection
 * - Password entry for secured networks
 * - Connection status feedback
 * - Ethernet status display
 *
 * ## Class-Based Architecture (Phase 6)
 *
 * This step has been migrated from function-based to class-based design:
 * - Instance members instead of static globals
 * - Shared pointer WiFiManager for async callback safety
 * - Static trampolines for LVGL event callbacks
 * - Global singleton getter for backwards compatibility
 *
 * ## Subject Bindings (7 total):
 *
 * - wifi_enabled (int) - 0=off, 1=on
 * - wifi_status (string) - Status message
 * - ethernet_status (string) - Ethernet connection status
 * - wifi_scanning (int) - 0=not scanning, 1=scanning
 * - wifi_password_modal_ssid (string) - SSID for password modal
 * - wifi_connecting (int) - 0=idle, 1=connecting
 * - wifi_hardware_available (int) - 0=unavailable, 1=available
 *
 * Initialization Order (CRITICAL):
 *   1. Register XML components (wizard_wifi_setup.xml, wifi_password_modal.xml)
 *   2. init_subjects()
 *   3. register_callbacks()
 *   4. create(parent)
 *   5. init_wifi_manager()
 */

/**
 * @class WizardWifiStep
 * @brief WiFi setup step for the first-run wizard
 *
 * Manages WiFi network discovery, selection, and connection.
 * Handles password entry via modal dialog for secured networks.
 */
class WizardWifiStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::Wifi;
    }
    const char* component_name() const override {
        return "wizard_wifi_setup";
    }
    const char* log_name() const override {
        return "WiFi Screen";
    }
    bool should_skip(const helix::wizard::StepContext& ctx) const override {
        return helix::is_android_platform() || ctx.is_subsequent_printer;
    }

    WizardWifiStep();
    ~WizardWifiStep();

    // Non-copyable, non-movable (singleton with lv_subject_t members that
    // contain internal linked lists — moving corrupts observer pointers)
    WizardWifiStep(const WizardWifiStep&) = delete;
    WizardWifiStep& operator=(const WizardWifiStep&) = delete;
    WizardWifiStep(WizardWifiStep&&) = delete;
    WizardWifiStep& operator=(WizardWifiStep&&) = delete;

    /**
     * @brief Initialize reactive subjects
     *
     * Creates and registers 8 subjects with defaults.
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks:
     * - on_wifi_toggle_changed
     * - on_network_item_clicked
     * - on_wifi_password_cancel
     * - on_wifi_password_connect
     */
    void register_callbacks() override;

    /**
     * @brief Create the WiFi setup UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Initialize WiFi and Ethernet managers
     *
     * Sets up WiFiManager callbacks for network scanning and connection.
     */
    void init_wifi_manager();

    /**
     * @brief Cleanup resources
     *
     * Stops scanning, destroys managers, and resets UI references.
     */
    void cleanup() override;

    /**
     * @brief Show password entry modal for secured network
     *
     * @param ssid Network SSID to connect to
     */
    void show_password_modal(const char* ssid);

    /**
     * @brief Hide password entry modal
     */
    void hide_password_modal();

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "WiFi Screen";
    }

  private:
    friend class WizardWifiStepTestAccess;

    // Screen instances
    lv_obj_t* screen_root_ = nullptr;
    lv_obj_t* password_modal_ = nullptr;
    lv_obj_t* network_list_container_ = nullptr;

    // Subjects (8 total - visibility controlled by Modal system)
    lv_subject_t wifi_enabled_;
    lv_subject_t wifi_status_;
    lv_subject_t wifi_ip_;
    lv_subject_t wifi_mac_;
    lv_subject_t ethernet_status_;
    lv_subject_t ethernet_mac_;
    lv_subject_t wifi_scanning_;
    lv_subject_t wifi_password_modal_ssid_;
    lv_subject_t wifi_connecting_;
    lv_subject_t wifi_hardware_available_;

    // String buffers (must be persistent)
    char wifi_status_buffer_[64];
    char wifi_ip_buffer_[32];
    char wifi_mac_buffer_[32];
    char ethernet_status_buffer_[64];
    char ethernet_mac_buffer_[32];
    char wifi_password_modal_ssid_buffer_[64];

    // WiFiManager and EthernetManager (shared_ptr for async safety)
    std::shared_ptr<helix::WiFiManager> wifi_manager_;
    std::unique_ptr<EthernetManager> ethernet_manager_;

    // Current network selection for password modal
    char current_ssid_[64];
    bool current_network_is_secured_ = false;

    // State tracking
    bool subjects_initialized_ = false;
    bool cleanup_called_ = false; // Set true in cleanup() to invalidate pending callbacks
    bool scan_started_ = false;   // Ensures the bringup scan is kicked exactly once

    // Async callback safety — tokens expire when cleanup() invalidates lifetime
    helix::AsyncLifetimeGuard lifetime_;

    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // Cached networks for async UI update
    std::vector<WiFiNetwork> cached_networks_;

    // Event handler implementations
    void handle_wifi_toggle_changed(lv_event_t* e);
    void handle_network_item_clicked(lv_event_t* e);
    void handle_modal_cancel_clicked();
    void handle_modal_connect_clicked();

    // Helper functions
    void update_wifi_status(const char* status);
    void update_wifi_ip(const char* ip);
    void update_ethernet_status();

    // Apply the current (already-initialized) WiFi backend state to the UI.
    // Called once during init_wifi_manager() and again on every backend
    // state change via add_state_observer(). Non-blocking: never calls
    // set_enabled(). Runs on the UI thread (state observer defers via token).
    void apply_wifi_backend_state();
    void populate_network_list(const std::vector<WiFiNetwork>& networks);
    void clear_network_list();

    // Static trampolines for LVGL callbacks
    static void network_item_delete_cb(lv_event_t* e);
    static void on_wifi_toggle_changed_static(lv_event_t* e);
    static void on_network_item_clicked_static(lv_event_t* e);
    static void on_modal_cancel_clicked_static(lv_event_t* e);
    static void on_modal_connect_clicked_static(lv_event_t* e);

    /// Drops password_modal_ when anything other than hide_password_modal()
    /// destroys the dialog, so the deferred connect result cannot walk a freed
    /// tree (prestonbrown/helixscreen#1579).
    static void on_modal_deleted(lv_event_t* e);

    /// Uninstalls on_modal_deleted and clears password_modal_. Teardown must
    /// run this while the step is still valid: modal_hide() only starts an exit
    /// animation, so the dialog tree outlives the step that the handler writes
    /// through.
    void stop_watching_password_modal();

    // Static helpers
    static const char* get_status_text(const char* status_name);
    static const char* get_wifi_signal_icon(int signal_strength, bool is_secured);
};

// ============================================================================
// Global Instance Access
// ============================================================================

/**
 * @brief Get the global WizardWifiStep instance
 *
 * Creates the instance on first call. Used by wizard framework.
 */
WizardWifiStep* get_wizard_wifi_step();
