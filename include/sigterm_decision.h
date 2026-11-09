// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix {

/// What the SIGTERM handler does with one delivery.
enum class SigtermAction {
    FastExit,     ///< Skip teardown and _exit(0)
    GracefulQuit, ///< Request the main loop's quit so teardown can send the heater off
    Ignore,       ///< The same stop delivered twice
};

/// One systemctl stop reaches the app twice: systemd signals the whole cgroup and
/// helix-watchdog forwards its own SIGTERM to the child. Inside this window a repeat
/// is that duplicate; after it, a repeat means the graceful quit is stuck.
inline constexpr uint32_t kSigtermRepeatWindowMs = 2000;

/// Pure and async-signal-safe.
/// @param heater_cleanup_pending A heater is on that only this app will turn off
/// @param graceful_underway An earlier SIGTERM already chose GracefulQuit
/// @param ms_since_graceful Time since that SIGTERM; unused otherwise
constexpr SigtermAction sigterm_action(bool heater_cleanup_pending, bool graceful_underway,
                                       uint32_t ms_since_graceful) noexcept {
    if (graceful_underway) {
        return ms_since_graceful < kSigtermRepeatWindowMs ? SigtermAction::Ignore
                                                          : SigtermAction::FastExit;
    }
    return heater_cleanup_pending ? SigtermAction::GracefulQuit : SigtermAction::FastExit;
}

} // namespace helix
