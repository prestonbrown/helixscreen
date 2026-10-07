// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer_reading.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace helix {

BufferReading buffer_reading(const AmsSystemInfo& info, int unit) {
    BufferReading r;
    const AmsUnit* own = info.get_unit(unit);
    if (own && own->buffer_health && !own->buffer_health->fps_reported) {
        return r; // a switched buffer: where it sits is not measured
    }
    const int sensor = (own && own->buffer_health) ? unit : info.feeding_pressure_unit();
    if (sensor >= 0) {
        const BufferHealth& fps = *info.units[static_cast<size_t>(sensor)].buffer_health;
        r.source = BufferSource::Fps;
        r.unit = sensor;
        r.value_pct = std::clamp(static_cast<int>(std::lround(fps.smoothed_fps * 100.0f)), 0, 100);
        if (fps.has_fps()) {
            r.has_slider = true;
            r.target_pct = static_cast<int>(std::lround(fps.fps_set_point * 100.0f));
            r.bias = fps.fps_to_bias();
        }
    } else if (info.sync_feedback_bias > -1.5f) {
        r.source = BufferSource::Sync;
        r.has_slider = true;
        r.bias = std::clamp(info.sync_feedback_bias, -1.0f, 1.0f);
        r.value_pct = static_cast<int>(std::lround(r.bias * 100.0f));
    }
    if (r.has_slider) {
        r.status = ui::pressure_status_of_bias(r.bias);
    }
    return r;
}

int buffer_view_unit(const AmsSystemInfo& info, int unit) {
    return unit >= 0 ? unit : std::max(buffer_reading(info, -1).unit, 0);
}

const char* buffer_label(const BufferReading& r) {
    switch (r.source) {
    case BufferSource::Fps:
        return "FPS"; // i18n: do not translate - hardware abbreviation
    case BufferSource::Sync:
        return lv_tr("Sync");
    case BufferSource::None:
        break;
    }
    return "";
}

std::string buffer_short_text(const BufferReading& r) {
    if (!r.present()) {
        return "";
    }
    if (r.has_slider && r.source == BufferSource::Sync && r.value_pct != 0) {
        return fmt::format("{:+d}%", r.value_pct);
    }
    return fmt::format("{}%", r.value_pct);
}

std::string buffer_value_text(const BufferReading& r) {
    if (r.present() && !r.has_slider) {
        return fmt::format("{} {}", lv_tr("Pressure:"), buffer_short_text(r));
    }
    return buffer_short_text(r);
}

std::string buffer_target_text(const BufferReading& r) {
    if (r.target_pct < 0) {
        return "";
    }
    return fmt::format(fmt::runtime(lv_tr("target {}%")), r.target_pct);
}

const char* buffer_lean_text(const BufferReading& r) {
    if (!r.has_slider) {
        return "";
    }
    switch (ui::buffer_lean(r.bias)) {
    case ui::BufferLean::Tight:
        return lv_tr("Running tight");
    case ui::BufferLean::Loose:
        return lv_tr("Running loose");
    case ui::BufferLean::Balanced:
        break;
    }
    return lv_tr("Balanced");
}

int64_t buffer_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void BufferTrace::record(int64_t now_ms, bool valid, float bias) {
    if (!points_.empty() && now_ms < points_.back().t_ms) {
        points_.clear();
    }
    if (!points_.empty()) {
        const BufferTracePoint& last = points_.back();
        if (last.valid == valid && (!valid || last.bias == bias)) {
            return;
        }
    }
    points_.push_back({now_ms, valid ? bias : 0.0f, valid});
    // One point at or before the window's start stays: it is the value held
    // into the window.
    while (points_.size() >= 2 && points_[1].t_ms <= now_ms - kWindowMs) {
        points_.pop_front();
    }
    while (points_.size() > kMaxPoints) {
        points_.pop_front();
    }
}

std::vector<BufferTracePoint> BufferTrace::window(int64_t now_ms) const {
    std::vector<BufferTracePoint> out;
    const int64_t start = now_ms - kWindowMs;
    for (const BufferTracePoint& p : points_) {
        if (p.t_ms > now_ms) {
            break;
        }
        if (p.t_ms <= start) {
            out.assign(1, BufferTracePoint{start, p.bias, p.valid});
        } else {
            out.push_back(p);
        }
    }
    return out;
}

} // namespace helix
