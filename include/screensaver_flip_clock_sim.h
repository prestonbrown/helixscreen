// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_stroke_digits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

/**
 * @file screensaver_flip_clock_sim.h
 * @brief A split-flap clock that counts a running print down, stepped one frame at a time
 *
 * No LVGL, no clock and no allocation per frame: the saver passes the frame time, the local
 * time of day and the print's state, so a run replays exactly and runs in tests without a
 * display. Text the cards cannot show (the caption line, AM/PM) is described here and drawn by
 * the saver, which owns fonts and translations.
 */

namespace helix::ui {

/// One level of the flip clock ladder.
struct FlipClockLevel {
    uint32_t period_ms; ///< frame period in ms
    bool animate;       ///< fold the top half down on a change, or swap the digits at once
};

/// The ladder, most expensive level first. Nothing is drawn between changes at any level.
inline constexpr FlipClockLevel kFlipClockLevels[] = {
    {kSaverFastPeriod, true},
    {33, true},
    {250, false},
};

inline constexpr size_t kFlipClockLevelCount = std::size(kFlipClockLevels);

/// How long one card takes to flip.
inline constexpr uint32_t kFlipDurationMs = 300;

/// One card: a single character, or blank.
struct FlipCard {
    char digit = '\0'; ///< one of kStrokeGlyphChars, or NUL for a blank card

    bool operator==(const FlipCard& o) const {
        return digit == o.digit;
    }
    bool operator!=(const FlipCard& o) const {
        return !(*this == o);
    }
};

/// Hours, minutes, seconds.
inline constexpr size_t kFlipMaxGroups = 3;
/// Three hour cards, two minute cards, two second cards.
inline constexpr size_t kFlipMaxCards = 7;
/// The small row under the cards: up to two hour, two minute and four percent cards.
inline constexpr size_t kFlipMaxSubCards = 8;
/// Size of a small card against a main one.
inline constexpr float kSubCardScale = 0.32f;

/// How the cards fall into groups (hours, minutes and seconds), one card per digit.
struct FlipShape {
    uint8_t group_count = 0;
    std::array<uint8_t, kFlipMaxGroups> groups{}; ///< cards in each group

    uint8_t card_count() const;
    bool operator==(const FlipShape& o) const {
        return group_count == o.group_count && groups == o.groups;
    }
    bool operator!=(const FlipShape& o) const {
        return !(*this == o);
    }
};

/// Everything the screen shows, without pixels.
struct FlipFace {
    FlipShape shape;
    std::array<FlipCard, kFlipMaxCards> cards{};  ///< shape.card_count() of them, left to right
    ClockMeridiem meridiem = ClockMeridiem::NONE; ///< the time of day only
    ClockMode mode = ClockMode::CLOCK;            ///< sets the ink: print_state_color()
    /// The small row under the cards, empty with no print.
    FlipShape sub_shape;
    std::array<FlipCard, kFlipMaxSubCards> sub{};
    /// The small row has a colon after its first `sub_colons` groups, a gap after the rest.
    uint8_t sub_colons = 0;

    bool has_sub() const {
        return sub_shape.group_count > 0;
    }
};

/// Two hour and two minute cards for `seconds_of_day`: 24-hour with a leading zero, or
/// 12-hour with the leading card blank below 10 and an AM/PM marker.
FlipFace clock_face(int32_t seconds_of_day, bool use_24h);

/// One card per digit of the hours (no leading zero, as many as needed), minutes and seconds
/// for `seconds`, clamped to 0 and kClockMaxCountdownS.
FlipFace countdown_face(int32_t seconds);

/**
 * @brief The cards for `reading`
 *
 * No print: the time of day. A print shows its state on the cards and its details on the small
 * row: before an estimate the percent over the time of day; counting down, the time left over
 * the finish time and percent; paused PAUSE over the time left and percent; ended DONE, FAIL
 * or STOP over the percent reached. Where the reading does not spell states, one card shows
 * the state's symbol instead of the word (kGlyphPause, kGlyphCheck, kGlyphCross, kGlyphSquare).
 */
FlipFace flip_face(const ClockReading& reading);

/// ClockState read as cards: every decision is ClockState's, the cards are flip_face()'s.
class FlipClockState {
  public:
    /// Advances by `dt_ms` with the outside state now `in`.
    void step(uint32_t dt_ms, const ClockInput& in);
    ClockMode mode() const {
        return state_.mode();
    }
    /// Seconds the countdown shows.
    int32_t countdown_s() const {
        return state_.countdown_s();
    }
    /// What the screen shows for the last step's input.
    FlipFace face() const {
        return face_;
    }
    /// The clock reading the cards show.
    const ClockReading& reading() const {
        return state_.reading();
    }
    void reset();

  private:
    ClockState state_;
    FlipFace face_;
};

/// One row of cards' sizes.
struct FlipRowMetrics {
    int32_t unit = 0;  ///< glyph scale: pixels per glyph width unit
    int32_t pad_x = 0; ///< glyph inset in a card
    int32_t pad_y = 0;
    int32_t split_top = 0;    ///< first row of the split line, relative to a card's top
    int32_t split_bottom = 0; ///< first row below the split line
    int32_t corner = 0;       ///< card corner radius
};

/// Where the cards and their text sit on a screen.
struct FlipLayout {
    int32_t unit = 0; ///< main cards' glyph scale
    FlipShape shape;
    FlipShape sub_shape;
    uint8_t sub_colons = 0;
    uint8_t card_count = 0; ///< main cards, first in `cards`
    uint8_t sub_count = 0;  ///< small cards, after the main ones in `cards`
    std::array<DirtyRect, kFlipMaxCards + kFlipMaxSubCards> cards{};
    uint8_t colon_count = 0;
    /// The colons between two groups in a row (a column of main groups has none), the main
    /// row's first.
    std::array<DirtyRect, kFlipMaxGroups + 1> colons{};
    uint8_t main_colon_count = 0; ///< colons[0 .. main_colon_count) belong to the main row
    DirtyRect meridiem;           ///< AM/PM, in the first card's top-left corner
    FlipRowMetrics main;
    FlipRowMetrics small;
    // The main row's metrics, as most callers read them.
    int32_t split_top = 0;
    int32_t split_bottom = 0;
    int32_t corner = 0;

    /// The metrics of card `i`'s row.
    const FlipRowMetrics& row(size_t i) const {
        return i < card_count ? main : small;
    }

    /// Box around every card and the caption.
    DirtyRect bounds() const;
};

/// Lays out the cards of `shape` as large as fit on a `w` x `h` screen: every group in one
/// row with a colon between groups, or, when that makes them larger (portrait), one group per
/// row; the small row of `sub_shape` centred under them (a colon after its first `sub_colons`
/// groups); centred, then moved by (`dx`, `dy`).
FlipLayout layout_flip_cards(int32_t w, int32_t h, const FlipShape& shape,
                             const FlipShape& sub_shape, int32_t dx, int32_t dy,
                             uint8_t sub_colons = 1);

/// Box round the layout of `shape` centred on a `w` x `h` screen, undrifted: what the clock's
/// drift range is measured from (ClockDrift::set_range()).
DirtyRect flip_centred_bounds(int32_t w, int32_t h, const FlipShape& shape,
                              const FlipShape& sub_shape, uint8_t sub_colons = 1);

/**
 * @brief Draws the clock into a frame, redrawing only the cards that change
 *
 * Each digit has its own card. A card whose digit changes folds its top half down over
 * kFlipDurationMs at an animating level, or swaps at once otherwise; cards that change
 * together fold together, and the rest are not touched. A changed shape or a drift step
 * repaints the whole layout. Between changes a frame writes nothing.
 */
class FlipClockSim {
  public:
    static constexpr const auto& kLevels = kFlipClockLevels;

    /// Lays out `face` for `frame`, paints the screen and draws the cards.
    void init(FrameTarget& frame, size_t level, const FlipFace& face);

    /**
     * @brief Advances by `dt_ms` towards showing `face` and draws into `frame`
     *
     * Replaces `dirty` with one box per card redrawn, or one box over the old and new layout
     * after a repaint.
     */
    void step(uint32_t dt_ms, FrameTarget& frame, const FlipFace& face,
              std::vector<DirtyRect>& dirty);

    /// Draws card `i` again as it stands and adds its box to `dirty`, for text drawn over it
    /// that has to go.
    void redraw_card(FrameTarget& frame, size_t i, std::vector<DirtyRect>& dirty) const;

    /// Runs at `level` (clamped) from now on; a flip in progress finishes at once when the new
    /// level does not animate.
    void request_level(size_t level);

    size_t level() const {
        return level_;
    }
    const FlipLayout& layout() const {
        return layout_;
    }
    /// True while any card is mid-flip.
    bool flipping() const;
    /// Share of the flip done for card `i`, 1 when it is still.
    float flip_progress(size_t i) const;

    /// Frees the glyph cache.
    void clear();
    /// Bytes the glyph cache holds.
    size_t heap_bytes() const;

    /// Background, card, split-line and digit colours.
    static constexpr Rgb kBackground{0, 0, 0};
    static constexpr Rgb kCardTop{30, 30, 32};
    static constexpr Rgb kCardBottom{24, 24, 26};
    static constexpr Rgb kDigit{222, 222, 222};
    static constexpr Rgb kColon{130, 130, 130};
    /// The folding flap's edge band and its outermost row.
    static constexpr Rgb kFlapEdge{72, 72, 78};
    static constexpr Rgb kFlapRim{118, 118, 124};

  private:
    struct CardAnim {
        FlipCard from;
        FlipCard to;
        uint32_t elapsed_ms = kFlipDurationMs;
    };

    void relayout(FrameTarget& frame, const FlipFace& face, std::vector<DirtyRect>& dirty);
    /// Sizes the glyph sets for the layout and builds every character `face` shows.
    void build_glyphs(const FlipFace& face);
    void draw_card(FrameTarget& frame, size_t i) const;
    void draw_colons(FrameTarget& frame) const;
    /// Card face pixel at card-local (x, y) for `card`, before the corner mask.
    Rgb face_pixel(size_t i, const FlipCard& card, int32_t x, int32_t y) const;
    uint8_t corner_coverage(size_t i, int32_t x, int32_t y) const;

    size_t level_ = 0;
    int32_t frame_w_ = 0;
    int32_t frame_h_ = 0;
    ClockDrift drift_;
    /// The shape drift_'s range was set for.
    FlipShape drift_shape_;
    FlipShape drift_sub_shape_;
    /// The digits' colour for the face shown.
    Rgb ink_ = kDigit;
    FlipLayout layout_;
    /// One per card in layout order: the main row, then the small row.
    std::array<CardAnim, kFlipMaxCards + kFlipMaxSubCards> anims_{};
    /// Coverage of the characters shown, at the main and small rows' units.
    StrokeGlyphSet glyphs_;
    StrokeGlyphSet sub_glyphs_;
};

} // namespace helix::ui
