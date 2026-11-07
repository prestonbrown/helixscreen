// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_registry.h"
#include "screensaver_stroke_digits.h"

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @file screensaver_print_card.h
 * @brief A compact card showing a print's progress, drawn over savers with no display of their own
 *
 * A ring fills with progress in the state colour beside large stroke-drawn text: the time left,
 * or the percentage before an estimate exists. An ended or paused print draws a mark in the
 * ring. No LVGL and no text rendering: the card draws in either pixel format.
 */

namespace helix::ui {

/// The card's size on a screen.
struct PrintCardLayout {
    int32_t w = 0;
    int32_t h = 0;
    int32_t ring_d = 0; ///< ring diameter
    int32_t pad = 0;    ///< margin round the ring and the text
    int32_t unit = 0;   ///< stroke digit glyph unit
};

/// The card for a `screen_w` x `screen_h` screen.
PrintCardLayout layout_print_card(int32_t screen_w, int32_t screen_h);

/// Where the card sits before drift: centred, a card's height up from the bottom.
DirtyRect print_card_home(int32_t screen_w, int32_t screen_h, const PrintCardLayout& layout);

/// What the card shows; equal faces draw identical pixels.
struct PrintCardFace {
    ClockMode mode = ClockMode::CLOCK;
    int32_t progress_pct = 0;
    int32_t countdown_s = 0;
    bool has_countdown = false;

    bool operator==(const PrintCardFace& o) const {
        return mode == o.mode && progress_pct == o.progress_pct && countdown_s == o.countdown_s &&
               has_countdown == o.has_countdown;
    }
    bool operator!=(const PrintCardFace& o) const {
        return !(*this == o);
    }
};

/// The card face for `r`, its countdown rounded to the whole minutes the text shows.
PrintCardFace print_card_face(const ClockReading& r);

/// The card's text: the time left as 1h05 from an hour up (hours shown to 99) and as 23m below
/// it, never under 1m; or 63% without an estimate and for an ended print. Unit letters, not a
/// colon, so a time left never reads as a time of day.
std::string print_card_text(const PrintCardFace& face);

/// Whether a saver that shows a print through `display` shows the card for `mode`: paused and
/// ended prints always, a running print only with the setting on, a saver with its own print
/// display never.
constexpr bool print_card_visible(PrintDisplay display, bool setting_on, ClockMode mode) {
    if (display != PrintDisplay::CARD) {
        return false;
    }
    if (mode == ClockMode::PAUSED || clock_mode_is_end(mode)) {
        return true;
    }
    return setting_on && (mode == ClockMode::PROGRESS || mode == ClockMode::COUNTDOWN);
}

/// Draws card faces at one layout.
class PrintCardPainter {
  public:
    /// Builds the digit glyphs for `layout`.
    void init(const PrintCardLayout& layout);
    /// Paints the whole card, `layout.w` x `layout.h`, into `card`.
    void draw(FrameTarget& card, const PrintCardFace& face) const;
    void clear();
    /// Bytes the glyph cache holds.
    size_t heap_bytes() const {
        return glyphs_.heap_bytes();
    }

    static constexpr Rgb kBackground{16, 16, 18};
    static constexpr Rgb kTrack{48, 48, 52};
    static constexpr Rgb kAccent{120, 180, 255};
    static constexpr Rgb kText{222, 222, 222};

  private:
    PrintCardLayout layout_;
    StrokeDigitSet glyphs_;
};

} // namespace helix::ui
