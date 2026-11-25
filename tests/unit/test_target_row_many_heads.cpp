// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_target_row_many_heads.cpp
 * @brief A target-tool row of eight heads in a narrow slot keeps every cell usable.
 *
 * The cells share the row's width; squeezed to a dozen pixels each they showed
 * nothing but a clipped "T". Past what fits, the row scrolls sideways.
 */

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "core/update_queue.h"
#include "target_tool.h"
#include "tool_heads_model.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::TargetTool;
using helix::ui::ToolHeadsModel;

TEST_CASE_METHOD(LVGLUITestFixture, "Target row: eight heads in a narrow slot stay usable",
                 "[target_tool][vrtx]") {
    helix::ToolChangerRig rig{8};
    std::vector<std::string> names = {"extruder"};
    for (int i = 1; i < 8; ++i) {
        names.push_back("extruder" + std::to_string(i));
    }
    state().temperature_state().init_extruders(names);
    TargetTool::instance().attach(state());
    ToolHeadsModel::instance().attach(state());
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* slot = lv_obj_create(test_screen());
    lv_obj_set_size(slot, 106, 48);
    lv_obj_set_style_pad_all(slot, 0, 0);
    const char* attrs[] = {"labels", "_row", nullptr};
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(slot, "tool_target_row", attrs));
    REQUIRE(row != nullptr);
    lv_obj_set_width(row, lv_pct(100));
    process_lvgl(60);
    lv_obj_update_layout(slot);

    REQUIRE(lv_obj_get_child_count(row) == 8);
    for (uint32_t i = 0; i < 8; ++i) {
        INFO("cell " << i);
        CHECK(lv_obj_get_width(lv_obj_get_child(row, static_cast<int32_t>(i))) >= 40);
    }
    // The cells did not fit: the row scrolls instead.
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_SCROLLABLE));
    CHECK(lv_obj_get_scroll_right(row) > 0);
}
