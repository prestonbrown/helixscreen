// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace {

/**
 * @brief User data stored on badge to track label reference
 *
 * NOTE: Magic number required for safety during style broadcasts.
 * When lv_obj_report_style_change(NULL) fires, the STYLE_CHANGED event
 * goes to all objects - including badges that may have been deleted
 * but whose observers haven't been cleaned up yet. The magic check
 * prevents crashes by detecting stale/invalid user_data.
 */
struct BadgeData {
    static constexpr uint32_t MAGIC = 0x42444745; // "BDGE"
    uint32_t magic{MAGIC};
    lv_obj_t* label; // Label widget for count display
};

/**
 * @brief Update badge text color based on background luminance
 */
void update_badge_text_contrast(lv_obj_t* badge) {
    // Check magic to ensure user_data is valid (not stale/overwritten)
    BadgeData* data = static_cast<BadgeData*>(lv_obj_get_user_data(badge));
    if (!data || data->magic != BadgeData::MAGIC) {
        return;
    }

    lv_obj_t* label = data->label;
    if (!label) {
        return;
    }

    // The severity fill is an accent, so the count is black-or-white, not palette text
    lv_color_t bg = lv_obj_get_style_bg_color(badge, LV_PART_MAIN);
    lv_color_t text_color = theme_manager_get_readable_on(bg);

    lv_obj_set_style_text_color(label, text_color, LV_PART_MAIN);

    spdlog::trace("[notification_badge] contrast update: bg=0x{:06X} text=0x{:06X}",
                  lv_color_to_u32(bg) & 0xFFFFFF, lv_color_to_u32(text_color) & 0xFFFFFF);
}

/**
 * @brief Event callback for style changes - update text contrast
 */
void badge_style_changed_cb(lv_event_t* e) {
    lv_obj_t* badge = lv_event_get_target_obj(e);
    // Defer to avoid setting styles during refresh_children_style cascade (#729)
    helix::ui::async_call(
        badge, [](void* data) { update_badge_text_contrast(static_cast<lv_obj_t*>(data)); }, badge);
}

/**
 * @brief Event callback for LV_EVENT_DELETE
 *
 * Called when badge is deleted. Frees the BadgeData user data.
 */
void badge_delete_cb(lv_event_t* e) {
    lv_obj_t* badge = lv_event_get_target_obj(e);
    BadgeData* data = static_cast<BadgeData*>(lv_obj_get_user_data(badge));
    // Only delete if magic matches - user_data may be invalid
    if (data && data->magic == BadgeData::MAGIC) {
        delete data;
        lv_obj_set_user_data(badge, nullptr);
    }
}

/**
 * @brief Observer callback to update label text when subject changes
 */
void badge_text_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* label = static_cast<lv_obj_t*>(lv_observer_get_user_data(observer));
    if (!label)
        return;

    const char* text = static_cast<const char*>(lv_subject_get_pointer(subject));
    if (text) {
        lv_label_set_text(label, text);
        // Re-center after text change
        lv_obj_center(label);
    }
}

/**
 * @brief XML create handler for notification_badge
 *
 * Creates a circular badge with:
 * - Background color bound to severity
 * - Auto-contrast text color
 * - Child label for count display
 */
void* notification_badge_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));

    // Create badge container
    lv_obj_t* badge = lv_obj_create(parent);

    // Default styling - circular badge using responsive token
    int32_t badge_sz = theme_manager_get_spacing("badge_size");
    if (badge_sz <= 0)
        badge_sz = 18; // fallback
    lv_obj_set_size(badge, badge_sz, badge_sz);
    lv_obj_set_style_radius(badge, badge_sz / 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 0, LV_PART_MAIN);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    // Background color is driven by the severity-bound styles in XML
    // (badge_info/badge_warning/badge_error via bind_style). Do NOT set a local
    // bg_color here: a local style outranks the added bind_style styles in
    // LVGL's cascade, which would freeze the badge at one color regardless of
    // severity. Only the opacity (which the bound styles don't set) is local.
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);

    // Parse text attribute
    const char* text = lv_xml_get_value_of(attrs, "text");
    if (!text) {
        text = "0";
    }

    // Create label for count
    lv_obj_t* label = lv_label_create(badge);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_small"), LV_PART_MAIN);
    lv_obj_center(label);

    // Allocate user data to track label reference (for safe style change handling)
    BadgeData* data = new BadgeData{.magic = BadgeData::MAGIC, .label = label};
    lv_obj_set_user_data(badge, data);

    // Handle bind_text - connect subject to internal label
    const char* bind_text = lv_xml_get_value_of(attrs, "bind_text");
    if (bind_text && strlen(bind_text) > 0) {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, bind_text);
        if (subject) {
            // Set initial value
            const char* initial = static_cast<const char*>(lv_subject_get_pointer(subject));
            if (initial) {
                lv_label_set_text(label, initial);
            }
            // Subscribe to updates - observer freed when label is deleted
            lv_subject_add_observer_obj(subject, badge_text_observer_cb, label, label);
            spdlog::trace("[notification_badge] Bound text to subject '{}'", bind_text);
        } else {
            spdlog::warn("[notification_badge] Subject '{}' not found for bind_text", bind_text);
        }
    }

    // Apply initial text contrast
    update_badge_text_contrast(badge);

    // Register for style changes to update contrast when bg changes
    lv_obj_add_event_cb(badge, badge_style_changed_cb, LV_EVENT_STYLE_CHANGED, nullptr);

    // Register delete callback to free BadgeData
    lv_obj_add_event_cb(badge, badge_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::trace("[notification_badge] Created badge text='{}'", text);
    return badge;
}

} // namespace

extern "C" {

void ui_notification_badge_init() {
    lv_xml_register_widget("notification_badge", notification_badge_create, lv_xml_obj_apply);
    spdlog::trace("[notification_badge] Registered widget");
}

} // extern "C"
