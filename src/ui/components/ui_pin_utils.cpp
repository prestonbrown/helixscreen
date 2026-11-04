// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pin_utils.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <lvgl.h>

namespace helix::ui {

void update_pin_dots(lv_obj_t* parent, const char* dot_prefix, int max_dots, int filled_count) {
    if (!parent) {
        return;
    }

    lv_color_t text_color = theme_manager_get_color("text");
    lv_color_t muted_color = theme_manager_get_color("text_muted");

    for (int i = 0; i < max_dots; i++) {
        char name[32];
        snprintf(name, sizeof(name), "%s%d", dot_prefix, i);
        lv_obj_t* dot = lv_obj_find_by_name(parent, name);
        if (!dot) {
            spdlog::warn("[PinUtils] Dot widget '{}' not found", name);
            continue;
        }

        if (i < filled_count) {
            // Filled -- digit entered at this position
            lv_obj_set_style_bg_color(dot, text_color, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
        } else {
            // Empty -- not yet entered
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(dot, 2, 0);
            lv_obj_set_style_border_color(dot, muted_color, 0);
            lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
        }
    }
}

void show_pin_error(lv_obj_t* parent, const char* label_name, const char* text) {
    if (!parent) {
        return;
    }
    lv_obj_t* label = lv_obj_find_by_name(parent, label_name);
    if (label) {
        if (text) {
            lv_label_set_text(label, text);
        }
        lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
    }
}

void hide_pin_error(lv_obj_t* parent, const char* label_name) {
    if (!parent) {
        return;
    }
    lv_obj_t* label = lv_obj_find_by_name(parent, label_name);
    if (label) {
        lv_label_set_text(label, " ");
        lv_obj_set_style_opa(label, LV_OPA_TRANSP, 0);
    }
}

} // namespace helix::ui
