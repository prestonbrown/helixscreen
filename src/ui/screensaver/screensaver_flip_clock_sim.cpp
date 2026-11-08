// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_flip_clock_sim.h"

#include "screensaver_fast_math.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_print_colors.h"

#include <algorithm>
#include <cmath>
#include <string>

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

/// Appends a group of one card per character of `text` to `shape` and `cards`.
template <size_t N>
void add_text_group(FlipShape& shape, std::array<FlipCard, N>& cards, const std::string& text) {
    uint8_t first = shape.card_count();
    if (shape.group_count >= kFlipMaxGroups || text.empty() || first + text.size() > N) {
        return;
    }
    shape.groups[shape.group_count++] = static_cast<uint8_t>(text.size());
    for (const char c : text) {
        cards[first++].digit = c;
    }
}

/// `pct` as a percent group: "63%".
std::string percent_text(int32_t pct) {
    return std::to_string(std::clamp(pct, 0, 100)) + "%";
}

/// Adds a clock time as two groups: the hour (24-hour with a leading zero, 12-hour without)
/// and the minute.
void add_sub_time(FlipFace& face, const ClockTime& t, bool use_24h) {
    std::string hour = std::to_string(t.hour);
    if (use_24h && hour.size() < 2) {
        hour = "0" + hour;
    }
    add_text_group(face.sub_shape, face.sub, hour);
    add_text_group(face.sub_shape, face.sub, (t.minute < 10 ? "0" : "") + std::to_string(t.minute));
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
/// Between the cards and the small row.
constexpr float kSubGap = 0.25f;
/// Share of the screen the layout fills along its tighter axis; the rest is drift room.
constexpr float FILL = 0.9f;

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
    int32_t sub_gap;
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
    m.sub_gap = px(kSubGap);
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

/// Width of a row of `shape`'s groups, in the unit `card_w`, `gap` and `group_gap` share.
float row_width(const FlipShape& shape, float card_w, float gap, float group_gap) {
    const auto groups = static_cast<float>(std::max<uint8_t>(1, shape.group_count));
    return group_width(shape.card_count(), card_w, gap) + (groups - 1) * (group_gap - gap);
}

Arrangement arrange(int32_t w, int32_t h, const FlipShape& shape, const FlipShape& sub) {
    const auto groups = static_cast<float>(std::max<uint8_t>(1, shape.group_count));
    const float card_w = kGlyphW + 2 * kCardPadX;
    const float card_h = kGlyphH + 2 * kCardPadY;
    const bool has_sub = sub.group_count > 0;
    // The small row in main units: a scaled copy of the cards.
    const float sub_w = has_sub ? kSubCardScale * row_width(sub, card_w, kCardGap, kGroupGap) : 0;
    const float tail = has_sub ? kSubGap + kSubCardScale * card_h : 0.0f;
    const float row_w = std::max(row_width(shape, card_w, kCardGap, kGroupGap), sub_w);
    const float row = std::min(FILL * static_cast<float>(w) / row_w,
                               FILL * static_cast<float>(h) / (card_h + tail));
    const float col_w = std::max(group_width(widest_group(shape), card_w, kCardGap), sub_w);
    const float col =
        std::min(FILL * static_cast<float>(w) / col_w,
                 FILL * static_cast<float>(h) / (groups * card_h + (groups - 1) * kRowGap + tail));
    Arrangement a{};
    a.column = shape.group_count > 1 && col > row;
    a.unit = std::max(1, static_cast<int32_t>(std::floor(a.column ? col : row)));
    const Metrics m = metrics_for(a.unit);
    const int32_t n = std::max<int32_t>(1, shape.group_count);
    int32_t sub_w_px = 0;
    int32_t tail_px = 0;
    if (has_sub) {
        const Metrics sm =
            metrics_for(std::max(1, static_cast<int32_t>(std::lround(kSubCardScale * a.unit))));
        const int32_t sn = sub.group_count;
        sub_w_px = group_width(sub.card_count(), sm.card_w, sm.card_gap) +
                   (sn - 1) * (sm.group_gap - sm.card_gap);
        tail_px = m.sub_gap + sm.card_h;
    }
    if (a.column) {
        a.content_w = std::max(group_width(widest_group(shape), m.card_w, m.card_gap), sub_w_px);
        a.content_h = n * m.card_h + (n - 1) * m.row_gap + tail_px;
    } else {
        a.content_w = std::max(group_width(shape.card_count(), m.card_w, m.card_gap) +
                                   (n - 1) * (m.group_gap - m.card_gap),
                               sub_w_px);
        a.content_h = m.card_h + tail_px;
    }
    return a;
}

/// The split line and corner of a row of cards at `unit`.
FlipRowMetrics row_metrics(int32_t unit) {
    const Metrics m = metrics_for(unit);
    FlipRowMetrics r;
    r.unit = unit;
    r.pad_x = m.pad_x;
    r.pad_y = m.pad_y;
    const int32_t split = std::max(2, static_cast<int32_t>(std::lround(0.04f * unit)));
    r.split_top = (m.card_h - split) / 2;
    r.split_bottom = r.split_top + split;
    r.corner = std::max(3, static_cast<int32_t>(std::lround(0.14f * unit)));
    return r;
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
    FlipFace face;
    switch (reading.mode) {
    case ClockMode::CLOCK:
        return clock_face(reading.time, reading.use_24h);
    case ClockMode::PROGRESS:
        add_text_group(face.shape, face.cards, percent_text(reading.progress_pct));
        add_sub_time(face, reading.time, reading.use_24h);
        face.sub_colons = 1;
        break;
    case ClockMode::COUNTDOWN: {
        face = countdown_face(reading.countdown_s);
        add_sub_time(face, clock_time(reading.eta_minute_of_day * 60, reading.use_24h),
                     reading.use_24h);
        add_text_group(face.sub_shape, face.sub, percent_text(reading.progress_pct));
        face.sub_colons = 1;
        break;
    }
    case ClockMode::PAUSED:
        add_text_group(face.shape, face.cards, "PAUSE");
        if (reading.has_countdown) {
            const int32_t s = std::max(0, reading.countdown_s);
            const int32_t hours = std::min(99, s / 3600);
            add_text_group(face.sub_shape, face.sub, std::to_string(hours));
            add_text_group(face.sub_shape, face.sub,
                           (s / 60 % 60 < 10 ? "0" : "") + std::to_string(s / 60 % 60));
            face.sub_colons = 1;
        }
        add_text_group(face.sub_shape, face.sub, percent_text(reading.progress_pct));
        break;
    case ClockMode::DONE:
    case ClockMode::FAILED:
    case ClockMode::CANCELLED:
        add_text_group(face.shape, face.cards,
                       reading.mode == ClockMode::DONE     ? "DONE"
                       : reading.mode == ClockMode::FAILED ? "FAIL"
                                                           : "STOP");
        add_text_group(face.sub_shape, face.sub, percent_text(reading.progress_pct));
        break;
    }
    face.mode = reading.mode;
    return face;
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
    for (size_t i = 0; i < static_cast<size_t>(card_count + sub_count); i++) {
        box.add(cards[i]);
    }
    return box;
}

namespace {

/// `shape` cut to at most `max_cards` cards.
FlipShape fit_shape(const FlipShape& shape, size_t max_cards) {
    FlipShape fit;
    for (size_t g = 0; g < shape.group_count; g++) {
        const auto room = static_cast<uint8_t>(max_cards - fit.card_count());
        const uint8_t cards = std::min(shape.groups[g], room);
        if (cards > 0) {
            fit.groups[fit.group_count++] = cards;
        }
    }
    return fit;
}

} // namespace

FlipLayout layout_flip_cards(int32_t w, int32_t h, const FlipShape& shape,
                             const FlipShape& sub_shape, int32_t dx, int32_t dy,
                             uint8_t sub_colons) {
    FlipShape fit = fit_shape(shape, kFlipMaxCards);
    if (fit.group_count == 0) {
        fit.groups[fit.group_count++] = 1;
    }
    const FlipShape sub = fit_shape(sub_shape, kFlipMaxSubCards);
    const Arrangement a = arrange(w, h, fit, sub);
    const Metrics m = metrics_for(a.unit);
    FlipLayout layout;
    layout.unit = a.unit;
    layout.shape = fit;
    layout.sub_shape = sub;
    layout.sub_colons = sub_colons;
    layout.card_count = fit.card_count();
    const int32_t x0 = (w - a.content_w) / 2 + dx;
    const int32_t y0 = (h - a.content_h) / 2 + dy;
    size_t i = 0;
    const int32_t main_w = a.column ? group_width(widest_group(fit), m.card_w, m.card_gap)
                                    : group_width(fit.card_count(), m.card_w, m.card_gap) +
                                          (fit.group_count - 1) * (m.group_gap - m.card_gap);
    int32_t gx = x0 + (a.content_w - main_w) / 2;
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
    layout.main_colon_count = layout.colon_count;
    layout.main = row_metrics(a.unit);

    if (sub.group_count > 0) {
        const int32_t sub_unit =
            std::max(1, static_cast<int32_t>(std::lround(kSubCardScale * a.unit)));
        const Metrics sm = metrics_for(sub_unit);
        layout.small = row_metrics(sub_unit);
        layout.sub_count = sub.card_count();
        const int32_t sub_w = group_width(sub.card_count(), sm.card_w, sm.card_gap) +
                              (sub.group_count - 1) * (sm.group_gap - sm.card_gap);
        const int32_t sy = layout.cards[layout.card_count - 1].y2 + 1 + m.sub_gap;
        int32_t sx = x0 + (a.content_w - sub_w) / 2;
        for (size_t g = 0; g < sub.group_count; g++) {
            if (g > 0 && g <= sub_colons) {
                const int32_t colon_x = sx - sm.group_gap;
                layout.colons[layout.colon_count++] = {colon_x, sy, colon_x + sm.group_gap - 1,
                                                       sy + sm.card_h - 1};
            }
            for (size_t c = 0; c < sub.groups[g]; c++, i++) {
                const int32_t cx = sx + static_cast<int32_t>(c) * (sm.card_w + sm.card_gap);
                layout.cards[i] = {cx, sy, cx + sm.card_w - 1, sy + sm.card_h - 1};
            }
            sx += group_width(sub.groups[g], sm.card_w, sm.card_gap) + sm.group_gap;
        }
    }

    layout.split_top = layout.main.split_top;
    layout.split_bottom = layout.main.split_bottom;
    layout.corner = layout.main.corner;

    const DirtyRect& first = layout.cards[0];
    const int32_t mx = first.x1 + layout.corner;
    const int32_t my = first.y1 + std::max(1, static_cast<int32_t>(std::lround(0.06f * a.unit)));
    layout.meridiem = {mx, my, mx + static_cast<int32_t>(std::lround(0.9f * a.unit)) - 1,
                       my + std::max(8, static_cast<int32_t>(std::lround(0.34f * a.unit))) - 1};
    layout.meridiem.y2 = std::min(layout.meridiem.y2, first.y1 + m.pad_y - 1);
    return layout;
}

DirtyRect flip_centred_bounds(int32_t w, int32_t h, const FlipShape& shape,
                              const FlipShape& sub_shape) {
    const Arrangement a = arrange(w, h, shape, fit_shape(sub_shape, kFlipMaxSubCards));
    const int32_t x1 = (w - a.content_w) / 2;
    const int32_t y1 = (h - a.content_h) / 2;
    return {x1, y1, x1 + a.content_w - 1, y1 + a.content_h - 1};
}

// ---------- FlipClockSim ----------

namespace {

/// Card `i` of `face` in layout order: the main row, then the small row.
FlipCard face_card(const FlipFace& face, size_t i) {
    const size_t main = face.shape.card_count();
    if (i < main) {
        return face.cards[i];
    }
    return i - main < kFlipMaxSubCards ? face.sub[i - main] : FlipCard{};
}

/// True when `face` needs a different layout from the one drawn for `layout`.
bool shape_differs(const FlipFace& face, const FlipLayout& layout) {
    return face.shape != layout.shape || face.sub_shape != layout.sub_shape ||
           face.sub_colons != layout.sub_colons;
}

} // namespace

void FlipClockSim::init(FrameTarget& frame, size_t level, const FlipFace& face) {
    level_ = std::min(level, kFlipClockLevelCount - 1);
    frame_w_ = static_cast<int32_t>(frame.w);
    frame_h_ = static_cast<int32_t>(frame.h);
    drift_.reset();
    drift_shape_ = face.shape;
    drift_sub_shape_ = face.sub_shape;
    drift_.set_range(flip_centred_bounds(frame_w_, frame_h_, drift_shape_, drift_sub_shape_),
                     frame_w_, frame_h_);
    PixelWriter(frame).fill(kBackground);
    ink_ = print_state_color(face.mode, kDigit);
    layout_ =
        layout_flip_cards(frame_w_, frame_h_, face.shape, face.sub_shape, 0, 0, face.sub_colons);
    build_glyphs(face);
    for (size_t i = 0; i < anims_.size(); i++) {
        anims_[i] = {face_card(face, i), face_card(face, i), kFlipDurationMs};
    }
    for (size_t i = 0; i < static_cast<size_t>(layout_.card_count + layout_.sub_count); i++) {
        draw_card(frame, i);
    }
    draw_colons(frame);
}

void FlipClockSim::redraw_card(FrameTarget& frame, size_t i, std::vector<DirtyRect>& dirty) const {
    if (i >= static_cast<size_t>(layout_.card_count + layout_.sub_count)) {
        return;
    }
    draw_card(frame, i);
    dirty.push_back(layout_.cards[i]);
}

void FlipClockSim::request_level(size_t level) {
    level_ = std::min(level, kFlipClockLevelCount - 1);
}

bool FlipClockSim::flipping() const {
    for (size_t i = 0; i < static_cast<size_t>(layout_.card_count + layout_.sub_count); i++) {
        if (anims_[i].elapsed_ms < kFlipDurationMs) {
            return true;
        }
    }
    return false;
}

float FlipClockSim::flip_progress(size_t i) const {
    if (i >= anims_.size()) {
        return 1.0f;
    }
    return static_cast<float>(std::min(anims_[i].elapsed_ms, kFlipDurationMs)) /
           static_cast<float>(kFlipDurationMs);
}

size_t FlipClockSim::heap_bytes() const {
    return glyphs_.heap_bytes() + sub_glyphs_.heap_bytes();
}

void FlipClockSim::clear() {
    glyphs_.clear();
    sub_glyphs_.clear();
    layout_ = {};
}

void FlipClockSim::step(uint32_t dt_ms, FrameTarget& frame, const FlipFace& face,
                        std::vector<DirtyRect>& dirty) {
    dirty.clear();
    const bool animate = kFlipClockLevels[level_].animate;

    // The drift's range follows the shape being drawn.
    if (face.shape != drift_shape_ || face.sub_shape != drift_sub_shape_) {
        drift_shape_ = face.shape;
        drift_sub_shape_ = face.sub_shape;
        drift_.set_range(flip_centred_bounds(frame_w_, frame_h_, drift_shape_, drift_sub_shape_),
                         frame_w_, frame_h_);
    }
    const bool moved = drift_.step(dt_ms);
    const Rgb ink = print_state_color(face.mode, kDigit);

    if (shape_differs(face, layout_) || moved || !(ink == ink_)) {
        const bool same_shape = !shape_differs(face, layout_) && ink == ink_;
        for (size_t i = 0; i < anims_.size(); i++) {
            const FlipCard card = face_card(face, i);
            if (same_shape && animate && anims_[i].to == card) {
                anims_[i].elapsed_ms = std::min(anims_[i].elapsed_ms + dt_ms, kFlipDurationMs);
            } else {
                anims_[i] = {card, card, kFlipDurationMs};
            }
        }
        ink_ = ink;
        relayout(frame, face, dirty);
        return;
    }

    for (size_t i = 0; i < static_cast<size_t>(layout_.card_count + layout_.sub_count); i++) {
        CardAnim& anim = anims_[i];
        const FlipCard card = face_card(face, i);
        bool redraw = false;
        if (card != anim.to) {
            (i < layout_.card_count ? glyphs_ : sub_glyphs_).ensure(card.digit);
            anim.from = anim.to;
            anim.to = card;
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

void FlipClockSim::relayout(FrameTarget& frame, const FlipFace& face,
                            std::vector<DirtyRect>& dirty) {
    DirtyRect area = layout_.bounds();
    PixelWriter(frame).fill_rect(area, kBackground);
    layout_ = layout_flip_cards(frame_w_, frame_h_, face.shape, face.sub_shape, drift_.dx,
                                drift_.dy, face.sub_colons);
    build_glyphs(face);
    for (size_t i = 0; i < static_cast<size_t>(layout_.card_count + layout_.sub_count); i++) {
        draw_card(frame, i);
    }
    draw_colons(frame);
    area.add(layout_.bounds());
    area = area.clipped(frame_w_, frame_h_);
    if (!area.empty()) {
        dirty.push_back(area);
    }
}

void FlipClockSim::build_glyphs(const FlipFace& face) {
    // A new size rebuilds; the same size keeps what is built and adds what this face needs.
    if (glyphs_.w != metrics_for(layout_.main.unit).glyph_w) {
        glyphs_.init(layout_.main.unit, kGlyphStroke);
    }
    if (layout_.sub_count > 0 && sub_glyphs_.w != metrics_for(layout_.small.unit).glyph_w) {
        sub_glyphs_.init(layout_.small.unit, kGlyphStroke);
    }
    for (size_t i = 0; i < layout_.card_count; i++) {
        glyphs_.ensure(face.cards[i].digit);
    }
    for (size_t i = 0; i < layout_.sub_count; i++) {
        sub_glyphs_.ensure(face.sub[i].digit);
    }
}

uint8_t FlipClockSim::corner_coverage(size_t i, int32_t x, int32_t y) const {
    const DirtyRect& card = layout_.cards[i];
    const int32_t w = card.x2 - card.x1 + 1;
    const int32_t h = card.y2 - card.y1 + 1;
    const int32_t r = layout_.row(i).corner;
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

Rgb FlipClockSim::face_pixel(size_t i, const FlipCard& card, int32_t x, int32_t y) const {
    const FlipRowMetrics& row = layout_.row(i);
    if (y >= row.split_top && y < row.split_bottom) {
        return kBackground;
    }
    const Rgb base = y < row.split_top ? kCardTop : kCardBottom;
    if (card.digit == '\0') {
        return base;
    }
    const StrokeGlyphSet& glyphs = i < layout_.card_count ? glyphs_ : sub_glyphs_;
    const uint8_t cov = glyphs.at(card.digit, x - row.pad_x, y - row.pad_y);
    return cov == 0 ? base : blend_cov(base, ink_, cov);
}

void FlipClockSim::draw_colons(FrameTarget& frame) const {
    PixelWriter writer(frame);
    for (size_t c = 0; c < layout_.colon_count; c++) {
        const DirtyRect& box = layout_.colons[c];
        const int32_t unit = c < layout_.main_colon_count ? layout_.main.unit : layout_.small.unit;
        const float r = kColonR * static_cast<float>(unit);
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
    const FlipRowMetrics& row = layout_.row(i);
    const int32_t w = box.x2 - box.x1 + 1;
    const int32_t h = box.y2 - box.y1 + 1;
    const int32_t top_h = row.split_top;
    const int32_t bottom_h = h - row.split_bottom;
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
        flap_y1 = row.split_bottom;
        flap_y2 = row.split_bottom + fh - 1;
    }
    const int32_t flap_h = flap_y2 - flap_y1 + 1;
    const auto flap_shade =
        static_cast<uint32_t>(std::lround(255.0f * (0.55f + 0.45f * std::fabs(fold))));

    // The card's thickness shows as a lit band along the flap's free edge, widest when the
    // flap stands edge-on. It sits above the face on the way up and below it on the way down.
    const float tilt = moving ? std::sin(kSaverPi * p) : 0.0f;
    const int32_t edge_px = std::max<int32_t>(2, h / 72);
    const auto edge_h = static_cast<int32_t>(std::lround(static_cast<float>(edge_px) * tilt));
    int32_t edge_y1 = 0;
    int32_t edge_y2 = -1;
    if (edge_h > 0 && p < 0.5f) {
        edge_y1 = std::max<int32_t>(0, flap_y1 - edge_h);
        edge_y2 = flap_y1 - 1;
    } else if (edge_h > 0) {
        edge_y1 = flap_y2 + 1;
        edge_y2 = std::min(h - 1, flap_y2 + edge_h);
    }
    // The free edge's rounded ends follow the card's corners.
    const int32_t edge_corner_row = row.corner / 3;

    // The flap shades the half it is passing over, darkest beside its edge.
    const int32_t free_edge_y = p < 0.5f ? flap_y1 : flap_y2;
    const float shadow_reach = static_cast<float>(std::max<int32_t>(1, top_h));

    for (int32_t y = 0; y < h; y++) {
        const bool in_flap = flap_h > 0 && y >= flap_y1 && y <= flap_y2;
        const bool in_edge = y >= edge_y1 && y <= edge_y2;
        uint32_t shadow = 255;
        if (tilt > 0.0f && !in_flap && !in_edge &&
            (p < 0.5f ? y < row.split_top : y >= row.split_bottom)) {
            const float near =
                1.0f - std::min(1.0f, static_cast<float>(std::abs(y - free_edge_y)) / shadow_reach);
            shadow = static_cast<uint32_t>(std::lround(255.0f * (1.0f - 0.45f * tilt * near)));
        }
        // The outer row of the band catches the most light.
        const bool rim = in_edge && (p < 0.5f ? y == edge_y1 : y == edge_y2);
        const Rgb edge_color = rim ? kFlapRim : kFlapEdge;
        int32_t src_y = 0;
        if (in_flap) {
            src_y = p < 0.5f ? (y - flap_y1) * top_h / flap_h
                             : row.split_bottom + (y - flap_y1) * bottom_h / flap_h;
        }
        // Behind the flap the top half already shows the new digits and the bottom half
        // still the old ones.
        const FlipCard& still = (moving && y >= row.split_top) ? anim.from : anim.to;
        const FlipCard& flap_card = p < 0.5f ? anim.from : anim.to;
        for (int32_t x = 0; x < w; x++) {
            Rgb face = face_pixel(i, still, x, y);
            if (shadow < 255) {
                face = scale_rgb(face, shadow);
            }
            Rgb c = blend_cov(kBackground, face, corner_coverage(i, x, y));
            if (in_edge) {
                c = blend_cov(c, edge_color, corner_coverage(i, x, edge_corner_row));
            } else if (in_flap) {
                const Rgb flap = scale_rgb(face_pixel(i, flap_card, x, src_y), flap_shade);
                c = blend_cov(c, flap, corner_coverage(i, x, src_y));
            }
            writer.put(box.x1 + x, box.y1 + y, c);
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
