// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix {

/// What the SIGTERM handler does with one delivery.
enum class SigtermAction {
    FastExit,     ///< Skip teardown and _exit(0)
    GracefulQuit, ///< Request the main loop's quit so teardown can send the owed cleanup
    Ignore,       ///< The same stop delivered twice
};

/// One systemctl stop reaches the app twice: systemd signals the whole cgroup and
/// helix-watchdog forwards its own SIGTERM to the child. Inside this window a repeat
/// is that duplicate; after it, a repeat means the graceful quit is stuck.
inline constexpr uint32_t kSigtermRepeatWindowMs = 2000;

/// Nonzero stamp for a graceful SIGTERM, where zero means none has been taken.
constexpr uint32_t sigterm_stamp(uint32_t now_ms) noexcept {
    return now_ms | 1u;
}

/// Time since `stamp`. Stamps `now_ms` first, so a repeat in the same millisecond reads as 0.
constexpr uint32_t ms_since_sigterm_stamp(uint32_t now_ms, uint32_t stamp) noexcept {
    return sigterm_stamp(now_ms) - stamp;
}

/// Pure and async-signal-safe.
/// @param cleanup_pending The printer is owed a cleanup (a heater off, a restored state) only this
/// app will send
/// @param graceful_underway An earlier SIGTERM already chose GracefulQuit
/// @param ms_since_graceful Time since that SIGTERM; unused otherwise
constexpr SigtermAction sigterm_action(bool cleanup_pending, bool graceful_underway,
                                       uint32_t ms_since_graceful) noexcept {
    if (graceful_underway) {
        return ms_since_graceful < kSigtermRepeatWindowMs ? SigtermAction::Ignore
                                                          : SigtermAction::FastExit;
    }
    return cleanup_pending ? SigtermAction::GracefulQuit : SigtermAction::FastExit;
}

} // namespace helix
