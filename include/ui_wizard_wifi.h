// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <memory>
#include <string>
#include <vector>

// Forward declarations
class WiFiManager;
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
 * - wifi_password_modal_visible (int) - 0=hidden, 1=visible
 * - wifi_password_modal_ssid (string) - SSID for password modal
 * - wifi_connecting (int) - 0=idle, 1=connecting
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
class WizardWifiStep {
  public:
    WizardWifiStep();
    ~WizardWifiStep();

    // Non-copyable
    WizardWifiStep(const WizardWifiStep&) = delete;
    WizardWifiStep& operator=(const WizardWifiStep&) = delete;

    // Movable
    WizardWifiStep(WizardWifiStep&& other) noexcept;
    WizardWifiStep& operator=(WizardWifiStep&& other) noexcept;

    /**
     * @brief Initialize reactive subjects
     *
     * Creates and registers 7 subjects with defaults.
     */
    void init_subjects();

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks:
     * - on_wifi_toggle_changed
     * - on_network_item_clicked
     * - on_modal_cancel_clicked
     * - on_modal_connect_clicked
     */
    void register_callbacks();

    /**
     * @brief Register responsive constants for WiFi network list
     *
     * Registers WiFi-specific constants to component scopes.
     */
    void register_responsive_constants();

    /**
     * @brief Create the WiFi setup UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent);

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
    void cleanup();

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
    const char* get_name() const { return "WiFi Screen"; }

  private:
    // Screen instances
    lv_obj_t* screen_root_ = nullptr;
    lv_obj_t* password_modal_ = nullptr;
    lv_obj_t* network_list_container_ = nullptr;

    // Subjects (7 total)
    lv_subject_t wifi_enabled_;
    lv_subject_t wifi_status_;
    lv_subject_t ethernet_status_;
    lv_subject_t wifi_scanning_;
    lv_subject_t wifi_password_modal_visible_;
    lv_subject_t wifi_password_modal_ssid_;
    lv_subject_t wifi_connecting_;

    // String buffers (must be persistent)
    char wifi_status_buffer_[64];
    char ethernet_status_buffer_[64];
    char wifi_password_modal_ssid_buffer_[64];

    // WiFiManager and EthernetManager (shared_ptr for async safety)
    std::shared_ptr<WiFiManager> wifi_manager_;
    std::unique_ptr<EthernetManager> ethernet_manager_;

    // Current network selection for password modal
    char current_ssid_[64];
    bool current_secured_ = false;

    // Theme-aware colors
    lv_color_t wifi_item_bg_color_;
    lv_color_t wifi_item_text_color_;

    // State tracking
    bool subjects_initialized_ = false;

    // Event handler implementations
    void handle_wifi_toggle_changed(lv_event_t* e);
    void handle_network_item_clicked(lv_event_t* e);
    void handle_modal_cancel_clicked();
    void handle_modal_connect_clicked();

    // Helper functions
    void update_wifi_status(const char* status);
    void update_ethernet_status();
    void populate_network_list(const std::vector<WiFiNetwork>& networks);
    void clear_network_list();
    void init_wifi_item_colors();
    void apply_connected_network_highlight(lv_obj_t* item);

    // Static trampolines for LVGL callbacks
    static void on_wifi_toggle_changed_static(lv_event_t* e);
    static void on_network_item_clicked_static(lv_event_t* e);
    static void on_modal_cancel_clicked_static(lv_event_t* e);
    static void on_modal_connect_clicked_static(lv_event_t* e);

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

/**
 * @brief Destroy the global WizardWifiStep instance
 *
 * Call during application shutdown.
 */
void destroy_wizard_wifi_step();

// ============================================================================
// Deprecated Legacy API
// ============================================================================

/**
 * @deprecated Use get_wizard_wifi_step()->init_subjects()
 */
[[deprecated("Use get_wizard_wifi_step()->init_subjects()")]]
void ui_wizard_wifi_init_subjects();

/**
 * @deprecated Use get_wizard_wifi_step()->register_callbacks()
 */
[[deprecated("Use get_wizard_wifi_step()->register_callbacks()")]]
void ui_wizard_wifi_register_callbacks();

/**
 * @deprecated Use get_wizard_wifi_step()->register_responsive_constants()
 */
[[deprecated("Use get_wizard_wifi_step()->register_responsive_constants()")]]
void ui_wizard_wifi_register_responsive_constants();

/**
 * @deprecated Use get_wizard_wifi_step()->create()
 */
[[deprecated("Use get_wizard_wifi_step()->create()")]]
lv_obj_t* ui_wizard_wifi_create(lv_obj_t* parent);

/**
 * @deprecated Use get_wizard_wifi_step()->init_wifi_manager()
 */
[[deprecated("Use get_wizard_wifi_step()->init_wifi_manager()")]]
void ui_wizard_wifi_init_wifi_manager();

/**
 * @deprecated Use get_wizard_wifi_step()->cleanup()
 */
[[deprecated("Use get_wizard_wifi_step()->cleanup()")]]
void ui_wizard_wifi_cleanup();

/**
 * @deprecated Use get_wizard_wifi_step()->show_password_modal()
 */
[[deprecated("Use get_wizard_wifi_step()->show_password_modal()")]]
void ui_wizard_wifi_show_password_modal(const char* ssid);

/**
 * @deprecated Use get_wizard_wifi_step()->hide_password_modal()
 */
[[deprecated("Use get_wizard_wifi_step()->hide_password_modal()")]]
void ui_wizard_wifi_hide_password_modal();
