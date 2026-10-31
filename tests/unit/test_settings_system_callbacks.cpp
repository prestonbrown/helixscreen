// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The System page's rows act on what they show. The XML names its callbacks by
// string and silently skips one that is not registered, so a row whose callback
// went missing stays on screen and does nothing. These tests build the page the
// way the overlay does (callbacks registered, then the XML) and drive each row.

#include "ui_panel_settings.h"
#include "ui_settings_system.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "settings_manager.h"
#include "system_settings_manager.h"

#include "../catch_amalgamated.hpp"

namespace {

struct SystemCallbacksFixture : LVGLUITestFixture {
    lv_obj_t* root_ = nullptr;

    SystemCallbacksFixture() {
        SettingsManager::instance().init_subjects();
        get_global_settings_panel().init_subjects();
        helix::settings::get_system_settings_overlay().init_subjects();
        helix::settings::get_system_settings_overlay().register_callbacks();
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_system_overlay", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    ~SystemCallbacksFixture() override {
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        helix::ui::UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    lv_obj_t* part(const char* row, const char* name) {
        lv_obj_t* r = lv_obj_find_by_name(root_, row);
        REQUIRE(r != nullptr);
        lv_obj_t* p = lv_obj_find_by_name(r, name);
        REQUIRE(p != nullptr);
        return p;
    }

    /// Whether @p row has the callback registered under @p callback attached.
    /// One that is not registered is skipped when the XML is built, so the row
    /// would stay on screen and do nothing.
    bool row_runs(const char* row, const char* callback) {
        lv_obj_t* r = lv_obj_find_by_name(root_, row);
        REQUIRE(r != nullptr);
        lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, callback);
        REQUIRE(cb != nullptr);
        for (uint32_t i = 0; i < lv_obj_get_event_count(r); ++i) {
            if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(r, i)) == cb) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE_METHOD(SystemCallbacksFixture, "System page: the telemetry toggle drives the setting",
                 "[settings][system_callbacks]") {
    auto& sys = SystemSettingsManager::instance();
    const bool before = sys.get_telemetry_enabled();

    lv_obj_t* toggle = part("row_telemetry", "toggle");
    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(sys.get_telemetry_enabled());

    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK_FALSE(sys.get_telemetry_enabled());

    sys.set_telemetry_enabled(before);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(SystemCallbacksFixture, "System page: the log-level dropdown applies its choice",
                 "[settings][system_callbacks]") {
    auto& sys = SystemSettingsManager::instance();
    const int before = sys.get_log_level_index();
    const int target = before == 2 ? 1 : 2;

    lv_obj_t* dropdown = part("row_log_level", "dropdown");
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(target));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(sys.get_log_level_index() == target);

    sys.set_log_level_by_index(before);
}

TEST_CASE_METHOD(SystemCallbacksFixture, "System page: every action row runs its callback",
                 "[settings][system_callbacks]") {
    struct Binding {
        const char* row;
        const char* callback;
    };
    for (const Binding& b : {Binding{"row_security", "on_security_clicked"},
                             Binding{"row_performance", "on_system_performance_clicked"},
                             Binding{"row_telemetry_view_data", "on_telemetry_view_data"},
                             Binding{"row_restart_helix", "on_restart_helix_settings_clicked"},
                             Binding{"row_factory_reset", "on_factory_reset_clicked"}}) {
        CAPTURE(b.row, b.callback);
        CHECK(row_runs(b.row, b.callback));
    }
}

TEST_CASE_METHOD(SystemCallbacksFixture,
                 "System page: telemetry data is offered only where the platform has it",
                 "[settings][system_callbacks]") {
    auto& sys = SystemSettingsManager::instance();
    const bool before = sys.get_telemetry_enabled();
    lv_subject_t* extras = lv_xml_get_subject(nullptr, "platform_extras_available");
    REQUIRE(extras != nullptr);
    const int extras_before = lv_subject_get_int(extras);
    lv_obj_t* row = lv_obj_find_by_name(root_, "row_telemetry_view_data");
    REQUIRE(row != nullptr);

    sys.set_telemetry_enabled(true);
    lv_subject_set_int(extras, 1);
    process_lvgl(5);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // The firmware build reports no platform extras: no data screen to open.
    lv_subject_set_int(extras, 0);
    process_lvgl(5);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(extras, extras_before);
    sys.set_telemetry_enabled(before);
    helix::ui::UpdateQueue::instance().drain();
}
