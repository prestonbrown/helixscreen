// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "tips_manager.h"

#include <memory>

// Forward declarations
class WiFiManager;
class TempControlPanel;

/**
 * @brief Home panel - Main dashboard showing printer status and quick actions
 *
 * Displays printer image, temperature, network status, light toggle, and
 * tip of the day with auto-rotation. Responsive sizing based on screen dimensions.
 *
 * @see TipsManager for tip of the day functionality
 */

// Network connection types
typedef enum { NETWORK_WIFI, NETWORK_ETHERNET, NETWORK_DISCONNECTED } network_type_t;

class HomePanel : public PanelBase {
  public:
    /**
     * @brief Construct HomePanel with injected dependencies
     * @param printer_state Reference to PrinterState
     * @param api Pointer to MoonrakerAPI (for light control)
     */
    HomePanel(PrinterState& printer_state, MoonrakerAPI* api);
    ~HomePanel() override;

    void init_subjects() override;
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    void on_activate() override;
    void on_deactivate() override;
    const char* get_name() const override {
        return "Home Panel";
    }
    const char* get_xml_component_name() const override {
        return "home_panel";
    }

    /**
     * @brief Update status text and temperature display
     * @param status_text New status/tip text (nullptr to keep current)
     * @param temp Temperature in degrees Celsius
     */
    void update(const char* status_text, int temp);

    /** @brief Set network status display */
    void set_network(network_type_t type);

    /** @brief Set light state (on=gold, off=grey) */
    void set_light(bool is_on);

    bool get_light_state() const {
        return light_on_;
    }

    /**
     * @brief Reload printer image and LED visibility from config
     *
     * Called after wizard completion to update the home panel with
     * newly configured printer type and LED settings.
     */
    void reload_from_config();

    /**
     * @brief Set reference to TempControlPanel for temperature overlay
     *
     * Must be called before temp icon click handler can work.
     * @param temp_panel Pointer to TempControlPanel instance
     */
    void set_temp_control_panel(TempControlPanel* temp_panel);

  private:
    TempControlPanel* temp_control_panel_ = nullptr;
    lv_subject_t status_subject_;
    lv_subject_t temp_subject_;
    lv_subject_t network_icon_state_; // Integer subject: 0-5 for conditional icon visibility
    lv_subject_t network_label_subject_;
    lv_subject_t printer_type_subject_;
    lv_subject_t printer_host_subject_;
    lv_subject_t printer_info_visible_;

    char status_buffer_[512];
    char temp_buffer_[32];
    char network_label_buffer_[32];
    char printer_type_buffer_[64];
    char printer_host_buffer_[64];

    bool light_on_ = false;
    network_type_t current_network_ = NETWORK_WIFI;
    PrintingTip current_tip_;
    std::string configured_led_;
    lv_timer_t* tip_rotation_timer_ = nullptr;
    lv_timer_t* signal_poll_timer_ = nullptr;   // Polls WiFi signal strength every 5s
    std::shared_ptr<WiFiManager> wifi_manager_; // For signal strength queries

    // Lazily-created overlay panels (owned by LVGL parent, not us)
    lv_obj_t* nozzle_temp_panel_ = nullptr;
    lv_obj_t* network_settings_overlay_ = nullptr;

    void update_tip_of_day();
    int compute_network_icon_state(); // Maps network type + signal → 0-5
    void update_network_icon_state(); // Updates the subject
    static void signal_poll_timer_cb(lv_timer_t* timer);

    void handle_light_toggle();
    void handle_print_card_clicked();
    void handle_tip_text_clicked();
    void handle_tip_rotation_timer();
    void handle_temp_clicked();
    void handle_printer_status_clicked();
    void handle_network_clicked();
    void on_extruder_temp_changed(int temp);
    void on_led_state_changed(int state);

    static void light_toggle_cb(lv_event_t* e);
    static void print_card_clicked_cb(lv_event_t* e);
    static void tip_text_clicked_cb(lv_event_t* e);
    static void temp_clicked_cb(lv_event_t* e);
    static void printer_status_clicked_cb(lv_event_t* e);
    static void network_clicked_cb(lv_event_t* e);
    static void tip_rotation_timer_cb(lv_timer_t* timer);
    static void extruder_temp_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void led_state_observer_cb(lv_observer_t* observer, lv_subject_t* subject);

    ObserverGuard extruder_temp_observer_;
    ObserverGuard led_state_observer_;
};

// Global instance accessor (needed by main.cpp)
HomePanel& get_global_home_panel();
