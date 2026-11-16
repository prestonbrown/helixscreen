// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_shaper_sweep_chart.h"

#include "ui_frequency_response_chart.h"

#include "platform_capabilities.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix::ui {

using helix::calibration::layout_preview;
using helix::calibration::LiveSweepPreview;
using helix::calibration::LiveSweepTarget;
using helix::calibration::SweepColumn;
using helix::calibration::SweepPoint;

lv_color_t shaper_raw_psd_color() {
    // The legend dot in input_shaper_panel.xml binds the same token.
    return theme_manager_get_color("chart_measured");
}

bool live_chart_layout_fits(const char* component_name, const char* gate_subject) {
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(component_name);
    lv_subject_t* gate = lv_xml_find_subject(scope, gate_subject);
    return gate == nullptr || lv_subject_get_int(gate) != 0;
}

ShaperSweepChart::~ShaperSweepChart() {
    // No LVGL here: the owner's widget tree may already be gone. Owners
    // destroy() while it is alive.
    preview_.stop();
}

void ShaperSweepChart::set_source_factory(LiveSweepPreview::SourceFactory factory) {
    preview_.set_source_factory(std::move(factory));
}

void ShaperSweepChart::set_on_curve_changed(std::function<void(bool has_curve)> on_changed) {
    on_curve_changed_ = std::move(on_changed);
}

bool ShaperSweepChart::begin(lv_obj_t* host, char axis,
                             const std::optional<LiveSweepTarget>& target) {
    preview_.stop();
    if (!target || host == nullptr) {
        return false;
    }

    if (chart_ != nullptr && host_ != host) {
        set_has_curve(false);
        destroy();
    }
    if (chart_ == nullptr) {
        chart_ = ui_frequency_response_chart_create(host);
        if (chart_ == nullptr) {
            return false;
        }
        host_ = host;
        ui_frequency_response_chart_configure_for_platform(chart_,
                                                           PlatformCapabilities::detect().tier);
        ui_frequency_response_chart_set_freq_range(chart_, 0.0f, kShaperChartMaxHz);
        series_ =
            ui_frequency_response_chart_add_series(chart_, "Live PSD", shaper_raw_psd_color());
        ui_frequency_response_chart_set_series_style(chart_, series_, kFrMeasuredCurveStyle);
    }

    column_ = (axis == 'Y' || axis == 'y') ? SweepColumn::Y : SweepColumn::X;
    preview_.arm(*target, [this](const std::vector<SweepPoint>& points) { on_points(points); });
    return true;
}

void ShaperSweepChart::on_progress(ShaperCalibrationPhase phase, float freq_hz) {
    if (phase == ShaperCalibrationPhase::Sweeping && freq_hz > 0.0f) {
        preview_.note_console_frequency(freq_hz);
    } else if (phase == ShaperCalibrationPhase::Analyzing) {
        preview_.finish();
    }
}

void ShaperSweepChart::show_curve(const std::vector<std::pair<float, float>>& freq_response) {
    preview_.stop();
    if (chart_ == nullptr) {
        return;
    }
    if (freq_response.empty()) {
        ui_frequency_response_chart_clear(chart_);
        set_has_curve(false);
        return;
    }
    std::vector<float> freqs;
    std::vector<float> amps;
    freqs.reserve(freq_response.size());
    amps.reserve(freq_response.size());
    float max_amp = 0.0f;
    for (const auto& [f, a] : freq_response) {
        freqs.push_back(f);
        amps.push_back(a);
        if (std::isfinite(a)) {
            max_amp = std::max(max_amp, a);
        }
    }
    draw(freqs, amps, max_amp);
    set_has_curve(true);
}

void ShaperSweepChart::stop() {
    preview_.stop();
}

void ShaperSweepChart::clear() {
    preview_.stop();
    if (chart_ != nullptr) {
        ui_frequency_response_chart_clear(chart_);
    }
    set_has_curve(false);
}

void ShaperSweepChart::destroy() {
    preview_.stop();
    if (chart_ != nullptr) {
        ui_frequency_response_chart_destroy(chart_);
        chart_ = nullptr;
    }
    host_ = nullptr;
    series_ = -1;
    // Owners reset what they bound to the curve themselves: this runs from
    // teardown, where their subjects may already be gone.
    has_curve_ = false;
}

bool ShaperSweepChart::has_curve() const {
    return has_curve_;
}

ui_frequency_response_chart_t* ShaperSweepChart::chart() const {
    return chart_;
}

const LiveSweepPreview& ShaperSweepChart::preview() const {
    return preview_;
}

void ShaperSweepChart::on_points(const std::vector<SweepPoint>& points) {
    const auto series = layout_preview(points, column_, 0.0f, kShaperChartMaxHz);
    // Until a finite point exists, whatever is drawn (the previous axis's
    // curve) stays up.
    if (series.max_amp <= 0.0f) {
        return;
    }
    draw(series.freqs, series.amps, series.max_amp);
    set_has_curve(true);
}

void ShaperSweepChart::draw(const std::vector<float>& freqs, const std::vector<float>& amps,
                            float max_amp) {
    if (chart_ == nullptr || series_ < 0 || freqs.empty()) {
        return;
    }
    ui_frequency_response_chart_set_amplitude_range(
        chart_, 0.0f, max_amp > 0.0f ? max_amp * kShaperChartHeadroom : 1.0f);
    ui_frequency_response_chart_set_data(chart_, series_, freqs.data(), amps.data(), freqs.size());
}

void ShaperSweepChart::set_has_curve(bool has_curve) {
    if (has_curve_ == has_curve) {
        return;
    }
    has_curve_ = has_curve;
    if (on_curve_changed_) {
        on_curve_changed_(has_curve);
    }
}

} // namespace helix::ui
