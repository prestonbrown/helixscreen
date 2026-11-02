// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_display_align_current.cpp
 * @brief align_current="true" puts stacked temp_displays' separators at one x.
 *
 * Each case pairs aligned displays against unaligned ones bound to the same
 * subjects, so the attribute is what lines the separators up rather than the
 * readings happening to share a width.
 */

#include "ui_fonts.h"
#include "ui_temp_display.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/ui/text_metrics.h"
#include "printer_state.h"
#include "printer_temperature_state.h"

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

// ui_temp_display_create_cb() creates its labels in a fixed order:
// current (0), separator (1), target (2), unit (3).
constexpr int kCurrentChild = 0;
constexpr int kSeparatorChild = 1;

lv_obj_t* create_display(lv_obj_t* parent, const char* current, const char* target,
                         const char* align) {
    const char* attrs[] = {
        "bind_current", current,         "bind_target", target,  "show_target", "true", "size",
        "md",           "align_current", align,         "align", "top_left",    nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "temp_display", attrs));
}

int32_t separator_x(lv_obj_t* display) {
    lv_obj_t* separator = lv_obj_get_child(display, kSeparatorChild);
    REQUIRE(separator != nullptr);
    return lv_obj_get_x(separator);
}

int32_t current_width(lv_obj_t* display) {
    return lv_obj_get_width(lv_obj_get_child(display, kCurrentChild));
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "align_current lines separators up across readings",
                 "[temp_display][align_current]") {
    lv_obj_t* column = lv_obj_create(test_screen());
    lv_obj_set_size(column, 400, 300);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* nozzle = create_display(column, "extruder_temp", "extruder_target", "true");
    lv_obj_t* bed = create_display(column, "bed_temp", "bed_target", "true");
    lv_obj_t* nozzle_plain = create_display(column, "extruder_temp", "extruder_target", "false");
    lv_obj_t* bed_plain = create_display(column, "bed_temp", "bed_target", "false");
    REQUIRE(nozzle != nullptr);
    REQUIRE(bed != nullptr);
    REQUIRE(nozzle_plain != nullptr);
    REQUIRE(bed_plain != nullptr);

    auto& temps = state().temperature_state();
    // "220" against "55.0": a three-digit reading beside a decimal one.
    lv_subject_set_int(temps.get_active_extruder_temp_subject(), 2200);
    lv_subject_set_int(temps.get_active_extruder_target_subject(), 2200);
    lv_subject_set_int(temps.get_bed_temp_subject(), 550);
    lv_subject_set_int(temps.get_bed_target_subject(), 550);
    UpdateQueue::instance().drain();
    lv_obj_update_layout(column);

    REQUIRE(separator_x(nozzle_plain) != separator_x(bed_plain));
    CHECK(separator_x(nozzle) == separator_x(bed));
    CHECK(current_width(nozzle) == current_width(bed));

    lv_obj_t* label = lv_obj_get_child(nozzle, kCurrentChild);
    CHECK(lv_obj_get_style_text_align(label, LV_PART_MAIN) == LV_TEXT_ALIGN_RIGHT);
    // Wide enough for the widest reading, so a 3-digit and a decimal value both fit.
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    CHECK(current_width(nozzle) >= helix::ui::text_width("00.0", font, 0));
    CHECK(current_width(nozzle) >= helix::ui::text_width("000", font, 0));
}

TEST_CASE_METHOD(XMLTestFixture, "align_current follows a face retiered after creation",
                 "[temp_display][align_current]") {
    lv_obj_t* display = create_display(test_screen(), "extruder_temp", "extruder_target", "true");
    REQUIRE(display != nullptr);
    lv_obj_t* label = lv_obj_get_child(display, kCurrentChild);
    lv_obj_update_layout(display);
    const int32_t before = current_width(display);

    const lv_font_t* bigger = &noto_sans_28;
    REQUIRE(bigger != lv_obj_get_style_text_font(label, LV_PART_MAIN));
    lv_obj_set_style_text_font(label, bigger, LV_PART_MAIN);
    lv_obj_update_layout(display);

    CHECK(current_width(display) == helix::ui::text_width("00.0", bigger, 0));
    CHECK(current_width(display) != before);
}

TEST_CASE_METHOD(XMLTestFixture, "the separator keeps a space's gap before the target",
                 "[temp_display][align_current]") {
    lv_obj_t* display = create_display(test_screen(), "extruder_temp", "extruder_target", "false");
    REQUIRE(display != nullptr);
    lv_obj_update_layout(test_screen());

    lv_obj_t* separator = lv_obj_get_child(display, kSeparatorChild);
    lv_obj_t* target = lv_obj_get_child(display, kSeparatorChild + 1);
    const lv_font_t* font = lv_obj_get_style_text_font(separator, LV_PART_MAIN);
    const int32_t space = lv_font_get_glyph_width(font, ' ', 0);
    REQUIRE(space > 0);
    CHECK(lv_obj_get_x(target) - (lv_obj_get_x(separator) + lv_obj_get_width(separator)) == space);
}
