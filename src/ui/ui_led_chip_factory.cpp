// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_led_chip_factory.h"

#include "ui_fonts.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <string>
#include <utility>

namespace helix {
namespace ui {

// Store callback in user data structure
struct ChipData {
    std::string led_name;
    std::function<void(const std::string&)> on_click;
};

static void chip_click_cb(lv_event_t* e) {
    auto* data = static_cast<ChipData*>(lv_event_get_user_data(e));
    if (data && data->on_click) {
        data->on_click(data->led_name);
    }
}

static void chip_delete_cb(lv_event_t* e) {
    auto* data = static_cast<ChipData*>(lv_event_get_user_data(e));
    delete data;
}

/// Apply a chip's selected/unselected styling. Chips are rebuilt wholesale by
/// their owning panels, so the only caller is create_led_chip() below.
static void update_led_chip_state(lv_obj_t* chip, bool selected);

lv_obj_t* create_led_chip(lv_obj_t* parent, const std::string& led_name,
                          const std::string& display_name, bool selected,
                          std::function<void(const std::string&)> on_click) {
    // Create as lv_button for themed styling
    lv_obj_t* chip = lv_button_create(parent);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_radius(chip, 16, 0); // Pill shape
    int32_t chip_pad_tight = theme_manager_get_spacing("space_xxs");
    lv_obj_set_style_pad_hor(chip, theme_manager_get_spacing("space_md"), 0);
    lv_obj_set_style_pad_ver(chip, chip_pad_tight, 0);
    lv_obj_set_layout(chip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(chip, chip_pad_tight, 0);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

    // Remove from default input group to prevent focus shift on click
    lv_group_t* group = lv_group_get_default();
    if (group) {
        lv_group_remove_obj(chip);
    }
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    // Check icon (hidden when unselected), uses icon font token
    lv_obj_t* icon = lv_label_create(chip);
    lv_obj_set_name(icon, "check_icon");
    lv_label_set_text(icon, ICON_CHECK);
    lv_obj_set_style_text_font(icon, theme_manager_get_font("icon_font_sm"), 0);

    // Label
    lv_obj_t* label = lv_label_create(chip);
    lv_obj_set_name(label, "chip_label");
    lv_label_set_text(label, display_name.c_str());

    // Apply initial state
    update_led_chip_state(chip, selected);

    // Store callback data (freed on chip delete)
    auto* data = new ChipData{led_name, std::move(on_click)};
    lv_obj_add_event_cb(chip, chip_click_cb, LV_EVENT_CLICKED, data);
    lv_obj_add_event_cb(chip, chip_delete_cb, LV_EVENT_DELETE, data);

    return chip;
}

static void update_led_chip_state(lv_obj_t* chip, bool selected) {
    lv_obj_t* icon = lv_obj_find_by_name(chip, "check_icon");
    auto& tm = ThemeManager::instance();

    if (selected) {
        // Tertiary button style for background
        lv_style_t* style = tm.get_style(StyleRole::ButtonTertiary);
        if (style) {
            lv_obj_add_style(chip, style, LV_PART_MAIN);
        }
        lv_obj_set_style_border_width(chip, 0, 0);

        // Tertiary is an accent fill: black-or-white, not the palette's muted text
        lv_color_t bg = theme_manager_get_color("tertiary");
        lv_color_t text = theme_manager_get_readable_on(bg);
        lv_obj_set_style_text_color(chip, text, 0);

        if (icon) {
            lv_obj_set_style_opa(icon, LV_OPA_COVER, 0);
        }
    } else {
        // Outline button style for unselected
        lv_style_t* style = tm.get_style(StyleRole::ButtonOutline);
        if (style) {
            lv_obj_add_style(chip, style, LV_PART_MAIN);
        }

        if (icon) {
            // Invisible but still occupies layout space
            lv_obj_set_style_opa(icon, LV_OPA_TRANSP, 0);
        }
    }
}

} // namespace ui
} // namespace helix
