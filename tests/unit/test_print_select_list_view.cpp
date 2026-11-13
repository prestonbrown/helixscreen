// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_format_utils.h"
#include "ui_print_select_list_view.h"

#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "lvgl/src/display/lv_display_private.h" // inv_areas
#include "lvgl/src/misc/lv_area_private.h"       // lv_area_is_in
#include "print_file_data.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

std::vector<PrintFileData> make_files(const std::string& print_time) {
    std::vector<PrintFileData> files;
    for (int i = 0; i < 3; i++) {
        PrintFileData f{};
        f.filename = "part" + std::to_string(i) + ".gcode";
        f.print_time_str = print_time;
        files.push_back(f);
    }
    return files;
}

std::string visible_print_times(lv_obj_t* container) {
    std::string out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
        lv_obj_t* row = lv_obj_get_child(container, i);
        if (lv_obj_is_hidden(row)) {
            continue;
        }
        if (lv_obj_t* t = lv_obj_find_by_name(row, "row_print_time")) {
            out += std::string(lv_label_get_text(t)) + ";";
        }
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrintSelectListView - repopulating the same indices shows the new data",
                 "[print_select_list_view][ui_integration]") {
    PrintSelectListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 600, 400);
    lv_obj_set_scrollable(container, true);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    view.setup(container, nullptr, nullptr);

    view.populate(make_files("1h 10m"), false);
    process_lvgl(50);
    REQUIRE(visible_print_times(container) == "1h 10m;1h 10m;1h 10m;");

    // Same count, same indices: only the row contents differ.
    view.populate(make_files("2h 20m"), false);
    process_lvgl(50);
    REQUIRE(visible_print_times(container) == "2h 20m;2h 20m;2h 20m;");
}

// Listing entries are built off the main thread and carry no date text: the row
// formats modified_timestamp in the time format live when it is drawn.
TEST_CASE_METHOD(LVGLUITestFixture, "PrintSelectListView - a row's date follows the time format",
                 "[print_select_list_view][ui_integration]") {
    auto& display = helix::DisplaySettingsManager::instance();
    const helix::TimeFormat saved_format = display.get_time_format();
    PrintSelectListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 600, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    view.setup(container, nullptr, nullptr);
    auto files = make_files("1h");
    files[0].modified_timestamp = 1733063400;
    auto row_date = [&]() {
        return std::string(lv_label_get_text(lv_obj_find_by_name(container, "row_modified")));
    };

    display.set_time_format(helix::TimeFormat::HOUR_12);
    view.populate(files, false);
    const std::string twelve_hour = row_date();
    display.set_time_format(helix::TimeFormat::HOUR_24);
    view.refresh_content(files);
    const std::string twenty_four_hour = row_date();
    const std::string expected = format_modified_date(1733063400);
    display.set_time_format(saved_format);

    CHECK(twelve_hour != twenty_four_hour);
    CHECK(twenty_four_hour == expected);
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "PrintSelectListView - a metadata refresh repaints only the rows whose data changed",
    "[print_select_list_view][ui_integration]") {
    PrintSelectListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 600, 400);
    lv_obj_set_scrollable(container, true);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    view.setup(container, nullptr, nullptr);

    auto files = make_files("1h 10m");
    files[0].history_status = FileHistoryStatus::COMPLETED; // a shown indicator and count
    files[0].success_count = 3;
    files[1].history_status = FileHistoryStatus::FAILED;
    view.populate(files, false);
    lv_refr_now(nullptr);
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp->inv_p == 0);

    // The same data again: nothing on screen changes, so nothing is redrawn.
    view.refresh_content(files);
    CHECK(disp->inv_p == 0);

    // One row's print time changes: only that row is redrawn.
    files[2].print_time_str = "2h 20m";
    view.refresh_content(files);
    REQUIRE(disp->inv_p > 0);
    lv_obj_t* row = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
        lv_obj_t* r = lv_obj_get_child(container, static_cast<int32_t>(i));
        if (!lv_obj_is_hidden(r) && reinterpret_cast<size_t>(lv_obj_get_user_data(r)) == 2) {
            row = r;
        }
    }
    REQUIRE(row != nullptr);
    lv_area_t row_area;
    lv_obj_get_coords(row, &row_area);
    for (int32_t i = 0; i < disp->inv_p; i++) {
        CHECK(lv_area_is_in(&disp->inv_areas[i], &row_area, 0));
    }
}
