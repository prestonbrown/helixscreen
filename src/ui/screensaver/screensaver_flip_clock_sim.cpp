// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_flip_clock_sim.h"

#include "screensaver_fast_math.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

// ---------- Faces ----------

/// Appends a group of `width` cards showing `value`, a blank card for each missing leading
/// digit unless `leading_zero`.
void add_group(FlipFace& face, int32_t value, uint8_t width, bool leading_zero) {
    const uint8_t first = face.shape.card_count();
    face.shape.groups[face.shape.group_count++] = width;
    for (int32_t i = width - 1; i >= 0; i--) {
        const bool blank = i < width - 1 && value == 0 && !leading_zero;
        face.cards[first + i].digit = blank ? '\0' : static_cast<char>('0' + value % 10);
        value /= 10;
    }
}

// ---------- Layout ----------
//
// Sizes in glyph units: a card holds one digit box.

/// The cards' bold digits.
constexpr float kGlyphStroke = 0.2f;
constexpr float kGlyphW = 1.0f + kGlyphStroke;
constexpr float kGlyphH = kStrokeDigitPathH + kGlyphStroke;

constexpr float kCardPadX = 0.32f;
constexpr float kCardPadY = 0.45f;
/// Between the cards of one group.
constexpr float kCardGap = 0.12f;
/// Between groups in a row, where the colon sits.
constexpr float kGroupGap = 0.6f;
/// Between rows when the groups stack.
constexpr float kRowGap = 0.3f;
constexpr float kColonR = kGlyphStroke * 0.55f;
constexpr float kCaptionGap = 0.2f;
constexpr float kCaptionH = 0.45f;
/// Share of the screen the layout fills along its tighter axis; the rest is drift room.
constexpr float FILL = 0.9f;
constexpr int32_t kMinCaptionH = 16;

struct Metrics {
    int32_t glyph_w;
    int32_t glyph_h;
    int32_t pad_x;
    int32_t pad_y;
    int32_t card_w;
    int32_t card_h;
    int32_t card_gap;
    int32_t group_gap;
    int32_t row_gap;
    int32_t caption_gap;
    int32_t caption_h;
};

Metrics metrics_for(int32_t unit) {
    const auto px = [unit](float v) { return static_cast<int32_t>(std::lround(v * unit)); };
    Metrics m{};
    m.glyph_w = px(kGlyphW);
    m.glyph_h = px(kGlyphH);
    m.pad_x = px(kCardPadX);
    m.pad_y = px(kCardPadY);
    m.card_w = m.glyph_w + 2 * m.pad_x;
    m.card_h = m.glyph_h + 2 * m.pad_y;
    m.card_gap = std::max(1, px(kCardGap));
    m.group_gap = px(kGroupGap);
    m.row_gap = px(kRowGap);
    m.caption_gap = px(kCaptionGap);
    m.caption_h = std::max(kMinCaptionH, px(kCaptionH));
    return m;
}

struct Arrangement {
    int32_t unit;
    bool column; ///< one group per row, instead of every group in one row
    int32_t content_w;
    int32_t content_h;
};

/// Width of a group of `cards` cards, in whatever unit `card_w` and `gap` share.
template <typename T> T group_width(uint8_t cards, T card_w, T gap) {
    return cards == 0 ? T{0} : static_cast<T>(cards) * card_w + static_cast<T>(cards - 1) * gap;
}

uint8_t widest_group(const FlipShape& shape) {
    uint8_t widest = 0;
    for (size_t g = 0; g < shape.group_count; g++) {
        widest = std::max(widest, shape.groups[g]);
    }
    return widest;
}

Arrangement arrange(int32_t w, int32_t h, const FlipShape& shape) {
    const auto groups = static_cast<float>(std::max<uint8_t>(1, shape.group_count));
    const float card_w = kGlyphW + 2 * kCardPadX;
    const float card_h = kGlyphH + 2 * kCardPadY;
    const float tail = shape.has_caption() ? kCaptionGap + kCaptionH : 0.0f;
    const float row_w =
        group_width(shape.card_count(), card_w, kCardGap) + (groups - 1) * (kGroupGap - kCardGap);
    const float row = std::min(FILL * static_cast<float>(w) / row_w,
                               FILL * static_cast<float>(h) / (card_h + tail));
    const float col =
        std::min(FILL * static_cast<float>(w) / group_width(widest_group(shape), card_w, kCardGap),
                 FILL * static_cast<float>(h) / (groups * card_h + (groups - 1) * kRowGap + tail));
    Arrangement a{};
    a.column = shape.group_count > 1 && col > row;
    a.unit = std::max(1, static_cast<int32_t>(std::floor(a.column ? col : row)));
    const Metrics m = metrics_for(a.unit);
    const int32_t tail_px = shape.has_caption() ? m.caption_gap + m.caption_h : 0;
    const int32_t n = std::max<int32_t>(1, shape.group_count);
    if (a.column) {
        a.content_w = group_width(widest_group(shape), m.card_w, m.card_gap);
        a.content_h = n * m.card_h + (n - 1) * m.row_gap + tail_px;
    } else {
        a.content_w = group_width(shape.card_count(), m.card_w, m.card_gap) +
                      (n - 1) * (m.group_gap - m.card_gap);
        a.content_h = m.card_h + tail_px;
    }
    return a;
}

} // namespace

uint8_t FlipShape::card_count() const {
    uint8_t count = 0;
    for (size_t g = 0; g < group_count; g++) {
        count = static_cast<uint8_t>(count + groups[g]);
    }
    return count;
}

namespace {

FlipFace clock_face(const ClockTime& t, bool use_24h) {
    FlipFace face;
    add_group(face, t.hour, 2, use_24h);
    face.meridiem = t.meridiem;
    add_group(face, t.minute, 2, true);
    return face;
}

} // namespace

FlipFace clock_face(int32_t seconds_of_day, bool use_24h) {
    return clock_face(clock_time(seconds_of_day, use_24h), use_24h);
}

FlipFace countdown_face(int32_t seconds) {
    const int32_t s = std::clamp(seconds, 0, kClockMaxCountdownS);
    const int32_t hours = s / 3600;
    FlipFace face;
    add_group(face, hours, hours >= 100 ? 3 : (hours >= 10 ? 2 : 1), false);
    add_group(face, s / 60 % 60, 2, true);
    add_group(face, s % 60, 2, true);
    return face;
}

// ---------- FlipClockState ----------

FlipFace flip_face(const ClockReading& reading) {
    switch (reading.mode) {
    case ClockMode::CLOCK:
        break;
    case ClockMode::COUNTDOWN: {
        FlipFace face = countdown_face(reading.countdown_s);
        face.caption = FlipCaption::DONE_AT;
        face.eta_minute_of_day = reading.eta_minute_of_day;
        face.progress_pct = reading.progress_pct;
        return face;
    }
    case ClockMode::PAUSED: {
        FlipFace face = countdown_face(reading.countdown_s);
        face.caption = FlipCaption::PAUSED;
        return face;
    }
    case ClockMode::DONE: {
        FlipFace face = countdown_face(0);
        face.caption = FlipCaption::DONE;
        return face;
    }
    }
    return clock_face(reading.time, reading.use_24h);
}

void FlipClockState::reset() {
    *this = FlipClockState{};
}

void FlipClockState::step(uint32_t dt_ms, const ClockInput& in) {
    state_.step(dt_ms, in);
    face_ = flip_face(state_.reading());
}

// ---------- Layout ----------

DirtyRect FlipLayout::bounds() const {
    DirtyRect box;
    for (size_t i = 0; i < card_count; i++) {
        box.add(cards[i]);
    }
    box.add(caption);
    return box;
}

FlipLayout layout_flip_cards(int32_t w, int32_t h, const FlipShape& shape, int32_t dx, int32_t dy) {
    FlipShape fit;
    for (size_t g = 0; g < shape.group_count; g++) {
        const auto room = static_cast<uint8_t>(kFlipMaxCards - fit.card_count());
        const uint8_t cards = std::min(shape.groups[g], room);
        if (cards > 0) {
            fit.groups[fit.group_count++] = cards;
        }
    }
    if (fit.group_count == 0) {
        fit.groups[fit.group_count++] = 1;
    }
    const Arrangement a = arrange(w, h, fit);
    const Metrics m = metrics_for(a.unit);
    FlipLayout layout;
    layout.unit = a.unit;
    layout.shape = fit;
    layout.card_count = fit.card_count();
    const int32_t x0 = (w - a.content_w) / 2 + dx;
    const int32_t y0 = (h - a.content_h) / 2 + dy;
    size_t i = 0;
    int32_t gx = x0;
    for (size_t g = 0; g < fit.group_count; g++) {
        const int32_t width = group_width(fit.groups[g], m.card_w, m.card_gap);
        if (a.column) {
            gx = x0 + (a.content_w - width) / 2;
        } else if (g > 0) {
            const int32_t colon_x = gx - m.group_gap;
            layout.colons[layout.colon_count++] = {colon_x, y0, colon_x + m.group_gap - 1,
                                                   y0 + m.card_h - 1};
        }
        const int32_t gy = a.column ? y0 + static_cast<int32_t>(g) * (m.card_h + m.row_gap) : y0;
        for (size_t c = 0; c < fit.groups[g]; c++, i++) {
            const int32_t cx = gx + static_cast<int32_t>(c) * (m.card_w + m.card_gap);
            layout.cards[i] = {cx, gy, cx + m.card_w - 1, gy + m.card_h - 1};
        }
        gx += width + m.group_gap;
    }
    const int32_t cards_bottom = layout.cards[layout.card_count - 1].y2;
    if (fit.has_caption()) {
        layout.caption = {x0, cards_bottom + 1 + m.caption_gap, x0 + a.content_w - 1,
                          cards_bottom + m.caption_gap + m.caption_h};
    }

    const int32_t split = std::max(2, static_cast<int32_t>(std::lround(0.04f * a.unit)));
    layout.split_top = (m.card_h - split) / 2;
    layout.split_bottom = layout.split_top + split;
    layout.corner = std::max(3, static_cast<int32_t>(std::lround(0.14f * a.unit)));

    const DirtyRect& first = layout.cards[0];
    const int32_t mx = first.x1 + layout.corner;
    const int32_t my = first.y1 + std::max(1, static_cast<int32_t>(std::lround(0.06f * a.unit)));
    layout.meridiem = {mx, my, mx + static_cast<int32_t>(std::lround(0.9f * a.unit)) - 1,
                       my + std::max(8, static_cast<int32_t>(std::lround(0.34f * a.unit))) - 1};
    layout.meridiem.y2 = std::min(layout.meridiem.y2, first.y1 + m.pad_y - 1);
    return layout;
}

DirtyRect flip_centred_bounds(int32_t w, int32_t h, const FlipShape& shape) {
    const Arrangement a = arrange(w, h, shape);
    const int32_t x1 = (w - a.content_w) / 2;
    const int32_t y1 = (h - a.content_h) / 2;
    return {x1, y1, x1 + a.content_w - 1, y1 + a.content_h - 1};
}

// ---------- FlipClockSim ----------

void FlipClockSim::init(FrameTarget& frame, size_t level, const FlipFace& face) {
    level_ = std::min(level, kFlipClockLevelCount - 1);
    frame_w_ = static_cast<int32_t>(frame.w);
    frame_h_ = static_cast<int32_t>(frame.h);
    drift_.reset();
    drift_shape_ = face.shape;
    drift_.set_range(flip_centred_bounds(frame_w_, frame_h_, drift_shape_), frame_w_, frame_h_);
    PixelWriter(frame).fill(kBackground);
    layout_ = layout_flip_cards(frame_w_, frame_h_, face.shape, 0, 0);
    build_glyphs();
    for (size_t i = 0; i < kFlipMaxCards; i++) {
        anims_[i] = {face.cards[i], face.cards[i], kFlipDurationMs};
    }
    for (size_t i = 0; i < layout_.card_count; i++) {
        draw_card(frame, i);
    }
    draw_colons(frame);
}

void FlipClockSim::redraw_card(FrameTarget& frame, size_t i, std::vector<DirtyRect>& dirty) const {
    if (i >= layout_.card_count) {
        return;
    }
    draw_card(frame, i);
    dirty.push_back(layout_.cards[i]);
}

void FlipClockSim::request_level(size_t level) {
    level_ = std::min(level, kFlipClockLevelCount - 1);
}

bool FlipClockSim::flipping() const {
    for (size_t i = 0; i < layout_.card_count; i++) {
        if (anims_[i].elapsed_ms < kFlipDurationMs) {
            return true;
        }
    }
    return false;
}

float FlipClockSim::flip_progress(size_t i) const {
    if (i >= kFlipMaxCards) {
        return 1.0f;
    }
    return static_cast<float>(std::min(anims_[i].elapsed_ms, kFlipDurationMs)) /
           static_cast<float>(kFlipDurationMs);
}

size_t FlipClockSim::heap_bytes() const {
    return glyphs_.heap_bytes();
}

void FlipClockSim::clear() {
    glyphs_.clear();
    layout_ = {};
}

void FlipClockSim::step(uint32_t dt_ms, FrameTarget& frame, const FlipFace& face,
                        std::vector<DirtyRect>& dirty) {
    dirty.clear();
    const bool animate = kFlipClockLevels[level_].animate;

    // The drift's range follows the shape being drawn.
    if (face.shape != drift_shape_) {
        drift_shape_ = face.shape;
        drift_.set_range(flip_centred_bounds(frame_w_, frame_h_, drift_shape_), frame_w_, frame_h_);
    }
    const bool moved = drift_.step(dt_ms);

    if (face.shape != layout_.shape || moved) {
        for (size_t i = 0; i < kFlipMaxCards; i++) {
            const bool same =
                face.shape == layout_.shape && animate && anims_[i].to == face.cards[i];
            if (same) {
                anims_[i].elapsed_ms = std::min(anims_[i].elapsed_ms + dt_ms, kFlipDurationMs);
            } else {
                anims_[i] = {face.cards[i], face.cards[i], kFlipDurationMs};
            }
        }
        relayout(frame, face.shape, dirty);
        return;
    }

    for (size_t i = 0; i < layout_.card_count; i++) {
        CardAnim& anim = anims_[i];
        bool redraw = false;
        if (face.cards[i] != anim.to) {
            anim.from = anim.to;
            anim.to = face.cards[i];
            anim.elapsed_ms = animate ? 0 : kFlipDurationMs;
            redraw = true;
        } else if (anim.elapsed_ms < kFlipDurationMs) {
            anim.elapsed_ms =
                animate ? std::min(anim.elapsed_ms + dt_ms, kFlipDurationMs) : kFlipDurationMs;
            redraw = true;
        }
        if (redraw) {
            draw_card(frame, i);
            dirty.push_back(layout_.cards[i]);
        }
    }
}

void FlipClockSim::relayout(FrameTarget& frame, const FlipShape& shape,
                            std::vector<DirtyRect>& dirty) {
    DirtyRect area = layout_.bounds();
    PixelWriter(frame).fill_rect(area, kBackground);
    const int32_t old_unit = layout_.unit;
    layout_ = layout_flip_cards(frame_w_, frame_h_, shape, drift_.dx, drift_.dy);
    if (layout_.unit != old_unit || glyphs_.coverage.empty()) {
        build_glyphs();
    }
    for (size_t i = 0; i < layout_.card_count; i++) {
        draw_card(frame, i);
    }
    draw_colons(frame);
    area.add(layout_.bounds());
    area = area.clipped(frame_w_, frame_h_);
    if (!area.empty()) {
        dirty.push_back(area);
    }
}

void FlipClockSim::build_glyphs() {
    build_stroke_digits(layout_.unit, kGlyphStroke, glyphs_);
}

uint8_t FlipClockSim::corner_coverage(int32_t x, int32_t y) const {
    const DirtyRect& card = layout_.cards[0];
    const int32_t w = card.x2 - card.x1 + 1;
    const int32_t h = card.y2 - card.y1 + 1;
    const int32_t r = layout_.corner;
    const int32_t cx = x < r ? r : (x >= w - r ? w - r : x);
    const int32_t cy = y < r ? r : (y >= h - r ? h - r : y);
    if (cx == x && cy == y) {
        return 255;
    }
    const float dist = std::hypot(static_cast<float>(x) + 0.5f - static_cast<float>(cx),
                                  static_cast<float>(y) + 0.5f - static_cast<float>(cy));
    const float cov = edge_coverage(static_cast<float>(r) - dist);
    return static_cast<uint8_t>(std::lround(cov * 255.0f));
}

Rgb FlipClockSim::face_pixel(const FlipCard& card, int32_t x, int32_t y) const {
    if (y >= layout_.split_top && y < layout_.split_bottom) {
        return kBackground;
    }
    const Rgb base = y < layout_.split_top ? kCardTop : kCardBottom;
    if (card.digit == '\0') {
        return base;
    }
    const Metrics m = metrics_for(layout_.unit);
    const uint8_t cov = glyphs_.at(card.digit - '0', x - m.pad_x, y - m.pad_y);
    return cov == 0 ? base : blend_cov(base, kDigit, cov);
}

void FlipClockSim::draw_colons(FrameTarget& frame) const {
    PixelWriter writer(frame);
    const float r = kColonR * static_cast<float>(layout_.unit);
    for (size_t c = 0; c < layout_.colon_count; c++) {
        const DirtyRect& box = layout_.colons[c];
        const float cx = static_cast<float>(box.x1 + box.x2 + 1) / 2.0f;
        const float h = static_cast<float>(box.y2 - box.y1 + 1);
        for (const float share : {0.36f, 0.64f}) {
            const float cy = static_cast<float>(box.y1) + h * share;
            const auto y1 = static_cast<int32_t>(std::floor(cy - r - 1.0f));
            const auto y2 = static_cast<int32_t>(std::ceil(cy + r + 1.0f));
            const auto x1 = static_cast<int32_t>(std::floor(cx - r - 1.0f));
            const auto x2 = static_cast<int32_t>(std::ceil(cx + r + 1.0f));
            for (int32_t y = std::max(y1, box.y1); y <= std::min(y2, box.y2); y++) {
                for (int32_t x = std::max(x1, box.x1); x <= std::min(x2, box.x2); x++) {
                    const float dist = std::hypot(static_cast<float>(x) + 0.5f - cx,
                                                  static_cast<float>(y) + 0.5f - cy);
                    const float cov = edge_coverage(r - dist);
                    if (cov > 0.0f && writer.contains(x, y)) {
                        writer.put(x, y,
                                   blend_cov(kBackground, kColon,
                                             static_cast<uint32_t>(std::lround(cov * 255.0f))));
                    }
                }
            }
        }
    }
}

void FlipClockSim::draw_card(FrameTarget& frame, size_t i) const {
    PixelWriter writer(frame);
    const DirtyRect& box = layout_.cards[i];
    const CardAnim& anim = anims_[i];
    const int32_t w = box.x2 - box.x1 + 1;
    const int32_t h = box.y2 - box.y1 + 1;
    const int32_t top_h = layout_.split_top;
    const int32_t bottom_h = h - layout_.split_bottom;
    const float p = static_cast<float>(std::min(anim.elapsed_ms, kFlipDurationMs)) /
                    static_cast<float>(kFlipDurationMs);
    const bool moving = anim.elapsed_ms < kFlipDurationMs;

    // The top half folds down: first the old digits' top half shrinks onto the split line,
    // then the new digits' bottom half unfolds below it. The projected height of the flap is
    // its full height times |cos| of the fold angle.
    const float fold = std::cos(kSaverPi * p);
    int32_t flap_y1 = 0;
    int32_t flap_y2 = -1;
    if (moving && p < 0.5f) {
        const int32_t fh = static_cast<int32_t>(std::lround(top_h * fold));
        flap_y1 = top_h - fh;
        flap_y2 = top_h - 1;
    } else if (moving) {
        const int32_t fh = static_cast<int32_t>(std::lround(bottom_h * -fold));
        flap_y1 = layout_.split_bottom;
        flap_y2 = layout_.split_bottom + fh - 1;
    }
    const int32_t flap_h = flap_y2 - flap_y1 + 1;
    const auto flap_shade =
        static_cast<uint32_t>(std::lround(255.0f * (0.55f + 0.45f * std::fabs(fold))));

    for (int32_t y = 0; y < h; y++) {
        const bool in_flap = flap_h > 0 && y >= flap_y1 && y <= flap_y2;
        int32_t src_y = 0;
        if (in_flap) {
            src_y = p < 0.5f ? (y - flap_y1) * top_h / flap_h
                             : layout_.split_bottom + (y - flap_y1) * bottom_h / flap_h;
        }
        // Behind the flap the top half already shows the new digits and the bottom half
        // still the old ones.
        const FlipCard& still = (moving && y >= layout_.split_top) ? anim.from : anim.to;
        const FlipCard& flap_card = p < 0.5f ? anim.from : anim.to;
        for (int32_t x = 0; x < w; x++) {
            Rgb c = blend_cov(kBackground, face_pixel(still, x, y), corner_coverage(x, y));
            if (in_flap) {
                const Rgb flap = scale_rgb(face_pixel(flap_card, x, src_y), flap_shade);
                c = blend_cov(c, flap, corner_coverage(x, src_y));
            }
            writer.put(box.x1 + x, box.y1 + y, c);
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
