// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/clock_face.h"

#include "translation_loader.h" // TR_NOOP

#include <array>

#if HELIX_ENABLE_SCREENSAVER
#include "screensaver_analog_clock_sim.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_nixie_clock_sim.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#endif

namespace helix {

namespace {

struct FaceRow {
    ClockFace face;
    const char* id;
    const char* label;
};

constexpr std::array<FaceRow, kClockFaceCount> kFaces{{
    {ClockFace::TEXT, "text", TR_NOOP("Text Clock")},
    {ClockFace::CHRONOGRAPH, "chronograph", TR_NOOP("Chronograph")},
    {ClockFace::STATION, "station", TR_NOOP("Station Clock")},
    {ClockFace::NEON, "neon", TR_NOOP("Neon")},
    {ClockFace::BAUHAUS, "bauhaus", TR_NOOP("Bauhaus")},
    {ClockFace::ORRERY, "orrery", TR_NOOP("Orrery")},
    {ClockFace::FLIP, "flip", TR_NOOP("Flip Clock")},
    {ClockFace::NIXIE, "nixie", TR_NOOP("Nixie Clock")},
}};

const FaceRow& row_of(ClockFace face) {
    const auto i = static_cast<size_t>(face);
    return kFaces[i < kFaces.size() ? i : 0];
}

} // namespace

const char* clock_face_id(ClockFace face) {
    return row_of(face).id;
}

ClockFace parse_clock_face(std::string_view id) {
    for (const FaceRow& row : kFaces) {
        if (id == row.id) {
            return row.face;
        }
    }
    return ClockFace::TEXT;
}

const char* clock_face_label(ClockFace face) {
    return row_of(face).label;
}

#if HELIX_ENABLE_SCREENSAVER

namespace {

bool is_analog(ClockFace face) {
    return face != ClockFace::TEXT && face != ClockFace::FLIP && face != ClockFace::NIXIE;
}

ui::AnalogStyle analog_style_of(ClockFace face) {
    return static_cast<ui::AnalogStyle>(static_cast<int>(face) -
                                        static_cast<int>(ClockFace::CHRONOGRAPH));
}

/// HH:MM: two groups of two cards.
ui::FlipShape hours_minutes_shape() {
    ui::FlipShape shape;
    shape.group_count = 2;
    shape.groups = {2, 2, 0};
    return shape;
}

/// The frame period of rung `level` (clamped) of `face`'s saver ladder.
template <typename Levels> uint32_t period_of(const Levels& levels, size_t level) {
    return levels[std::min(level, std::size(levels) - 1)].period_ms;
}

size_t ladder_size(ClockFace face) {
    switch (face) {
    case ClockFace::FLIP:
        return std::size(ui::kFlipClockLevels);
    case ClockFace::NIXIE:
        return std::size(ui::kNixieClockLevels);
    default:
        return std::size(ui::kAnalogClockLevels);
    }
}

uint32_t face_period(ClockFace face, size_t level) {
    switch (face) {
    case ClockFace::FLIP:
        return period_of(ui::kFlipClockLevels, level);
    case ClockFace::NIXIE:
        return period_of(ui::kNixieClockLevels, level);
    default:
        return period_of(ui::kAnalogClockLevels, level);
    }
}

} // namespace

int32_t clock_face_measure_px(ClockFace face, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) {
        return 0;
    }
    switch (face) {
    case ClockFace::TEXT:
        return 0;
    case ClockFace::FLIP:
        return ui::flip_row_card_px(w, h, hours_minutes_shape());
    case ClockFace::NIXIE:
        return ui::flip_row_card_px(w, h, hours_minutes_shape(), ui::kNixieTubeGeometry);
    default:
        break;
    }
    const ui::AnalogGeom g = ui::analog_layout(analog_style_of(face), w, h, false);
    return std::max(0, static_cast<int32_t>(2.0f * g.r));
}

int32_t clock_face_min_px(ClockFace face) {
    switch (face) {
    case ClockFace::TEXT:
        return 0;
    case ClockFace::CHRONOGRAPH:
        return kClockMinChronoDialPx;
    case ClockFace::FLIP:
        return kClockMinFlipCardPx;
    case ClockFace::NIXIE:
        return kClockMinNixieTubePx;
    default:
        return kClockMinDialPx;
    }
}

ClockFace clock_face_shown(ClockFace chosen, int32_t w, int32_t h) {
    if (chosen == ClockFace::TEXT) {
        return chosen;
    }
    return clock_face_measure_px(chosen, w, h) >= clock_face_min_px(chosen) ? chosen
                                                                            : ClockFace::TEXT;
}

int64_t clock_face_area_budget_px(PlatformTier tier) {
    switch (tier) {
    case PlatformTier::EMBEDDED:
        return 128 * 128;
    case PlatformTier::BASIC:
        return 192 * 192;
    case PlatformTier::STANDARD:
        break;
    }
    return 0;
}

ClockCanvasSize clock_face_canvas(ClockFace face, int32_t w, int32_t h, int64_t budget_px) {
    ClockCanvasSize size{std::max(0, w), std::max(0, h)};
    if (is_analog(face)) {
        size.w = size.h = std::min(size.w, size.h);
    }
    const int64_t area = static_cast<int64_t>(size.w) * size.h;
    if (budget_px > 0 && area > budget_px) {
        const double scale = std::sqrt(static_cast<double>(budget_px) / static_cast<double>(area));
        size.w = static_cast<int32_t>(std::floor(size.w * scale));
        size.h = static_cast<int32_t>(std::floor(size.h * scale));
    }
    return size;
}

ScreensaverType clock_face_saver(ClockFace face) {
    switch (face) {
    case ClockFace::TEXT:
        return ScreensaverType::OFF;
    case ClockFace::FLIP:
        return ScreensaverType::FLIP_CLOCK;
    case ClockFace::NIXIE:
        return ScreensaverType::NIXIE_CLOCK;
    default:
        return ScreensaverType::ANALOG_CLOCK;
    }
}

ClockFaceTempo clock_face_tempo(ClockFace face, const std::optional<ui::SaverLevelEntry>& standing,
                                int cpu_cores) {
    const size_t bottom = ladder_size(face) - 1;
    ClockFaceTempo t;
    if (standing && standing->too_heavy) {
        t.level = bottom;
        t.seconds = false;
    } else if (standing) {
        t.level = std::clamp(standing->level, kClockTileFastestLevel, bottom);
        t.seconds = true;
    } else {
        t.level = bottom;
        t.seconds = cpu_cores >= 2;
    }
    t.period_ms = face_period(face, t.level);
    // Cards show no seconds; they animate where their rung does.
    t.seconds = t.seconds && is_analog(face);
    return t;
}

uint32_t clock_ms_to_next_minute(int32_t seconds_of_day, uint32_t ms_into_second) {
    const int32_t into_minute = ((seconds_of_day % 60) + 60) % 60;
    const uint32_t ms = std::min<uint32_t>(ms_into_second, 999);
    return static_cast<uint32_t>(60 - into_minute) * 1000u - ms;
}

#endif // HELIX_ENABLE_SCREENSAVER

} // namespace helix
