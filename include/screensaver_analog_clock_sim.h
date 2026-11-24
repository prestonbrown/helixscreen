// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_analog_paint.h"
#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file screensaver_analog_clock_sim.h
 * @brief An analog clock with several faces, each showing a running print its own way, stepped
 * one frame at a time
 *
 * No LVGL, no clock: the saver passes the frame time and the reading. Everything but the hands
 * is drawn once into a cached dial (the face, its print display, the subdials' hands), and the
 * saver writes the dial's text into that cache too (texts()), so the hands always pass over the
 * text. The dial is drawn again only when what it shows changes: the minute, the print's state
 * or progress, the date, the drift step. Each frame restores the dial under the boxes the hands
 * left and redraws the hands there.
 */

namespace helix::ui {

/// The faces, in the order of the Clock Face setting. Frozen: the setting stores the number.
enum class AnalogStyle : uint8_t { CHRONOGRAPH, STATION, NEON, BAUHAUS, ORRERY };
inline constexpr size_t kAnalogStyleCount = 5;

/// How a face's hands move.
enum class AnalogMotion : uint8_t {
    SWEEP,  ///< the seconds hand glides
    TICK,   ///< the seconds hand steps once a second
    SPRUNG, ///< steps that overshoot and settle; the minute hand jumps once a minute
};

/// One level of the analog clock ladder.
struct AnalogClockLevel {
    uint32_t period_ms; ///< frame period in ms
    /// Each face moves its own way, with trails and glows; otherwise every face ticks once a
    /// second with neither.
    bool smooth;
};

/// The ladder, most expensive level first. A ticking frame between seconds writes nothing.
inline constexpr AnalogClockLevel kAnalogClockLevels[] = {
    {kSaverFastPeriod, true},
    {33, true},
    {250, false},
};

/// How long a sprung step takes to settle.
inline constexpr uint32_t kAnalogSpringMs = 400;

/// What the clock draws in a frame: which face, and the reading.
struct AnalogFace {
    AnalogStyle style = AnalogStyle::CHRONOGRAPH;
    ClockReading reading;
};

/// How `style` moves its hands at a level whose `smooth` is given.
AnalogMotion analog_motion(AnalogStyle style, bool smooth);

/// Share of a sprung step done `t_ms` after it starts: 0 at the start, past 1 on the
/// overshoot, settling to 1 by kAnalogSpringMs.
float analog_spring(uint32_t t_ms);

/// The hands' angles, degrees clockwise from 12.
struct AnalogHands {
    float hour = 0.0f;
    float minute = 0.0f;
    float second = 0.0f;

    bool operator==(const AnalogHands& o) const {
        return hour == o.hour && minute == o.minute && second == o.second;
    }
};

/// Where the hands point at `time`, `sub_ms` (0 to 999) past its second, moving by `motion`.
/// The minute hand moves each whole second, and the hour hand with it; a SPRUNG minute hand
/// holds for the whole minute and springs over at :00.
AnalogHands analog_hands(const ClockTime& time, uint32_t sub_ms, AnalogMotion motion);

/// What a text the saver writes into the dial says. The sim places it; the saver words it in
/// the user's language.
enum class AnalogTextRole : uint8_t {
    DATE_WINDOW,    ///< weekday and day of the month: "Tue 24"
    MAKER,          ///< "HelixScreen"
    CAPTION_24H,    ///< over the 24-hour subdial
    CAPTION_PRINT,  ///< over the progress subdial
    VALUE_PRINT,    ///< the progress: "62%"
    CAPTION_LEFT,   ///< over the time-left subdial
    VALUE_LEFT,     ///< the time left: "2:31"
    CAPTION_ENDS,   ///< over the finish-time subdial
    VALUE_ENDS,     ///< the finish time: "12:40"
    STATE_WORD,     ///< a paused or ended print: "PAUSED", "DONE", "FAILED"
    WEEKDAY_LETTER, ///< one weekday's initial round the weekday subdial; index 0 is Sunday
    COUNTDOWN,      ///< the print in one line under the hands: "2:31 left"
    COLUMN_WEEKDAY, ///< the side column's weekday
    COLUMN_DATE,    ///< the side column's date
    COLUMN_STATUS,  ///< the side column's print line: the ETA, or the state
    COLUMN_DETAIL,  ///< the side column's second print line: progress and time left
};

/// Horizontal placement of a text in its box.
enum class AnalogAlign : uint8_t { LEFT, CENTER };

/// One text the dial carries.
struct AnalogText {
    AnalogTextRole role = AnalogTextRole::DATE_WINDOW;
    uint8_t index = 0;
    DirtyRect box;
    Rgb color;
    AnalogAlign align = AnalogAlign::CENTER;
    /// Drawn in the bold display face, as a reading the eye looks for first.
    bool display = false;
};

/// Where a face sits on the frame, after its drift.
struct AnalogGeom {
    int32_t w = 0;
    int32_t h = 0;
    float cx = 0.0f;
    float cy = 0.0f;
    float r = 0.0f; ///< the face's outer radius
    /// The smallest screens: subdial captions are left out.
    bool compact = false;
    /// Bauhaus's side column: top left and width; on a portrait frame it sits under the clock.
    float col_x = 0.0f;
    float col_y = 0.0f;
    float col_w = 0.0f;
    float col_scale = 1.0f;
};

/// Lays `style` out undrifted on a `w` x `h` frame.
AnalogGeom analog_layout(AnalogStyle style, int32_t w, int32_t h);

/// The box everything `style` draws on `g` stays inside.
DirtyRect analog_face_box(AnalogStyle style, const AnalogGeom& g);

/**
 * @brief Draws an analog clock face into a frame, redrawing only where the hands moved
 */
class AnalogClockSim {
  public:
    static constexpr const auto& kLevels = kAnalogClockLevels;

    /// Lays `face` out for `frame`, draws the dial and paints the whole frame.
    void init(FrameTarget& frame, size_t level, const AnalogFace& face);

    /// prepare() then compose(): a frame with no text written into the dial.
    void step(uint32_t dt_ms, FrameTarget& frame, const AnalogFace& face,
              std::vector<DirtyRect>& dirty);

    /**
     * @brief Advances by `dt_ms` towards showing `face`
     *
     * Draws the dial again when what it shows changed, and returns true then: the caller writes
     * texts() into dial() before compose() paints it.
     */
    bool prepare(uint32_t dt_ms, const FrameTarget& frame, const AnalogFace& face);

    /// Paints `frame`: the whole dial after a redraw, else the dial and hands where the hands
    /// moved, replacing `dirty` with those boxes.
    void compose(FrameTarget& frame, std::vector<DirtyRect>& dirty);

    /// Paints the whole frame at the next compose(), for text written into the dial outside a
    /// redraw.
    void repaint() {
        repaint_ = true;
    }

    /// Runs at `level` (clamped) from now on.
    void request_level(size_t level);

    size_t level() const {
        return level_;
    }
    /// The cached dial, the frame's size and format; valid until clear().
    FrameTarget dial() const {
        return dial_;
    }
    /// The texts the dial carries, placed for the last redraw.
    const std::vector<AnalogText>& texts() const {
        return texts_;
    }
    const AnalogGeom& geom() const {
        return geom_;
    }
    const AnalogHands& hands() const {
        return hands_;
    }
    /// The boxes the hands covered at the last compose().
    const std::array<DirtyRect, 3>& hand_boxes() const {
        return boxes_;
    }

    /// Frees the dial.
    void clear();
    /// Bytes the dial and the texts hold.
    size_t heap_bytes() const;

  private:
    /// What the dial shows; it is drawn again when this changes.
    struct DialKey {
        AnalogStyle style = AnalogStyle::CHRONOGRAPH;
        ClockMode mode = ClockMode::CLOCK;
        int32_t progress_pct = 0;
        int32_t minute_of_day = -1;
        int32_t countdown_min = 0;
        int32_t eta_minute = 0;
        bool has_countdown = false;
        bool use_24h = false;
        ClockDate date;
        int32_t dx = 0;
        int32_t dy = 0;

        bool operator==(const DialKey& o) const;
    };

    void relayout(const FrameTarget& frame, AnalogStyle style);
    DialKey key_for(const AnalogFace& face) const;
    void draw_dial(const AnalogFace& face);
    AnalogHands hands_for(const AnalogFace& face) const;
    std::array<DirtyRect, 3> boxes_for(const AnalogHands& hands) const;
    void draw_hands(FrameTarget& frame, const DirtyRect& clip) const;

    size_t level_ = 0;
    AnalogStyle style_ = AnalogStyle::CHRONOGRAPH;
    ClockMode mode_ = ClockMode::CLOCK;
    AnalogGeom base_;
    AnalogGeom geom_;
    ClockDrift drift_;
    DialKey key_;
    bool keyed_ = false;
    bool repaint_ = true;

    std::vector<uint8_t> dial_bytes_;
    FrameTarget dial_;
    std::vector<AnalogText> texts_;

    int32_t last_second_ = -1;
    uint32_t sub_ms_ = 0;
    /// Where a paused chronograph's seconds hand stopped, negative while it runs.
    float frozen_second_ = -1.0f;
    /// Where the hands are drawn now, and where prepare() moved them for compose().
    AnalogHands hands_;
    AnalogHands pending_;
    std::array<DirtyRect, 3> boxes_{};
};

} // namespace helix::ui
