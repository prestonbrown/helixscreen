// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_clock_reading.h"

#include "screensaver_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace helix::ui {

ClockTime clock_time(int32_t seconds_of_day, bool use_24h) {
    const int32_t sod = screensaver::wrap_index(seconds_of_day, 86400);
    ClockTime t;
    t.hour_of_day = sod / 3600;
    t.minute = sod / 60 % 60;
    t.second = sod % 60;
    if (use_24h) {
        t.hour = t.hour_of_day;
    } else {
        t.hour = t.hour_of_day % 12 == 0 ? 12 : t.hour_of_day % 12;
        t.meridiem = t.hour_of_day < 12 ? ClockMeridiem::AM : ClockMeridiem::PM;
    }
    return t;
}

int32_t ClockState::countdown_s() const {
    return std::max(0, anchor_s_ - static_cast<int32_t>(since_anchor_ms_ / 1000));
}

void ClockState::reset() {
    *this = ClockState{};
}

void ClockState::step(uint32_t dt_ms, const ClockInput& in) {
    if (mode_ == ClockMode::COUNTDOWN) {
        since_anchor_ms_ += dt_ms;
    }
    if (mode_ == ClockMode::DONE) {
        done_ms_ += dt_ms;
    }
    const auto anchor = [this](int32_t seconds) {
        anchored_ = true;
        anchor_s_ = seconds;
        since_anchor_ms_ = 0;
    };

    const ClockPrint& print = in.print;
    switch (print.phase) {
    case ClockPrintPhase::PREPARING:
    case ClockPrintPhase::PRINTING:
        if (!anchored_ && print.remaining_s > 0) {
            anchor(print.remaining_s);
        } else if (anchored_ && print.remaining_s >= 0 &&
                   std::abs(print.remaining_s - countdown_s()) > kClockCountdownResyncS) {
            anchor(print.remaining_s);
        }
        mode_ = anchored_ ? ClockMode::COUNTDOWN : ClockMode::CLOCK;
        break;
    case ClockPrintPhase::PAUSED:
        if (!anchored_ && print.remaining_s > 0) {
            anchor(print.remaining_s);
        }
        if (anchored_) {
            anchor(countdown_s());
            mode_ = ClockMode::PAUSED;
        } else {
            mode_ = ClockMode::CLOCK;
        }
        break;
    case ClockPrintPhase::COMPLETE:
        if (mode_ == ClockMode::COUNTDOWN || mode_ == ClockMode::PAUSED) {
            mode_ = ClockMode::DONE;
            done_ms_ = 0;
        } else if (mode_ == ClockMode::DONE && done_ms_ >= kClockDoneHoldMs) {
            mode_ = ClockMode::CLOCK;
        }
        anchored_ = false;
        break;
    case ClockPrintPhase::NONE:
        mode_ = ClockMode::CLOCK;
        anchored_ = false;
        break;
    }

    reading_ = {};
    reading_.time = clock_time(in.seconds_of_day, in.use_24h);
    reading_.date = in.date;
    reading_.use_24h = in.use_24h;
    reading_.mode = mode_;
    switch (mode_) {
    case ClockMode::CLOCK:
        break;
    case ClockMode::COUNTDOWN: {
        reading_.countdown_s = countdown_s();
        const int32_t sod = screensaver::wrap_index(in.seconds_of_day, 86400);
        reading_.eta_minute_of_day =
            static_cast<int32_t>((static_cast<int64_t>(sod) + countdown_s()) / 60 % 1440);
        reading_.progress_pct = std::clamp(print.progress_pct, 0, 100);
        break;
    }
    case ClockMode::PAUSED:
        reading_.countdown_s = countdown_s();
        reading_.progress_pct = std::clamp(print.progress_pct, 0, 100);
        break;
    case ClockMode::DONE:
        reading_.progress_pct = 100;
        break;
    }
}

bool clock_face_same(const ClockReading& a, const ClockReading& b, bool shows_progress) {
    if (!(a.time == b.time) || a.use_24h != b.use_24h) {
        return false;
    }
    return !shows_progress || (a.mode == b.mode && a.progress_pct == b.progress_pct);
}

void clock_drift_range(const DirtyRect& bounds, int32_t w, int32_t h, int32_t& max_dx,
                       int32_t& max_dy) {
    const int32_t room_x = std::min(bounds.x1, w - 1 - bounds.x2) - 1;
    const int32_t room_y = std::min(bounds.y1, h - 1 - bounds.y2) - 1;
    max_dx = std::max(0, std::min(room_x, std::max(4, w / 25)));
    max_dy = std::max(0, std::min(room_y, std::max(4, h / 25)));
}

void clock_drift_at(uint32_t elapsed_ms, int32_t max_dx, int32_t max_dy, int32_t& dx, int32_t& dy) {
    const auto step = static_cast<float>(elapsed_ms / kClockDriftStepMs);
    // Two incommensurate rates, so the path does not retrace itself for hours.
    dx = static_cast<int32_t>(std::lround(static_cast<float>(max_dx) * std::sin(step * 0.61f)));
    dy = static_cast<int32_t>(std::lround(static_cast<float>(max_dy) * std::sin(step * 0.37f)));
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
