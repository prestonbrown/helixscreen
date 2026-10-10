// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "buffer_reading.h"

#include <array>
#include <cstdint>
#include <vector>

namespace helix::ui {

/// A rectangle in a slider's or trace's own pixels, y down from its top.
struct BufferBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

/// Pixel layout of the upright buffer slider in a box width x height. Loose is
/// up and tight is down, as filament flows top to bottom on the path canvas.
/// Vertical metrics depend on the height alone, horizontal ones on the width
/// alone. A zero width or height lays out nothing.
struct BufferSliderGeometry {
    int strand_x = 0; ///< Centre of the filament strand, which runs the full height
    int strand_w = 0;
    BufferBox housing; ///< Full width; the strand runs out past both ends
    int housing_radius = 0;
    BufferBox danger_top;    ///< Loose end stop: block centres past +kPressureFaultPct
    BufferBox danger_bottom; ///< Tight end stop: block centres past -kPressureFaultPct
    /// Dashed window: the block lies inside it while under kPressureWarningPct.
    BufferBox target;
    BufferBox block;
    int radius = 0; ///< End stops, window and block
    /// Grip lines across the block, each from grip_x1 to grip_x2. None on a
    /// block too short to show them apart.
    std::array<int, 3> grip_y{};
    int grip_count = 0;
    int grip_x1 = 0;
    int grip_x2 = 0;
};

/// Pixel layout of the upright pressure gauge for a compression-only reading,
/// in the same footprint as the slider: 0% at the bottom, 100% at the top, the
/// fill rising from the bottom to the reading and a tick at the set point.
struct BufferFillGeometry {
    BufferBox housing; ///< Full width, inset top and bottom as the slider's is
    int housing_radius = 0;
    BufferBox track; ///< The band inside the housing the fill rises in
    BufferBox fill;  ///< Bottom-anchored; zero height at 0%
    int radius = 0;
    bool has_target = false;
    BufferBox target; ///< A thin tick across the housing at the set point
};

/// Layout of the fill gauge for @p value_pct (clamped 0..100) and @p target_pct
/// (clamped; negative means no set point, so no tick) in a box width x height.
/// A zero width or height lays out nothing.
BufferFillGeometry buffer_fill_geometry(int value_pct, int target_pct, int width, int height);

/// y of @p pct (clamped 0..100) in a trace @p height tall: 100 at the top row,
/// 0 at the bottom row.
int buffer_fill_trace_y(int pct, int height);

/// Centre y of the block for @p bias (-1 tight .. +1 loose, clamped; NaN reads
/// as 0) in a slider @p height tall.
int buffer_slider_y(float bias, int height);

BufferSliderGeometry buffer_slider_geometry(float bias, int width, int height);

/// y of @p bias (clamped, NaN reads as 0) in a trace @p height tall: +1 at the
/// top row, -1 at the bottom row.
int buffer_trace_y(float bias, int height);

struct BufferTraceXY {
    int x;
    int y;
    /// pressure_status() of the reading this point belongs to.
    ClogMeterStatus status = ClogMeterStatus::Ok;
};

/// Severity a trace segment is drawn in: the worse of its end points', so a
/// step between two bands takes the band that is further from target.
ClogMeterStatus buffer_trace_segment_status(const BufferTraceXY& a, const BufferTraceXY& b);

/// The trace as polylines in a box width x height: newest at x = 0, the side
/// facing the slider, older readings further right, one polyline per run of
/// valid points.
/// Each reading holds as a step until the next.
std::vector<std::vector<BufferTraceXY>>
buffer_trace_polylines(const std::vector<BufferTracePoint>& window, int64_t now_ms, int width,
                       int height);

/// Where the not-yet-recorded part of the window begins, as an x in a box
/// @p width wide: the oldest point's x, running to @p width. Equals @p width
/// (zero length) once history reaches back a full window; 0 with no history.
int buffer_trace_unrecorded_x(const std::vector<BufferTracePoint>& window, int64_t now_ms,
                              int width);

} // namespace helix::ui
