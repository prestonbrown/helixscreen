// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_glyphs_panel.cpp
 * @brief The MDI Icons debug panel lists every icon map entry as a glyph_item
 *
 * Run with: ./build/bin/helix-tests "[glyphs_panel]"
 */

#include "ui_icon_codepoints.h"
#include "ui_panel_glyphs.h"

#include "../lvgl_ui_test_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "glyphs panel lists every icon with its name",
                 "[glyphs_panel]") {
    lv_obj_t* panel = ui_panel_glyphs_create(test_screen());
    REQUIRE(panel != nullptr);

    lv_obj_t* list = lv_xml_component_get_slot(panel, "glyph_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == helix::ui::icon::kIconMapSize);

    const size_t last = helix::ui::icon::kIconMapSize - 1;
    lv_obj_t* row = lv_obj_get_child(list, static_cast<int32_t>(last));
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(row, "glyph_label"))) ==
          helix::ui::icon::kIconMap[last].codepoint);
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(row, "glyph_name"))) ==
          helix::ui::icon::kIconMap[last].name);

    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(panel, "glyph_count_label"))) ==
          std::to_string(helix::ui::icon::kIconMapSize) + " icons");

    lv_obj_delete(panel);
}
