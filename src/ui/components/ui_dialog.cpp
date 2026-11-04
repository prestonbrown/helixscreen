// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_dialog.h"

#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

/**
 * XML create handler for ui_dialog
 * Creates an lv_obj widget when <ui_dialog> is encountered in XML
 * and applies theme-aware defaults. Defaults are set here (not in apply)
 * because create is called exactly once, while apply may be called multiple times.
 */
static void* ui_dialog_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create((lv_obj_t*)parent);

    if (!obj) {
        spdlog::error("[Dialog] Failed to create lv_obj");
        return nullptr;
    }

    // Apply shared dialog style (bg_color, bg_opa, radius - all reactive to theme changes)
    lv_style_t* dialog_style = ThemeManager::instance().get_style(StyleRole::Dialog);
    if (dialog_style) {
        // Remove any existing LV_PART_MAIN styles (from LVGL theme) so our shared style takes
        // effect
        lv_obj_remove_style(obj, nullptr, LV_PART_MAIN);
        lv_obj_add_style(obj, dialog_style, LV_PART_MAIN);
    } else {
        spdlog::warn("[Dialog] dialog_style is NULL - ThemeManager not initialized?");
    }

    // Disabled state: 50% opacity for visual feedback
    lv_obj_set_style_opa(obj, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);

    // No padding by default (dividers/buttons go edge-to-edge)
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);

    // No border by default
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);

    // No shadow by default
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);

    // Clip children to rounded corners (for full-bleed buttons at bottom)
    lv_obj_set_style_clip_corner(obj, true, LV_PART_MAIN);

    spdlog::trace("[Dialog] Created ui_dialog with theme-aware defaults");
    return (void*)obj;
}

void ui_dialog_register(void) {
    // Use standard lv_xml_obj_apply for XML attr processing - defaults are in create handler
    lv_xml_register_widget("ui_dialog", ui_dialog_xml_create, lv_xml_obj_apply);
    spdlog::trace("[Dialog] Registered <ui_dialog> widget with LVGL XML system");
}
