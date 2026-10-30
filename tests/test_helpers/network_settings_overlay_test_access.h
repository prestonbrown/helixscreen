// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_overlay_network_settings.h"

#include "wifi_backend.h"

#include <cstring>
#include <string>
#include <vector>

/**
 * @brief Reaches the overlay's cached modal pointers.
 *
 * These are the handles the async connect callbacks null-check and then walk
 * (#1341). A test needs to see them directly, because "the handle is null once
 * its modal dies" is the whole invariant; a dangling pointer would be
 * indistinguishable from a live one at the call site.
 */
class NetworkSettingsOverlayTestAccess {
  public:
    static helix::ui::WidgetRef& password_modal(NetworkSettingsOverlay& o) {
        return o.password_modal_;
    }
    static helix::ui::WidgetRef& hidden_network_modal(NetworkSettingsOverlay& o) {
        return o.hidden_network_modal_;
    }
    static helix::ui::WidgetRef& test_modal(NetworkSettingsOverlay& o) {
        return o.test_modal_;
    }
    static helix::ui::WidgetRef& step_widget(NetworkSettingsOverlay& o) {
        return o.step_widget_;
    }
    /// The overlay's manager pointer — tests point it at a locally owned
    /// WiFiManager instead of the process-global singleton, before create().
    static std::shared_ptr<helix::WiFiManager>& wifi_manager(NetworkSettingsOverlay& o) {
        return o.wifi_manager_;
    }
    /// The wifi_connected subject (0/1) — the observable a transport refresh
    /// must move.
    static lv_subject_t& wifi_connected(NetworkSettingsOverlay& o) {
        return o.wifi_connected_;
    }
    /// Apply a status read's answer the way the async read does.
    static void apply_wifi_status(NetworkSettingsOverlay& o,
                                  const WifiBackend::ConnectionStatus& status) {
        o.apply_wifi_status(status);
    }
    /// The cached country dropdown, which async country reads write to.
    static lv_obj_t* country_dropdown(NetworkSettingsOverlay& o) {
        return o.country_dropdown_;
    }
    /// The country dropdown's selection subject (0 = not set).
    static lv_subject_t& wifi_country_index(NetworkSettingsOverlay& o) {
        return o.wifi_country_index_;
    }
    /// The SSID the password modal is about. The handlers read it rather than
    /// taking it as an argument, so a test has to seed it.
    static void set_current_ssid(NetworkSettingsOverlay& o, const std::string& ssid) {
        strncpy(o.current_ssid_, ssid.c_str(), sizeof(o.current_ssid_) - 1);
        o.current_ssid_[sizeof(o.current_ssid_) - 1] = '\0';
    }
    /// Fold a radio answer back in, as the async completion and the backstop do.
    static void finish_wlan_toggle(NetworkSettingsOverlay& o, bool requested, bool success,
                                   bool actual) {
        o.finish_wlan_toggle(requested, success, actual);
    }
    /// The wifi_hardware_available subject (0/1).
    static lv_subject_t& wifi_hardware_available(NetworkSettingsOverlay& o) {
        return o.wifi_hardware_available_;
    }
    /// The wifi_toggle_locked subject (0/1) the switch's disabled state binds to.
    static lv_subject_t& wifi_toggle_locked(NetworkSettingsOverlay& o) {
        return o.wifi_toggle_locked_;
    }
    /// The wifi_enabled subject (0/1) the switch's checked state binds to.
    static lv_subject_t& wifi_enabled(NetworkSettingsOverlay& o) {
        return o.wifi_enabled_;
    }
    /// The production handler behind the WiFi switch's value_changed.
    static void wlan_toggle_changed(NetworkSettingsOverlay& o, lv_event_t* e) {
        o.handle_wlan_toggle_changed(e);
    }
    /// The production handler behind the password modal's Connect button.
    static void password_connect_clicked(NetworkSettingsOverlay& o) {
        o.handle_password_connect_clicked();
    }
    /// Open the hidden-network modal, exactly as the "Add other network"
    /// button does.
    static void add_other_clicked(NetworkSettingsOverlay& o) {
        o.handle_add_other_clicked();
    }
    /// The production handler behind the hidden-network modal's Connect button.
    static void hidden_connect_clicked(NetworkSettingsOverlay& o) {
        o.handle_hidden_connect_clicked();
    }
    /// Build the scan rows synchronously, with no network connected — the click
    /// handler reads per-row data only this path attaches.
    static void populate(NetworkSettingsOverlay& o, const std::vector<WiFiNetwork>& networks) {
        o.build_network_list(networks, WifiBackend::ConnectionStatus{});
    }
    /// What a completed scan does: the rows are built once the connection
    /// read lands.
    static void scan_completed(NetworkSettingsOverlay& o,
                               const std::vector<WiFiNetwork>& networks) {
        o.populate_network_list(networks);
    }
    static void clear_network_list(NetworkSettingsOverlay& o) {
        o.clear_network_list();
    }
    static void update_wifi_status(NetworkSettingsOverlay& o) {
        o.update_wifi_status();
    }
    static void forget_clicked(NetworkSettingsOverlay& o) {
        o.handle_network_settings_forget();
    }
    static const std::string& pending_forget_ssid(NetworkSettingsOverlay& o) {
        return o.pending_forget_ssid_;
    }
    static std::string ssid(NetworkSettingsOverlay& o) {
        return o.ssid_buffer_;
    }
    static std::string ip(NetworkSettingsOverlay& o) {
        return o.ip_buffer_;
    }
    static std::string mac(NetworkSettingsOverlay& o) {
        return o.mac_buffer_;
    }
    static lv_obj_t* networks_list(NetworkSettingsOverlay& o) {
        return o.networks_list_;
    }
    /// The wifi_only_24ghz subject (1 = the 2.4 GHz-only indicator shows).
    static lv_subject_t& wifi_only_24ghz(NetworkSettingsOverlay& o) {
        return o.wifi_only_24ghz_;
    }
    /// Whether the radio's country has been read since the overlay opened.
    static bool radio_country_read(NetworkSettingsOverlay& o) {
        return o.radio_country_.has_value();
    }
    /// Hand a scan's results to the overlay, as the scan callback does.
    static void populate_network_list(NetworkSettingsOverlay& o,
                                      const std::vector<WiFiNetwork>& networks) {
        o.populate_network_list(networks);
    }
};
