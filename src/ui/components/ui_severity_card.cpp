// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_severity_card.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

/**
 * Get the shared severity style for a given severity string.
 * These styles are managed by ThemeManager and update automatically when theme changes.
 */
static lv_style_t* get_severity_style(const char* severity) {
    auto& tm = ThemeManager::instance();
    if (!severity || strcmp(severity, "info") == 0) {
        return tm.get_style(StyleRole::SeverityInfo);
    } else if (strcmp(severity, "error") == 0) {
        return tm.get_style(StyleRole::SeverityDanger);
    } else if (strcmp(severity, "warning") == 0) {
        return tm.get_style(StyleRole::SeverityWarning);
    } else if (strcmp(severity, "success") == 0) {
        return tm.get_style(StyleRole::SeveritySuccess);
    }
    return tm.get_style(StyleRole::SeverityInfo);
}

/**
 * XML create handler for severity_card
 * Creates an lv_obj widget when <severity_card> is encountered in XML
 */
static void* severity_card_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create((lv_obj_t*)parent);

    if (!obj) {
        spdlog::error("[SeverityCard] Failed to create lv_obj");
        return nullptr;
    }

    spdlog::trace("[SeverityCard] Created base lv_obj");
    return (void*)obj;
}

/**
 * XML apply handler for severity_card
 * Applies severity-based styling + XML attributes
 */
static void severity_card_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = (lv_obj_t*)item;

    if (!obj) {
        spdlog::error("[SeverityCard] NULL object in xml_apply");
        return;
    }

    // Extract severity attribute from attrs
    const char* severity = "info"; // default
    for (int i = 0; attrs[i]; i += 2) {
        if (strcmp(attrs[i], "severity") == 0) {
            severity = attrs[i + 1];
            break;
        }
    }

    // Store severity as user data for finalize to use later
    // Note: severity string comes from XML attrs and is stable
    lv_obj_set_user_data(obj, (void*)severity);

    // Apply standard lv_obj properties from XML first
    lv_xml_obj_apply(state, attrs);

    // Apply severity-based border color via shared style
    // Using shared styles enables automatic theme reactivity via LVGL's style system
    lv_style_t* style = get_severity_style(severity);
    if (style) {
        lv_obj_add_style(obj, style, LV_PART_MAIN);
    }

    spdlog::trace("[SeverityCard] Applied severity='{}', stored for finalize", severity);
}

void ui_severity_card_register(void) {
    lv_xml_register_widget("severity_card", severity_card_xml_create, severity_card_xml_apply);
    spdlog::trace("[SeverityCard] Registered <severity_card> widget with LVGL XML system");
}

void ui_severity_card_finalize(lv_obj_t* obj) {
    if (!obj) {
        spdlog::warn("[SeverityCard] finalize called with NULL obj");
        return;
    }

    // Get stored severity from user data
    const char* severity = (const char*)lv_obj_get_user_data(obj);
    if (!severity) {
        spdlog::debug("[SeverityCard] No severity in user_data, defaulting to 'info'");
        severity = "info";
    }

    // The card's XML defines four icons (icon_info, icon_success, icon_warning,
    // icon_error), all hidden. Unhiding the matching one keeps every glyph and
    // colour in XML.
    const char* icon_name = nullptr;
    if (strcmp(severity, "info") == 0) {
        icon_name = "icon_info";
    } else if (strcmp(severity, "success") == 0) {
        icon_name = "icon_success";
    } else if (strcmp(severity, "warning") == 0) {
        icon_name = "icon_warning";
    } else if (strcmp(severity, "error") == 0) {
        icon_name = "icon_error";
    } else {
        icon_name = "icon_info"; // Default to info
    }

    // Find and unhide the correct icon
    lv_obj_t* icon = lv_obj_find_by_name(obj, icon_name);
    if (icon) {
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
        spdlog::debug("[SeverityCard] Finalized: showing '{}' for severity='{}'", icon_name,
                      severity);
    } else {
        spdlog::warn("[SeverityCard] Could not find icon for severity='{}'", severity);
    }
}
