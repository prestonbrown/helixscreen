// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal.h"
#include "ui_wizard_summary.h"
#include "ui_wizard_telemetry.h"

#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "system_settings_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

int g_info_calls = 0;

struct UpdatesDataRowsFixture : LVGLUITestFixture {
    lv_subject_t* bp_ = nullptr;
    int saved_bp_ = 0;
    bool saved_telemetry_ = false;

    UpdatesDataRowsFixture() {
        bp_ = lv_xml_get_subject(nullptr, "ui_breakpoint");
        REQUIRE(bp_ != nullptr);
        saved_bp_ = lv_subject_get_int(bp_);
        saved_telemetry_ = SystemSettingsManager::instance().get_telemetry_enabled();
        g_info_calls = 0;
        lv_xml_register_event_cb(nullptr, "test_rows_noop", [](lv_event_t*) {});
        lv_xml_register_event_cb(nullptr, "test_rows_info", [](lv_event_t*) { ++g_info_calls; });
    }
    ~UpdatesDataRowsFixture() override {
        while (lv_obj_t* dialog = ModalStack::instance().top_dialog()) {
            Modal::hide(dialog);
            process_lvgl(5);
        }
        SystemSettingsManager::instance().set_telemetry_enabled(saved_telemetry_);
        lv_subject_set_int(bp_, saved_bp_);
        helix::ui::UpdateQueue::instance().drain();
    }

    // Hidden if the widget or any ancestor up to `root` carries HIDDEN.
    static bool shown(lv_obj_t* root, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        for (lv_obj_t* o = obj; o && o != root; o = lv_obj_get_parent(o)) {
            if (lv_obj_is_hidden(o))
                return false;
        }
        return true;
    }

    lv_obj_t* make_toggle_row(const char* info_callback) {
        const char* attrs[] = {"name",          "row_under_test",   "label",    "Row",
                               "description",   "Explains the row", "callback", "test_rows_noop",
                               "info_callback", info_callback,      nullptr};
        auto* row =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "setting_toggle_row", attrs));
        REQUIRE(row != nullptr);
        process_lvgl(5);
        return row;
    }
};

} // namespace

TEST_CASE_METHOD(UpdatesDataRowsFixture,
                 "setting_toggle_row: info_callback gives an (i) at every breakpoint that calls it",
                 "[xml][settings][setting_toggle_row][telemetry]") {
    lv_obj_t* row = make_toggle_row("test_rows_info");
    for (int bp : {0, 1, 3, 5, 0}) {
        CAPTURE(bp);
        lv_subject_set_int(bp_, bp);
        process_lvgl(5);
        CHECK(shown(row, "info_modal_btn"));
        // One (i) per row: the description toggle never joins it.
        CHECK_FALSE(shown(row, "info_btn"));
    }
    lv_obj_send_event(lv_obj_find_by_name(row, "info_modal_btn"), LV_EVENT_CLICKED, nullptr);
    CHECK(g_info_calls == 1);
    lv_obj_delete(row);
}

TEST_CASE_METHOD(UpdatesDataRowsFixture,
                 "setting_toggle_row: without info_callback the (i) reveals the description",
                 "[xml][settings][setting_toggle_row]") {
    lv_obj_t* row = make_toggle_row("");
    lv_subject_set_int(bp_, 0);
    process_lvgl(5);
    CHECK_FALSE(shown(row, "info_modal_btn"));
    CHECK(shown(row, "info_btn"));
    CHECK_FALSE(shown(row, "description"));
    lv_obj_send_event(lv_obj_find_by_name(row, "info_btn"), LV_EVENT_CLICKED, nullptr);
    CHECK(shown(row, "description"));
    lv_obj_delete(row);
}

TEST_CASE_METHOD(UpdatesDataRowsFixture,
                 "setting_dropdown_row: no description means no (i) at any breakpoint",
                 "[xml][settings][setting_dropdown_row]") {
    const char* attrs[] = {"name", "row_under_test", "label",          "Row",  "options",
                           "A\nB", "callback",       "test_rows_noop", nullptr};
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "setting_dropdown_row", attrs));
    REQUIRE(row != nullptr);
    for (int bp : {0, 1, 3, 0}) {
        CAPTURE(bp);
        lv_subject_set_int(bp_, bp);
        process_lvgl(5);
        CHECK_FALSE(shown(row, "info_btn"));
    }
    lv_obj_delete(row);
}

TEST_CASE_METHOD(UpdatesDataRowsFixture,
                 "Updates & Data: the channel and statistics rows, the switch writing the setting",
                 "[wizard][telemetry]") {
    auto* step = get_wizard_telemetry_step();
    step->register_callbacks();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);
    process_lvgl(5);

    lv_obj_t* channel = lv_obj_find_by_name(root, "row_update_channel");
    lv_obj_t* telemetry = lv_obj_find_by_name(root, "row_telemetry");
    REQUIRE(channel != nullptr);
    REQUIRE(telemetry != nullptr);
    // One list: the statistics row sits below the channel row, not beside or over it.
    lv_obj_update_layout(root);
    lv_area_t channel_area, telemetry_area;
    lv_obj_get_coords(channel, &channel_area);
    lv_obj_get_coords(telemetry, &telemetry_area);
    CHECK(telemetry_area.y1 >= channel_area.y2);
    CHECK(telemetry_area.x1 == channel_area.x1);

    lv_obj_t* toggle = lv_obj_find_by_name(telemetry, "toggle");
    REQUIRE(toggle != nullptr);
    SystemSettingsManager::instance().set_telemetry_enabled(false);
    process_lvgl(5);
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(SystemSettingsManager::instance().get_telemetry_enabled());

    lv_subject_set_int(bp_, 3);
    process_lvgl(5);
    CHECK(shown(telemetry, "description"));
    lv_obj_send_event(lv_obj_find_by_name(telemetry, "info_modal_btn"), LV_EVENT_CLICKED, nullptr);
    process_lvgl(5);
    CHECK(ModalStack::instance().dialog_for_component("telemetry_info_modal") != nullptr);

    step->cleanup();
    lv_obj_delete(root);
}

TEST_CASE_METHOD(UpdatesDataRowsFixture, "Review step shows the same statistics row",
                 "[wizard][summary][telemetry]") {
    auto* cfg = Config::get_instance();
    const nlohmann::json printers_before = cfg->get<nlohmann::json>("/printers", nlohmann::json());
    nlohmann::json one = nlohmann::json::object();
    one["default"] = nlohmann::json::object();
    cfg->set<nlohmann::json>("/printers", one);

    auto* step = get_wizard_summary_step();
    step->register_callbacks();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);
    process_lvgl(5);

    lv_obj_t* telemetry = lv_obj_find_by_name(root, "row_telemetry");
    REQUIRE(telemetry != nullptr);
    CHECK(shown(root, "row_telemetry"));
    SystemSettingsManager::instance().set_telemetry_enabled(true);
    lv_obj_t* toggle = lv_obj_find_by_name(telemetry, "toggle");
    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK_FALSE(SystemSettingsManager::instance().get_telemetry_enabled());

    step->cleanup();
    lv_obj_delete(root);
    cfg->set<nlohmann::json>("/printers", printers_before);
}
