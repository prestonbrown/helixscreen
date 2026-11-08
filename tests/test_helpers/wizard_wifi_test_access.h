// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_wizard_wifi.h"

#include "wifi_backend.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Reaches the wizard WiFi step's password-modal handle.
 *
 * The deferred connect-result callback null-checks this handle and then walks
 * it with lv_obj_find_by_name(). "The handle is null once the dialog dies" is
 * the whole invariant, because a dangling pointer and a live one are
 * indistinguishable at the call site (prestonbrown/helixscreen#1579).
 */
class WizardWifiStepTestAccess {
  public:
    static helix::ui::WidgetRef& password_modal(WizardWifiStep& step) {
        return step.password_modal_;
    }
    /// The step's manager pointer — tests point it at a locally owned
    /// WiFiManager instead of the process-global singleton, since
    /// init_wifi_manager() also builds an EthernetManager and probes with it.
    static std::shared_ptr<helix::WiFiManager>& wifi_manager(WizardWifiStep& step) {
        return step.wifi_manager_;
    }
    /// The wifi_connecting subject (0/1). The deferred connect result clears it
    /// as its first statement, which makes it the observable for whether that
    /// body ran at all.
    static lv_subject_t& wifi_connecting(WizardWifiStep& step) {
        return step.wifi_connecting_;
    }
    /// The SSID the password modal is about. The handler reads it rather than
    /// taking it as an argument, so a test has to seed it.
    static void set_current_ssid(WizardWifiStep& step, const std::string& ssid) {
        strncpy(step.current_ssid_, ssid.c_str(), sizeof(step.current_ssid_) - 1);
        step.current_ssid_[sizeof(step.current_ssid_) - 1] = '\0';
    }
    /// The production handler behind the password modal's Connect button.
    static void password_connect_clicked(WizardWifiStep& step) {
        step.handle_modal_connect_clicked();
    }
    /// The production handler behind the Add Hidden Network button.
    static void add_hidden_clicked(WizardWifiStep& step) {
        step.handle_add_hidden_clicked();
    }

    /// The status-read paths: the backend-state apply, a list refresh after a
    /// scan, and a join's success.
    static void apply_backend_state(WizardWifiStep& step) {
        step.apply_wifi_backend_state();
    }
    static void refresh_list(WizardWifiStep& step, std::vector<WiFiNetwork> networks) {
        step.cached_networks_ = std::move(networks);
        step.refresh_network_list();
    }
    static void announce_connected(WizardWifiStep& step) {
        step.announce_connected();
    }

    static std::string text(lv_subject_t& subject) {
        return lv_subject_get_string(&subject);
    }
    static std::string status(WizardWifiStep& step) {
        return text(step.wifi_status_);
    }
    static std::string ip(WizardWifiStep& step) {
        return text(step.wifi_ip_);
    }
    static std::string mac(WizardWifiStep& step) {
        return text(step.wifi_mac_);
    }
    static lv_subject_t& wifi_enabled(WizardWifiStep& step) {
        return step.wifi_enabled_;
    }
    static void clear_list(WizardWifiStep& step) {
        step.clear_network_list();
    }
    static lv_obj_t* network_list(WizardWifiStep& step) {
        return step.network_list_container_;
    }

    /// Publish the capability subjects from the manager set above, as
    /// init_wifi_manager() does once it has obtained its manager.
    static void publish_wifi_capabilities(WizardWifiStep& step) {
        step.publish_wifi_capabilities();
    }

    /// Drive the production toggle handler for THIS step, from a switch left in
    /// `checked`. The XML trampoline resolves the step through the process-wide
    /// accessor, so an instance-scoped test needs an event carrying its own.
    static void toggle_changed(WizardWifiStep& step, lv_obj_t* toggle, bool checked) {
        if (checked) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            toggle,
            [](lv_event_t* e) {
                auto* self = static_cast<WizardWifiStep*>(lv_event_get_user_data(e));
                self->handle_wifi_toggle_changed(e);
            },
            LV_EVENT_VALUE_CHANGED, &step);
        lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    }
};
