// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file ui_variant.h
/// @brief Shared semantic color variants for icon, status_pill, and future components.
/// @pattern Variant enum + ThemeManager palette lookup. Used by ui_icon and ui_status_pill.
/// @threading Main thread only

#pragma once

#include "lvgl/lvgl.h"

namespace helix::ui {

/// Semantic color variants — shared across icon, status_pill, and future components.
enum class Variant {
    NONE,      // Primary text color (default)
    TEXT,      // Same as NONE
    MUTED,     // De-emphasized
    PRIMARY,   // Accent/brand
    SECONDARY, // Secondary accent
    TERTIARY,  // Tertiary accent
    DISABLED,  // Text @ 50% opacity
    SUCCESS,
    WARNING,
    DANGER,
    INFO
};

/// Parse variant string ("success", "danger", etc.) to enum. Returns NONE on unknown.
Variant parse_variant(const char* str);

/// Get the text/icon color for a variant from ThemeManager's current palette.
lv_color_t variant_color(Variant v);

/// Get the opacity for a variant (LV_OPA_COVER for all except DISABLED = LV_OPA_50).
lv_opa_t variant_opa(Variant v);

/// Apply variant as text color + opa to an lv_obj (convenience for icons/labels).
/// Removes previously applied variant style first, then adds the matching IconXxx style.
void apply_variant_text_style(lv_obj_t* obj, Variant v);

/// Remove all variant-related styles from obj (cleanup before re-applying).
void remove_variant_styles(lv_obj_t* obj);

/// True when obj carries the default text variant (IconText), i.e. it wants the
/// ambient text colour rather than an accent of its own.
bool has_text_variant(const lv_obj_t* obj);

} // namespace helix::ui
