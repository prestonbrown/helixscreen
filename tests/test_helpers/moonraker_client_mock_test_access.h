// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_client_mock.h"

namespace helix {

// Grants tests access to MoonrakerClientMock internals that have no public
// equivalent. Declared a friend of MoonrakerClientMock (see
// moonraker_client_mock.h). Follows the existing TestAccess pattern
// (tests/test_helpers/, cf. MoonrakerClientTestAccess) rather than adding
// production _for_testing() accessors.
class MoonrakerClientMockTestAccess {
  public:
    // The cached status key the mock uses for chamber-heater frames
    // (e.g. "heater_generic dragonbreath"). Reading it directly pins the
    // registry-consulting cache without guessing from dispatched frames.
    static std::string chamber_heater_status_key(const MoonrakerClientMock& c) {
        return c.chamber_heater_status_key();
    }

    // Synchronously dispatch the initial subscription state to registered
    // notify callbacks — the same frames connect() emits, without the
    // 250 ms connect delay, historical-temperature burst, and simulation
    // thread that make connect()-based assertions racy.
    static void dispatch_initial_state(MoonrakerClientMock& c) {
        c.dispatch_initial_state();
    }

    // Replay cursor control. start_replay_timer() puts the origin 2.5s into the
    // future and drives pump_replay() from an lv_timer; setting the origin and
    // pumping by hand reaches the same code with neither wait nor timer.
    static void set_replay_start(MoonrakerClientMock& c,
                                 std::chrono::steady_clock::time_point when) {
        c.replay_start_ = when;
    }

    static void pump_replay(MoonrakerClientMock& c) {
        c.pump_replay();
    }

    // How many armed events have fired so far.
    static size_t replay_next(const MoonrakerClientMock& c) {
        return c.replay_next_;
    }
};

} // namespace helix
