// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/auto_update_schedule.h"

namespace helix::update {

namespace {
constexpr int kMinutesPerDay = 24 * 60;
}

bool in_nightly_window(int minute_of_day, const NightlyWindow& window) {
    if (window.length_minutes <= 0 || minute_of_day < 0 || minute_of_day >= kMinutesPerDay) {
        return false;
    }
    if (window.length_minutes >= kMinutesPerDay) {
        return true;
    }
    const int offset =
        ((minute_of_day - window.start_minute) % kMinutesPerDay + kMinutesPerDay) % kMinutesPerDay;
    return offset < window.length_minutes;
}

AutoUpdateDecision decide_auto_update(const AutoUpdateInputs& in) {
    if (!in.update_available || in.mode == AutoUpdateMode::Off) {
        return AutoUpdateDecision::Wait;
    }
    if (in.mode == AutoUpdateMode::DownloadAndAsk) {
        return AutoUpdateDecision::Download;
    }
    if (!in_nightly_window(in.local_minute_of_day, in.window)) {
        return AutoUpdateDecision::Wait;
    }
    // A job or a pending power-loss resume owns tonight: restarting the screen
    // would hide the resume offer or the print's controls.
    if (job_holds_machine(in.lifecycle) || in.plr_pending) {
        return AutoUpdateDecision::RetryTomorrow;
    }
    // Someone is at the screen. The window may still have time once they leave.
    if (in.seconds_since_touch < kAutoUpdateIdleSeconds) {
        return AutoUpdateDecision::Wait;
    }
    return AutoUpdateDecision::InstallNow;
}

const char* decision_name(AutoUpdateDecision d) {
    switch (d) {
    case AutoUpdateDecision::Wait:
        return "wait";
    case AutoUpdateDecision::Download:
        return "download";
    case AutoUpdateDecision::InstallNow:
        return "install_now";
    case AutoUpdateDecision::RetryTomorrow:
        return "retry_tomorrow";
    }
    return "unknown";
}

} // namespace helix::update
