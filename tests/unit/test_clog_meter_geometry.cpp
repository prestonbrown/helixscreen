// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_clog_meter_geometry.cpp
 * @brief The FlowGuard bar's layout arithmetic and the shared indicator-colour
 *        rule (prestonbrown/helixscreen#1017).
 *
 * Both are pure, so they are tested without LVGL or a loaded theme: the tint
 * rule names design tokens rather than resolving them, and the geometry is
 * plain track-local pixels. What the widget layer then has to get right is only
 * "put this rectangle at that x", which the widget-size sweep covers.
 */

#include "clog_meter_geometry.h"

#include <cmath>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {
constexpr int kMode_Encoder = static_cast<int>(ClogMeterMode::Encoder);
constexpr int kMode_Flowguard = static_cast<int>(ClogMeterMode::Flowguard);
constexpr int kMode_Buffer = static_cast<int>(ClogMeterMode::Buffer);
constexpr int kTrack = 200;
} // namespace

// ===========================================================================
// clog_meter_tint
// ===========================================================================

TEST_CASE("clog_meter_tint: a warning is danger whatever the mode", "[clog][tint][1017]") {
    for (int mode : {kMode_Encoder, kMode_Flowguard, kMode_Buffer}) {
        auto t = clog_meter_tint(mode, /*value=*/0, /*warning=*/1);
        INFO("mode " << mode);
        CHECK(std::string(t.a) == "danger");
        CHECK(std::string(t.b) == "danger");
    }
}

TEST_CASE("clog_meter_tint: linear modes ramp primary -> warning -> danger", "[clog][tint][1017]") {
    // At rest the indicator is primary: mix_a of 0 yields all of `b`.
    auto safe = clog_meter_tint(kMode_Encoder, 0, 0);
    CHECK(std::string(safe.b) == "primary");
    CHECK(safe.mix_a == 0);

    // Halfway is the hand-off point — fully `warning` from either side.
    auto mid_lo = clog_meter_tint(kMode_Encoder, 49, 0);
    CHECK(std::string(mid_lo.a) == "warning");
    CHECK(mid_lo.mix_a > 240); // all but the last step toward warning

    auto mid_hi = clog_meter_tint(kMode_Encoder, 50, 0);
    CHECK(std::string(mid_hi.b) == "warning");
    CHECK(mid_hi.mix_a == 0);

    // Fully clogged is fully danger.
    auto hot = clog_meter_tint(kMode_Buffer, 100, 0);
    CHECK(std::string(hot.a) == "danger");
    CHECK(hot.mix_a == 255);
}

TEST_CASE("clog_meter_tint: Flowguard does not tint by magnitude", "[clog][tint][1017]") {
    // Its two ends mean opposite faults and are labelled as such, so a colour
    // ramp across the middle would be reporting severity the scale already
    // shows position for.
    for (int v : {-100, -40, 0, 40, 100}) {
        auto t = clog_meter_tint(kMode_Flowguard, v, 0);
        INFO("value " << v);
        CHECK(std::string(t.b) == "primary");
    }
}

TEST_CASE("clog_meter_tint: a negative reading tints by magnitude", "[clog][tint][1017]") {
    // Encoder values are never negative in practice, but the rule reads
    // abs(value) and must not index the ramp backwards if one ever is.
    CHECK(clog_meter_tint(kMode_Encoder, -100, 0).mix_a ==
          clog_meter_tint(kMode_Encoder, 100, 0).mix_a);
}

// ===========================================================================
// clog_meter_is_safe
// ===========================================================================

TEST_CASE("clog_meter_is_safe: only the buffer mode reports nothing", "[clog][safe][1017]") {
    CHECK(clog_meter_is_safe(kMode_Buffer, 0));

    // A buffer reading of any size is a measurement, so it draws.
    CHECK_FALSE(clog_meter_is_safe(kMode_Buffer, 1));
    CHECK_FALSE(clog_meter_is_safe(kMode_Buffer, 100));
}

TEST_CASE("clog_meter_is_safe: zero is a real reading in the other modes", "[clog][safe][1017]") {
    // Encoder zero means "no clog", and Flowguard zero means "dead centre" —
    // both are measurements the widget must keep drawing.
    CHECK_FALSE(clog_meter_is_safe(kMode_Encoder, 0));
    CHECK_FALSE(clog_meter_is_safe(kMode_Flowguard, 0));
}

TEST_CASE("clog_meter_is_safe: no hardware is not the safe state", "[clog][safe][1017]") {
    // Mode 0 hides the whole widget from XML; conflating the two would leave
    // a check icon standing in for a printer that cannot detect a clog at all.
    CHECK_FALSE(clog_meter_is_safe(static_cast<int>(ClogMeterMode::None), 0));
}

// ===========================================================================
// clog_meter_status
// ===========================================================================

TEST_CASE("clog_meter_status: the backend's warning outranks the threshold",
          "[clog][status][1017]") {
    // A tripped backend is a fault however far off the threshold the reading
    // looks — it is the thing that pauses the print.
    CHECK(clog_meter_status(kMode_Encoder, /*value=*/0, /*warning=*/1, /*danger=*/75) ==
          ClogMeterStatus::Fault);
    CHECK(clog_meter_status(kMode_Flowguard, -5, 1, 80) == ClogMeterStatus::Fault);
}

TEST_CASE("clog_meter_status: reaching the threshold warns before anything trips",
          "[clog][status][1017]") {
    CHECK(clog_meter_status(kMode_Encoder, 74, 0, 75) == ClogMeterStatus::Ok);
    CHECK(clog_meter_status(kMode_Encoder, 75, 0, 75) == ClogMeterStatus::Warning);
    CHECK(clog_meter_status(kMode_Encoder, 100, 0, 75) == ClogMeterStatus::Warning);
}

TEST_CASE("clog_meter_status: Flowguard's tangle side counts by magnitude",
          "[clog][status][1017]") {
    // -85 is as far into the tangle end as +85 is into the clog end.
    CHECK(clog_meter_status(kMode_Flowguard, -85, 0, 80) == ClogMeterStatus::Warning);
    CHECK(clog_meter_status(kMode_Flowguard, 85, 0, 80) == ClogMeterStatus::Warning);
    CHECK(clog_meter_status(kMode_Flowguard, -2, 0, 80) == ClogMeterStatus::Ok);
}

TEST_CASE("clog_meter_status: nothing to report is OK, not a zero-distance fault",
          "[clog][status][1017]") {
    // An untracked AFC buffer reads zero; without the safe check a zero
    // threshold would have to be relied on to keep it quiet.
    CHECK(clog_meter_status(kMode_Buffer, 0, 0, 75) == ClogMeterStatus::Ok);
}

TEST_CASE("clog_meter_status: an unset threshold has no opinion", "[clog][status][1017]") {
    // danger_pct of 0 would otherwise make every reading, including a neutral
    // one, compare as "at or past the threshold".
    CHECK(clog_meter_status(kMode_Flowguard, 0, 0, /*danger=*/0) == ClogMeterStatus::Ok);
    CHECK(clog_meter_status(kMode_Encoder, 50, 0, 0) == ClogMeterStatus::Ok);
}

// ===========================================================================
// ClogMeterSample — the derived state both renderers must agree on
// ===========================================================================

TEST_CASE("ClogMeterSample: only Flowguard reads out from a centre", "[clog][model][1017]") {
    // The arc encodes this as LV_ARC_MODE_SYMMETRICAL over 0..200 and the bar
    // as centre-out geometry. Two encodings are fine; two decisions are not.
    ClogMeterSample fg;
    fg.mode = kMode_Flowguard;
    CHECK(fg.is_symmetrical());

    for (int mode : {static_cast<int>(ClogMeterMode::None), kMode_Encoder, kMode_Buffer}) {
        ClogMeterSample s;
        s.mode = mode;
        INFO("mode " << mode);
        CHECK_FALSE(s.is_symmetrical());
    }
}

TEST_CASE("ClogMeterSample: derived state matches the free functions", "[clog][model][1017]") {
    // The sample is a convenience over the same rules, so a renderer reading
    // either spelling cannot disagree with one reading the other.
    ClogMeterSample s;
    s.mode = kMode_Buffer;
    s.value = 0;
    s.danger_pct = 75;
    CHECK(s.is_safe() == clog_meter_is_safe(s.mode, s.value));
    CHECK(s.status() == clog_meter_status(s.mode, s.value, s.warning, s.danger_pct));

    s.mode = kMode_Flowguard;
    s.value = -90;
    s.danger_pct = 80;
    s.warning = 0;
    CHECK(s.is_safe() == clog_meter_is_safe(s.mode, s.value));
    CHECK(s.status() == ClogMeterStatus::Warning);
}

TEST_CASE("ClogMeterSample: a default sample draws nothing", "[clog][model][1017]") {
    // UiClogBar::relayout() runs from SIZE_CHANGED before the model exists and
    // falls back to a default sample. That must be inert, not a fault.
    ClogMeterSample s;
    CHECK(s.kind() == ClogMeterMode::None);
    CHECK_FALSE(s.is_safe());
    CHECK_FALSE(s.is_symmetrical());
    CHECK(s.status() == ClogMeterStatus::Ok);
    CHECK(clog_bar_geometry(s.mode, s.value, s.danger_pct, s.peak_pct, kTrack).fill_w == 0);
}

// ===========================================================================
// clog_bar_geometry — linear modes
// ===========================================================================

TEST_CASE("clog_bar_geometry: a linear mode fills from the left", "[clog][bar][1017]") {
    auto g = clog_bar_geometry(kMode_Encoder, /*value=*/25, /*danger=*/75, /*peak=*/40, kTrack);
    CHECK(g.fill_x == 0);
    CHECK(g.fill_w == kTrack / 4);

    // The danger shading is the far end only.
    CHECK(g.danger_lo_w == 0);
    CHECK(g.danger_hi_x == kTrack * 3 / 4);
    CHECK(g.danger_hi_w == kTrack / 4);

    CHECK(g.peak_x == kTrack * 40 / 100 - 1); // centred on the tick's own width
}

TEST_CASE("clog_bar_geometry: an empty and a full linear reading", "[clog][bar][1017]") {
    auto empty = clog_bar_geometry(kMode_Buffer, 0, 75, 0, kTrack);
    CHECK(empty.fill_w == 0);
    CHECK(empty.marker_x == 0);

    auto full = clog_bar_geometry(kMode_Buffer, 100, 75, 100, kTrack);
    CHECK(full.fill_w == kTrack);
    // Both ticks stay inside the track rather than hanging off the end.
    CHECK(full.marker_x <= kTrack - 2);
    CHECK(full.peak_x <= kTrack - 2);
}

// ===========================================================================
// clog_bar_geometry — Flowguard's symmetrical range
// ===========================================================================

TEST_CASE("clog_bar_geometry: Flowguard fills out from the centre", "[clog][bar][1017]") {
    const int centre = kTrack / 2;

    auto neutral = clog_bar_geometry(kMode_Flowguard, 0, 80, 0, kTrack);
    CHECK(neutral.fill_w == 0);
    CHECK(neutral.fill_x == centre);

    // Clog side: grows right from the centre.
    auto clog = clog_bar_geometry(kMode_Flowguard, 50, 80, 50, kTrack);
    CHECK(clog.fill_x == centre);
    CHECK(clog.fill_w == kTrack / 4);
    CHECK(clog.marker_x > centre);

    // Tangle side: same magnitude, mirrored.
    auto tangle = clog_bar_geometry(kMode_Flowguard, -50, 80, 50, kTrack);
    CHECK(tangle.fill_w == clog.fill_w);
    CHECK(tangle.fill_x == centre - clog.fill_w);
    CHECK(tangle.marker_x < centre);
}

TEST_CASE("clog_bar_geometry: Flowguard shades both ends", "[clog][bar][1017]") {
    auto g = clog_bar_geometry(kMode_Flowguard, 0, /*danger=*/80, 0, kTrack);
    // 80% of the way out from the centre, on each side.
    CHECK(g.danger_lo_x == 0);
    CHECK(g.danger_lo_w == kTrack / 2 - kTrack * 80 / 200);
    CHECK(g.danger_hi_x == kTrack / 2 + kTrack * 80 / 200);
    CHECK(g.danger_hi_w == kTrack - g.danger_hi_x);
    // Symmetrical, which is the whole point of the mode.
    CHECK(g.danger_lo_w == g.danger_hi_w);
}

TEST_CASE("clog_bar_geometry: Flowguard shades both ends alike on an odd track",
          "[clog][bar][1017]") {
    for (int track : {199, 201, 97, 3}) {
        INFO("track " << track);
        auto g = clog_bar_geometry(kMode_Flowguard, 0, /*danger=*/80, 0, track);
        CHECK(g.danger_lo_w == g.danger_hi_w);
        // The high zone still runs to the track's far edge.
        CHECK(g.danger_hi_x + g.danger_hi_w == track);
    }
}

TEST_CASE("clog_bar_geometry: the Flowguard peak follows the side in fault", "[clog][bar][1017]") {
    // peak_pct is max(|clog|, |tangle|) — a magnitude with no side of its own,
    // so it is drawn on the side the current reading leans toward.
    auto clog = clog_bar_geometry(kMode_Flowguard, 30, 80, 60, kTrack);
    auto tangle = clog_bar_geometry(kMode_Flowguard, -30, 80, 60, kTrack);
    CHECK(clog.peak_x > kTrack / 2);
    CHECK(tangle.peak_x < kTrack / 2);
}

// ===========================================================================
// Degenerate inputs
// ===========================================================================

TEST_CASE("clog_bar_geometry: a zero-width track draws nothing", "[clog][bar][1017]") {
    // Reached on the first layout pass, before the track has been measured.
    auto g = clog_bar_geometry(kMode_Flowguard, 50, 80, 50, 0);
    CHECK(g.fill_w == 0);
    CHECK(g.danger_lo_w == 0);
    CHECK(g.danger_hi_w == 0);
    CHECK(g.marker_x == 0);
    CHECK(g.peak_x == 0);
}

TEST_CASE("clog_bar_geometry: out-of-range values are clamped, not wrapped", "[clog][bar][1017]") {
    auto over = clog_bar_geometry(kMode_Flowguard, 5000, 80, 5000, kTrack);
    CHECK(over.fill_x + over.fill_w <= kTrack);
    CHECK(over.marker_x <= kTrack - 2);

    auto under = clog_bar_geometry(kMode_Flowguard, -5000, 80, -5000, kTrack);
    CHECK(under.fill_x >= 0);
    CHECK(under.peak_x >= 0);

    auto silly_danger = clog_bar_geometry(kMode_Encoder, 50, /*danger=*/900, 0, kTrack);
    CHECK(silly_danger.danger_hi_w == 0); // shading collapses, it does not invert
}

// ===========================================================================
// Buffer reading bands
// ===========================================================================

TEST_CASE("pressure_status: the bands, by magnitude", "[clog][status][pressure]") {
    CHECK(pressure_status(0) == ClogMeterStatus::Ok);
    CHECK(pressure_status(kPressureWarningPct - 1) == ClogMeterStatus::Ok);
    CHECK(pressure_status(kPressureWarningPct) == ClogMeterStatus::Warning);
    CHECK(pressure_status(-kPressureWarningPct) == ClogMeterStatus::Warning);
    CHECK(pressure_status(kPressureFaultPct - 1) == ClogMeterStatus::Warning);
    CHECK(pressure_status(kPressureFaultPct) == ClogMeterStatus::Fault);
    CHECK(pressure_status(-100) == ClogMeterStatus::Fault);
}

TEST_CASE("pressure_status_of_bias: the bands at their edges", "[clog][status][pressure]") {
    CHECK(pressure_status_of_bias(-0.71f) == ClogMeterStatus::Fault);
    CHECK(pressure_status_of_bias(-0.70f) == ClogMeterStatus::Fault);
    CHECK(pressure_status_of_bias(-0.30f) == ClogMeterStatus::Warning);
    CHECK(pressure_status_of_bias(-0.29f) == ClogMeterStatus::Ok);
    CHECK(pressure_status_of_bias(0.0f) == ClogMeterStatus::Ok);
    CHECK(pressure_status_of_bias(0.29f) == ClogMeterStatus::Ok);
    CHECK(pressure_status_of_bias(0.30f) == ClogMeterStatus::Warning);
    CHECK(pressure_status_of_bias(0.70f) == ClogMeterStatus::Fault);
    CHECK(pressure_status_of_bias(5.0f) == ClogMeterStatus::Fault);
    CHECK(pressure_status_of_bias(std::nanf("")) == ClogMeterStatus::Ok);
}

TEST_CASE("buffer_lean: tension is tight, compression loose, a deadband between",
          "[clog][pressure]") {
    CHECK(buffer_lean(0.0f) == BufferLean::Balanced);
    CHECK(buffer_lean(-0.019f) == BufferLean::Balanced);
    CHECK(buffer_lean(0.019f) == BufferLean::Balanced);
    CHECK(buffer_lean(-0.3f) == BufferLean::Tight);
    CHECK(buffer_lean(0.15f) == BufferLean::Loose);
}

TEST_CASE("buffer_status_token: neutral on target, warning off it, danger at an end stop",
          "[clog][status][buffer]") {
    CHECK(std::string(buffer_status_token(ClogMeterStatus::Ok)) == "text_muted");
    CHECK(std::string(buffer_status_token(ClogMeterStatus::Warning)) == "warning");
    CHECK(std::string(buffer_status_token(ClogMeterStatus::Fault)) == "danger");
}
