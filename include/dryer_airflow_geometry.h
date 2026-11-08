// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

/// Pure geometry behind <dryer_airflow>: a stream of chevrons rising through the
/// widget's box, each fading in at the bottom and out at the top. No LVGL here.
namespace helix::ui::dryer_airflow {

constexpr int kChevronCount = 3;

/// The share of one full rise each animation tick moves the stream: a chevron
/// crosses the box in kTicksPerRise ticks.
constexpr int kTicksPerRise = 30;

struct Chevron {
    float rise = 0;  ///< 0 at the bottom of the box, 1 at the top
    uint8_t opa = 0; ///< 0..255
};

/// Where each chevron sits at @p phase (0..1, wrapping), evenly spaced so the
/// stream reads the same at every phase. Opacity peaks mid-box and falls to zero
/// at both ends, so a chevron never pops in or out.
inline std::array<Chevron, kChevronCount> chevrons(float phase) {
    std::array<Chevron, kChevronCount> out{};
    for (int i = 0; i < kChevronCount; ++i) {
        float rise = phase + static_cast<float>(i) / kChevronCount;
        rise -= std::floor(rise);
        const float fade = std::sin(rise * 3.14159265f);
        out[static_cast<std::size_t>(i)] = {rise, static_cast<uint8_t>(std::lround(255.0f * fade))};
    }
    return out;
}

/// The phase one tick after @p phase. With animations off the stream holds still.
inline float advance(float phase, bool animations_enabled) {
    if (!animations_enabled) {
        return phase;
    }
    phase += 1.0f / kTicksPerRise;
    return phase - std::floor(phase);
}

} // namespace helix::ui::dryer_airflow
