// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clog_meter_geometry.h"

#include "theme_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace helix::ui {

namespace {

/// Half the symmetrical range, so a magnitude of 100 reaches one end of the
/// track from the middle.
constexpr int kHalfRange = 200;

int scale(int magnitude, int track_w, int range) {
    if (range <= 0) {
        return 0;
    }
    return std::clamp(magnitude, 0, range) * track_w / range;
}

} // namespace

ClogMeterTint clog_meter_tint(int mode, int value, int warning) {
    if (warning != 0) {
        return {"danger", "danger", 255};
    }

    const auto m = static_cast<ClogMeterMode>(mode);
    if (m != ClogMeterMode::Encoder && m != ClogMeterMode::Buffer) {
        return {"primary", "primary", 255};
    }

    // primary -> warning over the safe half, warning -> danger over the rest.
    const int val = std::clamp(std::abs(value), 0, 100);
    if (val < 50) {
        return {"warning", "primary", static_cast<uint8_t>(val * 255 / 50)};
    }
    return {"danger", "warning", static_cast<uint8_t>((val - 50) * 255 / 50)};
}

lv_color_t resolve_clog_tint(int mode, int value, int warning) {
    const ClogMeterTint t = clog_meter_tint(mode, value, warning);
    return lv_color_mix(theme_manager_get_color(t.a), theme_manager_get_color(t.b), t.mix_a);
}

ClogMeterStatus clog_meter_status(int mode, int value, int warning, int danger_pct) {
    if (warning != 0) {
        return ClogMeterStatus::Fault;
    }
    if (clog_meter_is_safe(mode, value)) {
        return ClogMeterStatus::Ok;
    }
    // A threshold of zero would make every reading a warning, including a
    // perfectly neutral one, so an unset threshold means "no opinion".
    if (danger_pct > 0 && std::abs(value) >= danger_pct) {
        return ClogMeterStatus::Warning;
    }
    return ClogMeterStatus::Ok;
}

ClogMeterStatus pressure_status_of_bias(float bias) {
    const float clamped = std::isnan(bias) ? 0.0f : std::clamp(bias, -1.0f, 1.0f);
    return pressure_status(static_cast<int>(std::lround(clamped * 100.0f)));
}

bool clog_meter_is_symmetrical(int mode) {
    return static_cast<ClogMeterMode>(mode) == ClogMeterMode::Flowguard;
}

ClogMeterStatus pressure_status(int pct) {
    const int magnitude = std::abs(pct);
    if (magnitude >= kPressureFaultPct) {
        return ClogMeterStatus::Fault;
    }
    if (magnitude >= kPressureWarningPct) {
        return ClogMeterStatus::Warning;
    }
    return ClogMeterStatus::Ok;
}

const char* buffer_status_token(ClogMeterStatus s) {
    switch (s) {
    case ClogMeterStatus::Warning:
        return "warning";
    case ClogMeterStatus::Fault:
        return "danger";
    case ClogMeterStatus::Ok:
        break;
    }
    return "text_muted";
}

BufferLean buffer_lean(float bias) {
    if (std::fabs(bias) < 0.02f) {
        return BufferLean::Balanced;
    }
    return bias < 0 ? BufferLean::Tight : BufferLean::Loose;
}

bool clog_meter_is_safe(int mode, int value) {
    return static_cast<ClogMeterMode>(mode) == ClogMeterMode::Buffer && value == 0;
}

ClogBarGeometry clog_bar_geometry(int mode, int value, int danger_pct, int peak_pct, int track_w) {
    ClogBarGeometry g;
    if (track_w <= 0) {
        return g;
    }

    const bool symmetrical = clog_meter_is_symmetrical(mode);
    danger_pct = std::clamp(danger_pct, 0, 100);
    peak_pct = std::clamp(std::abs(peak_pct), 0, 100);

    if (symmetrical) {
        const int centre = track_w / 2;
        const int v = std::clamp(value, -100, 100);
        const int offset = scale(std::abs(v), track_w, kHalfRange);

        g.fill_x = v < 0 ? centre - offset : centre;
        g.fill_w = offset;
        g.marker_x = v < 0 ? centre - offset : centre + offset;

        // Beyond +/- danger_pct at either end, the same width at both: an odd
        // track's spare pixel sits in the safe middle, not in one danger zone.
        const int safe_half = scale(danger_pct, track_w, kHalfRange);
        g.danger_lo_x = 0;
        g.danger_lo_w = std::max(0, centre - safe_half);
        g.danger_hi_w = g.danger_lo_w;
        g.danger_hi_x = track_w - g.danger_hi_w;

        // The peak is a magnitude; show it on the side the reading leans to,
        // and to the right when it is sitting exactly in the middle.
        const int peak_off = scale(peak_pct, track_w, kHalfRange);
        g.peak_x = v < 0 ? centre - peak_off : centre + peak_off;
    } else {
        const int v = std::clamp(value, 0, 100);
        g.fill_x = 0;
        g.fill_w = scale(v, track_w, 100);
        g.marker_x = g.fill_w;

        g.danger_lo_w = 0;
        g.danger_hi_x = scale(danger_pct, track_w, 100);
        g.danger_hi_w = std::max(0, track_w - g.danger_hi_x);

        g.peak_x = scale(peak_pct, track_w, 100);
    }

    // Keep both ticks inside the track: a reading at either extreme puts the
    // marker exactly on the edge, where a tick drawn from that x would hang
    // half its width outside.
    const int last = std::max(0, track_w - kClogBarTickW);
    g.marker_x = std::clamp(g.marker_x - kClogBarTickW / 2, 0, last);
    g.peak_x = std::clamp(g.peak_x - kClogBarTickW / 2, 0, last);
    return g;
}

} // namespace helix::ui
