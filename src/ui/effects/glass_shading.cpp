// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "glass_shading.h"

#include <cstddef>

namespace helix::ui {

namespace {

enum class Toward : uint8_t { Base, White, Black };

/// One stop: `pct` percent of white or black mixed into the base, at position `frac`.
struct StopSpec {
    Toward toward;
    uint8_t pct;
    uint8_t frac;
};

struct StopTable {
    const StopSpec* stops;
    int count;
};

constexpr StopSpec kRaisedDark[] = {{Toward::White, 45, 0},
                                    {Toward::White, 22, 23},
                                    {Toward::White, 6, 102},
                                    {Toward::Black, 22, 255}};
constexpr StopSpec kRaisedLight[] = {{Toward::White, 50, 0},
                                     {Toward::White, 22, 23},
                                     {Toward::White, 6, 102},
                                     {Toward::Black, 18, 255}};
constexpr StopSpec kRaisedPressed[] = {{Toward::Black, 25, 0}, {Toward::Black, 5, 255}};
constexpr StopSpec kSurfaceDark[] = {{Toward::White, 24, 0},
                                     {Toward::White, 10, 70},
                                     {Toward::Base, 0, 160},
                                     {Toward::Black, 12, 255}};
constexpr StopSpec kSurfaceLight[] = {
    {Toward::Base, 0, 0}, {Toward::Black, 3, 140}, {Toward::Black, 9, 255}};
constexpr StopSpec kRecessedDark[] = {{Toward::Black, 30, 0}, {Toward::Base, 0, 255}};
constexpr StopSpec kRecessedLight[] = {{Toward::Black, 12, 0}, {Toward::Base, 0, 255}};

template <size_t N> constexpr StopTable table(const StopSpec (&stops)[N]) {
    return {stops, static_cast<int>(N)};
}

StopTable stop_table(GlassFamily family, bool dark, bool pressed) {
    switch (family) {
    case GlassFamily::Raised:
        if (pressed) {
            return table(kRaisedPressed);
        }
        return dark ? table(kRaisedDark) : table(kRaisedLight);
    case GlassFamily::Surface:
        return dark ? table(kSurfaceDark) : table(kSurfaceLight);
    case GlassFamily::Recessed:
        return dark ? table(kRecessedDark) : table(kRecessedLight);
    case GlassFamily::None:
        break;
    }
    return {nullptr, 0};
}

lv_color_t shade(lv_color_t base, Toward toward, uint8_t pct) {
    const auto mix = static_cast<uint8_t>(pct * 255 / 100);
    switch (toward) {
    case Toward::White:
        return lv_color_mix(lv_color_white(), base, mix);
    case Toward::Black:
        return lv_color_mix(lv_color_black(), base, mix);
    case Toward::Base:
        break;
    }
    return base;
}

/// The rim: which way and how far, per family and mode.
StopSpec rim_spec(GlassFamily family, bool dark) {
    switch (family) {
    case GlassFamily::Raised:
        return dark ? StopSpec{Toward::White, 40, 0} : StopSpec{Toward::Black, 22, 0};
    case GlassFamily::Surface:
        return dark ? StopSpec{Toward::White, 12, 0} : StopSpec{Toward::Black, 14, 0};
    case GlassFamily::Recessed:
        return dark ? StopSpec{Toward::Black, 40, 0} : StopSpec{Toward::Black, 20, 0};
    case GlassFamily::None:
        break;
    }
    return {Toward::Base, 0, 0};
}

} // namespace

GlassStops glass_stops(GlassFamily family, lv_color_t base, bool dark, bool pressed) {
    const StopTable t = stop_table(family, dark, pressed);
    GlassStops out;
    out.count = t.count;
    for (int i = 0; i < t.count; ++i) {
        out.color[i] = shade(base, t.stops[i].toward, t.stops[i].pct);
        out.frac[i] = t.stops[i].frac;
    }
    return out;
}

bool glass_gradient(lv_grad_dsc_t* grad, GlassFamily family, lv_color_t base, bool dark,
                    bool pressed) {
    const GlassStops s = glass_stops(family, base, dark, pressed);
    if (s.count < 2) {
        return false;
    }
    lv_grad_init_stops(grad, s.color, nullptr, s.frac, s.count);
    if (family == GlassFamily::Surface) {
        lv_grad_radial_init(grad, 0, 0, kGlassGlowRadiusPx, 0, LV_GRAD_EXTEND_PAD);
    } else {
        lv_grad_vertical_init(grad);
    }
    return true;
}

lv_color_t glass_rim_color(GlassFamily family, lv_color_t base, bool dark) {
    const StopSpec rim = rim_spec(family, dark);
    return shade(base, rim.toward, rim.pct);
}

} // namespace helix::ui
