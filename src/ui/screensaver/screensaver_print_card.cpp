// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_print_card.h"

#include "screensaver_fast_math.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_print_colors.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

/// Share of the screen's short side the ring spans.
constexpr float kRingShare = 0.22f;
/// Margin, ring stroke and glyph height as shares of the ring diameter.
constexpr float kPadShare = 0.12f;
constexpr float kRingStrokeShare = 0.12f;
constexpr float kGlyphHeightShare = 0.55f;
/// Glyph stroke, in glyph units: bolder than High Fly's numerals, for distance.
constexpr float kGlyphStroke = 0.22f;
/// Hours the text shows at most.
constexpr int32_t kMaxHours = 99;

struct TextMetrics {
    int32_t glyph_w;
    int32_t glyph_h;
    int32_t gap;
};

TextMetrics text_metrics(int32_t unit) {
    TextMetrics m{};
    m.glyph_w = static_cast<int32_t>(std::lround((1.0f + kGlyphStroke) * static_cast<float>(unit)));
    m.glyph_h = static_cast<int32_t>(
        std::lround((kStrokeDigitPathH + kGlyphStroke) * static_cast<float>(unit)));
    m.gap = std::max<int32_t>(1, static_cast<int32_t>(std::lround(0.25f * unit)));
    return m;
}

/// Widest text the card holds: "99h59", five glyph cells.
int32_t max_text_width(const TextMetrics& m) {
    return 5 * m.glyph_w + 4 * m.gap;
}

float segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax;
    const float vy = by - ay;
    const float len2 = vx * vx + vy * vy;
    const float t =
        len2 > 0.0f ? std::clamp(((px - ax) * vx + (py - ay) * vy) / len2, 0.0f, 1.0f) : 0.0f;
    const float dx = px - (ax + t * vx);
    const float dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

/// Distance from the upper half of the circle at (cx, cy) with radius r.
float upper_arc_distance(float px, float py, float cx, float cy, float r) {
    if (py <= cy) {
        return std::fabs(std::hypot(px - cx, py - cy) - r);
    }
    return std::min(std::hypot(px - (cx - r), py - cy), std::hypot(px - (cx + r), py - cy));
}

/// Distance from the centre line of the unit letter `c` ('h' or 'm') at (px, py) inside a
/// `gw` x `gh` glyph box, whose strokes are `stroke` px wide.
float unit_glyph_distance(char c, float px, float py, float gw, float gh, float stroke) {
    const float top = stroke / 2.0f;
    const float base = gh - stroke / 2.0f;
    if (c == 'h') {
        const float left = 0.2f * gw;
        const float right = 0.8f * gw;
        const float r = (right - left) / 2.0f;
        const float shoulder = 0.45f * gh + r;
        return std::min({segment_distance(px, py, left, top, left, base),
                         upper_arc_distance(px, py, left + r, shoulder, r),
                         segment_distance(px, py, right, shoulder, right, base)});
    }
    const float left = 0.1f * gw;
    const float mid = 0.5f * gw;
    const float right = 0.9f * gw;
    const float r = (mid - left) / 2.0f;
    const float shoulder = 0.45f * gh + r;
    return std::min({segment_distance(px, py, left, 0.45f * gh, left, base),
                     upper_arc_distance(px, py, left + r, shoulder, r),
                     segment_distance(px, py, mid, shoulder, mid, base),
                     upper_arc_distance(px, py, mid + r, shoulder, r),
                     segment_distance(px, py, right, shoulder, right, base)});
}

/// Signed distance from (px, py) to the box centred at (cx, cy) with half sizes hw, hh.
float box_distance(float px, float py, float cx, float cy, float hw, float hh) {
    const float dx = std::fabs(px - cx) - hw;
    const float dy = std::fabs(py - cy) - hh;
    const float outside = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f));
    return outside + std::min(std::max(dx, dy), 0.0f);
}

/// Blends `colour` over the card at full precision, so full coverage stores it exactly.
void blend_exact(PixelWriter& writer, int32_t x, int32_t y, Rgb colour, float cov) {
    if (cov <= 0.0f || !writer.contains(x, y)) {
        return;
    }
    const auto c = static_cast<uint32_t>(std::lround(std::min(cov, 1.0f) * 255.0f));
    writer.put(x, y, blend_cov(writer.get(x, y), colour, c));
}

/// The mark an ended or paused print draws in the ring's hole, as coverage at (px, py)
/// relative to the ring's centre, with `r` the hole's radius and `w` the line width.
float mark_coverage(ClockMode mode, float px, float py, float r, float w) {
    const float half = w / 2.0f;
    switch (mode) {
    case ClockMode::DONE: {
        const float d =
            std::min(segment_distance(px, py, -0.45f * r, 0.0f, -0.12f * r, 0.35f * r),
                     segment_distance(px, py, -0.12f * r, 0.35f * r, 0.5f * r, -0.35f * r));
        return edge_coverage(half - d);
    }
    case ClockMode::FAILED: {
        const float d =
            std::min(segment_distance(px, py, -0.4f * r, -0.4f * r, 0.4f * r, 0.4f * r),
                     segment_distance(px, py, -0.4f * r, 0.4f * r, 0.4f * r, -0.4f * r));
        return edge_coverage(half - d);
    }
    case ClockMode::CANCELLED:
        return edge_coverage(-box_distance(px, py, 0.0f, 0.0f, 0.4f * r, 0.4f * r));
    case ClockMode::PAUSED: {
        const float d = std::min(box_distance(px, py, -0.25f * r, 0.0f, 0.12f * r, 0.45f * r),
                                 box_distance(px, py, 0.25f * r, 0.0f, 0.12f * r, 0.45f * r));
        return edge_coverage(-d);
    }
    case ClockMode::CLOCK:
    case ClockMode::PROGRESS:
    case ClockMode::COUNTDOWN:
        break;
    }
    return 0.0f;
}

} // namespace

PrintCardLayout layout_print_card(int32_t screen_w, int32_t screen_h) {
    PrintCardLayout l;
    const float short_side = static_cast<float>(std::min(screen_w, screen_h));
    l.ring_d = std::max<int32_t>(16, static_cast<int32_t>(std::lround(kRingShare * short_side)));
    l.pad = std::max<int32_t>(2, static_cast<int32_t>(std::lround(kPadShare * l.ring_d)));
    l.unit = std::max<int32_t>(
        4, static_cast<int32_t>(std::lround(kGlyphHeightShare * static_cast<float>(l.ring_d) /
                                            (kStrokeDigitPathH + kGlyphStroke))));
    l.h = l.ring_d + 2 * l.pad;
    l.w = 3 * l.pad + l.ring_d + max_text_width(text_metrics(l.unit)) + l.pad;
    return l;
}

DirtyRect print_card_home(int32_t screen_w, int32_t screen_h, const PrintCardLayout& layout) {
    const int32_t x1 = (screen_w - layout.w) / 2;
    const int32_t y1 = std::max<int32_t>(0, screen_h - 2 * layout.h);
    return {x1, y1, x1 + layout.w - 1, y1 + layout.h - 1};
}

PrintCardFace print_card_face(const ClockReading& r) {
    PrintCardFace f;
    f.mode = r.mode;
    f.progress_pct = std::clamp(r.progress_pct, 0, 100);
    f.has_countdown = r.has_countdown;
    if (r.has_countdown) {
        // The text shows whole minutes: down from an hour up, as a clock reads, and up below it,
        // so a running print never reads 0m.
        const int32_t s = std::max(0, r.countdown_s);
        f.countdown_s = s >= 3600 ? s - s % 60 : (s + 59) / 60 * 60;
    }
    return f;
}

std::string print_card_text(const PrintCardFace& face) {
    if (face.has_countdown && !clock_mode_is_end(face.mode)) {
        const int32_t s = std::max(0, face.countdown_s);
        if (s >= 3600) {
            const int32_t hours = std::min(kMaxHours, s / 3600);
            const int32_t minutes = hours == kMaxHours && s / 3600 > kMaxHours ? 59 : s / 60 % 60;
            return fmt::format("{}h{:02}", hours, minutes);
        }
        return fmt::format("{}m", std::max(1, (s + 59) / 60));
    }
    return fmt::format("{}%", std::clamp(face.progress_pct, 0, 100));
}

void PrintCardPainter::init(const PrintCardLayout& layout) {
    layout_ = layout;
    build_stroke_digits(layout.unit, kGlyphStroke, glyphs_);
}

void PrintCardPainter::clear() {
    glyphs_.clear();
    layout_ = {};
}

void PrintCardPainter::draw(FrameTarget& card, const PrintCardFace& face) const {
    PixelWriter writer(card);
    writer.fill_rect({0, 0, layout_.w - 1, layout_.h - 1}, kBackground);

    const Rgb state = print_state_color(face.mode, kAccent);
    const bool ended = clock_mode_is_end(face.mode);
    const float fill = ended ? 1.0f : static_cast<float>(face.progress_pct) / 100.0f;

    // Ring: track all round, the filled share clockwise from twelve o'clock.
    const float cx = static_cast<float>(layout_.pad) + static_cast<float>(layout_.ring_d) / 2.0f;
    const float cy = cx;
    const float outer = static_cast<float>(layout_.ring_d) / 2.0f;
    const float stroke =
        std::max(2.0f, std::round(kRingStrokeShare * static_cast<float>(layout_.ring_d)));
    const float mid = outer - stroke / 2.0f;
    for (int32_t y = layout_.pad; y < layout_.pad + layout_.ring_d; y++) {
        for (int32_t x = layout_.pad; x < layout_.pad + layout_.ring_d; x++) {
            const float px = static_cast<float>(x) + 0.5f - cx;
            const float py = static_cast<float>(y) + 0.5f - cy;
            const float d = std::hypot(px, py);
            const float cov = edge_coverage(stroke / 2.0f - std::fabs(d - mid));
            if (cov <= 0.0f) {
                continue;
            }
            float turn = std::atan2(px, -py) / kSaverTwoPi;
            if (turn < 0.0f) {
                turn += 1.0f;
            }
            blend_exact(writer, x, y, turn < fill ? state : kTrack, cov);
        }
    }

    // The mark in the ring's hole.
    if (ended || face.mode == ClockMode::PAUSED) {
        const float hole = outer - stroke;
        const float line = std::max(2.0f, stroke * 0.9f);
        for (int32_t y = layout_.pad; y < layout_.pad + layout_.ring_d; y++) {
            for (int32_t x = layout_.pad; x < layout_.pad + layout_.ring_d; x++) {
                const float px = static_cast<float>(x) + 0.5f - cx;
                const float py = static_cast<float>(y) + 0.5f - cy;
                blend_exact(writer, x, y, state, mark_coverage(face.mode, px, py, hole, line));
            }
        }
    }

    // Text beside the ring, vertically centred.
    const TextMetrics m = text_metrics(layout_.unit);
    const Rgb ink = ended ? state : kText;
    const float stroke_px = kGlyphStroke * static_cast<float>(layout_.unit);
    int32_t x = 2 * layout_.pad + layout_.ring_d + layout_.pad;
    const int32_t y = (layout_.h - m.glyph_h) / 2;
    const auto dot = [&](float dcx, float dcy, float r) {
        for (auto py = static_cast<int32_t>(dcy - r - 1); py <= static_cast<int32_t>(dcy + r + 1);
             py++) {
            for (auto px = static_cast<int32_t>(dcx - r - 1);
                 px <= static_cast<int32_t>(dcx + r + 1); px++) {
                const float d = std::hypot(static_cast<float>(px) + 0.5f - dcx,
                                           static_cast<float>(py) + 0.5f - dcy);
                blend_exact(writer, px, py, ink, edge_coverage(r - d));
            }
        }
    };
    for (const char c : print_card_text(face)) {
        if (c >= '0' && c <= '9') {
            blit_stroke_digit(writer, glyphs_, c - '0', x, y, ink, 1.0f);
            x += m.glyph_w + m.gap;
        } else if (c == 'h' || c == 'm') {
            const auto gw = static_cast<float>(m.glyph_w);
            const auto gh = static_cast<float>(m.glyph_h);
            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);
            for (auto py = y; py < y + m.glyph_h; py++) {
                for (auto px = x; px < x + m.glyph_w; px++) {
                    const float d =
                        unit_glyph_distance(c, static_cast<float>(px) + 0.5f - fx,
                                            static_cast<float>(py) + 0.5f - fy, gw, gh, stroke_px);
                    blend_exact(writer, px, py, ink, edge_coverage(stroke_px / 2.0f - d));
                }
            }
            x += m.glyph_w + m.gap;
        } else if (c == '%') {
            const auto gw = static_cast<float>(m.glyph_w);
            const auto gh = static_cast<float>(m.glyph_h);
            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);
            dot(fx + 0.25f * gw, fy + 0.25f * gh, 0.14f * gw);
            dot(fx + 0.75f * gw, fy + 0.75f * gh, 0.14f * gw);
            for (auto py = y; py < y + m.glyph_h; py++) {
                for (auto px = x; px < x + m.glyph_w; px++) {
                    const float d = segment_distance(
                        static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f,
                        fx + 0.85f * gw, fy + 0.08f * gh, fx + 0.15f * gw, fy + 0.92f * gh);
                    blend_exact(writer, px, py, ink, edge_coverage(stroke_px / 2.0f - d));
                }
            }
            x += m.glyph_w + m.gap;
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
