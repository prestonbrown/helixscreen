// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "helix/ui/text_metrics.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "fit_dropdown_to_text widens a dropdown to its closed text",
                 "[text_metrics][target_tool]") {
    lv_obj_t* dd = lv_dropdown_create(test_screen());
    lv_dropdown_set_options(dd, "Tool 1 (T0)\nTool 2 (T1)");
    lv_obj_set_style_min_width(dd, 120, LV_PART_MAIN);
    const lv_font_t* font = lv_obj_get_style_text_font(dd, LV_PART_MAIN);

    lv_dropdown_set_text(dd, "1");
    helix::ui::fit_dropdown_to_text(dd);
    lv_obj_update_layout(dd);
    const int32_t narrow = lv_obj_get_width(dd);
    // Short text keeps the XML floor.
    CHECK(narrow == 120);

    lv_dropdown_set_text(dd, "Toolhead Number 1");
    helix::ui::fit_dropdown_to_text(dd);
    lv_obj_update_layout(dd);
    const int32_t wide = lv_obj_get_width(dd);
    // Room for the text and the arrow beside it.
    CHECK(wide > helix::ui::text_width("Toolhead Number 1", font) +
                     lv_font_get_line_height(lv_obj_get_style_text_font(dd, LV_PART_INDICATOR)));

    // Shrinking the text shrinks the control again.
    lv_dropdown_set_text(dd, "1");
    helix::ui::fit_dropdown_to_text(dd);
    lv_obj_update_layout(dd);
    CHECK(lv_obj_get_width(dd) == 120);
}
