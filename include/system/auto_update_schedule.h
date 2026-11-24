// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_lifecycle_state.h"

#include <cstdint>

namespace helix::update {

/// The Automatic updates setting. Values are the dropdown index and the persisted int,
/// ordered by how much the screen does on its own so that Install overnight is the
/// last entry and a picker without it is the same list minus its tail.
enum class AutoUpdateMode : int {
    Off = 0,              ///< Never act on an update unprompted
    DownloadAndAsk = 1,   ///< Tell the user an update is ready; they confirm the install
    InstallOvernight = 2, ///< Install in the nightly window when idle and untouched
};

/// What the scheduler wants done with an available update right now.
enum class AutoUpdateDecision {
    Wait,          ///< Nothing to do now; look again later
    Download,      ///< Offer the update to the user (Download and ask)
    InstallNow,    ///< Run the download and install path without the confirm step
    RetryTomorrow, ///< The window is open but the printer is busy; skip tonight
};

/// The nightly window, in minutes after local midnight. It may wrap past midnight.
struct NightlyWindow {
    int start_minute = 3 * 60; ///< 03:00 local
    int length_minutes = 60;
};

/// Minimum time without a touch before an install may restart the screen.
inline constexpr int64_t kAutoUpdateIdleSeconds = 30 * 60;

struct AutoUpdateInputs {
    AutoUpdateMode mode = AutoUpdateMode::DownloadAndAsk;
    bool update_available = false; ///< An installable, forward, verified release is cached
    int local_minute_of_day = 0;   ///< 0..1439, by the screen's timezone
    NightlyWindow window{};
    PrintState lifecycle = PrintState::Idle;
    bool plr_pending = false;        ///< A power-loss resume is waiting to be offered
    int64_t seconds_since_touch = 0; ///< Time since the last touch on the screen
};

/// The mode the scheduler runs for a stored setting. Install overnight is only
/// offered where a failed install rolls itself back; elsewhere a stored Install
/// overnight runs as Download and ask. Out-of-range values run as Download and ask.
AutoUpdateMode effective_auto_update_mode(int stored, bool install_overnight_offered);

/// Is @p minute_of_day inside @p window? Handles a window that wraps past midnight.
bool in_nightly_window(int minute_of_day, const NightlyWindow& window);

/// The whole scheduling rule: pure, no clocks, no globals.
AutoUpdateDecision decide_auto_update(const AutoUpdateInputs& in);

const char* decision_name(AutoUpdateDecision d);

} // namespace helix::update
