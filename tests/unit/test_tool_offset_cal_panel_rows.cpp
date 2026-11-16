// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offset_cal_panel_rows.cpp
 * @brief The tool offset panel's rows, built by the XML <repeat>.
 *
 * The row count, the per-row subject names and the two bind_style_if
 * conditions are only exercised once the widget tree exists, so these build the
 * panel the way the overlay entry point does rather than driving the subjects
 * alone.
 */

#include "ui_panel_calibration_tool_offset.h"

#include "../lvgl_ui_test_fixture.h"
#include "../toolchanger_panel_fixture.h"
#include "core/update_queue.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// LVGLUITestFixture, not LVGLTestFixture: lv_xml_create must be able to build
/// the panel's component tree.
struct ToolCalRowsFixture : helix::test::ToolchangerPanelFixture<LVGLUITestFixture> {
    /// Every label under @p root, in tree order.
    static void collect_labels(lv_obj_t* root, std::vector<std::string>& out) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
            lv_obj_t* child = lv_obj_get_child(root, i);
            if (lv_obj_check_type(child, &lv_label_class)) {
                out.emplace_back(lv_label_get_text(child));
            }
            collect_labels(child, out);
        }
    }
};

} // namespace

TEST_CASE_METHOD(ToolCalRowsFixture, "tool offset panel: the repeat builds one row per tool",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    lv_obj_t* root = panel.create(lv_screen_active());
    REQUIRE(root != nullptr);
    helix::ui::UpdateQueue::instance().drain();

    // One row per tool the printer has - no fixed cap, the pools grew to fit.
    CHECK(lv_subject_get_int(panel.get_tool_count_subject()) == 4);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(lv_obj_find_by_name(root, ("tool_cal_row_" + std::to_string(i)).c_str()) != nullptr);
    }
    CHECK(lv_obj_find_by_name(root, "tool_cal_row_4") == nullptr);

    panel.cleanup();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolCalRowsFixture, "tool offset panel: a row shows its tool's offsets",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // The per-row subjects are pool slots resolved by name from the <repeat>'s
    // ${i}; a slot the pool never registered leaves the label empty.
    helix::ToolState& ts = helix::ToolState::instance();
    ts.set_tool_offset_local(1, helix::Axis::X, -120);
    ts.set_tool_offset_local(1, helix::Axis::Y, 45);

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    lv_obj_t* root = panel.create(lv_screen_active());
    REQUIRE(root != nullptr);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* values = lv_obj_find_by_name(root, "tool_cal_values_1");
    REQUIRE(values != nullptr);
    std::vector<std::string> labels;
    collect_labels(values, labels);
    // X, its value, Y, its value, Z, its value.
    REQUIRE(labels.size() == 6);
    CHECK(labels[0] == "X");
    CHECK(labels[1] == "-0.120");
    CHECK(labels[2] == "Y");
    CHECK(labels[3] == "+0.045");
    CHECK(labels[4] == "Z");

    panel.cleanup();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolCalRowsFixture, "tool offset panel: the mounted tool's row is highlighted",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // Both bind_style_if conds read tool_cal_active AND active_tool, so only a
    // run in flight highlights, and only the mounted tool's row.
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    lv_obj_t* root = panel.create(lv_screen_active());
    REQUIRE(root != nullptr);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* row1 = lv_obj_find_by_name(root, "tool_cal_row_1");
    lv_obj_t* row2 = lv_obj_find_by_name(root, "tool_cal_row_2");
    REQUIRE(row1 != nullptr);
    REQUIRE(row2 != nullptr);

    lv_subject_set_int(helix::ToolState::instance().get_active_tool_subject(), 2);
    helix::ui::UpdateQueue::instance().drain();

    // Idle: no row is highlighted, whichever tool is mounted.
    CHECK(lv_obj_get_style_border_width(row2, LV_PART_MAIN) == 1);

    panel.begin_run();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_obj_get_style_border_width(row2, LV_PART_MAIN) == 2);
    CHECK(lv_obj_get_style_border_width(row1, LV_PART_MAIN) == 1);

    REQUIRE(panel.abort_in_progress_calibration());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_obj_get_style_border_width(row2, LV_PART_MAIN) == 1);

    panel.cleanup();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolCalRowsFixture, "tool offset panel: the rows stop following a destroyed UI",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // on_ui_destroyed() reclaims the pools, unregistering every tool_cal_*_<i>
    // subject. Re-growing them for a <repeat> that no longer exists would
    // publish a count nothing can build and leave the next create() to bind
    // rows to subjects the pool never re-registered.
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    REQUIRE(panel.create(lv_screen_active()) != nullptr);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(panel.get_tool_count_subject()) == 4);

    panel.on_ui_destroyed();
    CHECK(lv_subject_get_int(panel.get_tool_count_subject()) == 0);

    panel.on_activate();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(panel.get_tool_count_subject()) == 0);

    panel.cleanup();
    helix::ui::UpdateQueue::instance().drain();
}
