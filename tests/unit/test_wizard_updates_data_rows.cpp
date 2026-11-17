// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal.h"
#include "ui_wizard_summary.h"
#include "ui_wizard_telemetry.h"

#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

int g_info_calls = 0;

struct RowsFixture : LVGLUITestFixture {
    lv_subject_t* bp_ = lv_xml_get_subject(nullptr, "ui_breakpoint");
    int saved_bp_ = lv_subject_get_int(bp_);

    RowsFixture() {
        lv_xml_register_event_cb(nullptr, "test_rows_noop", [](lv_event_t*) {});
        lv_xml_register_event_cb(nullptr, "test_rows_info", [](lv_event_t*) { ++g_info_calls; });
    }
    ~RowsFixture() override {
        while (lv_obj_t* dialog = ModalStack::instance().top_dialog()) {
            Modal::hide(dialog);
            process_lvgl(5);
        }
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

    // A row with @p description; @p key/@p value is the one component-specific prop.
    lv_obj_t* make(const char* component, const char* description, const char* key,
                   const char* value) {
        const char* attrs[] = {"label",     "Row", "callback", "test_rows_noop", "description",
                               description, key,   value,      nullptr};
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), component, attrs));
        REQUIRE(row != nullptr);
        return row;
    }
};

} // namespace

TEST_CASE_METHOD(RowsFixture, "Setting rows show one working (i) and the wizard rows use it",
                 "[xml][settings][setting_toggle_row][setting_dropdown_row][wizard][telemetry]") {
    SECTION("each breakpoint: the info_callback (i) alone, and none without a description") {
        lv_obj_t* with_info =
            make("setting_toggle_row", "Explains", "info_callback", "test_rows_info");
        lv_obj_t* plain = make("setting_toggle_row", "Explains", "info_callback", "");
        lv_obj_t* no_desc = make("setting_dropdown_row", "", "options", "A\nB");
        for (int bp : {0, 1, 3, 5, 0}) {
            CAPTURE(bp);
            lv_subject_set_int(bp_, bp);
            process_lvgl(5);
            CHECK(shown(with_info, "info_modal_btn"));
            CHECK_FALSE(shown(with_info, "info_btn"));
            CHECK_FALSE(shown(plain, "info_modal_btn"));
            CHECK(shown(plain, "info_btn") == (bp <= 1));
            CHECK_FALSE(shown(no_desc, "info_btn"));
        }
        g_info_calls = 0;
        lv_obj_send_event(lv_obj_find_by_name(with_info, "info_modal_btn"), LV_EVENT_CLICKED,
                          nullptr);
        CHECK(g_info_calls == 1);
    }

    SECTION("Updates & Data and Review: statistics below the channel, (i) explains it") {
        auto* cfg = Config::get_instance();
        const nlohmann::json printers_before =
            cfg->get<nlohmann::json>("/printers", nlohmann::json());
        cfg->set<nlohmann::json>("/printers",
                                 nlohmann::json{{"default", nlohmann::json::object()}});
        helix::wizard::Step* steps[] = {get_wizard_telemetry_step(), get_wizard_summary_step()};
        for (helix::wizard::Step* step : steps) {
            CAPTURE(step->component_name());
            step->register_callbacks();
            lv_obj_t* root = step->create(test_screen());
            REQUIRE(root != nullptr);
            lv_obj_update_layout(root);
            lv_obj_t* telemetry = lv_obj_find_by_name(root, "row_telemetry");
            REQUIRE(telemetry != nullptr);
            CHECK(shown(root, "row_telemetry"));
            lv_area_t channel_area, telemetry_area;
            lv_obj_get_coords(lv_obj_find_by_name(root, "row_update_channel"), &channel_area);
            lv_obj_get_coords(telemetry, &telemetry_area);
            CHECK(telemetry_area.y1 >= channel_area.y2);

            lv_obj_send_event(lv_obj_find_by_name(telemetry, "info_modal_btn"), LV_EVENT_CLICKED,
                              nullptr);
            CHECK(ModalStack::instance().dialog_for_component("telemetry_info_modal") != nullptr);
            Modal::hide(ModalStack::instance().top_dialog());
            process_lvgl(5);
            step->cleanup();
            lv_obj_delete(root);
        }
        cfg->set<nlohmann::json>("/printers", printers_before);
    }
}
