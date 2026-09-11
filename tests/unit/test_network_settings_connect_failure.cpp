// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_connect_failure.cpp
 * @brief What the network overlay puts in front of a user whose Wi-Fi join
 *        failed.
 *
 * Every one of the overlay's three join paths has a different surface - the
 * password modal's status label, the hidden-network modal's error label, and
 * a toast for an open network, which opens no modal at all. All three must
 * carry the reason the join failed. A canned "check your password" is correct
 * for exactly one WiFiResult; substituting it for a daemon that is down, a
 * network out of range or a transport that took the link tells a user whose
 * password is correct to retype it indefinitely.
 */

#include "ui_overlay_network_settings.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../ui_test_utils.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace {

/// An SSID no mock backend knows, so connect_network() refuses it
/// synchronously with its own NETWORK_NOT_FOUND reason - a failure that is
/// emphatically NOT a credential rejection.
constexpr const char* kUnknownSsid = "NoSuchNetwork";

class ConnectFailureFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;

    /// A locally owned manager over an empty mock backend.
    std::shared_ptr<helix::WiFiManager> make_manager() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        auto backend = std::make_unique<WifiBackendMock>();
        REQUIRE(backend->start().success());
        auto wm = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
        wm->init_self_reference(wm);
        return wm;
    }

    /// A stand-in for a modal built from XML: the widgets the handler looks
    /// up by name, and nothing else.
    lv_obj_t* make_modal(const char* input_name, const char* status_name) {
        lv_obj_t* modal = lv_obj_create(lv_screen_active());
        REQUIRE(modal != nullptr);
        lv_obj_t* input = lv_textarea_create(modal);
        lv_obj_set_name(input, input_name);
        lv_textarea_set_text(input, "some-password");
        lv_obj_t* status = lv_label_create(modal);
        lv_obj_set_name(status, status_name);
        lv_label_set_text(status, "");
        return modal;
    }

    static std::string label_text(lv_obj_t* modal, const char* name) {
        lv_obj_t* label = lv_obj_find_by_name(modal, name);
        REQUIRE(label != nullptr);
        const char* text = lv_label_get_text(label);
        return text ? text : "";
    }
};

/// Captures the error toasts raised for the duration of one test.
struct ErrorToastCapture {
    std::vector<std::string> errors;

    ErrorToastCapture() {
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& msg) { errors.push_back(msg); });
    }
    ~ErrorToastCapture() {
        helix::ui::set_test_notification_error_hook(nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(ConnectFailureFixture,
                 "Password modal shows the backend's reason, not a password prompt",
                 "[network_settings][1542][wifi]") {
    auto wm = make_manager();
    auto overlay = std::make_unique<NetworkSettingsOverlay>();
    overlay->init_subjects();
    overlay->register_callbacks();
    Access::wifi_manager(*overlay) = wm;
    REQUIRE(overlay->create(test_screen()) != nullptr);

    lv_obj_t* modal = make_modal("password_input", "modal_status");
    Access::password_modal(*overlay) = modal;
    Access::set_current_ssid(*overlay, kUnknownSsid);

    Access::password_connect_clicked(*overlay);
    helix::ui::UpdateQueue::instance().drain();

    const std::string shown = label_text(modal, "modal_status");
    // The reason reached the label. Asserting the precondition first: an
    // empty label would mean the failure branch never ran, and then the
    // inequality below would pass having tested nothing.
    REQUIRE_FALSE(shown.empty());
    REQUIRE(shown != lv_tr("Connection failed. Check password."));
    REQUIRE(shown.find(kUnknownSsid) != std::string::npos);

    Access::password_modal(*overlay) = nullptr;
}

TEST_CASE_METHOD(ConnectFailureFixture,
                 "Hidden-network modal shows the backend's reason, not a credential prompt",
                 "[network_settings][1542][wifi]") {
    auto wm = make_manager();
    auto overlay = std::make_unique<NetworkSettingsOverlay>();
    overlay->init_subjects();
    overlay->register_callbacks();
    Access::wifi_manager(*overlay) = wm;
    REQUIRE(overlay->create(test_screen()) != nullptr);

    // The hidden-network form: an SSID field, and security left at "None" so
    // no password is required of it.
    lv_obj_t* modal = lv_obj_create(lv_screen_active());
    REQUIRE(modal != nullptr);
    lv_obj_t* ssid_input = lv_textarea_create(modal);
    lv_obj_set_name(ssid_input, "ssid_input");
    lv_textarea_set_text(ssid_input, kUnknownSsid);
    lv_obj_t* error_label = lv_label_create(modal);
    lv_obj_set_name(error_label, "error_label");
    lv_label_set_text(error_label, "");
    Access::hidden_network_modal(*overlay) = modal;

    Access::hidden_connect_clicked(*overlay);
    helix::ui::UpdateQueue::instance().drain();

    const std::string shown = label_text(modal, "error_label");
    REQUIRE_FALSE(shown.empty());
    REQUIRE(shown != lv_tr("Connection failed. Check credentials."));
    REQUIRE(shown.find(kUnknownSsid) != std::string::npos);

    Access::hidden_network_modal(*overlay) = nullptr;
}

TEST_CASE_METHOD(ConnectFailureFixture, "Tapping an open network that fails to join tells the user",
                 "[network_settings][1542][wifi]") {
    ErrorToastCapture toasts;

    // The row's click handler routes through the process-wide overlay, so
    // this path can only be driven on that instance.
    auto& overlay = get_network_settings_overlay();
    auto wm = make_manager();
    auto previous = Access::wifi_manager(overlay);
    Access::wifi_manager(overlay) = wm;
    overlay.init_subjects();
    overlay.register_callbacks();
    REQUIRE(overlay.create(test_screen()) != nullptr);

    WiFiNetwork open_net;
    open_net.ssid = kUnknownSsid; // in the list, unknown to the backend
    open_net.is_secured = false;
    open_net.signal_strength = 70;
    Access::populate(overlay, {open_net});

    REQUIRE(overlay.get_root() != nullptr);

    // Row names carry a process-wide counter, so the row is found by its
    // rendered SSID rather than by guessing an index.
    lv_obj_t* list_row = nullptr;
    for (uint32_t i = 0; i < 64 && !list_row; i++) {
        char name[32];
        snprintf(name, sizeof(name), "network_item_%u", i);
        lv_obj_t* candidate = lv_obj_find_by_name(overlay.get_root(), name);
        if (candidate) {
            lv_obj_t* ssid_label = lv_obj_find_by_name(candidate, "ssid_label");
            if (ssid_label && std::string(lv_label_get_text(ssid_label)) == kUnknownSsid) {
                list_row = candidate;
            }
        }
    }
    REQUIRE(list_row != nullptr);

    lv_obj_send_event(list_row, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    // An open network opens no modal, so a toast is the only surface. Silence
    // here is a tap that visibly does nothing at all.
    REQUIRE_FALSE(toasts.errors.empty());
    bool named_the_reason = false;
    for (const auto& e : toasts.errors) {
        if (e.find(kUnknownSsid) != std::string::npos)
            named_the_reason = true;
    }
    REQUIRE(named_the_reason);

    Access::wifi_manager(overlay) = previous;
}
