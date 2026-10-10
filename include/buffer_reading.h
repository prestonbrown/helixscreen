// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "clog_meter_geometry.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace helix {

/// What a buffer reading comes from, which is also what it is called on screen.
enum class BufferSource : int {
    None = 0, ///< No proportional reading: a switched buffer, or no buffer at all
    Fps = 1,  ///< A filament pressure sensor (OpenAMS lane, AFC FPS_PSF buffer)
    Sync = 2, ///< Happy Hare sync feedback, one buffer for the whole system
};

/// How a reading is drawn and worded.
enum class BufferGauge : int {
    /// A two-ended scale around a set point: tension below it, compression above
    /// (Happy Hare, AFC FPS_PSF). Drawn as a slider, worded tight / loose.
    Bias = 0,
    /// A one-sided pressure that runs from nothing up to full and is only ever
    /// compression (BufferHealth::compression_only). Drawn as a fill from the
    /// bottom with a tick at the set point, worded against the target.
    Fill = 1,
};

/// Where a filament buffer sits right now, for every surface that draws one.
struct BufferReading {
    BufferSource source = BufferSource::None;
    /// Position in AmsSystemInfo::units of the sensor read; -1 for Happy Hare's
    /// system-level buffer.
    int unit = -1;
    BufferGauge gauge = BufferGauge::Bias;
    /// A gauge draws. For a Bias reading that means there is a set point to
    /// center on, so a pressure sensor without one is shown as text only. A
    /// Fill reading always draws: the fill needs no set point, only its tick does.
    bool has_slider = false;
    /// Fps: the pressure, 0..100. Sync: the bias, -100..+100.
    int value_pct = 0;
    /// Fps with a set point: the set point, 0..100. -1 otherwise.
    int target_pct = -1;
    /// Bias gauge: -1 tight .. +1 loose around the set point; 0 without one.
    /// Always 0 for a Fill reading, which has no tight or loose.
    float bias = 0.0f;
    /// Bias gauge: ui::pressure_status() of the bias; Ok without a set point.
    /// Fill gauge: ui::fill_pressure_status() of the pressure.
    ui::ClogMeterStatus status = ui::ClogMeterStatus::Ok;

    [[nodiscard]] bool is_fill() const {
        return gauge == BufferGauge::Fill;
    }
    /// The value is worded as a bare "Pressure: N%": nothing to center it on.
    [[nodiscard]] bool text_only() const {
        return present() && (!has_slider || (is_fill() && target_pct < 0));
    }

    [[nodiscard]] bool present() const {
        return source != BufferSource::None;
    }
};

/// The reading for one unit's buffer, or the system's with @p unit -1.
///
/// A unit with its own pressure sensor reads that sensor, and a unit with a
/// switched buffer has no reading. Any other unit, and -1, reads the system:
/// the pressure sensor feeding the toolhead (feeding_pressure_unit()), else
/// Happy Hare's sync feedback.
[[nodiscard]] BufferReading buffer_reading(const AmsSystemInfo& info, int unit);

/// The unit whose rows (AFC state, fault distance) describe @p unit's buffer:
/// @p unit itself, else the unit the system reading came from, else 0.
[[nodiscard]] int buffer_view_unit(const AmsSystemInfo& info, int unit);

/// "FPS" or "Sync"; empty with no reading.
[[nodiscard]] const char* buffer_label(const BufferReading& r);

/// The number: "32%" for a pressure, "-45%" for a bias, "Pressure: 32%" for a
/// pressure with no set point; empty with no reading.
[[nodiscard]] std::string buffer_value_text(const BufferReading& r);

/// The number alone, for a surface too narrow for a prefix: "32%", "-45%".
/// Empty with no reading.
[[nodiscard]] std::string buffer_short_text(const BufferReading& r);

/// "target 50%" where a set point is known, else empty.
[[nodiscard]] std::string buffer_target_text(const BufferReading& r);

/// A Bias reading: "Running tight" / "Running loose" / "Balanced" from
/// buffer_lean(). A Fill reading: "At target" / "Above target" / "Below target"
/// against its set point, within kPressureTargetDeadbandPct. Empty without a
/// gauge, and for a Fill reading with no set point.
[[nodiscard]] const char* buffer_lean_text(const BufferReading& r);

/// The line under the trace that says what its axis is: "last 60 s · LOOSE up,
/// TIGHT down" for a Bias reading, "last 60 s · pressure, target dashed" for a
/// Fill reading.
[[nodiscard]] const char* buffer_trace_caption(const BufferReading& r);

/// Milliseconds on the clock buffer traces are stamped with. Monotonic, so a
/// wall-clock change cannot move a trace.
[[nodiscard]] int64_t buffer_clock_ms();

/// One reading in a BufferTrace. A point with valid false starts a gap.
struct BufferTracePoint {
    int64_t t_ms = 0;
    float bias = 0.0f; ///< Bias gauge only
    bool valid = false;
    BufferGauge gauge = BufferGauge::Bias;
    int fill_pct = 0; ///< Fill gauge only: the pressure, 0..100
    /// Fill gauge only: severity of that pressure when it was read.
    ui::ClogMeterStatus status = ui::ClogMeterStatus::Ok;
};

/// About a minute of one buffer's reading, for the trace drawn beside a gauge.
///
/// Readings arrive only when they change (OpenAMS republishes on a 0.02 move),
/// so each point holds until the next one: the trace is a step line, and a
/// reading that never changes still draws across the whole window. Main thread
/// only.
class BufferTrace {
  public:
    static constexpr int64_t kWindowMs = 60000;
    /// Bounds memory for a reading that changes on every status update.
    static constexpr std::size_t kMaxPoints = 512;

    /// Note the reading at @p now_ms. A repeat of the newest point adds
    /// nothing. A stamp older than the newest means the clock went back, and
    /// the history it can no longer place is dropped.
    void record(int64_t now_ms, bool valid, float bias);

    /// Note a whole reading: its bias, or its pressure and severity for a Fill
    /// gauge. A reading with no gauge (has_slider false) starts a gap.
    void record(int64_t now_ms, const BufferReading& reading);

    /// The points that draw the kWindowMs ending at @p now_ms, oldest first.
    /// When history reaches past the window, the first point is the value held
    /// at its start, stamped now_ms - kWindowMs. Points after @p now_ms are
    /// left out.
    [[nodiscard]] std::vector<BufferTracePoint> window(int64_t now_ms) const;

    void clear() {
        points_.clear();
    }
    [[nodiscard]] std::size_t size() const {
        return points_.size();
    }

  private:
    void push(int64_t now_ms, const BufferTracePoint& point);

    std::deque<BufferTracePoint> points_;
};

} // namespace helix
