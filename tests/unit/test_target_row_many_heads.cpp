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

namespace {

/// A target row of @p heads heads in a slot @p slot_w wide, laid out.
struct RowRig {
    helix::ToolChangerRig rig;
    lv_obj_t* row = nullptr;

    RowRig(LVGLUITestFixture& fx, int heads, int slot_w) : rig(heads) {
        std::vector<std::string> names = {"extruder"};
        for (int i = 1; i < heads; ++i) {
            names.push_back("extruder" + std::to_string(i));
        }
        fx.state().temperature_state().init_extruders(names);
        TargetTool::instance().attach(fx.state());
        ToolHeadsModel::instance().attach(fx.state());
        helix::ui::UpdateQueue::instance().drain();

        lv_obj_t* slot = lv_obj_create(fx.test_screen());
        lv_obj_set_size(slot, slot_w, 48);
        lv_obj_set_style_pad_all(slot, 0, 0);
        const char* attrs[] = {"labels", "_row", nullptr};
        row = static_cast<lv_obj_t*>(lv_xml_create(slot, "tool_target_row", attrs));
        REQUIRE(row != nullptr);
        lv_obj_set_width(row, lv_pct(100));
        fx.process_lvgl(60);
        lv_obj_update_layout(slot);
    }

    int cell_w(int i) const {
        return lv_obj_get_width(lv_obj_get_child(row, i));
    }
};

// The Filament panel's row at 480x272 is 106 px wide (measured with ctl geom).
constexpr int kNarrowSlotPx = 106;

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Target row: four heads still share a narrow slot unscrolled",
                 "[target_tool][vrtx]") {
    RowRig r(*this, 4, kNarrowSlotPx);
    REQUIRE(lv_obj_get_child_count(r.row) == 4);
    CHECK(lv_obj_get_scroll_right(r.row) == 0);
    for (int i = 0; i < 4; ++i) {
        INFO("cell " << i);
        CHECK(r.cell_w(i) >= 20); // a badge fits
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Target row: eight heads in a narrow slot keep every badge legible and scroll",
                 "[target_tool][vrtx]") {
    RowRig r(*this, 8, kNarrowSlotPx);
    REQUIRE(lv_obj_get_child_count(r.row) == 8);
    for (int i = 0; i < 8; ++i) {
        INFO("cell " << i);
        CHECK(r.cell_w(i) >= 20);
    }
    CHECK(lv_obj_has_flag(r.row, LV_OBJ_FLAG_SCROLLABLE));
    CHECK(lv_obj_get_scroll_right(r.row) > 0);
}
