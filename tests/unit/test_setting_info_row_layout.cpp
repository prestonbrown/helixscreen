// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

namespace {

constexpr int32_t kNarrowCardWidth = 200;
constexpr const char* kLongVersion = "v2026.07.00-0-g30c65e09-dirty-20260719_feature_branch_name";

struct InfoRowFixture : LVGLUITestFixture {
    lv_obj_t* card_ = nullptr;

    InfoRowFixture() {
        card_ = lv_obj_create(test_screen());
        lv_obj_set_size(card_, kNarrowCardWidth, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card_, 0, 0);
    }
    ~InfoRowFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    lv_obj_t* make(const char* label, const char* value) {
        const char* attrs[] = {"label", label, "icon", "info", "value", value, nullptr};
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(card_, "setting_info_row", attrs));
        REQUIRE(row != nullptr);
        lv_obj_update_layout(lv_screen_active());
        return row;
    }

    static lv_obj_t* child(lv_obj_t* row, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(row, name);
        REQUIRE(obj != nullptr);
        return obj;
    }
};

} // namespace

TEST_CASE_METHOD(InfoRowFixture, "setting_info_row: a value wider than the row wraps inside it",
                 "[xml][settings][setting_info_row]") {
    lv_obj_t* row = make("Kalico", kLongVersion);
    lv_obj_t* value = child(row, "value");

    lv_area_t row_area;
    lv_area_t value_area;
    lv_obj_get_content_coords(row, &row_area);
    lv_obj_get_coords(value, &value_area);

    REQUIRE(lv_obj_get_width(value) > 0);
    CHECK(value_area.x1 >= row_area.x1);
    CHECK(value_area.x2 <= row_area.x2);
    CHECK(lv_obj_get_width(value) <= lv_area_get_width(&row_area));
    // Wrapped over several lines rather than clipped on one.
    CHECK(lv_obj_get_height(value) > lv_obj_get_height(child(row, "label")));
}

TEST_CASE_METHOD(InfoRowFixture, "setting_info_row: the label never breaks to make room",
                 "[xml][settings][setting_info_row]") {
    lv_obj_t* short_row = make("Kalico", "v1");
    lv_obj_t* long_row = make("Kalico", kLongVersion);

    lv_obj_t* short_label = child(short_row, "label");
    lv_obj_t* long_label = child(long_row, "label");
    CHECK(lv_obj_get_width(long_label) == lv_obj_get_width(short_label));
    CHECK(lv_obj_get_height(long_label) == lv_obj_get_height(short_label));
}

TEST_CASE_METHOD(InfoRowFixture,
                 "setting_info_row: a short value sits beside the label, right-aligned",
                 "[xml][settings][setting_info_row]") {
    lv_obj_t* row = make("Kalico", "v1");
    lv_obj_t* value = child(row, "value");
    lv_obj_t* label = child(row, "label");

    lv_area_t row_area;
    lv_area_t value_area;
    lv_obj_get_content_coords(row, &row_area);
    lv_obj_get_coords(value, &value_area);

    CHECK(lv_obj_get_y(value) == lv_obj_get_y(label));
    CHECK(value_area.x2 == row_area.x2);
}
