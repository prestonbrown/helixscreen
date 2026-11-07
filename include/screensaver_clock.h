// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "print_lifecycle_state.h"
#include "screensaver_canvas.h"
#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"

#include <cstdint>
#include <lvgl.h>
#include <string>
#include <vector>

/**
 * @file screensaver_clock.h
 * @brief The app side of the clock savers: the local time, the print, and their text
 *
 * The pure decision is helix::ui::ClockState; this file reads the wall clock and the Time
 * Format setting, maps the print lifecycle, turns a reading into translated text, and draws
 * text onto a saver canvas.
 */

namespace helix {
enum class PrintOutcome;
} // namespace helix

namespace helix::ui {

/// The local time of day, the date and the 12/24-hour setting, with no print.
ClockInput saver_clock_now();

/// The print phase for the app's lifecycle `state`. An idle printer reports how its last print
/// ended (`outcome`, which persists until the next print), so a printer that goes back to
/// standby after a print still reads that end.
ClockPrintPhase clock_print_phase(PrintState state, PrintOutcome outcome);

/// The print as two short lines: "ETA 9:41 PM" and "1h 12m left" while counting down,
/// "PAUSED" and the time left while paused, "DONE" and nothing when done, both empty for the
/// time of day.
struct ClockPrintText {
    std::string eta;
    std::string left;
};
ClockPrintText clock_print_text(const ClockReading& reading);

/// The date as a long date and a weekday, through locale_formats: "October 21, 2019" and
/// "Monday" in English.
struct ClockDateText {
    std::string date;
    std::string weekday;
};
ClockDateText clock_date_text(const ClockDate& date);

/// True when `a` and `b` give the same clock_print_text, clock_date_text and
/// clock_meridiem_text, so a saver can keep the strings it built instead of building them
/// again every frame.
bool clock_text_same(const ClockReading& a, const ClockReading& b);

/// The two lines a clock face shows under its time, and its AM/PM: the print's ETA and time
/// left while a print is on the readout, else the date and weekday.
struct ClockLines {
    std::string first;
    std::string second;
    std::string meridiem;
};
ClockLines clock_lines(const ClockReading& reading);

/// clock_lines() for the last reading asked about, built again only when clock_text_same()
/// says the text changed.
class ClockTextCache {
  public:
    const ClockLines& get(const ClockReading& reading);
    /// Forgets the text and frees its strings.
    void reset();

  private:
    ClockLines lines_;
    ClockReading built_for_;
    bool built_ = false;
};

/// The C library's AM/PM for `meridiem`, empty for NONE.
std::string clock_meridiem_text(ClockMeridiem meridiem);

/// Local minute `minute_of_day` (0 to 1439) as the app shows a time of day: "9:41 PM" or
/// "21:41", per the Time Format setting.
std::string clock_minute_text(int32_t minute_of_day);

/// Largest breakpoint text font no taller than `height` px, or the smallest when none fits.
const lv_font_t* clock_font_fitting(int32_t height);

/**
 * @brief One line of text a saver keeps on its canvas over pixels it redraws itself
 *
 * draw() puts the text back whenever it changed, its box moved, or a box in `under` (the
 * pixels the frame just rewrote) crosses it, so text survives a scene repainting beneath it.
 * Removing old text is the scene's job: the caller repaints the old box before draw() when
 * the text changes or moves.
 */
/// A soft shadow a SaverLabel draws under its text: the text in `color`, smeared `radius` px
/// round in every direction at about `opa` all told.
struct SaverLabelHalo {
    Rgb color{};
    lv_opa_t opa = LV_OPA_TRANSP;
    int32_t radius = 0;
};

class SaverLabel {
  public:
    /// Draws a soft shadow under the text from now on; the text then reaches `halo.radius`
    /// px outside its box, which is what draw() adds to `dirty`.
    void set_halo(const SaverLabelHalo& halo) {
        halo_ = halo;
    }

    /// True when draw() would draw: the text or its box changed, or a box in `under` crosses
    /// the box (with the halo's reach).
    bool needs_draw(const std::string& text, const DirtyRect& box,
                    const std::vector<DirtyRect>& under) const;

    /// Draws `text` into `box` if needed and adds `box` to `dirty` when it does. Returns true
    /// when it drew.
    bool draw(SaverCanvas& canvas, const std::string& text, const DirtyRect& box, Rgb color,
              lv_opa_t opa, lv_text_align_t align, const std::vector<DirtyRect>& under,
              std::vector<DirtyRect>& dirty);

    /// The box last drawn into, empty when nothing is drawn.
    const DirtyRect& box() const {
        return box_;
    }
    const std::string& text() const {
        return text_;
    }
    /// Forgets what was drawn, so the next draw() draws.
    void reset();

  private:
    std::string text_;
    DirtyRect box_;
    SaverLabelHalo halo_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
