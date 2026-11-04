// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_card.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

/**
 * XML create handler for ui_card
 * Creates an lv_obj widget when <ui_card> is encountered in XML
 * and applies theme-aware defaults. Defaults are set here (not in apply)
 * because create is called exactly once, while apply may be called multiple times.
 */
static void* ui_card_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create((lv_obj_t*)parent);

    if (!obj) {
        spdlog::error("[Card] Failed to create lv_obj");
        return nullptr;
    }

    // Apply shared card style (bg_color, bg_opa, border, radius - all reactive to theme changes)
    // Remove any existing LV_PART_MAIN styles (from LVGL theme) so our shared style takes effect
    lv_obj_remove_style(obj, nullptr, LV_PART_MAIN);
    lv_obj_add_style(obj, ThemeManager::instance().get_style(StyleRole::Card), LV_PART_MAIN);

    // Restore content sizing that was lost when we removed theme styles above
    // (theme applies ObjBase with LV_SIZE_CONTENT, but remove_style strips it)
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    // Pressed state: the theme's tier rule, shared with buttons.
    lv_obj_add_style(obj, ThemeManager::instance().get_style(StyleRole::CardPressed),
                     LV_PART_MAIN | LV_STATE_PRESSED);

    // Disabled state: 50% opacity for visual feedback
    lv_obj_set_style_opa(obj, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);

    // Disable scrolling (cards are fixed containers, not scroll areas)
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    // Shadow: 0 (no shadow by default - can be overridden in XML)
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);

    // Padding: responsive via space_md token
    int32_t padding = theme_manager_get_spacing("space_md");
    lv_obj_set_style_pad_all(obj, padding, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(obj, padding, LV_PART_MAIN);

    spdlog::trace("[Card] Created ui_card with theme-aware defaults");
    return (void*)obj;
}

void ui_card_register(void) {
    // Use standard lv_xml_obj_apply for XML attr processing - defaults are in create handler
    lv_xml_register_widget("ui_card", ui_card_xml_create, lv_xml_obj_apply);
    spdlog::trace("[Card] Registered <ui_card> widget with LVGL XML system");
}
