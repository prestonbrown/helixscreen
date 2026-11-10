// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_connect_failure.cpp
 * @brief What the network overlay puts in front of a user whose Wi-Fi join
 *        failed.
 *
 * Every one of the overlay's three join paths has a different surface - the
 * password modal's status label, the hidden-network modal's error label
 * (test_network_settings_hidden_connect.cpp), and a toast for an open network,
 * which opens no modal at all. All three must
 * carry the reason the join failed. A canned "check your password" is correct
 * for exactly one WiFiResult; substituting it for a daemon that is down, a
 * network out of range or a transport that took the link tells a user whose
 * password is correct to retype it indefinitely.
 */

#include "ui_modal.h"
#include "ui_overlay_network_settings.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
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

TEST_CASE_METHOD(ConnectFailureFixture, "Tapping a network that fails to join tells the user why",
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
    WiFiNetwork secured_net;
    secured_net.ssid = "#LockedNet"; // a leading '#' must reach the label as typed
    secured_net.is_secured = true;
    secured_net.security_type = "WPA2";
    secured_net.signal_strength = 30;
    Access::populate(overlay, {open_net, secured_net});

    REQUIRE(overlay.get_root() != nullptr);

    // Row names carry a process-wide counter, so a row is found by its
    // rendered SSID rather than by guessing an index.
    auto find_row = [&](const std::string& ssid) -> lv_obj_t* {
        for (uint32_t i = 0; i < 64; i++) {
            char name[32];
            snprintf(name, sizeof(name), "network_item_%u", i);
            lv_obj_t* candidate = lv_obj_find_by_name(overlay.get_root(), name);
            lv_obj_t* label = candidate ? lv_obj_find_by_name(candidate, "ssid_label") : nullptr;
            if (label && std::string(lv_label_get_text(label)) == ssid) {
                return candidate;
            }
        }
        return nullptr;
    };
    // The one signal icon a row shows, by name.
    auto shown_signal_icon = [](lv_obj_t* row) {
        std::string shown;
        for (const char* name : {"sig_1", "sig_2", "sig_3", "sig_4", "sig_1_lock", "sig_2_lock",
                                 "sig_3_lock", "sig_4_lock"}) {
            lv_obj_t* icon = lv_obj_find_by_name(row, name);
            REQUIRE(icon != nullptr);
            if (!lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN)) {
                REQUIRE(shown.empty());
                shown = name;
            }
        }
        return shown;
    };
    auto security_text = [](lv_obj_t* row) {
        lv_obj_t* label = lv_obj_find_by_name(row, "security_label");
        REQUIRE(label != nullptr);
        return std::string(lv_label_get_text(label));
    };

    lv_obj_t* locked_row = find_row(secured_net.ssid);
    REQUIRE(locked_row != nullptr);
    CHECK(security_text(locked_row) == "WPA2");
    CHECK(shown_signal_icon(locked_row).find("_lock") != std::string::npos);

    lv_obj_t* list_row = find_row(kUnknownSsid);
    REQUIRE(list_row != nullptr);
    CHECK(security_text(list_row).empty());
    const std::string open_icon = shown_signal_icon(list_row);
    CHECK_FALSE(open_icon.empty());
    CHECK(open_icon.find("_lock") == std::string::npos);

    lv_obj_send_event(list_row, LV_EVENT_CLICKED, nullptr);
    REQUIRE(helix::WiFiManagerTestAccess::settle(*wm));

    // An open network opens no modal, so a toast is the only surface. Silence
    // here is a tap that visibly does nothing at all.
    REQUIRE_FALSE(toasts.errors.empty());
    bool named_the_reason = false;
    for (const auto& e : toasts.errors) {
        if (e.find(kUnknownSsid) != std::string::npos)
            named_the_reason = true;
    }
    REQUIRE(named_the_reason);

    // A secured row opens the password modal, whose status line says why
    // Connect did nothing, then carries the backend's reason rather than a
    // canned password prompt.
    lv_obj_send_event(locked_row, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* modal = ModalStack::instance().top_dialog();
    REQUIRE(modal != nullptr);
    lv_obj_t* status = lv_obj_find_by_name(modal, "modal_status");
    lv_obj_t* connect = lv_obj_find_by_name(modal, "modal_connect_btn");
    REQUIRE(status != nullptr);
    REQUIRE(connect != nullptr);
    REQUIRE(lv_obj_has_flag(status, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(connect, LV_EVENT_CLICKED, nullptr);
    CHECK(label_text(modal, "modal_status") == lv_tr("Password cannot be empty"));
    CHECK_FALSE(lv_obj_has_flag(status, LV_OBJ_FLAG_HIDDEN));

    lv_textarea_set_text(lv_obj_find_by_name(modal, "password_input"), "some-password");
    lv_obj_send_event(connect, LV_EVENT_CLICKED, nullptr);
    REQUIRE(helix::WiFiManagerTestAccess::settle(*wm));
    const std::string shown = label_text(modal, "modal_status");
    // Not empty first: an empty label would pass the inequality having tested nothing.
    REQUIRE_FALSE(shown.empty());
    CHECK(shown != lv_tr("Connection failed. Check password."));
    CHECK(shown.find(secured_net.ssid) != std::string::npos);
    CHECK_FALSE(lv_obj_has_flag(status, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(lv_obj_find_by_name(modal, "modal_cancel_btn"), LV_EVENT_CLICKED, nullptr);

    Access::wifi_manager(overlay) = previous;
}
