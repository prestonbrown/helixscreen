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

// The default badge box rides a shared ADDED style, never a local one: a local
// width/height outranks every style bound from XML, so the tile rung styles
// (styles.tile_badge_*) could never move it. Keyed by px value and static for
// the process, the same shape as helix::ui::shared_font_style.
constexpr size_t MAX_BOX_STYLES = 16;
int32_t g_box_sizes[MAX_BOX_STYLES] = {};
lv_style_t g_box_styles[MAX_BOX_STYLES];

lv_style_t* shared_badge_box_style(int32_t size) {
    for (size_t i = 0; i < MAX_BOX_STYLES; ++i) {
        if (g_box_sizes[i] == size)
            return &g_box_styles[i];
        if (g_box_sizes[i] == 0) {
            g_box_sizes[i] = size;
            lv_style_init(&g_box_styles[i]);
            lv_style_set_width(&g_box_styles[i], size);
            lv_style_set_height(&g_box_styles[i], size);
            lv_style_set_radius(&g_box_styles[i], LV_RADIUS_CIRCLE);
            return &g_box_styles[i];
        }
    }
    spdlog::critical("[notification_badge] shared box style table full ({} entries)",
                     MAX_BOX_STYLES);
    return nullptr;
}

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

    // The severity fill is an accent, so the count starts from the palette text colour
    // and shifts toward its pole as 4:1 needs
    lv_color_t bg = lv_obj_get_style_bg_color(badge, LV_PART_MAIN);
    lv_color_t text_color =
        theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"), bg);

    lv_obj_set_style_text_color(label, text_color, LV_PART_MAIN);

    spdlog::trace("[notification_badge] contrast update: bg=0x{:06X} text=0x{:06X}",
                  lv_color_to_u32(bg) & 0xFFFFFF, lv_color_to_u32(text_color) & 0xFFFFFF);
}

/**
 * @brief Pick the count face that fits the badge circle
 *
 * The badge box runs 6-38px across the tile rungs, so no single face serves
 * it: take the largest small-text face whose line height fits the box, falling
 * back to the smallest face when the box is too small for any (the digit then
 * rides the dot rather than the circle).
 */
void fit_badge_label_font(lv_obj_t* badge) {
    BadgeData* data = static_cast<BadgeData*>(lv_obj_get_user_data(badge));
    if (!data || data->magic != BadgeData::MAGIC || !data->label) {
        return;
    }

    const int32_t box = lv_obj_get_style_width(badge, LV_PART_MAIN);
    const lv_font_t* best = nullptr;
    const lv_font_t* smallest = nullptr;
    for (const char* name : {"font_xs", "font_small"}) {
        const lv_font_t* font = theme_manager_get_font(name);
        if (!font)
            continue;
        if (!smallest || font->line_height < smallest->line_height)
            smallest = font;
        if (font->line_height <= box && (!best || font->line_height > best->line_height))
            best = font;
    }
    if (best || smallest) {
        lv_obj_set_style_text_font(data->label, best ? best : smallest, LV_PART_MAIN);
        lv_obj_center(data->label);
    }
}

/**
 * @brief Event callback for style changes - update text contrast
 */
void badge_style_changed_cb(lv_event_t* e) {
    lv_obj_t* badge = lv_event_get_target_obj(e);
    // Defer to avoid setting styles during refresh_children_style cascade (#729)
    helix::ui::queue_widget_update(badge, [](lv_obj_t* b) {
        update_badge_text_contrast(b);
        fit_badge_label_font(b);
    });
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

/// The text a bound subject carries: a string subject, or a pointer subject
/// holding a C string.
const char* badge_subject_text(lv_subject_t* subject) {
    if (subject->type == LV_SUBJECT_TYPE_STRING) {
        return lv_subject_get_string(subject);
    }
    return static_cast<const char*>(lv_subject_get_pointer(subject));
}

/**
 * @brief Observer callback to update label text when subject changes
 */
void badge_text_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* label = static_cast<lv_obj_t*>(lv_observer_get_user_data(observer));
    if (!label)
        return;

    const char* text = badge_subject_text(subject);
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

    // Default styling - circular badge using responsive token. The box rides
    // an added style so the rung styles bound in XML can replace it; only the
    // props no binding ever carries stay local.
    int32_t badge_sz = theme_manager_get_spacing("badge_size");
    if (badge_sz <= 0)
        badge_sz = 18; // fallback
    if (lv_style_t* box = shared_badge_box_style(badge_sz)) {
        lv_obj_add_style(badge, box, LV_PART_MAIN);
    }
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
    lv_obj_center(label);

    // Allocate user data to track label reference (for safe style change handling)
    BadgeData* data = new BadgeData{.magic = BadgeData::MAGIC, .label = label};
    lv_obj_set_user_data(badge, data);

    // Face follows the box (the rung bindings land after create and refit it
    // through the style-change callback)
    fit_badge_label_font(badge);

    // Handle bind_text - connect subject to internal label
    const char* bind_text = lv_xml_get_value_of(attrs, "bind_text");
    if (bind_text && strlen(bind_text) > 0) {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, bind_text);
        if (subject) {
            // Set initial value
            const char* initial = badge_subject_text(subject);
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
