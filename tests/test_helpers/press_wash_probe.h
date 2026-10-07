// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix-xml/src/xml/lv_xml_style.h"
#include "lvgl/lvgl.h"
#include "misc/lv_event_private.h" // lv_event_dsc_t::filter: no public accessor

#include <cstring>

namespace helix::test {

/// True when @p obj paints the shared styles.press_wash (ui_xml/styles.xml)
/// while pressed, and something else at rest.
///
/// The expected color and opacity are read from the registered style itself,
/// so retuning the shared wash does not break every row's test. The at-rest
/// check keeps a row that already happens to sit at the wash's values from
/// passing without a pressed style at all.
inline bool paints_press_wash(lv_obj_t* obj) {
    lv_xml_style_t* wash = lv_xml_get_style_by_name(nullptr, "styles.press_wash");
    if (!obj || !wash) {
        return false;
    }
    lv_style_value_t color{};
    lv_style_value_t opa{};
    if (lv_style_get_prop(&wash->style, LV_STYLE_BG_COLOR, &color) != LV_STYLE_RES_FOUND ||
        lv_style_get_prop(&wash->style, LV_STYLE_BG_OPA, &opa) != LV_STYLE_RES_FOUND) {
        return false;
    }

    auto paints = [&] {
        return lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == opa.num &&
               lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color.color);
    };

    const bool at_rest = paints();
    lv_obj_add_state(obj, LV_STATE_PRESSED);
    const bool pressed = paints();
    lv_obj_remove_state(obj, LV_STATE_PRESSED);
    return pressed && !at_rest;
}

/// True when @p obj carries a CLICKED handler (from XML or C++) - the part
/// of "tapping it does something" a row's own test can see without driving
/// the screen that owns it.
inline bool has_clicked_handler(lv_obj_t* obj, lv_event_cb_t cb = nullptr) {
    const uint32_t n = obj ? lv_obj_get_event_count(obj) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        const uint32_t code = dsc->filter & ~static_cast<uint32_t>(LV_EVENT_PREPROCESS);
        if ((code == LV_EVENT_CLICKED || code == LV_EVENT_ALL) &&
            (cb == nullptr || lv_event_dsc_get_cb(dsc) == cb)) {
            return true;
        }
    }
    return false;
}

/// The clickable ancestor of the label under @p root whose text is @p text.
inline lv_obj_t* row_with_label(lv_obj_t* root, const char* text) {
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* t = lv_label_get_text(child);
            if (t && std::strcmp(t, text) == 0) {
                return lv_obj_has_flag(root, LV_OBJ_FLAG_CLICKABLE) ? root : nullptr;
            }
        }
        if (lv_obj_t* found = row_with_label(child, text)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace helix::test
