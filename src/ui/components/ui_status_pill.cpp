// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_status_pill.h"

#include "ui_variant.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

// ----- Internal helpers -----

static void store_variant(lv_obj_t* pill, helix::ui::Variant v) {
    lv_obj_set_user_data(pill, reinterpret_cast<void*>(static_cast<intptr_t>(v)));
}

/// Find the child label (first child).
static lv_obj_t* get_label(lv_obj_t* pill) {
    return lv_obj_get_child(pill, 0);
}

/// Apply variant colors to pill: bg @ 40% opa, text @ full opa.
static void apply_pill_variant(lv_obj_t* pill, helix::ui::Variant v) {
    store_variant(pill, v);
    lv_color_t color = helix::ui::variant_color(v);
    lv_opa_t text_opa = helix::ui::variant_opa(v);

    // Background: variant color at 40% opacity
    lv_obj_set_style_bg_color(pill, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, 40, LV_PART_MAIN);

    // Text: variant color at full (or reduced for disabled) opacity
    lv_obj_t* label = get_label(pill);
    if (label) {
        lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
        lv_obj_set_style_text_opa(label, text_opa, LV_PART_MAIN);
    }
}

// ----- XML widget callbacks -----

static void* ui_status_pill_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs;
    lv_obj_t* parent = (lv_obj_t*)lv_xml_state_get_parent(state);

    // Container: content-sized pill with rounded corners
    lv_obj_t* pill = lv_obj_create(parent);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_scrollbar_mode(pill, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(pill, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(pill, 0, LV_PART_MAIN);

    // Padding: space_xs horizontal, space_xxs vertical (matches beta_badge pattern)
    int32_t pad_hor = theme_manager_get_spacing("space_xs");
    int32_t pad_ver = theme_manager_get_spacing("space_xxs");
    lv_obj_set_style_pad_left(pill, pad_hor, LV_PART_MAIN);
    lv_obj_set_style_pad_right(pill, pad_hor, LV_PART_MAIN);
    lv_obj_set_style_pad_top(pill, pad_ver, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(pill, pad_ver, LV_PART_MAIN);

    // Child label for text
    lv_obj_t* label = lv_label_create(pill);
    lv_label_set_text(label, "");

    // Use small font from theme
    const lv_font_t* font = theme_manager_get_font("font_small");
    if (font) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }

    // Default: muted variant
    apply_pill_variant(pill, helix::ui::Variant::MUTED);

    return pill;
}

static void ui_status_pill_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* pill = (lv_obj_t*)lv_xml_state_get_item(state);

    // Apply common lv_obj properties first (width, height, align, etc.)
    lv_xml_obj_apply(state, attrs);

    // Process status_pill-specific properties
    const char* text = nullptr;
    const char* variant_str = nullptr;
    const char* bind_text = nullptr;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "text") == 0) {
            text = value;
        } else if (strcmp(name, "variant") == 0) {
            variant_str = value;
        } else if (strcmp(name, "bind_text") == 0) {
            bind_text = value;
        }
    }

    if (text) {
        lv_obj_t* label = get_label(pill);
        if (label)
            lv_label_set_text(label, text);
    }

    // An empty name is an optional binding left unset.
    if (bind_text && bind_text[0] != '\0') {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, bind_text);
        lv_obj_t* label = get_label(pill);
        if (!subject) {
            spdlog::warn("[StatusPill] Subject \"{}\" doesn't exist in bind_text", bind_text);
        } else if (label) {
            lv_label_bind_text(label, subject,
                               subject->type == LV_SUBJECT_TYPE_INT ? "%d" : nullptr);
        }
    }

    if (variant_str) {
        auto v = helix::ui::parse_variant(variant_str);
        apply_pill_variant(pill, v);
    }
}

// ----- Public API -----

void ui_status_pill_register_widget() {
    lv_xml_register_widget("status_pill", ui_status_pill_xml_create, ui_status_pill_xml_apply);
    spdlog::trace("[StatusPill] Widget registered with XML system");
}

void ui_status_pill_set_text(lv_obj_t* pill, const char* text) {
    if (!pill || !text)
        return;
    lv_obj_t* label = get_label(pill);
    if (label)
        lv_label_set_text(label, text);
}

void ui_status_pill_set_variant(lv_obj_t* pill, const char* variant_str) {
    if (!pill || !variant_str)
        return;
    auto v = helix::ui::parse_variant(variant_str);
    apply_pill_variant(pill, v);
}
