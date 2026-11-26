// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_analog_clock_sim.h"

#include "screensaver_analog_faces.h"
#include "screensaver_buffer.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace helix::ui {

namespace {

/// Alignment of the dial's first pixel, so LVGL can draw text into it.
constexpr size_t kDialAlign = 16;
/// Texts any face places at most.
constexpr size_t kMaxTexts = 32;
/// Screens whose short side is this or less leave out the subdial captions.
constexpr int32_t kCompactSide = 390;

/// Copies the dial's pixels inside `box` (clipped) into `frame`, which has its size and format.
void copy_box(const FrameTarget& dial, FrameTarget& frame, const DirtyRect& box) {
    const DirtyRect in = box.clipped(static_cast<int32_t>(frame.w), static_cast<int32_t>(frame.h));
    if (in.empty()) {
        return;
    }
    const size_t bpp = pixel_bytes(frame.format);
    const size_t offset = static_cast<size_t>(in.x1) * bpp;
    const size_t len = static_cast<size_t>(in.x2 - in.x1 + 1) * bpp;
    for (int32_t y = in.y1; y <= in.y2; y++) {
        std::memcpy(frame.data + static_cast<size_t>(y) * frame.stride + offset,
                    dial.data + static_cast<size_t>(y) * dial.stride + offset, len);
    }
}

} // namespace

int32_t analog_countdown_minutes(int32_t countdown_s) {
    return (std::max(0, countdown_s) + 59) / 60;
}

AnalogMotion analog_motion(AnalogStyle style, bool smooth) {
    if (!smooth) {
        return AnalogMotion::TICK;
    }
    switch (style) {
    case AnalogStyle::STATION:
        return AnalogMotion::SPRUNG;
    case AnalogStyle::BAUHAUS:
        return AnalogMotion::TICK;
    case AnalogStyle::CHRONOGRAPH:
    case AnalogStyle::NEON:
    case AnalogStyle::ORRERY:
        break;
    }
    return AnalogMotion::SWEEP;
}

float analog_spring(uint32_t t_ms) {
    if (t_ms >= kAnalogSpringMs) {
        return 1.0f;
    }
    const float t = static_cast<float>(t_ms) / 1000.0f;
    return 1.0f - std::exp(-18.0f * t) * std::cos(45.0f * t);
}

AnalogHands analog_hands(const ClockTime& time, uint32_t sub_ms, AnalogMotion motion) {
    const auto s = static_cast<float>(time.second);
    const auto m = static_cast<float>(time.minute);
    const uint32_t sub = std::min<uint32_t>(sub_ms, 999);
    AnalogHands h;
    switch (motion) {
    case AnalogMotion::SWEEP:
        h.second = (s + static_cast<float>(sub) / 1000.0f) * 6.0f;
        h.minute = (m + s / 60.0f) * 6.0f;
        break;
    case AnalogMotion::TICK:
        h.second = s * 6.0f;
        h.minute = (m + s / 60.0f) * 6.0f;
        break;
    case AnalogMotion::SPRUNG:
        h.second = (s - 1.0f + analog_spring(sub)) * 6.0f;
        h.minute = time.second == 0 ? (m - 1.0f + analog_spring(sub)) * 6.0f : m * 6.0f;
        break;
    }
    h.hour = static_cast<float>(time.hour_of_day % 12) * 30.0f + h.minute / 12.0f;
    return h;
}

AnalogGeom analog_layout(AnalogStyle style, int32_t w, int32_t h, bool side_column) {
    AnalogGeom g;
    g.w = w;
    g.h = h;
    const float side = static_cast<float>(std::min(w, h));
    g.compact = std::min(w, h) <= kCompactSide;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    if (style != AnalogStyle::BAUHAUS || !side_column) {
        // A margin either side is the room the face drifts in.
        g.cx = fw * 0.5f;
        g.cy = fh * 0.5f;
        g.r = side * 0.5f - std::max(6.0f, side * 0.02f);
        return g;
    }
    g.col_scale = std::max(0.55f, side / 480.0f);
    const float f = g.col_scale;
    if (fh >= fw * 1.3f) {
        g.r = std::min(fw * 0.5f - 30.0f * f, fh * 0.3f);
        g.cx = fw * 0.5f;
        g.cy = g.r + 40.0f * f;
        g.col_x = 40.0f * f;
        g.col_w = fw - 80.0f * f;
        g.col_y = g.cy + g.r + 40.0f * f;
    } else {
        g.cx = fw * 0.34f;
        g.r = std::min(fh * 0.5f - 24.0f * f, g.cx - 24.0f * f);
        g.cy = fh * 0.5f;
        g.col_x = g.cx + g.r + 0.07f * fw;
        g.col_w = fw - g.col_x - 0.05f * fw;
        g.col_y = fh * 0.5f - 90.0f * f;
    }
    return g;
}

DirtyRect analog_face_box(AnalogStyle style, const AnalogGeom& g) {
    DirtyRect box = apaint_box(g.cx, g.cy, g.cx, g.cy, g.r + 1.0f);
    if (style == AnalogStyle::BAUHAUS && g.col_w > 0.0f) {
        // The column's text and bar run about 190 px of the column's scale below its top.
        box.add(
            apaint_box(g.col_x, g.col_y, g.col_x + g.col_w, g.col_y + 190.0f * g.col_scale, 1.0f));
    }
    return box;
}

bool AnalogClockSim::DialKey::operator==(const DialKey& o) const {
    return style == o.style && mode == o.mode && progress_pct == o.progress_pct &&
           minute_of_day == o.minute_of_day && countdown_min == o.countdown_min &&
           eta_minute == o.eta_minute && has_countdown == o.has_countdown && use_24h == o.use_24h &&
           date.year == o.date.year && date.month == o.date.month && date.day == o.date.day &&
           date.weekday == o.date.weekday;
}

void AnalogClockSim::init(FrameTarget& frame, size_t level, const AnalogFace& face) {
    level_ = std::min(level, std::size(kLevels) - 1);
    dial_bytes_.clear();
    keyed_ = false;
    last_second_ = -1;
    sub_ms_ = 0;
    frozen_second_ = -1.0f;
    drift_.reset();
    drift_pending_ = false;
    prepare(0, frame, face);
    std::vector<DirtyRect> dirty;
    compose(frame, dirty);
}

void AnalogClockSim::step(uint32_t dt_ms, FrameTarget& frame, const AnalogFace& face,
                          std::vector<DirtyRect>& dirty) {
    prepare(dt_ms, frame, face);
    compose(frame, dirty);
}

void AnalogClockSim::relayout(const FrameTarget& frame, AnalogStyle style) {
    style_ = style;
    base_ = analog_layout(style, static_cast<int32_t>(frame.w), static_cast<int32_t>(frame.h),
                          options_.side_column);
    drift_.set_range(analog_face_box(style, base_), base_.w, base_.h);
    geom_ = base_;
    geom_.cx += static_cast<float>(drift_.dx);
    geom_.cy += static_cast<float>(drift_.dy);
    geom_.col_x += static_cast<float>(drift_.dx);
    geom_.col_y += static_cast<float>(drift_.dy);

    const size_t bytes = static_cast<size_t>(frame.stride) * frame.h;
    if (dial_bytes_.size() != bytes + kDialAlign) {
        std::vector<uint8_t>(bytes + kDialAlign).swap(dial_bytes_);
        texts_.reserve(kMaxTexts);
    }
    uint8_t* base = dial_bytes_.data();
    const auto misalign = reinterpret_cast<uintptr_t>(base) % kDialAlign;
    dial_ = {base + (misalign == 0 ? 0 : kDialAlign - misalign), frame.stride, frame.w, frame.h,
             frame.format};
    keyed_ = false;
}

AnalogClockSim::DialKey AnalogClockSim::key_for(const AnalogFace& face) const {
    const ClockReading& r = face.reading;
    DialKey k;
    k.style = face.style;
    k.mode = r.mode;
    k.progress_pct = r.progress_pct;
    k.minute_of_day = r.time.hour_of_day * 60 + r.time.minute;
    k.has_countdown = r.has_countdown;
    k.countdown_min = r.has_countdown ? analog_countdown_minutes(r.countdown_s) : 0;
    k.eta_minute = r.mode == ClockMode::COUNTDOWN ? r.eta_minute_of_day : 0;
    k.use_24h = r.use_24h;
    k.date = r.date;
    return k;
}

void AnalogClockSim::draw_dial(const AnalogFace& face) {
    APainter p(dial_,
               DirtyRect::whole(static_cast<int32_t>(dial_.w), static_cast<int32_t>(dial_.h)));
    analog_draw_dial(p, style_, geom_, face.reading, texts_);
}

AnalogHands AnalogClockSim::hands_for(const AnalogFace& face) const {
    if (!options_.seconds) {
        // The minute hand holds for the whole minute, so nothing moves between minutes.
        ClockTime minute = face.reading.time;
        minute.second = 0;
        return analog_hands(minute, 0, AnalogMotion::TICK);
    }
    return analog_hands(face.reading.time, sub_ms_,
                        analog_motion(face.style, kLevels[level_].smooth));
}

std::array<DirtyRect, 3> AnalogClockSim::boxes_for(const AnalogHands& hands) const {
    const std::array<AnalogPartSpec, 3> specs =
        analog_part_specs(style_, geom_, kLevels[level_].smooth);
    return {analog_part_box(specs[0], geom_, hands.hour),
            analog_part_box(specs[1], geom_, hands.minute),
            options_.seconds ? analog_part_box(specs[2], geom_, hands.second) : DirtyRect{}};
}

void AnalogClockSim::draw_hands(FrameTarget& frame, const DirtyRect& clip) const {
    APainter p(frame, clip);
    analog_draw_hands(p, style_, geom_, hands_, mode_, kLevels[level_].smooth, options_.seconds);
}

bool AnalogClockSim::prepare(uint32_t dt_ms, const FrameTarget& frame, const AnalogFace& face) {
    if (dial_bytes_.empty() || face.style != style_ || static_cast<int32_t>(frame.w) != base_.w ||
        static_cast<int32_t>(frame.h) != base_.h || frame.stride != dial_.stride ||
        frame.format != dial_.format) {
        drift_.reset();
        relayout(frame, face.style);
    }
    const ClockTime& t = face.reading.time;
    const int32_t second_of_day = t.hour_of_day * 3600 + t.minute * 60 + t.second;
    if (second_of_day != last_second_) {
        last_second_ = second_of_day;
        sub_ms_ = 0;
    } else {
        sub_ms_ = std::min<uint32_t>(sub_ms_ + dt_ms, 999);
    }
    // A drift step waits for the next dial redraw, at most a minute away, rather than
    // repainting the whole frame on its own.
    drift_pending_ = (options_.drift && drift_.step(dt_ms)) || drift_pending_;
    mode_ = face.reading.mode;

    AnalogHands next = hands_for(face);
    if (style_ == AnalogStyle::CHRONOGRAPH && mode_ == ClockMode::PAUSED) {
        // A paused chronograph stops its seconds hand where it stands.
        if (frozen_second_ < 0.0f) {
            frozen_second_ = std::fmod(next.second, 360.0f);
        }
        next.second = frozen_second_;
    } else {
        frozen_second_ = -1.0f;
    }

    const DialKey key = key_for(face);
    const bool redraw = !keyed_ || !(key == key_);
    if (redraw) {
        if (drift_pending_) {
            drift_pending_ = false;
            relayout(frame, style_);
        }
        draw_dial(face);
        key_ = key;
        keyed_ = true;
        repaint_ = true;
    }
    pending_ = next;
    return redraw;
}

void AnalogClockSim::compose(FrameTarget& frame, std::vector<DirtyRect>& dirty) {
    dirty.clear();
    const AnalogHands next = pending_;
    const std::array<DirtyRect, 3> next_boxes = boxes_for(next);
    const DirtyRect whole =
        DirtyRect::whole(static_cast<int32_t>(frame.w), static_cast<int32_t>(frame.h));
    if (repaint_) {
        hands_ = next;
        copy_box(dial_, frame, whole);
        draw_hands(frame, whole);
        dirty.push_back(whole);
        boxes_ = next_boxes;
        repaint_ = false;
        return;
    }

    const std::array<float, 3> was{hands_.hour, hands_.minute, hands_.second};
    const std::array<float, 3> now{next.hour, next.minute, next.second};
    for (size_t i = 0; i < 3; i++) {
        if (was[i] == now[i]) {
            continue;
        }
        DirtyRect box = boxes_[i];
        box.add(next_boxes[i]);
        box = box.clipped(static_cast<int32_t>(frame.w), static_cast<int32_t>(frame.h));
        if (!box.empty()) {
            dirty.push_back(box);
        }
    }
    hands_ = next;
    boxes_ = next_boxes;
    // Each box restores the dial and draws every hand clipped to it, so where boxes overlap
    // the later one leaves the same pixels. Joining them would cover the whole face whenever
    // two hands move, as every hand's box holds the centre.
    for (const DirtyRect& box : dirty) {
        copy_box(dial_, frame, box);
        draw_hands(frame, box);
    }
}

void AnalogClockSim::request_level(size_t level) {
    const size_t clamped = std::min(level, std::size(kLevels) - 1);
    if (clamped != level_) {
        level_ = clamped;
        repaint_ = true;
    }
}

void AnalogClockSim::clear() {
    release_buffer(dial_bytes_);
    release_buffer(texts_);
    dial_ = FrameTarget{};
    base_ = AnalogGeom{};
    geom_ = AnalogGeom{};
    drift_.reset();
    drift_pending_ = false;
    keyed_ = false;
    repaint_ = true;
    last_second_ = -1;
    sub_ms_ = 0;
    frozen_second_ = -1.0f;
    hands_ = AnalogHands{};
    pending_ = AnalogHands{};
    boxes_ = {};
}

size_t AnalogClockSim::heap_bytes() const {
    return buffer_bytes(dial_bytes_) + buffer_bytes(texts_);
}

} // namespace helix::ui
