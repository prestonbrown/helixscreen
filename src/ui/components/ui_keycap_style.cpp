// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_keycap_style.h"

#include <algorithm>
#include <cstring>
#include <strings.h>

namespace helix::ui {

namespace {

/// How far the skirt sits below the key background. Dark themes need more: the
/// key already sits on a darker keyboard background, so a light-mode amount
/// lands on that background and the rim disappears.
constexpr lv_opa_t kSkirtDarkenLight = LV_OPA_40;
constexpr lv_opa_t kSkirtDarkenDark = LV_OPA_60;

/// The edge replaces a shadow on EMBOSS, so it carries the depth on its own and
/// is drawn harder than the skirt would be. On a light theme it is the shaded
/// underside of the key; on a dark theme it is the lit top, because a darker
/// edge on a dark key reads as more background rather than as depth.
constexpr lv_opa_t kEdgeShadeLight = LV_OPA_50;
constexpr lv_opa_t kEdgeLightenDark = LV_OPA_30;
constexpr int32_t kEdgeWidthPx = 2;

/// The edge added to a skirt on dark themes. Thinner than a full EMBOSS edge:
/// there the edge is the only depth there is, here it only has to give the
/// skirt something the background cannot swallow.
constexpr int32_t kDarkSkirtEdgePx = 1;

/// Held-key shading. Follows the same light-darkens / dark-lightens rule the
/// keyboard already uses to derive its special-key colour.
constexpr lv_opa_t kPressedShift = LV_OPA_20;

/// Divisor mapping the theme's 0-50 shadow_intensity onto single-digit pixels,
/// with half of it added first so the mapping rounds rather than truncates.
constexpr int kIntensityPerPx = 10;

/// Give @p style the key's own edge: shaded underside on a light theme, lit top
/// on a dark one. Pressing the key flips it to the opposite corner, so the light
/// moves as the key sinks.
void apply_edge(KeycapStyle& style, lv_color_t key_bg, bool dark_mode, int32_t width) {
    style.edge_width = width;
    if (dark_mode) {
        style.edge_color = lv_color_lighten(key_bg, kEdgeLightenDark);
        style.edge_side = static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
        style.edge_side_pressed =
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT);
    } else {
        style.edge_color = lv_color_darken(key_bg, kEdgeShadeLight);
        style.edge_side =
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT);
        style.edge_side_pressed =
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
    }
}

} // namespace

KeycapDepth decide_keycap_depth(PlatformTier tier, bool platform_has_shadows) {
    if (!platform_has_shadows) {
        return KeycapDepth::FLAT;
    }
    return tier == PlatformTier::EMBEDDED ? KeycapDepth::EMBOSS : KeycapDepth::SKIRT;
}

int32_t keycap_skirt_offset_px(int theme_shadow_intensity) {
    if (theme_shadow_intensity <= 0) {
        return kKeycapDefaultSkirtPx;
    }
    const int32_t px = (theme_shadow_intensity + kIntensityPerPx / 2) / kIntensityPerPx;
    return std::clamp(px, static_cast<int32_t>(1), kKeycapMaxSkirtPx);
}

KeycapStyle make_keycap_style(KeycapDepth depth, lv_color_t key_bg, bool dark_mode,
                              int theme_shadow_intensity) {
    KeycapStyle style;
    style.depth = depth;
    style.pressed_bg = dark_mode ? lv_color_lighten(key_bg, kPressedShift)
                                 : lv_color_darken(key_bg, kPressedShift);

    switch (depth) {
    case KeycapDepth::SKIRT:
        style.skirt_offset_y = keycap_skirt_offset_px(theme_shadow_intensity);
        style.skirt_color =
            lv_color_darken(key_bg, dark_mode ? kSkirtDarkenDark : kSkirtDarkenLight);
        // Full opacity: the rim is an opaque edge of the key, and a translucent
        // one would blend with whatever sits under the keyboard instead.
        style.skirt_opa = LV_OPA_COVER;
        if (dark_mode) {
            // On a dark theme the keyboard background is darker than the key, so
            // the skirt has nothing to contrast against however far it is
            // darkened — measured on a #4a4a54 key it lands within four values
            // of the background. The lit top edge is what reads as raised there.
            apply_edge(style, key_bg, true, kDarkSkirtEdgePx);
        }
        break;

    case KeycapDepth::EMBOSS:
        apply_edge(style, key_bg, dark_mode, kEdgeWidthPx);
        break;

    case KeycapDepth::FLAT:
        break;
    }

    return style;
}

std::optional<KeycapDepth> parse_keycap_depth_override(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    if (strcasecmp(value, "skirt") == 0) {
        return KeycapDepth::SKIRT;
    }
    if (strcasecmp(value, "emboss") == 0) {
        return KeycapDepth::EMBOSS;
    }
    if (strcasecmp(value, "flat") == 0) {
        return KeycapDepth::FLAT;
    }
    return std::nullopt;
}

} // namespace helix::ui
