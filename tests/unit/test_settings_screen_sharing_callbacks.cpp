// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Screen Sharing page's toggles act on the setting they show. The XML names its
// callbacks by string and silently skips one that is not registered, so a toggle
// whose callback went missing stays on screen and does nothing. These tests build
// the page the way the overlay does (callbacks registered, then the XML) and drive
// each toggle.

#include "ui_panel_settings.h"
#include "ui_settings_screen_sharing.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/widget_in_tree.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "remote_screen_settings_manager.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct ScreenSharingCallbacksFixture : LVGLUITestFixture {
    lv_obj_t* root_ = nullptr;

    ScreenSharingCallbacksFixture() {
        setup_printer_data(*Config::get_instance(), json::object());
        SettingsManager::instance().init_subjects();
        auto& m = RemoteScreenSettingsManager::instance();
        m.deinit_subjects();
        m.init_subjects();
        get_global_settings_panel().init_subjects();
        settings::get_screen_sharing_settings_overlay().register_callbacks();
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_screen_sharing_overlay", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    ~ScreenSharingCallbacksFixture() override {
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        helix::ui::UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
        setup_printer_data(*Config::get_instance(), json::object());
        RemoteScreenSettingsManager::instance().on_printer_changed();
    }

    void set_toggle(const char* row, bool on) {
        lv_obj_t* r = lv_obj_find_by_name(root_, row);
        REQUIRE(r != nullptr);
        lv_obj_t* toggle = lv_obj_find_by_name(r, "toggle");
        REQUIRE(toggle != nullptr);
        if (on) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(ScreenSharingCallbacksFixture,
                 "Screen Sharing page: the Share Screen toggle drives the setting",
                 "[settings][remote_screen][screen_sharing_callbacks]") {
    auto& m = RemoteScreenSettingsManager::instance();
    REQUIRE_FALSE(m.get_enabled());

    set_toggle("row_remote_screen", true);
    CHECK(m.get_enabled());

    set_toggle("row_remote_screen", false);
    CHECK_FALSE(m.get_enabled());
}

TEST_CASE_METHOD(ScreenSharingCallbacksFixture,
                 "Screen Sharing page: the webcams toggle drives the setting",
                 "[settings][remote_screen][screen_sharing_callbacks]") {
    auto& m = RemoteScreenSettingsManager::instance();
    set_toggle("row_remote_screen", true);
    REQUIRE(m.get_webcam());

    set_toggle("row_remote_webcam", false);
    CHECK_FALSE(m.get_webcam());

    set_toggle("row_remote_webcam", true);
    CHECK(m.get_webcam());
}

TEST_CASE_METHOD(ScreenSharingCallbacksFixture,
                 "Screen Sharing page: the remote control toggle drives the setting",
                 "[settings][remote_screen][screen_sharing_callbacks]") {
    auto& m = RemoteScreenSettingsManager::instance();
    set_toggle("row_remote_screen", true);
    REQUIRE_FALSE(m.get_control());

    set_toggle("row_remote_control", true);
    CHECK(m.get_control());

    set_toggle("row_remote_control", false);
    CHECK_FALSE(m.get_control());
}
