// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_frequency_response_chart.h"

#include "calibration_types.h"
#include "live_sweep_preview.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

// NAMESPACE_OK: the chart's global C-style API type
struct ui_frequency_response_chart_t;

namespace helix::ui {

/// The input shaper charts' frequency axis, Hz. The results charts and the
/// live chart share it so a curve sits at the same x on both.
inline constexpr float kShaperChartMaxHz = 200.0f;
/// Amplitude axis ceiling over the tallest point.
inline constexpr float kShaperChartHeadroom = 1.1f;

/// The results charts' raw PSD series color (the theme's chart_measured); the
/// live curve uses it too.
[[nodiscard]] lv_color_t shaper_raw_psd_color();

/**
 * @brief Whether a component's layout shows the live chart at the current size
 *
 * Reads the gate subject the component's XML publishes (a <subject_expr> over
 * the breakpoint), so the size rule lives in the layout and C++ only asks it.
 * True when the gate is absent. Main thread.
 */
[[nodiscard]] bool live_chart_layout_fits(const char* component_name, const char* gate_subject);

/**
 * @brief The input shaper's live sweep chart
 *
 * One series drawn the way the results charts draw the raw PSD, fed by a
 * LiveSweepPreview during the sweep and, for callers that keep it, by the CSV
 * curve after it. Shared by InputShaperPanel's MEASURING view and the wizard
 * step. Main thread only.
 */
class ShaperSweepChart {
  public:
    ShaperSweepChart() = default;
    ~ShaperSweepChart();
    ShaperSweepChart(const ShaperSweepChart&) = delete;
    ShaperSweepChart& operator=(const ShaperSweepChart&) = delete;

    void set_source_factory(helix::calibration::LiveSweepPreview::SourceFactory factory);

    /// Fires with true when the first finite point is drawn and false when the
    /// curve is cleared; callers bind their host's visibility to it.
    void set_on_curve_changed(std::function<void(bool has_curve)> on_changed);

    /**
     * @brief Start a fresh curve for `axis` ('X' or 'Y') in `host`
     *
     * Whatever is drawn (the previous axis's CSV curve) stays until the new
     * preview's first points replace it, so the chart never goes blank between
     * axes; clear() is how a caller starts from empty. Creates the chart in
     * `host` on first use (and again, empty, when `host` changed). Returns
     * false, arming nothing, when `target` is nullopt or `host` is null.
     */
    bool begin(lv_obj_t* host, char axis,
               const std::optional<helix::calibration::LiveSweepTarget>& target);

    /// A progress report: Sweeping with freq_hz > 0 feeds the console
    /// frequency, Analyzing finishes the stream. Other phases do nothing.
    void on_progress(ShaperCalibrationPhase phase, float freq_hz);

    /// Mark the drawn curve's highest point with its frequency, redrawn with
    /// every update. Off by default.
    void set_marks_peak(bool marks);

    /// Stop the preview and draw the CSV curve (freq, psd) in its place.
    void show_curve(const std::vector<std::pair<float, float>>& freq_response);

    /// Stop streaming; what is drawn stays.
    void stop();
    /// Stop streaming and empty the chart (has_curve() false).
    void clear();
    /// Stop and free the chart. Call before the host's widget tree goes.
    void destroy();

    [[nodiscard]] bool has_curve() const;
    [[nodiscard]] ui_frequency_response_chart_t* chart() const;
    [[nodiscard]] const helix::calibration::LiveSweepPreview& preview() const;

  private:
    void on_points(const std::vector<helix::calibration::SweepPoint>& points);
    /// Main thread: one set_data, amplitude range 0..max * kShaperChartHeadroom.
    void draw(const std::vector<float>& freqs, const std::vector<float>& amps, float max_amp);
    void set_has_curve(bool has_curve);

    helix::calibration::LiveSweepPreview preview_;
    ui_frequency_response_chart_t* chart_ = nullptr;
    lv_obj_t* host_ = nullptr;
    int series_ = -1;
    helix::calibration::SweepColumn column_ = helix::calibration::SweepColumn::X;
    bool has_curve_ = false;
    bool marks_peak_ = false;
    std::function<void(bool)> on_curve_changed_;
};

} // namespace helix::ui
