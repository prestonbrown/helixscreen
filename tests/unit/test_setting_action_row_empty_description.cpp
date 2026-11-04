// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

namespace {

struct EmptyDescFixture : LVGLUITestFixture {
    lv_subject_t* bp_ = nullptr;
    int saved_bp_ = 0;
    lv_subject_t bound_{};
    char bound_buf_[32] = "0.8 mm";

    EmptyDescFixture() {
        bp_ = lv_xml_get_subject(nullptr, "ui_breakpoint");
        REQUIRE(bp_ != nullptr);
        saved_bp_ = lv_subject_get_int(bp_);
        lv_xml_register_event_cb(nullptr, "test_empty_desc_noop", [](lv_event_t*) {});
        lv_subject_init_string(&bound_, bound_buf_, nullptr, sizeof(bound_buf_), bound_buf_);
        lv_xml_register_subject(nullptr, "test_empty_desc_bound", &bound_);
    }
    ~EmptyDescFixture() override {
        lv_subject_set_int(bp_, saved_bp_);
        helix::ui::UpdateQueue::instance().drain();
    }

    lv_obj_t* make(const char* description, const char* bind, const char* min_bp) {
        const char* attrs[] = {"name",
                               "row_under_test",
                               "label",
                               "Row",
                               "icon",
                               "cog",
                               "callback",
                               "test_empty_desc_noop",
                               "description",
                               description,
                               bind ? "bind_description" : nullptr,
                               bind,
                               bind ? "description_min_bp" : nullptr,
                               min_bp,
                               nullptr};
        auto* row =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "setting_action_row", attrs));
        REQUIRE(row != nullptr);
        process_lvgl(5);
        return row;
    }

    // Hidden if the widget or any ancestor up to `row` carries HIDDEN.
    static bool shown(lv_obj_t* row, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(row, name);
        REQUIRE(obj != nullptr);
        for (lv_obj_t* o = obj; o && o != row; o = lv_obj_get_parent(o)) {
            if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
                return false;
        }
        return true;
    }
};

} // namespace

TEST_CASE_METHOD(EmptyDescFixture,
                 "setting_action_row: no description shows no second line and no info icon",
                 "[xml][settings][setting_action_row]") {
    for (int bp = 0; bp <= 5; ++bp) {
        CAPTURE(bp);
        lv_subject_set_int(bp_, bp);
        lv_obj_t* row = make("", nullptr, nullptr);
        CHECK_FALSE(shown(row, "description"));
        CHECK_FALSE(shown(row, "status"));
        CHECK_FALSE(shown(row, "info_btn"));
        lv_obj_delete(row);
    }
}

TEST_CASE_METHOD(EmptyDescFixture,
                 "setting_action_row: breakpoint change after creation keeps empty parts hidden",
                 "[xml][settings][setting_action_row]") {
    lv_subject_set_int(bp_, 3);
    lv_obj_t* row = make("", nullptr, nullptr);
    for (int bp : {0, 1, 3, 5, 0}) {
        CAPTURE(bp);
        lv_subject_set_int(bp_, bp);
        process_lvgl(5);
        CHECK_FALSE(shown(row, "description"));
        CHECK_FALSE(shown(row, "info_btn"));
    }
    lv_obj_delete(row);
}

TEST_CASE_METHOD(EmptyDescFixture,
                 "setting_action_row: static description keeps its breakpoint behaviour",
                 "[xml][settings][setting_action_row]") {
    lv_subject_set_int(bp_, 3);
    lv_obj_t* row = make("Explains the row", nullptr, nullptr);
    CHECK(shown(row, "description"));
    CHECK_FALSE(shown(row, "info_btn"));
    lv_subject_set_int(bp_, 0);
    process_lvgl(5);
    CHECK_FALSE(shown(row, "description"));
    CHECK(shown(row, "info_btn"));
    lv_obj_delete(row);
}

TEST_CASE_METHOD(EmptyDescFixture,
                 "setting_action_row: bound status never offers an empty info popup",
                 "[xml][settings][setting_action_row]") {
    lv_subject_set_int(bp_, 0);
    lv_obj_t* row = make("", "test_empty_desc_bound", "0");
    CHECK(shown(row, "status"));
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(row, "status"))) == "0.8 mm");
    CHECK_FALSE(shown(row, "info_btn"));
    CHECK_FALSE(shown(row, "description"));
    lv_obj_delete(row);
}
