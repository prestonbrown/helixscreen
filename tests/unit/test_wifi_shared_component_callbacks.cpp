// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wifi_shared_component_callbacks.cpp
 * @brief The wizard WiFi step and the network settings overlay share the
 *        wifi_network_item and wifi_password_modal components; each owner's
 *        rows and buttons must reach that owner's handlers.
 */

#include "ui_overlay_network_settings.h"
#include "ui_wizard_wifi.h"

#include "../lvgl_ui_test_fixture.h"

#include "../catch_amalgamated.hpp"

namespace {

bool has_cb(lv_obj_t* obj, lv_event_cb_t cb) {
    uint32_t count = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i)) == cb)
            return true;
    }
    return false;
}

lv_obj_t* make_row(lv_obj_t* parent, const char* click_callback) {
    const char* attrs[] = {"click_callback", click_callback, nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "wifi_network_item", attrs));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "wifi rows and password buttons dispatch to their own owner",
                 "[wifi][callbacks]") {
    WizardWifiStep wizard;
    wizard.register_callbacks();
    get_network_settings_overlay().register_callbacks();

    SECTION("network rows") {
        lv_obj_t* wizard_row = make_row(test_screen(), "on_wizard_wifi_network_clicked");
        lv_obj_t* overlay_row = make_row(test_screen(), "on_network_settings_item_clicked");
        REQUIRE(wizard_row != nullptr);
        REQUIRE(overlay_row != nullptr);

        lv_event_cb_t wizard_cb = lv_xml_get_event_cb(nullptr, "on_wizard_wifi_network_clicked");
        lv_event_cb_t overlay_cb = lv_xml_get_event_cb(nullptr, "on_network_settings_item_clicked");
        REQUIRE(wizard_cb != nullptr);
        REQUIRE(overlay_cb != nullptr);
        CHECK(wizard_cb != overlay_cb);
        CHECK(has_cb(wizard_row, wizard_cb));
        CHECK(has_cb(overlay_row, overlay_cb));
        CHECK_FALSE(has_cb(wizard_row, overlay_cb));
        CHECK_FALSE(has_cb(overlay_row, wizard_cb));
    }

    SECTION("password modal buttons") {
        const char* wizard_attrs[] = {"cancel_callback", "on_wizard_wifi_password_cancel",
                                      "connect_callback", "on_wizard_wifi_password_connect",
                                      nullptr};
        const char* overlay_attrs[] = {"cancel_callback", "on_network_settings_password_cancel",
                                       "connect_callback", "on_network_settings_password_connect",
                                       nullptr};
        lv_obj_t* wizard_modal = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "wifi_password_modal", wizard_attrs));
        lv_obj_t* overlay_modal = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "wifi_password_modal", overlay_attrs));
        REQUIRE(wizard_modal != nullptr);
        REQUIRE(overlay_modal != nullptr);

        struct Pair {
            const char* button;
            const char* wizard_name;
            const char* overlay_name;
        };
        for (const Pair& p : {Pair{"modal_cancel_btn", "on_wizard_wifi_password_cancel",
                                   "on_network_settings_password_cancel"},
                              Pair{"modal_connect_btn", "on_wizard_wifi_password_connect",
                                   "on_network_settings_password_connect"}}) {
            lv_event_cb_t w = lv_xml_get_event_cb(nullptr, p.wizard_name);
            lv_event_cb_t o = lv_xml_get_event_cb(nullptr, p.overlay_name);
            REQUIRE(w != nullptr);
            REQUIRE(o != nullptr);
            CHECK(w != o);
            CHECK_FALSE(has_cb(lv_obj_find_by_name(wizard_modal, p.button), o));
            CHECK_FALSE(has_cb(lv_obj_find_by_name(overlay_modal, p.button), w));
            CHECK(has_cb(lv_obj_find_by_name(wizard_modal, p.button), w));
            CHECK(has_cb(lv_obj_find_by_name(overlay_modal, p.button), o));
        }
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "wifi password modal withdraws Connect while connecting",
                 "[wifi][callbacks]") {
    auto& overlay = get_network_settings_overlay();
    overlay.init_subjects();
    overlay.register_callbacks();
    lv_subject_t* connecting = lv_xml_get_subject(nullptr, "wifi_connecting");
    REQUIRE(connecting != nullptr);

    const char* attrs[] = {"cancel_callback", "on_network_settings_password_cancel",
                           "connect_callback", "on_network_settings_password_connect", nullptr};
    auto* modal =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "wifi_password_modal", attrs));
    REQUIRE(modal != nullptr);
    lv_obj_t* connect = lv_obj_find_by_name(modal, "modal_connect_btn");
    lv_obj_t* cancel = lv_obj_find_by_name(modal, "modal_cancel_btn");
    REQUIRE(connect != nullptr);
    REQUIRE(cancel != nullptr);
    // The divider between the two buttons sits directly before Connect.
    lv_obj_t* divider = lv_obj_get_child(lv_obj_get_parent(connect), lv_obj_get_index(connect) - 1);
    REQUIRE(divider != nullptr);

    lv_subject_set_int(connecting, 1);
    CHECK(lv_obj_is_hidden(connect));
    CHECK(lv_obj_is_hidden(divider));
    CHECK_FALSE(lv_obj_is_hidden(cancel));

    lv_subject_set_int(connecting, 0);
    CHECK_FALSE(lv_obj_is_hidden(connect));
    CHECK_FALSE(lv_obj_is_hidden(divider));
}
