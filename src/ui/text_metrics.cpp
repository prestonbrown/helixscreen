// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix/ui/text_metrics.h"

#include "lvgl/src/misc/lv_text_private.h" // lv_text_get_width

#include <cstring>

namespace helix::ui {

int32_t text_width_n(const char* txt, uint32_t len, const lv_font_t* font, int32_t letter_space) {
    if (!txt || !font || len == 0) {
        return 0;
    }
    lv_text_attributes_t attributes = {};
    attributes.letter_space = letter_space;
    attributes.max_width = LV_COORD_MAX;
    attributes.text_flags = LV_TEXT_FLAG_NONE;
    return lv_text_get_width(txt, len, font, &attributes);
}

int32_t text_width(const char* txt, const lv_font_t* font, int32_t letter_space) {
    if (!txt) {
        return 0;
    }
    return text_width_n(txt, static_cast<uint32_t>(std::strlen(txt)), font, letter_space);
}

void fit_dropdown_to_text(lv_obj_t* dropdown) {
    if (!dropdown)
        return;
    const char* text = lv_dropdown_get_text(dropdown);
    char selected[64] = {};
    if (!text) {
        lv_dropdown_get_selected_str(dropdown, selected, sizeof(selected));
        text = selected;
    }
    const lv_font_t* font = lv_obj_get_style_text_font(dropdown, LV_PART_MAIN);
    const lv_font_t* arrow_font = lv_obj_get_style_text_font(dropdown, LV_PART_INDICATOR);
    // DECLARATIVE_OK: measured layout; the dropdown cannot size itself to its text.
    const int32_t arrow = arrow_font ? lv_font_get_line_height(arrow_font) : 0;
    const int32_t width = text_width(text, font) + arrow +
                          lv_obj_get_style_pad_left(dropdown, LV_PART_MAIN) +
                          lv_obj_get_style_pad_right(dropdown, LV_PART_MAIN) +
                          lv_obj_get_style_pad_column(dropdown, LV_PART_MAIN) +
                          2 * lv_obj_get_style_border_width(dropdown, LV_PART_MAIN);
    const int32_t min_w = lv_obj_get_style_min_width(dropdown, LV_PART_MAIN);
    lv_obj_set_width(dropdown, LV_MAX(width, min_w));
}

} // namespace helix::ui
