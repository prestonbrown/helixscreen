// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_split_button.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "sound_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace helix;

namespace {

struct SplitButtonData {
    static constexpr uint32_t MAGIC = 0x53504C54; // "SPLT"
    uint32_t magic{MAGIC};
    lv_obj_t* main_btn{nullptr};
    lv_obj_t* arrow_btn{nullptr};
    lv_obj_t* dropdown{nullptr};
    lv_obj_t* icon{nullptr};
    lv_obj_t* label{nullptr};
    lv_obj_t* click_overlay{nullptr}; // screen-level overlay for click-outside dismiss
    char* text_format{nullptr};       // owned, nullable
    bool show_selection{true};
};

// Store data on main_btn (first child), NOT on sb itself.
// Callers (preheat_widget) may overwrite sb's user_data.
SplitButtonData* get_data(lv_obj_t* sb) {
    if (!sb)
        return nullptr;
    lv_obj_t* first_child = lv_obj_get_child(sb, 0);
    if (!first_child)
        return nullptr;
    auto* data = static_cast<SplitButtonData*>(lv_obj_get_user_data(first_child));
    if (!data || data->magic != SplitButtonData::MAGIC)
        return nullptr;
    return data;
}

static const lv_font_t* s_icon_font = nullptr;
static bool s_icon_font_resolved = false;

/**
 * @brief Get icon font for button icons (same approach as ui_button)
 */
const lv_font_t* get_button_icon_font() {
    if (s_icon_font_resolved)
        return s_icon_font;

    const char* font_name = lv_xml_get_const_silent(nullptr, "icon_font_sm");
    const lv_font_t* font = nullptr;
    if (font_name) {
        font = lv_xml_get_font(nullptr, font_name);
    }
    if (!font) {
        font = &mdi_icons_24;
    }

    s_icon_font = font;
    s_icon_font_resolved = true;
    return s_icon_font;
}

/**
 * @brief Update text contrast for inner widgets based on container background
 */
void update_split_button_contrast(lv_obj_t* sb) {
    auto* data = get_data(sb);
    if (!data)
        return;

    lv_opa_t bg_opa = lv_obj_get_style_bg_opa(sb, LV_PART_MAIN);
    bool is_ghost = bg_opa < LV_OPA_50;

    lv_color_t text_color;
    if (is_ghost) {
        text_color = theme_manager_get_color("text");
    } else {
        lv_color_t bg = lv_obj_get_style_bg_color(sb, LV_PART_MAIN);
        text_color = theme_manager_get_contrast_color(bg);
    }

    auto set_contrast = [&](lv_obj_t* obj) {
        if (!obj)
            return;
        lv_obj_set_style_text_color(obj, text_color, LV_PART_MAIN);
    };

    set_contrast(data->label);
    set_contrast(data->icon);

    // Update the arrow button's chevron icon
    if (data->arrow_btn) {
        lv_obj_t* arrow_icon = lv_obj_get_child(data->arrow_btn, 0);
        set_contrast(arrow_icon);
    }
}

void split_button_style_changed_cb(lv_event_t* e) {
    lv_obj_t* sb = lv_event_get_target_obj(e);
    // Defer contrast update to avoid setting styles during refresh_children_style
    // cascade — same fix as ui_button.cpp (#729).
    helix::ui::async_call(
        sb, [](void* data) { update_split_button_contrast(static_cast<lv_obj_t*>(data)); }, sb);
}

/**
 * @brief Constrain label width to available space in main_btn.
 * Must be called after any lv_label_set_text() which resets label to content width.
 */
void constrain_label_width(SplitButtonData* data) {
    if (!data || !data->label || !data->main_btn)
        return;

    int32_t avail = lv_obj_get_content_width(data->main_btn);
    if (data->icon) {
        avail -= lv_obj_get_width(data->icon);
        avail -= lv_obj_get_style_pad_column(data->main_btn, LV_PART_MAIN);
    }
    if (avail > 0) {
        lv_obj_set_width(data->label, avail);
    }
}

/**
 * @brief Update the button label from current dropdown selection
 */
void update_label_from_selection(SplitButtonData* data) {
    if (!data || !data->label || !data->dropdown || !data->show_selection)
        return;

    char selected_text[128];
    lv_dropdown_get_selected_str(data->dropdown, selected_text, sizeof(selected_text));

    if (data->text_format) {
        char formatted[256];
        snprintf(formatted, sizeof(formatted), data->text_format, selected_text);
        lv_label_set_text(data->label, formatted);
    } else {
        lv_label_set_text(data->label, selected_text);
    }
    constrain_label_width(data);
}

/**
 * @brief Main button clicked — forward to container + play sound
 */
void main_btn_clicked_cb(lv_event_t* e) {
    lv_obj_t* main_btn = lv_event_get_target_obj(e);
    lv_obj_t* sb = lv_obj_get_parent(main_btn);
    SoundManager::instance().play("button_tap");
    lv_obj_send_event(sb, LV_EVENT_CLICKED, nullptr);
}

/**
 * @brief Style the dropdown list to match the button variant colors
 */
void style_dropdown_list(lv_obj_t* sb, lv_obj_t* list) {
    if (!sb || !list)
        return;

    lv_color_t bg_color = lv_obj_get_style_bg_color(sb, LV_PART_MAIN);
    lv_color_t text_color = theme_manager_get_contrast_color(bg_color);

    lv_obj_set_style_bg_color(list, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(list, text_color, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, text_color, LV_PART_MAIN);
    lv_obj_set_style_border_opa(list, LV_OPA_30, LV_PART_MAIN);
    // Selected item highlight
    lv_obj_set_style_bg_color(list, text_color, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(list, LV_OPA_20, LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, text_color, LV_PART_SELECTED);
}

/**
 * @brief Close dropdown and remove the click-outside overlay
 */
void close_dropdown(SplitButtonData* data) {
    if (!data)
        return;
    if (data->click_overlay) {
        lv_obj_delete(data->click_overlay);
        data->click_overlay = nullptr;
    }
    if (data->dropdown) {
        lv_dropdown_close(data->dropdown);
    }
}

/**
 * @brief Arrow button clicked — toggle dropdown open/close + play sound
 */
void arrow_btn_clicked_cb(lv_event_t* e) {
    lv_obj_t* arrow_btn = lv_event_get_target_obj(e);
    lv_obj_t* sb = lv_obj_get_parent(arrow_btn);
    auto* data = get_data(sb);
    if (!data || !data->dropdown)
        return;

    lv_event_stop_bubbling(e); // Don't trigger container's CLICKED (main action)
    SoundManager::instance().play("button_tap");

    // Toggle: close if already open
    if (lv_dropdown_is_open(data->dropdown)) {
        close_dropdown(data);
        return;
    }

    lv_dropdown_open(data->dropdown);

    // Style and reposition the list
    lv_obj_t* list = lv_dropdown_get_list(data->dropdown);
    if (list) {
        style_dropdown_list(sb, list);

        // Smart positioning: open upward if dropdown would overflow the screen bottom
        lv_obj_update_layout(list);
        int32_t sb_y = lv_obj_get_y(sb);
        // Walk up parents to get absolute screen Y
        lv_obj_t* p = lv_obj_get_parent(sb);
        while (p && p != lv_screen_active()) {
            sb_y += lv_obj_get_y(p) - lv_obj_get_scroll_y(p);
            p = lv_obj_get_parent(p);
        }
        int32_t sb_h = lv_obj_get_height(sb);
        int32_t list_h = lv_obj_get_height(list);
        int32_t screen_h = lv_display_get_vertical_resolution(nullptr);

        if (sb_y + sb_h + list_h > screen_h) {
            lv_obj_align_to(list, sb, LV_ALIGN_OUT_TOP_RIGHT, 0, 0);
        } else {
            lv_obj_align_to(list, sb, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 0);
        }

        // Create a transparent fullscreen overlay behind the list to catch outside clicks
        lv_obj_t* screen = lv_screen_active();
        data->click_overlay = lv_obj_create(screen);
        lv_obj_set_size(data->click_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(data->click_overlay, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(data->click_overlay, 0, LV_PART_MAIN);
        lv_obj_remove_flag(data->click_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(data->click_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(data->click_overlay, sb);

        // Move overlay behind the dropdown list but above everything else
        lv_obj_move_to_index(data->click_overlay, -1);
        lv_obj_move_to_index(list, -1);

        lv_obj_add_event_cb(
            data->click_overlay,
            [](lv_event_t* ev) {
                lv_obj_t* overlay = lv_event_get_target_obj(ev);
                auto* split_btn = static_cast<lv_obj_t*>(lv_obj_get_user_data(overlay));
                auto* d = get_data(split_btn);
                close_dropdown(d);
            },
            LV_EVENT_CLICKED, nullptr);
    }
}

/**
 * @brief Dropdown value changed — update label + forward event to container
 */
void dropdown_value_changed_cb(lv_event_t* e) {
    lv_obj_t* dropdown = lv_event_get_target_obj(e);
    lv_obj_t* sb = lv_obj_get_parent(dropdown);
    auto* data = get_data(sb);
    if (!data)
        return;

    // Clean up the click-outside overlay since the dropdown is closing
    if (data->click_overlay) {
        lv_obj_delete(data->click_overlay);
        data->click_overlay = nullptr;
    }

    update_label_from_selection(data);
    lv_obj_send_event(sb, LV_EVENT_VALUE_CHANGED, nullptr);
}

/**
 * @brief Delete callback — clean up user data
 */
void split_button_delete_cb(lv_event_t* e) {
    lv_obj_t* sb = lv_event_get_target_obj(e);
    auto* data = get_data(sb);
    if (!data)
        return;

    if (data->click_overlay) {
        lv_obj_delete(data->click_overlay);
        data->click_overlay = nullptr;
    }
    lv_free(data->text_format);
    if (data->main_btn) {
        lv_obj_set_user_data(data->main_btn, nullptr);
    }
    delete data;
}

/**
 * @brief Create an icon label inside a parent
 */
lv_obj_t* create_icon(lv_obj_t* parent, const char* icon_name) {
    if (!icon_name || strlen(icon_name) == 0)
        return nullptr;

    const char* codepoint = ui_icon::lookup_codepoint(icon_name);
    if (!codepoint) {
        const char* stripped = ui_icon::strip_legacy_prefix(icon_name);
        if (stripped != icon_name) {
            codepoint = ui_icon::lookup_codepoint(stripped);
        }
    }
    if (!codepoint) {
        spdlog::warn("[ui_split_button] Icon '{}' not found", icon_name);
        return nullptr;
    }

    lv_obj_t* icon = lv_label_create(parent);
    lv_label_set_text(icon, codepoint);
    lv_obj_set_style_text_font(icon, get_button_icon_font(), LV_PART_MAIN);
    return icon;
}

/**
 * @brief Apply variant style to the container (same logic as ui_button)
 */
lv_style_t* get_variant_style(const char* variant_str) {
    auto& tm = ThemeManager::instance();
    if (strcmp(variant_str, "secondary") == 0)
        return tm.get_style(StyleRole::ButtonSecondary);
    if (strcmp(variant_str, "danger") == 0)
        return tm.get_style(StyleRole::ButtonDanger);
    if (strcmp(variant_str, "success") == 0)
        return tm.get_style(StyleRole::ButtonSuccess);
    if (strcmp(variant_str, "tertiary") == 0)
        return tm.get_style(StyleRole::ButtonTertiary);
    if (strcmp(variant_str, "warning") == 0)
        return tm.get_style(StyleRole::ButtonWarning);
    if (strcmp(variant_str, "ghost") == 0)
        return tm.get_style(StyleRole::ButtonGhost);
    if (strcmp(variant_str, "outline") == 0)
        return tm.get_style(StyleRole::ButtonOutline);
    return tm.get_style(StyleRole::ButtonPrimary);
}

#if LV_USE_TRANSLATION
// Translate a newline-delimited options list through the active language and
// apply it to the dropdown. Mirrors the <lv_dropdown options_tag="..."> handling
// in lv_xml_dropdown_parser.c: each line is an i18n key passed through lv_tr().
void apply_translated_options(lv_obj_t* dropdown, const char* tags) {
    std::string translated;
    const char* p = tags;
    while (*p) {
        const char* nl = std::strchr(p, '\n');
        size_t seg_len = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
        std::string key(p, seg_len);
        translated += lv_tr(key.c_str());
        if (nl) {
            translated += '\n';
            p = nl + 1;
        } else {
            break;
        }
    }
    lv_dropdown_set_options(dropdown, translated.c_str());
}

// Re-translate options when the UI language changes at runtime. The owned tags
// copy is freed on the dropdown's LV_EVENT_DELETE.
void split_options_language_changed_cb(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* tags = static_cast<char*>(lv_event_get_user_data(e));
    if (dropdown && tags)
        apply_translated_options(dropdown, tags);
}

void split_options_free_tags_cb(lv_event_t* e) {
    lv_free(lv_event_get_user_data(e));
}
#endif

/**
 * @brief XML create callback for <ui_split_button>
 */
void* ui_split_button_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));

    // Create outer container (lv_obj, not lv_button — so we get raw obj styling)
    lv_obj_t* sb = lv_obj_create(parent);
    lv_obj_set_height(sb, theme_manager_get_spacing("button_height"));
    lv_obj_set_style_pad_all(sb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(sb, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    // Apply variant style to container
    const char* variant_str = lv_xml_get_value_of(attrs, "variant");
    if (!variant_str)
        variant_str = "primary";

    lv_style_t* style = get_variant_style(variant_str);
    if (style) {
        lv_obj_add_style(sb, style, LV_PART_MAIN);
    }

    // Parse attributes
    const char* text = lv_xml_get_value_of(attrs, "text");
    if (!text)
        text = "";
    const char* icon_name = lv_xml_get_value_of(attrs, "icon");
    const char* options = lv_xml_get_value_of(attrs, "options");
    const char* options_tag = lv_xml_get_value_of(attrs, "options_tag");

    // Allocate user data (stored on main_btn child, not sb — see get_data())
    auto* data = new SplitButtonData{};

    // --- Main button area (clickable lv_obj, flex_grow=1) ---
    // Using lv_obj instead of lv_button to avoid button's content-based min sizing
    data->main_btn = lv_obj_create(sb);
    lv_obj_set_user_data(data->main_btn, data); // Store data here (sb's user_data is for callers)
    lv_obj_add_flag(data->main_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(data->main_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(data->main_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data->main_btn, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(data->main_btn, 1);
    lv_obj_set_width(data->main_btn, 0);
    lv_obj_set_style_min_width(data->main_btn, 0, LV_PART_MAIN);
    lv_obj_set_height(data->main_btn, lv_pct(100));
    lv_obj_remove_flag(data->main_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(data->main_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(data->main_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(data->main_btn, theme_manager_get_spacing("space_xxs"),
                                LV_PART_MAIN);
    lv_obj_set_style_pad_left(data->main_btn, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);

    // Icon (optional)
    if (icon_name && strlen(icon_name) > 0) {
        data->icon = create_icon(data->main_btn, icon_name);
    }

    // Label — width=1 so flex doesn't expand main_btn to content size.
    // Height = 1 line so DOTS mode truncates (DOTS needs height overflow to trigger).
    // Correct width is set by async callback after layout resolves.
    data->label = lv_label_create(data->main_btn);
    lv_obj_set_width(data->label, 1);
    const lv_font_t* text_font = lv_obj_get_style_text_font(data->label, LV_PART_MAIN);
    lv_obj_set_height(data->label, lv_font_get_line_height(text_font));
    lv_label_set_long_mode(data->label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(data->label, text);

    // Main button click handler
    lv_obj_add_event_cb(data->main_btn, main_btn_clicked_cb, LV_EVENT_CLICKED, nullptr);

    // --- Divider ---
    lv_obj_t* divider = lv_obj_create(sb);
    lv_obj_set_size(divider, 1, lv_pct(60));
    lv_obj_set_style_bg_opa(divider, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        divider, theme_manager_get_contrast_color(lv_obj_get_style_bg_color(sb, LV_PART_MAIN)),
        LV_PART_MAIN);
    lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    // --- Arrow button (ghost, covers full area right of divider) ---
    data->arrow_btn = lv_obj_create(sb);
    lv_obj_add_flag(data->arrow_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(data->arrow_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(data->arrow_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data->arrow_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(data->arrow_btn, theme_manager_get_spacing("space_xxs"),
                              LV_PART_MAIN);
    lv_obj_set_style_pad_right(data->arrow_btn, theme_manager_get_spacing("space_xs"),
                               LV_PART_MAIN);
    lv_obj_set_size(data->arrow_btn, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_remove_flag(data->arrow_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(data->arrow_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(data->arrow_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Chevron down icon
    lv_obj_t* chevron = lv_label_create(data->arrow_btn);
    const char* chevron_cp = ui_icon::lookup_codepoint("chevron_down");
    if (chevron_cp) {
        lv_label_set_text(chevron, chevron_cp);
    }
    lv_obj_set_style_text_font(chevron, get_button_icon_font(), LV_PART_MAIN);

    // Arrow button click handler
    lv_obj_add_event_cb(data->arrow_btn, arrow_btn_clicked_cb, LV_EVENT_CLICKED, nullptr);

    // --- Hidden dropdown (zero-height, full-width for popup list positioning) ---
    data->dropdown = lv_dropdown_create(sb);
    lv_obj_set_size(data->dropdown, LV_SIZE_CONTENT, 0);
    lv_obj_set_style_opa(data->dropdown, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(data->dropdown, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data->dropdown, 0, LV_PART_MAIN);
    lv_obj_remove_flag(data->dropdown, LV_OBJ_FLAG_CLICKABLE);
    lv_dropdown_set_dir(data->dropdown, LV_DIR_BOTTOM);

#if LV_USE_TRANSLATION
    if (options_tag && strlen(options_tag) > 0) {
        // Translate the options through the active language, and re-translate on
        // runtime language change (tags copy freed on dropdown delete).
        char* tags_copy = lv_strdup(options_tag);
        apply_translated_options(data->dropdown, options_tag);
        lv_obj_add_event_cb(data->dropdown, split_options_language_changed_cb,
                            LV_EVENT_TRANSLATION_LANGUAGE_CHANGED, tags_copy);
        lv_obj_add_event_cb(data->dropdown, split_options_free_tags_cb, LV_EVENT_DELETE, tags_copy);
    } else
#endif
        if (options && strlen(options) > 0) {
        lv_dropdown_set_options(data->dropdown, options);
    }

    // Dropdown value changed handler
    lv_obj_add_event_cb(data->dropdown, dropdown_value_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Compute label width from available space after layout resolves.
    // Must defer because flex_grow sizes aren't final during create.
    // Widget-safe async_call (lv_obj_is_valid guard): during rapid panel
    // teardown the container can be freed before this deferral fires, and a
    // raw lv_async_call would then dereference freed memory via get_data()
    // (#980 SIGSEGV). Same guard the style-change deferral above uses.
    helix::ui::async_call(
        sb,
        [](void* user_data) {
            auto* sb = static_cast<lv_obj_t*>(user_data);
            auto* d = get_data(sb);
            if (!d || !d->label || !d->main_btn)
                return;

            lv_obj_update_layout(sb);

            int32_t avail = lv_obj_get_content_width(d->main_btn);
            if (d->icon) {
                avail -= lv_obj_get_width(d->icon);
                avail -= lv_obj_get_style_pad_column(d->main_btn, LV_PART_MAIN);
            }
            if (avail < 0)
                avail = 0;
            lv_obj_set_width(d->label, avail);
            spdlog::debug("[ui_split_button] Label width set to {} (main_btn content_w={})", avail,
                          lv_obj_get_content_width(d->main_btn));
        },
        sb);

    // Also recompute label width on resize (breakpoint changes, etc.)
    lv_obj_add_event_cb(
        sb,
        [](lv_event_t* e) {
            lv_obj_t* obj = lv_event_get_target_obj(e);
            auto* d = get_data(obj);
            if (!d || !d->label || !d->main_btn)
                return;

            int32_t avail = lv_obj_get_content_width(d->main_btn);
            if (d->icon) {
                avail -= lv_obj_get_width(d->icon);
                avail -= lv_obj_get_style_pad_column(d->main_btn, LV_PART_MAIN);
            }
            if (avail < 0)
                avail = 0;
            lv_obj_set_width(d->label, avail);
        },
        LV_EVENT_SIZE_CHANGED, nullptr);

    // Register event handlers on container
    lv_obj_add_event_cb(sb, split_button_style_changed_cb, LV_EVENT_STYLE_CHANGED, nullptr);
    lv_obj_add_event_cb(sb, split_button_style_changed_cb, LV_EVENT_STATE_CHANGED, nullptr);
    lv_obj_add_event_cb(sb, split_button_delete_cb, LV_EVENT_DELETE, nullptr);

    // Apply initial contrast
    update_split_button_contrast(sb);

    spdlog::trace("[ui_split_button] Created variant='{}' text='{}' icon='{}' options='{}'",
                  variant_str, text, icon_name ? icon_name : "", options ? options : "");

    return sb;
}

/**
 * @brief XML apply callback for <ui_split_button>
 */
void ui_split_button_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_xml_obj_apply(state, attrs);

    void* item = lv_xml_state_get_item(state);
    lv_obj_t* sb = static_cast<lv_obj_t*>(item);
    auto* data = get_data(sb);
    if (!data)
        return;

    // Handle text_format attribute
    const char* text_format = lv_xml_get_value_of(attrs, "text_format");
    if (text_format && strlen(text_format) > 0) {
        lv_free(data->text_format);
        data->text_format = lv_strdup(text_format);
    }

    // Handle show_selection attribute
    const char* show_sel = lv_xml_get_value_of(attrs, "show_selection");
    if (show_sel) {
        data->show_selection = (strcmp(show_sel, "false") != 0);
    }

    // Handle selected attribute
    const char* selected = lv_xml_get_value_of(attrs, "selected");
    if (selected && data->dropdown) {
        uint32_t idx = static_cast<uint32_t>(atoi(selected));
        lv_dropdown_set_selected(data->dropdown, idx);
    }

    // Update label from selection if show_selection is active
    if (data->show_selection && data->text_format) {
        update_label_from_selection(data);
    }
}

} // namespace

void ui_split_button_invalidate_icon_font_cache() {
    s_icon_font_resolved = false;
}

void ui_split_button_init() {
    lv_xml_register_widget("ui_split_button", ui_split_button_create, ui_split_button_apply);
    spdlog::trace("[ui_split_button] Registered split button widget");
}

void ui_split_button_set_options(lv_obj_t* sb, const char* options) {
    auto* data = get_data(sb);
    if (!data || !data->dropdown || !options)
        return;
    lv_dropdown_set_options(data->dropdown, options);
    update_label_from_selection(data);
}

void ui_split_button_set_selected(lv_obj_t* sb, uint32_t index) {
    auto* data = get_data(sb);
    if (!data || !data->dropdown)
        return;
    lv_dropdown_set_selected(data->dropdown, index);
    update_label_from_selection(data);
}

uint32_t ui_split_button_get_selected(lv_obj_t* sb) {
    auto* data = get_data(sb);
    if (!data || !data->dropdown)
        return 0;
    return lv_dropdown_get_selected(data->dropdown);
}

void ui_split_button_set_text(lv_obj_t* sb, const char* text) {
    auto* data = get_data(sb);
    if (!data || !data->label || !text)
        return;
    lv_label_set_text(data->label, text);
    constrain_label_width(data);
    lv_obj_invalidate(sb);
}
