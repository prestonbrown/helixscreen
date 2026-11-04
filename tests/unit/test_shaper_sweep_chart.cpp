// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_frequency_response_chart.h"
#include "ui_shaper_sweep_chart.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/sweep_preview_fakes.h"
#include "core/update_queue.h"

#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using namespace helix::calibration::test;
using helix::ui::ShaperSweepChart;

namespace {

/// The chart's only series: the first one added.
constexpr int kSeries = 0;

/// Equal element for element, NaN matching NaN.
bool same_curve(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!(a[i] == b[i] || (std::isnan(a[i]) && std::isnan(b[i])))) {
            return false;
        }
    }
    return true;
}

struct ShaperSweepChartFixture : LVGLTestFixture {
    SourceLog log;
    ShaperSweepChart chart;
    lv_obj_t* host = nullptr;
    std::vector<bool> curve_events;

    ShaperSweepChartFixture() {
        host = lv_obj_create(test_screen());
        lv_obj_set_size(host, 600, 300);
        chart.set_source_factory(manual_factory(log));
        chart.set_on_curve_changed([this](bool has) { curve_events.push_back(has); });
    }
    ~ShaperSweepChartFixture() override {
        chart.destroy();
    }

    static LiveSweepTarget target(SweepPlan plan = {5.0f, 60.0f, 1.0f}) {
        return LiveSweepTarget{"/tmp/k.sock", "adxl345", plan};
    }

    /// Play a sweep's samples, console hints leading by 1.5 s, until `until_s`.
    void play(SweepSynth& synth, const SweepPlan& plan, double until_s) {
        while (synth.time_s() < until_s) {
            const double t = synth.time_s();
            const double f = plan.freq_start_hz + plan.hz_per_sec * (t + 1.5 - 1.7);
            if (f >= plan.freq_start_hz) {
                chart.on_progress(ShaperCalibrationPhase::Sweeping,
                                  std::floor(std::min(static_cast<float>(f), plan.freq_end_hz)));
            }
            AccelBatch batch;
            batch.samples = synth.next(300);
            if (log.last != nullptr) {
                log.last->push(batch);
            }
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    static SweepSynth synth_for(SweepPlan plan) {
        SweepSynthSpec spec;
        spec.plan = plan;
        spec.drive_start_s = 1.7;
        return SweepSynth(spec, [](float f) {
            const float d = (f - 30.0f) / 4.0f;
            const float level = 600.0f * std::sqrt(0.05f + 1.0f / (1.0f + d * d));
            return std::array<float, 3>{level, 0.5f * level, 0.1f * level};
        });
    }

    /// The 0-200 Hz layout as the chart stores it: STANDARD holds 200 points,
    /// so the 201-slot grid is point-picked the way set_data documents
    /// (first and last kept, slot i from source i * (n - 1) / (max - 1)).
    [[nodiscard]] std::vector<float> as_stored(const std::vector<float>& layout) const {
        const size_t max_points = ui_frequency_response_chart_get_max_points(chart.chart());
        if (max_points == 0 || layout.size() <= max_points) {
            return layout;
        }
        std::vector<float> out(max_points);
        for (size_t i = 0; i < max_points; ++i) {
            const size_t src = i == max_points - 1 ? layout.size() - 1
                                                   : i * (layout.size() - 1) / (max_points - 1);
            out[i] = layout[src];
        }
        return out;
    }

    [[nodiscard]] std::vector<float> amps() const {
        return ui_frequency_response_chart_get_series_amplitudes(chart.chart(), kSeries);
    }
};

} // namespace

TEST_CASE_METHOD(ShaperSweepChartFixture, "ShaperSweepChart needs a target",
                 "[input_shaper][preview]") {
    CHECK_FALSE(chart.begin(host, 'X', std::nullopt));
    CHECK(chart.chart() == nullptr);
    CHECK(log.created == 0);
    CHECK_FALSE(chart.has_curve());
    CHECK_FALSE(chart.begin(nullptr, 'X', target()));
    CHECK(chart.chart() == nullptr);
}

TEST_CASE_METHOD(ShaperSweepChartFixture, "ShaperSweepChart draws like the results' raw PSD",
                 "[input_shaper][preview]") {
    REQUIRE(chart.begin(host, 'X', target()));
    REQUIRE(chart.chart() != nullptr);
    CHECK(lv_obj_get_parent(ui_frequency_response_chart_get_obj(chart.chart())) == host);
    const auto range = ui_frequency_response_chart_get_freq_range(chart.chart());
    CHECK(range.first == 0.0f);
    CHECK(range.second == helix::ui::kShaperChartMaxHz);
    CHECK(lv_color_eq(ui_frequency_response_chart_get_series_color(chart.chart(), kSeries),
                      helix::ui::shaper_raw_psd_color()));
    // The same look as the results' measured curve, as the tier allows it.
    const auto style = ui_frequency_response_chart_get_series_style(chart.chart(), kSeries);
    const auto caps = helix::PlatformCapabilities::detect();
    const auto expected =
        fr_chart_effective_style(kFrMeasuredCurveStyle, caps.tier, caps.supports_animations);
    CHECK(style.line_width == expected.line_width);
    CHECK(style.glow == expected.glow);
    CHECK(style.fill == expected.fill);
    CHECK(style.fill);

    chart.on_progress(ShaperCalibrationPhase::Sweeping, 0.0f);
    CHECK(log.created == 0);
    chart.on_progress(ShaperCalibrationPhase::Sweeping, 5.0f);
    CHECK(log.created == 1);
    CHECK(log.accel_chip == "adxl345");
}

TEST_CASE_METHOD(ShaperSweepChartFixture, "ShaperSweepChart lays the points on its 0-200 Hz axis",
                 "[input_shaper][preview]") {
    const char axis = GENERATE('X', 'Y');
    const SweepPlan plan{5.0f, 60.0f, 1.0f};
    REQUIRE(chart.begin(host, axis, target(plan)));
    auto synth = synth_for(plan);
    play(synth, plan, 1.7 + 55.0 + 2.0);

    const auto& points = chart.preview().points();
    REQUIRE(points.size() == 56);
    // Slot i of the 0-200 Hz grid is i Hz: NaN outside the sweep, else that
    // point's own column.
    std::vector<float> layout(201, NAN);
    float max_amp = 0.0f;
    for (size_t i = 5; i <= 60; ++i) {
        const SweepPoint& p = points[i - 5];
        REQUIRE(p.freq_hz == static_cast<float>(i));
        REQUIRE(p.clean);
        layout[i] = axis == 'X' ? p.psd_x : p.psd_y;
        max_amp = std::max(max_amp, layout[i]);
    }
    const auto expected = as_stored(layout);
    const auto drawn = amps();
    REQUIRE(drawn.size() == expected.size());
    for (size_t i = 0; i < drawn.size(); ++i) {
        INFO(axis << " stored slot " << i);
        if (std::isnan(expected[i])) {
            CHECK(std::isnan(drawn[i]));
        } else {
            CHECK(drawn[i] == expected[i]);
        }
    }
    const auto amp_range = ui_frequency_response_chart_get_amplitude_range(chart.chart());
    CHECK(amp_range.first == 0.0f);
    CHECK(amp_range.second == max_amp * helix::ui::kShaperChartHeadroom);
    CHECK(curve_events == std::vector<bool>{true});
    CHECK(chart.has_curve());
}

TEST_CASE_METHOD(ShaperSweepChartFixture, "ShaperSweepChart finishes on Analyzing",
                 "[input_shaper][preview]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    REQUIRE(chart.begin(host, 'X', target(plan)));
    auto synth = synth_for(plan);
    play(synth, plan, 20.0);
    REQUIRE(chart.preview().state() == LiveSweepPreview::State::STREAMING);
    chart.on_progress(ShaperCalibrationPhase::Analyzing, 0.0f);
    CHECK(chart.preview().state() == LiveSweepPreview::State::STREAMING);
    play(synth, plan, 23.0);
    CHECK(chart.preview().state() == LiveSweepPreview::State::DONE);
    CHECK(log.stopped == 1);
}

TEST_CASE_METHOD(ShaperSweepChartFixture, "ShaperSweepChart swaps the preview for the CSV curve",
                 "[input_shaper][preview]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    REQUIRE(chart.begin(host, 'X', target(plan)));
    auto synth = synth_for(plan);
    play(synth, plan, 20.0);
    REQUIRE(log.created == 1);

    chart.show_curve({{5.0f, 1.0f}, {6.0f, 4.0f}});
    CHECK(log.stopped == 1);
    CHECK(amps() == std::vector<float>{1.0f, 4.0f});
    const auto amp_range = ui_frequency_response_chart_get_amplitude_range(chart.chart());
    CHECK(amp_range.first == 0.0f);
    CHECK(amp_range.second == Catch::Approx(4.4f));
    CHECK(chart.has_curve());

    chart.stop();
    CHECK(amps() == std::vector<float>{1.0f, 4.0f});
}

TEST_CASE_METHOD(ShaperSweepChartFixture,
                 "ShaperSweepChart keeps the last curve until the next axis draws",
                 "[input_shaper][preview]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    REQUIRE(chart.begin(host, 'X', target(plan)));
    auto synth = synth_for(plan);
    play(synth, plan, 20.0);
    REQUIRE(curve_events == std::vector<bool>{true});
    chart.show_curve({{5.0f, 1.0f}, {6.0f, 4.0f}});

    // Y arms: X's curve stays up, never blank, until Y's first points.
    REQUIRE(chart.begin(host, 'Y', target(plan)));
    CHECK(curve_events == std::vector<bool>{true});
    CHECK(chart.has_curve());
    CHECK(amps() == std::vector<float>{1.0f, 4.0f});
    auto second = synth_for(plan);
    play(second, plan, 1.5); // the drive has not started: nothing finite
    CHECK(amps() == std::vector<float>{1.0f, 4.0f});
    play(second, plan, 20.0);
    CHECK(log.created == 2);
    CHECK(curve_events == std::vector<bool>{true});
    CHECK(amps().size() > 2);

    const auto drawn = amps();
    chart.stop();
    CHECK(same_curve(amps(), drawn));

    chart.clear();
    CHECK(curve_events == std::vector<bool>{true, false});
    CHECK_FALSE(chart.has_curve());
    CHECK(log.stopped == 2);

    lv_obj_t* other = lv_obj_create(test_screen());
    lv_obj_set_size(other, 400, 200);
    chart.destroy();
    CHECK(chart.chart() == nullptr);
    REQUIRE(chart.begin(other, 'X', target(plan)));
    REQUIRE(chart.chart() != nullptr);
    CHECK(lv_obj_get_parent(ui_frequency_response_chart_get_obj(chart.chart())) == other);
}
