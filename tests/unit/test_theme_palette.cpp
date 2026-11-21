// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The theme palette (color_palette(ColorPalette::Theme)) is the theme editor's
// presets, shown 6 x 5 in the color picker's grid. It is structured: the first
// 12 colors are a dark->light surface ramp (screen_bg / card_bg / border / text
// tokens) and the remaining 18 are 6 hue families x 3 tones, laid out so that
// column == hue family. These tests pin that structure: a reordered or dropped
// color breaks the contract the theme editor's users navigate by.

#include "color_picker_model.h"

#include <algorithm>
#include <cmath>

#include "../catch_amalgamated.hpp"

namespace {

constexpr int kCols = 6;
constexpr int kSurfaceCount = 12;
constexpr int kAccentCount = 18;
constexpr int kTotal = kSurfaceCount + kAccentCount;

/// Hue in degrees [0,360). Undefined (returns -1) for near-neutral colors.
double hue_of(uint32_t rgb) {
    const double r = ((rgb >> 16) & 0xFF) / 255.0;
    const double g = ((rgb >> 8) & 0xFF) / 255.0;
    const double b = (rgb & 0xFF) / 255.0;
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    const double c = mx - mn;
    if (c < 0.04) {
        return -1.0; // neutral: no meaningful hue
    }
    double h = 0.0;
    if (mx == r) {
        h = std::fmod((g - b) / c, 6.0);
    } else if (mx == g) {
        h = (b - r) / c + 2.0;
    } else {
        h = (r - g) / c + 4.0;
    }
    h *= 60.0;
    return h < 0 ? h + 360.0 : h;
}

double relative_luminance(uint32_t rgb) {
    return 0.2126 * ((rgb >> 16) & 0xFF) + 0.7152 * ((rgb >> 8) & 0xFF) + 0.0722 * (rgb & 0xFF);
}

/// Smallest angular distance between two hues, in degrees.
double hue_delta(double a, double b) {
    double d = std::fabs(a - b);
    return d > 180.0 ? 360.0 - d : d;
}

} // namespace

TEST_CASE("theme palette: 30 colors in the picker's 6 columns", "[theme][swatch][color_picker]") {
    REQUIRE(helix::color_palette(helix::ColorPalette::Theme).size() == kTotal);
    REQUIRE(helix::color_palette_columns(helix::ColorPalette::Theme) == kCols);
}

TEST_CASE("theme palette: the surface block is a monotonic dark->light ramp",
          "[theme][swatch][color_picker]") {
    const auto& colors = helix::color_palette(helix::ColorPalette::Theme);
    REQUIRE(colors.size() == kTotal);

    // The theme editor's surface tokens (screen_bg -> text) are picked by
    // lightness, so the first 12 swatches must read as an ordered ramp.
    for (int i = 1; i < kSurfaceCount; i++) {
        const double prev = relative_luminance(colors[i - 1]);
        const double cur = relative_luminance(colors[i]);
        INFO("surface swatch " << i << " luminance " << cur << " must exceed " << prev);
        REQUIRE(cur > prev);
    }

    // The ramp must actually span the usable range: dark enough for an OLED-black
    // screen_bg, light enough for a light-mode background.
    REQUIRE(relative_luminance(colors.front()) < 30.0);
    REQUIRE(relative_luminance(colors[kSurfaceCount - 1]) > 230.0);
}

TEST_CASE("theme palette: accent columns hold one hue family each",
          "[theme][swatch][color_picker]") {
    const auto& colors = helix::color_palette(helix::ColorPalette::Theme);

    // red, amber, green, teal, blue, violet — the six families the accent
    // block is organised by. Column position is the navigation affordance.
    const double family_hue[kCols] = {0.0, 42.0, 110.0, 175.0, 215.0, 275.0};
    constexpr double kTolerance = 32.0;

    for (int row = 0; row < kAccentCount / kCols; row++) {
        for (int col = 0; col < kCols; col++) {
            const uint32_t rgb = colors[kSurfaceCount + row * kCols + col];
            const double h = hue_of(rgb);
            INFO("accent row " << row << " col " << col << " rgb=" << std::hex << rgb
                               << " hue=" << std::dec << h);
            REQUIRE(h >= 0.0); // accents must be chromatic, not neutral
            REQUIRE(hue_delta(h, family_hue[col]) <= kTolerance);
        }
    }
}

TEST_CASE("theme palette: accent rows darken from muted to deep", "[theme][swatch][color_picker]") {
    const auto& colors = helix::color_palette(helix::ColorPalette::Theme);

    // Row 0 = muted (dark-mode accents), row 2 = deep (light-mode accents).
    // Each column must get consistently darker down the block, otherwise the
    // "pick a row for your mode" affordance is a lie.
    for (int col = 0; col < kCols; col++) {
        const double muted = relative_luminance(colors[kSurfaceCount + 0 * kCols + col]);
        const double mid = relative_luminance(colors[kSurfaceCount + 1 * kCols + col]);
        const double deep = relative_luminance(colors[kSurfaceCount + 2 * kCols + col]);
        INFO("accent column " << col << " luminance " << muted << " / " << mid << " / " << deep);
        REQUIRE(muted > mid);
        REQUIRE(mid > deep);
    }
}

TEST_CASE("theme palette: no two swatches are identical", "[theme][swatch][color_picker]") {
    const auto& colors = helix::color_palette(helix::ColorPalette::Theme);
    for (size_t i = 0; i < colors.size(); i++) {
        for (size_t j = i + 1; j < colors.size(); j++) {
            INFO("swatch " << i << " vs " << j << " both " << std::hex << colors[i]);
            REQUIRE(colors[i] != colors[j]);
        }
    }
}
