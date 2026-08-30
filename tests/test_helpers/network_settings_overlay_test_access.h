// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_overlay_network_settings.h"

/**
 * @brief Reaches the overlay's cached modal pointers.
 *
 * These are the pointers #1341 crashed on: raw lv_obj_t* the async connect
 * callbacks null-check and then walk. A test needs to see them directly,
 * because "the pointer is null" is the whole invariant the fix installs — the
 * dangling case is indistinguishable from the live one at the call site.
 */
class NetworkSettingsOverlayTestAccess {
  public:
    static lv_obj_t*& password_modal(NetworkSettingsOverlay& o) {
        return o.password_modal_;
    }
    static lv_obj_t*& hidden_network_modal(NetworkSettingsOverlay& o) {
        return o.hidden_network_modal_;
    }
    static lv_obj_t*& test_modal(NetworkSettingsOverlay& o) {
        return o.test_modal_;
    }
    static lv_obj_t*& step_widget(NetworkSettingsOverlay& o) {
        return o.step_widget_;
    }
    /// Arm the production DELETE handler on an arbitrary object, exactly as
    /// show_password_modal() and friends do.
    static void watch(NetworkSettingsOverlay& o, lv_obj_t* modal) {
        lv_obj_add_event_cb(modal, NetworkSettingsOverlay::on_modal_deleted, LV_EVENT_DELETE, &o);
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
};
