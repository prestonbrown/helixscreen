// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_frame.h"

#include <cstdint>

/**
 * @file screensaver_clock_reading.h
 * @brief What a clock saver's readout says: the time, the date and the print, without pixels
 *
 * Shared by the flip clock, Aqua Surface and High Fly, which each draw it their own way. No
 * LVGL, no clock: the saver passes the local time, the date and the print each frame.
 */

namespace helix::ui {

/// A remaining-time estimate this far from the running countdown resets it; nearer ones are
/// ignored, so estimator jitter never runs the seconds backwards.
inline constexpr int32_t kClockCountdownResyncS = 10;
/// Longest countdown a readout shows: 999:59:59.
inline constexpr int32_t kClockMaxCountdownS = 999 * 3600 + 59 * 60 + 59;
/// How often a clock drifts to a new position, so no pixel stays lit in one place.
inline constexpr uint32_t kClockDriftStepMs = 60000;

/// What the print is doing, as far as a clock cares.
enum class ClockPrintPhase : uint8_t {
    NONE,
    PREPARING,
    PRINTING,
    PAUSED,
    COMPLETE,
    FAILED,
    CANCELLED
};

/// The print, as a clock reads it.
struct ClockPrint {
    ClockPrintPhase phase = ClockPrintPhase::NONE;
    int32_t remaining_s = -1; ///< print time left, negative when unknown
    int32_t progress_pct = 0;
};

/// The local calendar date.
struct ClockDate {
    int32_t year = 1970;
    uint8_t month = 1;   ///< 1 to 12
    uint8_t day = 1;     ///< 1 to 31
    uint8_t weekday = 4; ///< 0 = Sunday, as struct tm's tm_wday
};

/// One frame's worth of outside state.
struct ClockInput {
    int32_t seconds_of_day = 0; ///< local wall clock, 0 to 86399
    bool use_24h = false;
    ClockDate date;
    ClockPrint print;
};

/// What the readout shows: the time, a running print's progress (PROGRESS before a remaining-time
/// estimate exists, COUNTDOWN after), a paused print, or how a print ended.
enum class ClockMode : uint8_t { CLOCK, PROGRESS, COUNTDOWN, PAUSED, DONE, FAILED, CANCELLED };

/// True for the modes that say how a print ended: DONE, FAILED and CANCELLED.
constexpr bool clock_mode_is_end(ClockMode m) {
    return m == ClockMode::DONE || m == ClockMode::FAILED || m == ClockMode::CANCELLED;
}

/// The 12-hour marker.
enum class ClockMeridiem : uint8_t { NONE, AM, PM };

/// The time of day as a clock shows it.
struct ClockTime {
    int32_t hour = 0;        ///< as shown: 0 to 23, or 1 to 12 in 12-hour mode
    int32_t hour_of_day = 0; ///< 0 to 23 whatever the format
    int32_t minute = 0;
    int32_t second = 0;
    ClockMeridiem meridiem = ClockMeridiem::NONE; ///< NONE in 24-hour mode

    bool operator==(const ClockTime& o) const {
        return hour == o.hour && hour_of_day == o.hour_of_day && minute == o.minute &&
               second == o.second && meridiem == o.meridiem;
    }
};

/// `seconds_of_day` (wrapped into one day) as a clock shows it.
ClockTime clock_time(int32_t seconds_of_day, bool use_24h);

/// Everything a clock readout says for one frame.
struct ClockReading {
    ClockTime time;
    ClockDate date;
    bool use_24h = false;
    ClockMode mode = ClockMode::CLOCK;
    int32_t countdown_s = 0; ///< COUNTDOWN and PAUSED: seconds left; 0 otherwise
    /// COUNTDOWN, and PAUSED once an estimate has been seen: countdown_s is a real time left
    bool has_countdown = false;
    int32_t eta_minute_of_day = 0; ///< COUNTDOWN: local minute the print ends, 0 to 1439
    /// PROGRESS, COUNTDOWN and PAUSED: 0 to 100; DONE: 100; FAILED and CANCELLED: the last
    /// running value
    int32_t progress_pct = 0;
};

/**
 * @brief Decides what a clock reads from the print's state over time
 *
 * No print reads the time of day. A running print reads PROGRESS until it has a remaining-time
 * estimate, then counts down at wall-clock rate, frozen while paused. An ended print reads DONE,
 * FAILED or CANCELLED for as long as the input reports it; how long that is the caller's
 * decision (PrintEndLatch).
 */
class ClockState {
  public:
    /// Advances by `dt_ms` with the outside state now `in`.
    void step(uint32_t dt_ms, const ClockInput& in);

    ClockMode mode() const {
        return mode_;
    }
    /// Seconds the countdown shows.
    int32_t countdown_s() const;
    /// The readout for the last step's input.
    const ClockReading& reading() const {
        return reading_;
    }
    void reset();

  private:
    ClockMode mode_ = ClockMode::CLOCK;
    bool anchored_ = false;
    int32_t anchor_s_ = 0;
    uint32_t since_anchor_ms_ = 0;
    int32_t last_progress_pct_ = 0;
    ClockReading reading_;
};

/// True when `a` and `b` draw the same clock face: the same time in the same format and, for a
/// face that shows the print (`shows_progress`), the same mode and progress.
bool clock_face_same(const ClockReading& a, const ClockReading& b, bool shows_progress);

/// Largest drift either way that keeps a clock whose undrifted box is `bounds` on a `w` x `h`
/// screen with a pixel to spare, and never more than a 25th of the screen (4 px at least).
void clock_drift_range(const DirtyRect& bounds, int32_t w, int32_t h, int32_t& max_dx,
                       int32_t& max_dy);

/// Drift offset `elapsed_ms` into a run, stepping every kClockDriftStepMs within
/// +-`max_dx`, +-`max_dy`.
void clock_drift_at(uint32_t elapsed_ms, int32_t max_dx, int32_t max_dy, int32_t& dx, int32_t& dy);

/**
 * @brief Where a clock sits as it drifts, so no pixel stays lit in one place
 *
 * set_range() takes the clock's undrifted box and the screen (clock_drift_range()); call it at
 * init and whenever the face's size changes. step() moves the run's time on and the offset
 * with it (clock_drift_at()).
 */
struct ClockDrift {
    uint32_t elapsed_ms = 0;
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t max_dx = 0;
    int32_t max_dy = 0;

    void set_range(const DirtyRect& bounds, int32_t w, int32_t h) {
        clock_drift_range(bounds, w, h, max_dx, max_dy);
    }

    /// Advances by `dt_ms`; true when the offset moved.
    bool step(uint32_t dt_ms) {
        elapsed_ms += dt_ms;
        int32_t nx = 0;
        int32_t ny = 0;
        clock_drift_at(elapsed_ms, max_dx, max_dy, nx, ny);
        const bool moved = nx != dx || ny != dy;
        dx = nx;
        dy = ny;
        return moved;
    }

    /// Back to the start of a run: no time, no offset, no range.
    void reset() {
        *this = ClockDrift{};
    }
};

} // namespace helix::ui
