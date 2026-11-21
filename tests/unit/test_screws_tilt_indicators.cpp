// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_screws_tilt_indicators.cpp
 * @brief The Screws Tilt bed diagram draws one screw_indicator per probed screw
 *
 * Run with: ./build/bin/helix-tests "[screws_tilt][indicator]"
 */

#include "ui_panel_screws_tilt.h"

#include "../lvgl_ui_test_fixture.h"
#include "calibration_types.h"
#include "core/update_queue.h"
#include "theme_manager.h"

#include <cstring>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

constexpr const char* kCheckGlyph = "\xF3\xB0\x84\xAC";       // F012C check
constexpr const char* kRotateRightGlyph = "\xF3\xB0\x91\xA7"; // F0467 rotate-right

std::vector<ScrewTiltResult> reference_and_clockwise() {
    ScrewTiltResult ref;
    ref.screw_name = "front_left";
    ref.adjustment = "CW 00:00";
    ref.is_reference = true;
    ref.x_pos = 0.0f;
    ref.y_pos = 0.0f;
    ScrewTiltResult other;
    other.screw_name = "rear_right";
    other.adjustment = "CW 00:45";
    other.x_pos = 200.0f;
    other.y_pos = 200.0f;
    return {ref, other};
}

/// Bed diagram children that are screw indicators, in creation order.
std::vector<lv_obj_t*> indicators(lv_obj_t* root) {
    std::vector<lv_obj_t*> out;
    lv_obj_t* diagram = lv_obj_find_by_name(root, "bed_diagram_container");
    if (!diagram)
        return out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(diagram); i++) {
        lv_obj_t* child = lv_obj_get_child(diagram, static_cast<int32_t>(i));
        if (lv_obj_find_by_name(child, "screw_glyph"))
            out.push_back(child);
    }
    return out;
}

const char* glyph_of(lv_obj_t* indicator) {
    return lv_label_get_text(lv_obj_find_by_name(indicator, "screw_glyph"));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "screws tilt draws one indicator per screw",
                 "[screws_tilt][indicator]") {
    ScrewsTiltPanel panel;
    panel.init_subjects();
    lv_obj_t* root = panel.create(test_screen());
    REQUIRE(root != nullptr);
    panel.on_activate();

    panel.on_screws_tilt_results(reference_and_clockwise());

    const auto found = indicators(root);
    REQUIRE(found.size() == 2);

    // The reference screw is settled: a check on the success fill.
    CHECK(std::strcmp(glyph_of(found[0]), kCheckGlyph) == 0);
    CHECK(lv_color_to_u32(lv_obj_get_style_bg_color(found[0], LV_PART_MAIN)) ==
          lv_color_to_u32(theme_manager_get_color("success")));

    // Klipper's CW reads as the clockwise arrow, on a heavier ring as the worst screw.
    CHECK(std::strcmp(glyph_of(found[1]), kRotateRightGlyph) == 0);
    CHECK(lv_obj_get_style_border_width(found[1], LV_PART_MAIN) >
          lv_obj_get_style_border_width(found[0], LV_PART_MAIN));

    // Clearing the results takes the indicators down.
    panel.on_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain();
}
