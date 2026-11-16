// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Pure colour arithmetic: hex parsing, brightness and saturation, and the WCAG
// contrast helpers behind the readable-text accessors.

#include "color_utils.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

using namespace helix;

// Parse hex color string "#FF4444" -> lv_color_hex(0xFF4444)
lv_color_t theme_manager_parse_hex_color(const char* hex_str) {
    if (!hex_str || hex_str[0] == '\0') {
        // Unset palette field. The theme loader substitutes defaults so this
        // shouldn't happen, but a per-widget tree-walk would otherwise flood the
        // log if it did — keep it quiet (prestonbrown/helixscreen#989).
        spdlog::debug("[Theme] Empty hex color string, using black fallback");
        return lv_color_hex(0x000000);
    }
    uint32_t rgb = 0;
    if (hex_str[0] != '#' || !helix::parse_hex_color(hex_str, rgb)) {
        spdlog::error("[Theme] Invalid hex color string: {}", hex_str);
        return lv_color_hex(0x000000);
    }
    return lv_color_hex(rgb);
}

/**
 * @brief Calculate perceived brightness of an lv_color_t
 * Uses standard luminance formula: 0.299*R + 0.587*G + 0.114*B
 * @return Brightness value 0-255
 */
int theme_compute_brightness(lv_color_t color) {
    uint32_t c = lv_color_to_u32(color);
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >> 8) & 0xFF;
    uint8_t b = c & 0xFF;
    return (299 * r + 587 * g + 114 * b) / 1000;
}

/**
 * @brief Return the brighter of two colors
 */
lv_color_t theme_compute_brighter_color(lv_color_t a, lv_color_t b) {
    return theme_compute_brightness(a) >= theme_compute_brightness(b) ? a : b;
}

/**
 * @brief Compute saturation of a color (0-255)
 *
 * Uses HSV saturation: (max - min) / max * 255
 * Returns 0 for grayscale colors, higher for more vivid colors.
 */
int theme_compute_saturation(lv_color_t c) {
    int max_val =
        c.red > c.green ? (c.red > c.blue ? c.red : c.blue) : (c.green > c.blue ? c.green : c.blue);
    int min_val =
        c.red < c.green ? (c.red < c.blue ? c.red : c.blue) : (c.green < c.blue ? c.green : c.blue);
    if (max_val == 0)
        return 0;
    return (max_val - min_val) * 255 / max_val;
}

/**
 * @brief Return the more saturated of two colors
 *
 * Useful for accent colors where you want the more vivid/colorful option
 * rather than the literally brighter one.
 */
lv_color_t theme_compute_more_saturated(lv_color_t a, lv_color_t b) {
    return theme_compute_saturation(a) >= theme_compute_saturation(b) ? a : b;
}

lv_color_t theme_get_knob_color() {
    // Knob color: more saturated of primary vs tertiary (for switch/slider handles)
    const char* primary_str = lv_xml_get_const(nullptr, "primary");
    const char* tertiary_str = lv_xml_get_const(nullptr, "tertiary");

    if (!primary_str) {
        spdlog::warn("[Theme] theme_get_knob_color: missing 'primary' constant");
        return lv_color_hex(0x5e81ac); // Fallback to Nord blue
    }

    lv_color_t primary = theme_manager_parse_hex_color(primary_str);
    lv_color_t tertiary = tertiary_str ? theme_manager_parse_hex_color(tertiary_str) : primary;

    return theme_compute_more_saturated(primary, tertiary);
}

lv_color_t theme_get_accent_color() {
    // Accent color: more saturated of primary vs secondary (for icon accents)
    const char* primary_str = lv_xml_get_const(nullptr, "primary");
    const char* secondary_str = lv_xml_get_const(nullptr, "secondary");

    if (!primary_str) {
        spdlog::warn("[Theme] theme_get_accent_color: missing 'primary' constant");
        return lv_color_hex(0x5e81ac); // Fallback to Nord blue
    }

    lv_color_t primary = theme_manager_parse_hex_color(primary_str);
    lv_color_t secondary = secondary_str ? theme_manager_parse_hex_color(secondary_str) : primary;

    return theme_compute_more_saturated(primary, secondary);
}

lv_color_t theme_manager_get_contrast_color(lv_color_t bg_color) {
    int brightness = theme_compute_brightness(bg_color);
    auto& tm = ThemeManager::instance();
    // Dark background needs light text (dark palette has light-colored text for readability)
    // Light background needs dark text (light palette has dark-colored text for readability)
    return (brightness < 140) ? tm.dark_palette().text : tm.light_palette().text;
}

/// WCAG relative luminance of one 8-bit channel. Tabulated: the contrast search
/// evaluates dozens of ratios per widget, and the ESP32's FPU is single
/// precision, so each double pow() runs in software.
static double srgb_channel_luminance(uint8_t v) {
    static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[i] = (c <= 0.03928) ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return t;
    }();
    return table[v];
}

/// WCAG relative luminance of a color.
static double srgb_relative_luminance(lv_color_t c) {
    return 0.2126 * srgb_channel_luminance(c.red) + 0.7152 * srgb_channel_luminance(c.green) +
           0.0722 * srgb_channel_luminance(c.blue);
}

/// WCAG contrast ratio between two colors (1.0 = identical).
static double srgb_contrast_ratio(lv_color_t a, lv_color_t b) {
    const double la = srgb_relative_luminance(a), lb = srgb_relative_luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

double helix::contrast_ratio(lv_color_t a, lv_color_t b) {
    return srgb_contrast_ratio(a, b);
}

lv_color_t theme_manager_get_readable_on(lv_color_t fill) {
    const double lum = srgb_relative_luminance(fill);
    // Contrast against white is (1.05 / (lum + 0.05)); against black it is
    // ((lum + 0.05) / 0.05). They cross where lum == sqrt(1.05 * 0.05) - 0.05.
    constexpr double kCrossover = 0.1791; // sqrt(0.0525) - 0.05
    return (lum > kCrossover) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
}

/// Core of the contrast_adjusted_text pair: returns @p text when it already
/// clears @p min_ratio on @p fill, else the smallest blend toward its pole
/// that does, else the readable pure pole.
static lv_color_t contrast_adjusted_text_for_ratio(lv_color_t text, lv_color_t fill,
                                                   double min_ratio) {
    if (srgb_contrast_ratio(text, fill) >= min_ratio)
        return text;

    // Blend toward the pole on the text's own side of the fill so the theme's
    // tint survives; the ratio rises monotonically with the blend amount.
    const lv_color_t pole = (srgb_relative_luminance(text) > srgb_relative_luminance(fill))
                                ? lv_color_hex(0xFFFFFF)
                                : lv_color_hex(0x000000);

    // Smallest 8-bit blend that clears the threshold. The ratio at mix 0 is
    // the failing ratio checked above and grows monotonically toward the pole,
    // so a binary search finds the first passing mix.
    uint8_t lo = 0, hi = 255;
    while (lo < hi) {
        const uint8_t mid = lo + (hi - lo) / 2;
        if (srgb_contrast_ratio(lv_color_mix(pole, text, mid), fill) >= min_ratio)
            hi = mid;
        else
            lo = mid + 1;
    }
    const lv_color_t blended = lv_color_mix(pole, text, lo);
    // Even the pure pole misses the threshold when no tint on the text's own
    // side can reach it; fall back to whichever pure pole reads best.
    if (srgb_contrast_ratio(blended, fill) < min_ratio)
        return theme_manager_get_readable_on(fill);
    return blended;
}

lv_color_t helix::field_outline_color(lv_color_t text_subtle, lv_color_t screen_bg,
                                      lv_color_t overlay_bg, lv_color_t card_bg,
                                      lv_color_t elevated_bg) {
    // A palette's surfaces share one side of the luminance range, so each shift
    // moves away from all of them and never undoes an earlier surface's margin.
    lv_color_t c = text_subtle;
    for (lv_color_t surface : {screen_bg, overlay_bg, card_bg, elevated_bg})
        c = contrast_adjusted_text_for_ratio(c, surface, kFieldOutlineContrastThreshold);
    return c;
}

// NAMESPACE_OK: joins this header's global theme_manager_* free-function API
lv_color_t theme_manager_get_contrast_adjusted_text(lv_color_t text, lv_color_t fill) {
    return contrast_adjusted_text_for_ratio(text, fill, kThemeTextContrastThreshold);
}

// NAMESPACE_OK: joins this header's global theme_manager_* free-function API
lv_color_t theme_manager_get_contrast_adjusted_text(lv_color_t text, lv_color_t fill,
                                                    lv_color_t backing, lv_opa_t fill_opa,
                                                    double min_ratio) {
    return contrast_adjusted_text_for_ratio(text, lv_color_mix(fill, backing, fill_opa), min_ratio);
}
