// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_analog_faces.h"

#include "screensaver_print_colors.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace helix::ui {

namespace {

// ---------------------------------------------------------------------------------------------
// Palettes
// ---------------------------------------------------------------------------------------------

namespace chrono {
constexpr Rgb kSteelLight{232, 232, 234};
constexpr Rgb kSteelDark{106, 108, 112};
constexpr Rgb kSteelEdge{196, 198, 202};
constexpr Rgb kInsert{17, 17, 17};
constexpr Rgb kTrack{28, 28, 28};
constexpr Rgb kScale{221, 221, 221};
constexpr Rgb kDialInner{29, 42, 62};
constexpr Rgb kDialOuter{10, 15, 24};
constexpr Rgb kMinuteTrack{124, 138, 160};
constexpr Rgb kLume{207, 233, 200};
constexpr Rgb kSubFace{11, 17, 27};
constexpr Rgb kSubRim{85, 101, 125};
constexpr Rgb kSubTick{135, 150, 173};
constexpr Rgb kSubIdleHand{200, 210, 224};
constexpr Rgb kDateWindow{238, 238, 238};
constexpr Rgb kDateText{17, 17, 17};
constexpr Rgb kMaker{154, 166, 184};
constexpr Rgb kAccent{232, 144, 42};
} // namespace chrono

namespace station {
constexpr Rgb kRim{42, 42, 46};
constexpr Rgb kFace{244, 244, 240};
constexpr Rgb kInk{17, 17, 17};
constexpr Rgb kRed{215, 25, 32};
constexpr Rgb kPrint{47, 140, 255};
} // namespace station

namespace neon {
constexpr Rgb kBackground{3, 4, 10};
constexpr Rgb kCyan{56, 232, 255};
constexpr Rgb kMagenta{255, 63, 180};
constexpr Rgb kWhite{255, 255, 255};
} // namespace neon

namespace bauhaus {
constexpr Rgb kBackground{14, 15, 17};
constexpr Rgb kFace{27, 28, 31};
constexpr Rgb kCream{217, 214, 207};
constexpr Rgb kGrey{141, 138, 132};
constexpr Rgb kRule{58, 59, 63};
constexpr Rgb kBarTrack{44, 45, 49};
constexpr Rgb kOrange{255, 106, 26};
} // namespace bauhaus

namespace orrery {
constexpr Rgb kBackground{4, 6, 13};
constexpr Rgb kSun{255, 207, 90};
constexpr Rgb kOrbit{42, 51, 80};
constexpr Rgb kMarker{85, 96, 138};
constexpr Rgb kHour{255, 179, 71};
constexpr Rgb kMinute{111, 211, 255};
constexpr Rgb kSecond{224, 224, 255};
constexpr Rgb kPrint{125, 255, 154};
constexpr Rgb kStar{255, 255, 255};
} // namespace orrery

constexpr Rgb kBlack{0, 0, 0};

// Each face is drawn at a design radius and scaled by r / that.
constexpr float kChronoDesignR = 235.0f;
constexpr float kStationDesignR = 228.0f;
constexpr float kNeonDesignR = 244.0f;
constexpr float kBauhausDesignR = 200.0f;
constexpr float kOrreryDesignR = 236.0f;

float scale_of(AnalogStyle style, const AnalogGeom& g) {
    switch (style) {
    case AnalogStyle::CHRONOGRAPH:
        return g.r / kChronoDesignR;
    case AnalogStyle::STATION:
        return g.r / kStationDesignR;
    case AnalogStyle::NEON:
        return g.r / kNeonDesignR;
    case AnalogStyle::BAUHAUS:
        return g.r / kBauhausDesignR;
    case AnalogStyle::ORRERY:
        return g.r / kOrreryDesignR;
    }
    return 1.0f;
}

bool shows_print(const ClockReading& r) {
    return r.mode != ClockMode::CLOCK;
}

int32_t shown_progress(const ClockReading& r) {
    return r.mode == ClockMode::DONE ? 100 : std::clamp(r.progress_pct, 0, 100);
}

/// The hour hand's angle at the reading's minute.
float hour_angle(const ClockReading& r) {
    return static_cast<float>(r.time.hour_of_day % 12) * 30.0f +
           static_cast<float>(r.time.minute) * 0.5f;
}

/// The finish time's angle on a 12-hour dial, at or after `from` (so a span from the hour
/// hand runs clockwise to it).
float eta_angle_after(const ClockReading& r, float from) {
    float a = static_cast<float>(r.eta_minute_of_day % 720) * 0.5f;
    while (a < from) {
        a += 360.0f;
    }
    return a;
}

/// True when the reading knows when the print ends.
bool knows_end(const ClockReading& r) {
    return r.mode == ClockMode::COUNTDOWN;
}

void add_text(std::vector<AnalogText>& texts, AnalogTextRole role, float cx, float cy, float w,
              float h, Rgb color, AnalogAlign align = AnalogAlign::CENTER, uint8_t index = 0,
              bool display = false) {
    AnalogText t;
    t.display = display;
    t.role = role;
    t.index = index;
    const float x1 = align == AnalogAlign::CENTER ? cx - w * 0.5f : cx;
    t.box = {static_cast<int32_t>(std::lround(x1)),
             static_cast<int32_t>(std::lround(cy - h * 0.5f)),
             static_cast<int32_t>(std::lround(x1 + w)) - 1,
             static_cast<int32_t>(std::lround(cy + h * 0.5f)) - 1};
    t.color = color;
    t.align = align;
    texts.push_back(t);
}

/// A shadow under a hand: the shape offset down-right and softened.
AEdge shadow_edge(float k) {
    return AEdge{0.45f, 1.0f * k, 6.0f * k};
}

// ---------------------------------------------------------------------------------------------
// Chronograph
// ---------------------------------------------------------------------------------------------

void chrono_baton(APainter& p, APt c, float r_out, float deg, float len, float w, float k) {
    const APt a = polar(c.x, c.y, r_out - len, deg);
    const APt b = polar(c.x, c.y, r_out, deg);
    p.taper(a, b, w + 3.0f * k, w + 3.0f * k, chrono::kLume, AEdge{0.22f, 3.0f * k, 6.0f * k});
    p.taper(a, b, w + 3.0f * k, w + 3.0f * k, chrono::kSteelEdge);
    const APt ia = polar(c.x, c.y, r_out - len + 3.0f * k, deg);
    const APt ib = polar(c.x, c.y, r_out - 3.0f * k, deg);
    p.taper(ia, ib, std::max(1.0f, w - 3.0f * k), std::max(1.0f, w - 3.0f * k), chrono::kLume);
}

/// A subdial with its ring of ticks (none when `ticks` is false) and a short hand, which the
/// subdial's value text, written later, covers where they cross.
void chrono_subdial(APainter& p, APt s, float r, float hand_deg, Rgb hand, float k, bool ticks) {
    p.disc(s, r, chrono::kSubFace);
    p.ring(s, r - 0.75f * k, r + 0.75f * k, chrono::kSubRim);
    if (ticks) {
        for (int i = 0; i < 12; i++) {
            p.segment(polar(s.x, s.y, r - 7.0f * k, i * 30.0f),
                      polar(s.x, s.y, r - 2.0f * k, i * 30.0f), 1.5f * k, chrono::kSubTick, false);
        }
    }
    p.segment(polar(s.x, s.y, -6.0f * k, hand_deg), polar(s.x, s.y, r * 0.62f, hand_deg), 2.5f * k,
              hand, true);
    p.disc(s, 4.0f * k, hand);
}

void chrono_dial(APainter& p, const AnalogGeom& g, const ClockReading& rd, int32_t min_text_px,
                 std::vector<AnalogText>& texts) {
    const float k = scale_of(AnalogStyle::CHRONOGRAPH, g);
    const APt c{g.cx, g.cy};
    const float R = 225.0f * k;
    const bool printing = shows_print(rd);
    const Rgb accent = print_state_color(rd.mode, chrono::kAccent);

    p.steel_ring(c, R - 1.0f, R + 10.0f * k, chrono::kSteelLight, chrono::kSteelDark);
    p.disc(c, R, chrono::kInsert);
    if (printing) {
        p.ring(c, R - 20.0f * k, R - 2.0f * k, chrono::kTrack);
        p.arc(c, R - 20.0f * k, R - 2.0f * k, 0.0f, shown_progress(rd) * 3.6f, accent);
        for (int i = 0; i < 10; i++) {
            p.segment(polar(c.x, c.y, R - 20.0f * k, i * 36.0f),
                      polar(c.x, c.y, R - 2.0f * k, i * 36.0f), 2.0f * k, chrono::kScale, false);
        }
    } else {
        for (int i = 0; i < 60; i++) {
            const bool big = i % 5 == 0;
            p.segment(polar(c.x, c.y, R - (big ? 18.0f : 8.0f) * k, i * 6.0f),
                      polar(c.x, c.y, R - 3.0f * k, i * 6.0f), (big ? 2.2f : 1.0f) * k,
                      chrono::kScale, false);
        }
        p.disc(polar(c.x, c.y, R - 11.0f * k, 0.0f), 5.0f * k, chrono::kLume);
    }

    const float dial_r = R - 22.0f * k;
    p.radial_disc(c, dial_r, chrono::kDialInner, chrono::kDialOuter, 0.0f, -0.2f);
    for (int i = 0; i < 60; i++) {
        p.segment(polar(c.x, c.y, R - 34.0f * k, i * 6.0f),
                  polar(c.x, c.y, R - 28.0f * k, i * 6.0f), 1.5f * k, chrono::kMinuteTrack, false);
    }
    const float baton_out = R - 40.0f * k;
    for (int i = 1; i < 12; i++) {
        const bool quarter = i % 3 == 0;
        chrono_baton(p, c, baton_out, i * 30.0f, (quarter ? 34.0f : 26.0f) * k,
                     (quarter ? 15.0f : 9.0f) * k, k);
    }
    chrono_baton(p, c, baton_out, -3.2f, 40.0f * k, 11.0f * k, k);
    chrono_baton(p, c, baton_out, 3.2f, 40.0f * k, 11.0f * k, k);

    // Subdials: left and right at 3 and 9, a smaller one above 6.
    const float sd = 100.0f * k;
    const float sr = 46.0f * k;
    const APt left{c.x - sd, c.y};
    const APt right{c.x + sd, c.y};
    const APt bottom{c.x, c.y + 80.0f * k};
    const float br = 36.0f * k;
    const float cap_h = 16.0f * k;
    const float val_h = 24.0f * k;
    const auto captions = !g.compact;

    if (!printing) {
        const float day_deg = static_cast<float>(rd.time.hour_of_day * 60 + rd.time.minute) * 0.25f;
        chrono_subdial(p, left, sr, day_deg, chrono::kSubIdleHand, k, true);
        if (captions) {
            add_text(texts, AnalogTextRole::CAPTION_24H, left.x, left.y - sr * 0.42f, sr * 1.3f,
                     cap_h, chrono::kSubTick);
        }
        chrono_subdial(p, right, sr, rd.date.weekday * (360.0f / 7.0f), chrono::kSubIdleHand, k,
                       false);
        for (uint8_t d = 0; d < 7; d++) {
            const APt at = polar(right.x, right.y, sr - 10.0f * k, d * (360.0f / 7.0f));
            add_text(texts, AnalogTextRole::WEEKDAY_LETTER, at.x, at.y, 14.0f * k, 13.0f * k,
                     chrono::kSubTick, AnalogAlign::CENTER, d);
        }
    } else {
        const int32_t pct = shown_progress(rd);
        chrono_subdial(p, left, sr, pct * 3.6f, accent, k, true);
        if (captions) {
            add_text(texts, AnalogTextRole::CAPTION_PRINT, left.x, left.y - sr * 0.42f, sr * 1.3f,
                     cap_h, chrono::kSubTick);
        }
        add_text(texts, AnalogTextRole::VALUE_PRINT, left.x, left.y + sr * 0.45f, sr * 1.5f, val_h,
                 accent);

        const bool counts =
            rd.has_countdown && (rd.mode == ClockMode::COUNTDOWN || rd.mode == ClockMode::PAUSED);
        const float left_deg =
            counts ? static_cast<float>(rd.countdown_s % (12 * 3600)) / 120.0f : 0.0f;
        chrono_subdial(p, right, sr, left_deg, counts ? accent : chrono::kSubRim, k, true);
        if (counts) {
            if (captions) {
                add_text(texts, AnalogTextRole::CAPTION_LEFT, right.x, right.y - sr * 0.42f,
                         sr * 1.3f, cap_h, chrono::kSubTick);
            }
            add_text(texts, AnalogTextRole::VALUE_LEFT, right.x, right.y + sr * 0.45f, sr * 1.5f,
                     val_h, accent);
        }

        if (knows_end(rd)) {
            const float eta_deg = static_cast<float>(rd.eta_minute_of_day % 720) * 0.5f;
            chrono_subdial(p, bottom, br, eta_deg, accent, k, true);
            if (captions) {
                add_text(texts, AnalogTextRole::CAPTION_ENDS, bottom.x, bottom.y - br * 0.42f,
                         br * 1.4f, cap_h * 0.9f, chrono::kSubTick);
            }
            add_text(texts, AnalogTextRole::VALUE_ENDS, bottom.x, bottom.y + br * 0.42f, br * 1.7f,
                     val_h * 0.9f, accent);
        } else if (rd.mode != ClockMode::PROGRESS) {
            chrono_subdial(p, bottom, br, 0.0f, accent, k, true);
            add_text(texts, AnalogTextRole::STATE_WORD, bottom.x, bottom.y + br * 0.42f, br * 1.8f,
                     val_h * 0.9f, accent);
        }
    }

    // Date window under 12, maker's mark above 6. A window too small for legible text is left
    // out, frame and all.
    const float dw = 52.0f * k;
    const float dh = 24.0f * k;
    const float dy = c.y - 106.0f * k;
    if (dh - 2.0f * k >= static_cast<float>(min_text_px)) {
        p.round_rect(c.x - dw * 0.5f, dy - dh * 0.5f, c.x + dw * 0.5f, dy + dh * 0.5f, 3.0f * k,
                     chrono::kDateWindow);
        add_text(texts, AnalogTextRole::DATE_WINDOW, c.x, dy, dw - 4.0f * k, dh - 2.0f * k,
                 chrono::kDateText);
    }
    add_text(texts, AnalogTextRole::MAKER, c.x, c.y + 134.0f * k, 120.0f * k, 15.0f * k,
             chrono::kMaker);
}

void chrono_hand(APainter& p, APt c, float deg, float len, float w, float k, bool glow) {
    const APt tip = polar(c.x, c.y, len, deg);
    const APt sc{c.x + 3.0f * k, c.y + 5.0f * k};
    p.taper(sc, polar(sc.x, sc.y, len, deg), w, w * 0.5f, kBlack, shadow_edge(k));
    p.taper(c, tip, w, w * 0.5f, chrono::kSteelEdge);
    const APt la = polar(c.x, c.y, 24.0f * k, deg);
    const APt lb = polar(c.x, c.y, len - 8.0f * k, deg);
    const float lw = w - 6.0f * k;
    if (glow) {
        p.taper(la, lb, lw, lw * 0.5f, chrono::kLume, AEdge{0.25f, 2.0f * k, 5.0f * k});
    }
    p.taper(la, lb, lw, lw * 0.5f, chrono::kLume);
}

void chrono_hands(APainter& p, const AnalogGeom& g, const AnalogHands& h, ClockMode mode,
                  bool smooth, bool seconds) {
    const float k = scale_of(AnalogStyle::CHRONOGRAPH, g);
    const APt c{g.cx, g.cy};
    const float R = 225.0f * k;
    chrono_hand(p, c, h.hour, 120.0f * k, 16.0f * k, k, smooth);
    chrono_hand(p, c, h.minute, 180.0f * k, 12.0f * k, k, smooth);
    p.disc(c, 9.0f * k, chrono::kSteelEdge);
    const Rgb accent = print_state_color(mode, chrono::kAccent);
    if (seconds) {
        p.segment(polar(c.x, c.y, -40.0f * k, h.second), polar(c.x, c.y, R - 34.0f * k, h.second),
                  2.0f * k, accent, false);
    }
    p.disc(c, 4.0f * k, accent);
}

// ---------------------------------------------------------------------------------------------
// Station
// ---------------------------------------------------------------------------------------------

void station_dial(APainter& p, const AnalogGeom& g, const ClockReading& rd) {
    const float k = scale_of(AnalogStyle::STATION, g);
    const APt c{g.cx, g.cy};
    const float R = 220.0f * k;
    p.disc(c, R + 8.0f * k, station::kRim);
    p.disc(c, R, station::kFace);
    if (shows_print(rd)) {
        const Rgb col = print_state_color(rd.mode, station::kPrint);
        const float from = hour_angle(rd);
        if (knows_end(rd)) {
            const float to = eta_angle_after(rd, from);
            p.arc(c, R - 7.0f * k, R - 1.0f * k, from, to, col);
            const APt dot = polar(c.x, c.y, R - 4.0f * k, to);
            p.disc(dot, 9.0f * k, station::kFace);
            p.disc(dot, 7.0f * k, col);
        } else {
            // No finish time yet, or the print has ended: the rim colour alone says so.
            p.ring(c, R - 7.0f * k, R - 1.0f * k, col, AEdge{0.85f});
        }
    }
    for (int i = 0; i < 60; i++) {
        const bool hour = i % 5 == 0;
        const float len = (hour ? 46.0f : 14.0f) * k;
        p.segment(polar(c.x, c.y, R - 12.0f * k - len, i * 6.0f),
                  polar(c.x, c.y, R - 12.0f * k, i * 6.0f), (hour ? 13.0f : 4.0f) * k,
                  station::kInk, false);
    }
}

void station_hands(APainter& p, const AnalogGeom& g, const AnalogHands& h, bool seconds) {
    const float k = scale_of(AnalogStyle::STATION, g);
    const APt c{g.cx, g.cy};
    const APt sc{c.x + 3.0f * k, c.y + 5.0f * k};
    for (const auto& [deg, len, w0, w1] :
         {std::array<float, 4>{h.hour, 125.0f * k, 20.0f * k, 15.0f * k},
          std::array<float, 4>{h.minute, 190.0f * k, 16.0f * k, 11.0f * k}}) {
        p.taper(polar(sc.x, sc.y, -40.0f * k, deg), polar(sc.x, sc.y, len, deg), w0, w1, kBlack,
                shadow_edge(k));
        p.taper(polar(c.x, c.y, -40.0f * k, deg), polar(c.x, c.y, len, deg), w0, w1, station::kInk);
    }
    if (seconds) {
        const APt tip = polar(c.x, c.y, 130.0f * k, h.second);
        const APt stip = polar(sc.x, sc.y, 130.0f * k, h.second);
        p.segment(polar(sc.x, sc.y, -50.0f * k, h.second), stip, 4.0f * k, kBlack, false,
                  shadow_edge(k));
        p.disc(stip, 17.0f * k, kBlack, shadow_edge(k));
        p.segment(polar(c.x, c.y, -50.0f * k, h.second), tip, 4.0f * k, station::kRed, false);
        p.disc(tip, 17.0f * k, station::kRed);
    }
    p.disc(c, 5.0f * k, station::kRed);
}

// ---------------------------------------------------------------------------------------------
// Neon
// ---------------------------------------------------------------------------------------------

AEdge neon_glow(float k) {
    return AEdge{0.35f, 3.0f * k, 9.0f * k};
}

void neon_line(APainter& p, APt a, APt b, float w, Rgb color, float k, bool glow) {
    if (glow) {
        p.segment(a, b, w, color, true, neon_glow(k));
    }
    p.segment(a, b, w, color, true);
}

void neon_dial(APainter& p, const AnalogGeom& g, const ClockReading& rd,
               std::vector<AnalogText>& texts) {
    const float k = scale_of(AnalogStyle::NEON, g);
    const APt c{g.cx, g.cy};
    const float R = 215.0f * k;
    p.ring(c, R - 1.0f * k, R + 1.0f * k, neon::kCyan, neon_glow(k));
    p.ring(c, R - 1.0f * k, R + 1.0f * k, neon::kCyan, AEdge{0.75f});
    for (int i = 0; i < 12; i++) {
        const bool quarter = i % 3 == 0;
        neon_line(p, polar(c.x, c.y, R - (quarter ? 26.0f : 14.0f) * k, i * 30.0f),
                  polar(c.x, c.y, R - 4.0f * k, i * 30.0f), (quarter ? 3.0f : 2.0f) * k,
                  neon::kCyan, k, true);
    }
    if (shows_print(rd)) {
        const Rgb col = print_state_color(rd.mode, neon::kMagenta);
        const float ring_r = R + 14.0f * k;
        p.ring(c, ring_r - 2.0f * k, ring_r + 2.0f * k, col, AEdge{0.15f});
        const float to = shown_progress(rd) * 3.6f;
        p.arc(c, ring_r - 2.0f * k, ring_r + 2.0f * k, 0.0f, to, col, neon_glow(k));
        p.arc(c, ring_r - 2.0f * k, ring_r + 2.0f * k, 0.0f, to, col);
        add_text(texts, AnalogTextRole::COUNTDOWN, c.x, c.y + 96.0f * k, 300.0f * k, 46.0f * k, col,
                 AnalogAlign::CENTER, 0, true);
    }
}

void neon_hands(APainter& p, const AnalogGeom& g, const AnalogHands& h, bool smooth, bool seconds) {
    const float k = scale_of(AnalogStyle::NEON, g);
    const APt c{g.cx, g.cy};
    neon_line(p, c, polar(c.x, c.y, 125.0f * k, h.hour), 7.0f * k, neon::kCyan, k, smooth);
    neon_line(p, c, polar(c.x, c.y, 185.0f * k, h.minute), 4.0f * k, neon::kCyan, k, smooth);
    if (seconds) {
        if (smooth) {
            p.trail(c, 20.0f * k, 195.0f * k, h.second, 17.0f, neon::kMagenta, 0.6f);
        }
        neon_line(p, polar(c.x, c.y, 20.0f * k, h.second), polar(c.x, c.y, 195.0f * k, h.second),
                  2.0f * k, neon::kWhite, k, smooth);
    }
    p.ring(c, 5.0f * k, 7.0f * k, neon::kWhite);
}

// ---------------------------------------------------------------------------------------------
// Bauhaus
// ---------------------------------------------------------------------------------------------

void bauhaus_dial(APainter& p, const AnalogGeom& g, const ClockReading& rd,
                  std::vector<AnalogText>& texts) {
    const float k = scale_of(AnalogStyle::BAUHAUS, g);
    const APt c{g.cx, g.cy};
    const float R = g.r;
    p.disc(c, R, bauhaus::kFace);
    for (int i = 0; i < 12; i++) {
        p.disc(polar(c.x, c.y, R - 24.0f * k, i * 30.0f), (i % 3 == 0 ? 7.0f : 4.0f) * k,
               bauhaus::kCream);
    }
    const Rgb col = print_state_color(rd.mode, bauhaus::kOrange);
    if (knows_end(rd)) {
        const float from = hour_angle(rd);
        p.wedge(c, R - 44.0f * k, from, eta_angle_after(rd, from), col, AEdge{0.18f});
    }

    // The column: weekday, date, a rule, then the print. A face laid out without it has none.
    if (g.col_w <= 0.0f) {
        return;
    }
    const float f = g.col_scale;
    const float x0 = g.col_x;
    const float cw = g.col_w;
    float y = g.col_y;
    const auto line = [&](AnalogTextRole role, float size, Rgb color) {
        const float hgt = size * 1.35f;
        add_text(texts, role, x0, y + hgt * 0.5f, cw, hgt, color, AnalogAlign::LEFT);
        y += hgt;
    };
    line(AnalogTextRole::COLUMN_WEEKDAY, 30.0f * f, bauhaus::kCream);
    line(AnalogTextRole::COLUMN_DATE, 22.0f * f, bauhaus::kGrey);
    y += 20.0f * f;
    p.round_rect(x0, y, x0 + cw, y + 1.5f, 0.0f, bauhaus::kRule);
    y += 14.0f * f;
    if (shows_print(rd)) {
        line(AnalogTextRole::COLUMN_STATUS, 18.0f * f, bauhaus::kGrey);
        y += 6.0f * f;
        const float bh = 8.0f * f;
        p.round_rect(x0, y, x0 + cw, y + bh, bh * 0.5f, bauhaus::kBarTrack);
        const float fill = cw * static_cast<float>(shown_progress(rd)) / 100.0f;
        if (fill >= bh) {
            p.round_rect(x0, y, x0 + fill, y + bh, bh * 0.5f, col);
        }
        y += bh + 8.0f * f;
        line(AnalogTextRole::COLUMN_DETAIL, 18.0f * f, bauhaus::kCream);
    }
}

void bauhaus_hands(APainter& p, const AnalogGeom& g, const AnalogHands& h, bool seconds) {
    const float k = scale_of(AnalogStyle::BAUHAUS, g);
    const APt c{g.cx, g.cy};
    p.segment(c, polar(c.x, c.y, 110.0f * k, h.hour), 14.0f * k, bauhaus::kCream, true);
    p.segment(c, polar(c.x, c.y, 160.0f * k, h.minute), 8.0f * k, bauhaus::kCream, true);
    if (seconds) {
        p.segment(polar(c.x, c.y, -30.0f * k, h.second), polar(c.x, c.y, 170.0f * k, h.second),
                  3.0f * k, bauhaus::kOrange, false);
    }
    p.disc(c, 8.0f * k, bauhaus::kOrange);
}

// ---------------------------------------------------------------------------------------------
// Orrery
// ---------------------------------------------------------------------------------------------

constexpr float kOrbitHour = 80.0f;
constexpr float kOrbitMinute = 140.0f;
constexpr float kOrbitSecond = 195.0f;
constexpr float kOrreryTrailDeg = 40.0f;

/// A small integer hash, so the stars fall in the same places every run.
uint32_t star_hash(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

void orrery_dial(APainter& p, const AnalogGeom& g, const ClockReading& rd) {
    const float k = scale_of(AnalogStyle::ORRERY, g);
    const APt c{g.cx, g.cy};
    // Stars across the frame, kept outside the orbits.
    const int stars = std::max(20, g.w * g.h / 4200);
    for (int i = 0; i < stars; i++) {
        const uint32_t hsh = star_hash(static_cast<uint32_t>(i) * 2654435761U + 17U);
        const APt s{static_cast<float>(hsh % static_cast<uint32_t>(g.w)),
                    static_cast<float>((hsh >> 12) % static_cast<uint32_t>(g.h))};
        if (std::hypot(s.x - c.x, s.y - c.y) < 240.0f * k) {
            continue;
        }
        const float size = 0.5f + static_cast<float>(hsh >> 28) / 16.0f;
        p.disc(s, size, orrery::kStar, AEdge{0.3f + static_cast<float>((hsh >> 24) & 15) / 25.0f});
    }
    p.disc(c, 26.0f * k, orrery::kSun, AEdge{0.35f, 6.0f * k, 14.0f * k});
    p.disc(c, 26.0f * k, orrery::kSun);
    for (const float r : {kOrbitHour, kOrbitMinute, kOrbitSecond}) {
        p.ring(c, r * k - 0.75f * k, r * k + 0.75f * k, orrery::kOrbit);
    }
    for (int i = 0; i < 12; i++) {
        p.disc(polar(c.x, c.y, 222.0f * k, i * 30.0f), (i % 3 == 0 ? 4.0f : 2.5f) * k,
               orrery::kMarker);
    }
    if (shows_print(rd)) {
        const Rgb col = print_state_color(rd.mode, orrery::kPrint);
        const float to = shown_progress(rd) * 3.6f;
        for (float deg = 0.0f; deg < to; deg += 2.5f) {
            p.disc(polar(c.x, c.y, 232.0f * k, deg), 1.6f * k, col);
        }
    }
}

void orrery_planet(APainter& p, APt c, float orbit, float deg, float radius, Rgb color, float k,
                   bool smooth) {
    if (smooth) {
        p.trail(c, orbit - 1.5f * k, orbit + 1.5f * k, deg, kOrreryTrailDeg, color, 0.45f);
        p.disc(polar(c.x, c.y, orbit, deg), radius, color, AEdge{0.3f, 2.0f * k, 6.0f * k});
    }
    p.disc(polar(c.x, c.y, orbit, deg), radius, color);
}

void orrery_hands(APainter& p, const AnalogGeom& g, const AnalogHands& h, bool smooth,
                  bool seconds) {
    const float k = scale_of(AnalogStyle::ORRERY, g);
    const APt c{g.cx, g.cy};
    orrery_planet(p, c, kOrbitHour * k, h.hour, 15.0f * k, orrery::kHour, k, smooth);
    orrery_planet(p, c, kOrbitMinute * k, h.minute, 10.0f * k, orrery::kMinute, k, smooth);
    if (seconds) {
        orrery_planet(p, c, kOrbitSecond * k, h.second, 6.0f * k, orrery::kSecond, k, smooth);
    }
}

} // namespace

Rgb analog_background(AnalogStyle style) {
    switch (style) {
    case AnalogStyle::NEON:
        return neon::kBackground;
    case AnalogStyle::BAUHAUS:
        return bauhaus::kBackground;
    case AnalogStyle::ORRERY:
        return orrery::kBackground;
    case AnalogStyle::CHRONOGRAPH:
    case AnalogStyle::STATION:
        break;
    }
    return kBlack;
}

std::array<AnalogPartSpec, 3> analog_part_specs(AnalogStyle style, const AnalogGeom& g,
                                                bool smooth) {
    const float k = scale_of(style, g);
    // Every hand's pad covers half its width, its glow and its shadow's offset and blur.
    const float shadow = 13.0f * k;
    switch (style) {
    case AnalogStyle::CHRONOGRAPH:
        return {{{0.0f, 120.0f * k, 0.0f, 8.0f * k + shadow},
                 {0.0f, 180.0f * k, 0.0f, 6.0f * k + shadow},
                 {40.0f * k, 191.0f * k, 0.0f, 11.0f * k}}};
    case AnalogStyle::STATION:
        return {{{40.0f * k, 125.0f * k, 0.0f, 10.0f * k + shadow},
                 {40.0f * k, 190.0f * k, 0.0f, 8.0f * k + shadow},
                 {50.0f * k, 130.0f * k, 0.0f, 17.0f * k + shadow}}};
    case AnalogStyle::NEON: {
        const float glow = smooth ? 14.0f * k : 2.0f * k;
        AnalogPartSpec sec{0.0f, 195.0f * k, 0.0f, 8.0f * k + glow};
        if (smooth) {
            sec.trail_span = 17.0f;
            sec.trail_r0 = 20.0f * k;
            sec.trail_r1 = 195.0f * k;
        }
        return {{{0.0f, 125.0f * k, 0.0f, 4.0f * k + glow},
                 {0.0f, 185.0f * k, 0.0f, 2.0f * k + glow},
                 sec}};
    }
    case AnalogStyle::BAUHAUS:
        return {{{0.0f, 110.0f * k, 0.0f, 9.0f * k},
                 {0.0f, 160.0f * k, 0.0f, 9.0f * k},
                 {30.0f * k, 170.0f * k, 0.0f, 9.0f * k}}};
    case AnalogStyle::ORRERY: {
        const float glow = smooth ? 9.0f * k : 2.0f * k;
        std::array<AnalogPartSpec, 3> parts{};
        const std::array<std::array<float, 2>, 3> bodies{
            {{kOrbitHour, 15.0f}, {kOrbitMinute, 10.0f}, {kOrbitSecond, 6.0f}}};
        for (size_t i = 0; i < 3; i++) {
            parts[i].orbit = bodies[i][0] * k;
            parts[i].pad = bodies[i][1] * k + glow;
            if (smooth) {
                parts[i].trail_span = kOrreryTrailDeg;
                parts[i].trail_r0 = (bodies[i][0] - 1.5f) * k;
                parts[i].trail_r1 = (bodies[i][0] + 1.5f) * k;
            }
        }
        return parts;
    }
    }
    return {};
}

DirtyRect analog_part_box(const AnalogPartSpec& spec, const AnalogGeom& g, float deg) {
    DirtyRect box;
    if (spec.orbit > 0.0f) {
        const APt at = polar(g.cx, g.cy, spec.orbit, deg);
        box = apaint_box(at.x, at.y, at.x, at.y, spec.pad + 2.0f);
    } else {
        box = apaint_segment_box(polar(g.cx, g.cy, -spec.tail, deg),
                                 polar(g.cx, g.cy, spec.len, deg), spec.pad + 2.0f);
    }
    if (spec.trail_span > 0.0f) {
        for (int i = 0; i <= 8; i++) {
            const float a = deg - spec.trail_span * static_cast<float>(i) / 8.0f;
            for (const float r : {spec.trail_r0, spec.trail_r1}) {
                const APt q = polar(g.cx, g.cy, r, a);
                box.add(apaint_box(q.x, q.y, q.x, q.y, 3.0f));
            }
        }
    }
    return box;
}

void analog_draw_dial(APainter& p, AnalogStyle style, const AnalogGeom& g,
                      const ClockReading& reading, std::vector<AnalogText>& texts,
                      const AnalogClockOptions& options) {
    texts.clear();
    p.fill(options.backdrop.value_or(analog_background(style)));
    if (options.backdrop && (style == AnalogStyle::NEON || style == AnalogStyle::ORRERY)) {
        p.disc(APt{g.cx, g.cy}, g.r, analog_background(style));
    }
    switch (style) {
    case AnalogStyle::CHRONOGRAPH:
        chrono_dial(p, g, reading, options.min_text_px, texts);
        break;
    case AnalogStyle::STATION:
        station_dial(p, g, reading);
        break;
    case AnalogStyle::NEON:
        neon_dial(p, g, reading, texts);
        break;
    case AnalogStyle::BAUHAUS:
        bauhaus_dial(p, g, reading, texts);
        break;
    case AnalogStyle::ORRERY:
        orrery_dial(p, g, reading);
        break;
    }
}

void analog_draw_hands(APainter& p, AnalogStyle style, const AnalogGeom& g,
                       const AnalogHands& hands, ClockMode mode, bool smooth, bool seconds) {
    switch (style) {
    case AnalogStyle::CHRONOGRAPH:
        chrono_hands(p, g, hands, mode, smooth, seconds);
        break;
    case AnalogStyle::STATION:
        station_hands(p, g, hands, seconds);
        break;
    case AnalogStyle::NEON:
        neon_hands(p, g, hands, smooth, seconds);
        break;
    case AnalogStyle::BAUHAUS:
        bauhaus_hands(p, g, hands, seconds);
        break;
    case AnalogStyle::ORRERY:
        orrery_hands(p, g, hands, smooth, seconds);
        break;
    }
}

} // namespace helix::ui
