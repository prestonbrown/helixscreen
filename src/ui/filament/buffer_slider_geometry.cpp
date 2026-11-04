// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer_slider_geometry.h"

#include "clog_meter_geometry.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

float clamp_bias(float bias) {
    return std::isnan(bias) ? 0.0f : std::clamp(bias, -1.0f, 1.0f);
}

/// The strand's run past each end of the housing.
int overhang(int height) {
    return std::max(2, height / 16);
}

/// Between the housing's edge and the end stops inside it.
int inner_pad(int height) {
    return std::max(2, (height - 2 * overhang(height)) / 40);
}

/// Top and height of the band the block travels in.
int inner_top(int height) {
    return overhang(height) + inner_pad(height);
}
int inner_height(int height) {
    return std::max(0, height - 2 * inner_top(height));
}

int block_height(int height) {
    return std::max(6, inner_height(height) / 6);
}

BufferBox span(int x, int w, int y1, int y2) {
    return {x, y1, w, std::max(0, y2 - y1)};
}

} // namespace

int buffer_slider_y(float bias, int height) {
    if (height <= 0) {
        return 0;
    }
    const int block_h = block_height(height);
    const int travel = std::max(0, inner_height(height) - block_h);
    return inner_top(height) + block_h / 2 +
           static_cast<int>(std::lround((1.0f - clamp_bias(bias)) * travel / 2.0f));
}

BufferSliderGeometry buffer_slider_geometry(float bias, int width, int height) {
    BufferSliderGeometry g;
    if (width <= 0 || height <= 0) {
        return g;
    }
    g.strand_x = width / 2;
    g.strand_w = std::max(2, width / 8);
    g.housing = {0, overhang(height), width, height - 2 * overhang(height)};
    g.housing_radius = std::max(2, width / 6);
    g.radius = std::max(1, width / 12);

    const int top = inner_top(height);
    const int bottom = top + inner_height(height);
    const int pad_x = std::max(2, width / 10);
    const float fault = kPressureFaultPct / 100.0f;
    g.danger_top = span(pad_x, width - 2 * pad_x, top, buffer_slider_y(fault, height));
    g.danger_bottom = span(pad_x, width - 2 * pad_x, buffer_slider_y(-fault, height), bottom);

    const int block_h = block_height(height);
    const int block_x = std::max(2, width / 8);
    g.block = {block_x, buffer_slider_y(bias, height) - block_h / 2, width - 2 * block_x, block_h};

    // One pixel clear of the block at either edge of the band, so the dashed
    // line never runs under it.
    const float warning = kPressureWarningPct / 100.0f;
    const int window_x = std::max(1, width / 12);
    g.target =
        span(window_x, width - 2 * window_x, buffer_slider_y(warning, height) - block_h / 2 - 1,
             buffer_slider_y(-warning, height) - block_h / 2 + block_h + 1);

    if (block_h >= 9) {
        g.grip_count = 3;
        for (int i = 0; i < 3; ++i) {
            g.grip_y[i] = g.block.y + block_h * (i + 1) / 4;
        }
        g.grip_x1 = g.block.x + g.block.w / 5;
        g.grip_x2 = g.block.x + g.block.w - g.block.w / 5 - 1;
    }
    return g;
}

BufferFillGeometry buffer_fill_geometry(int value_pct, int target_pct, int width, int height) {
    BufferFillGeometry g;
    if (width <= 0 || height <= 0) {
        return g;
    }
    g.housing = {0, overhang(height), width, height - 2 * overhang(height)};
    g.housing_radius = std::max(2, width / 6);
    g.radius = std::max(1, width / 12);

    const int pad_x = std::max(2, width / 10);
    const int top = inner_top(height);
    const int track_h = inner_height(height);
    g.track = {pad_x, top, std::max(0, width - 2 * pad_x), track_h};

    auto rise = [track_h](int pct) {
        return static_cast<int>(std::lround(std::clamp(pct, 0, 100) * track_h / 100.0));
    };
    const int fill_h = rise(value_pct);
    g.fill = {g.track.x, top + track_h - fill_h, g.track.w, fill_h};

    if (target_pct >= 0) {
        constexpr int kTickH = 2;
        const int tick_x = std::max(1, width / 12);
        g.has_target = true;
        g.target = {tick_x, top + track_h - rise(target_pct) - kTickH / 2, width - 2 * tick_x,
                    kTickH};
    }
    return g;
}

int buffer_fill_trace_y(int pct, int height) {
    if (height <= 0) {
        return 0;
    }
    return static_cast<int>(std::lround((100 - std::clamp(pct, 0, 100)) * (height - 1) / 100.0));
}

int buffer_trace_y(float bias, int height) {
    if (height <= 0) {
        return 0;
    }
    return static_cast<int>(std::lround((1.0f - clamp_bias(bias)) * (height - 1) / 2.0f));
}

ClogMeterStatus buffer_trace_segment_status(const BufferTraceXY& a, const BufferTraceXY& b) {
    return static_cast<int>(a.status) > static_cast<int>(b.status) ? a.status : b.status;
}

std::vector<std::vector<BufferTraceXY>>
buffer_trace_polylines(const std::vector<BufferTracePoint>& window, int64_t now_ms, int width,
                       int height) {
    std::vector<std::vector<BufferTraceXY>> lines;
    if (width <= 0 || height <= 0) {
        return lines;
    }
    auto x_of = [&](int64_t t_ms) {
        return static_cast<int>(
            std::clamp<int64_t>((now_ms - t_ms) * width / BufferTrace::kWindowMs, 0, width));
    };
    std::vector<BufferTraceXY> run;
    int newer_x = 0; // where the next newer reading began; the newest holds from now
    for (auto it = window.rbegin(); it != window.rend(); ++it) {
        const int older_x = x_of(it->t_ms);
        if (it->valid) {
            const bool fill = it->gauge == BufferGauge::Fill;
            const int y =
                fill ? buffer_fill_trace_y(it->fill_pct, height) : buffer_trace_y(it->bias, height);
            const auto status = fill ? it->status : pressure_status_of_bias(it->bias);
            run.push_back({newer_x, y, status});
            run.push_back({older_x, y, status});
        } else if (!run.empty()) {
            lines.push_back(std::move(run));
            run.clear();
        }
        newer_x = older_x;
    }
    if (!run.empty()) {
        lines.push_back(std::move(run));
    }
    return lines;
}

int buffer_trace_unrecorded_x(const std::vector<BufferTracePoint>& window, int64_t now_ms,
                              int width) {
    if (width <= 0 || window.empty()) {
        return 0;
    }
    return static_cast<int>(std::clamp<int64_t>(
        (now_ms - window.front().t_ms) * width / BufferTrace::kWindowMs, 0, width));
}

} // namespace helix::ui
