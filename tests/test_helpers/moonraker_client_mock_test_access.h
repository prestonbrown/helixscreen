// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_shared_callback.h"
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

    // The standalone IFS module mock's pushed objects, as the simulation tick
    // publishes them, and its toolhead switch reading.
    static nlohmann::json ifs_module_status(const MoonrakerClientMock& c) {
        return c.ifs_module_status_json();
    }
    static nlohmann::json ifs_module_vars(const MoonrakerClientMock& c) {
        return c.ifs_module_vars_json();
    }
    // The CFS `box` frame the mock publishes, as HELIX_MOCK_CFS_BOXES shapes it.
    static nlohmann::json cfs_box_status(const MoonrakerClientMock& c) {
        return c.cfs_box_status_json();
    }
    static bool ifs_module_head_loaded(const MoonrakerClientMock& c) {
        return c.ifs_module_head_loaded_.load();
    }

    // One notification interval of the MedusaHC swap simulation.
    static void advance_secondary_extruders(MoonrakerClientMock& c, double dt) {
        c.advance_secondary_extruders(dt);
    }
    static void advance_medusa(MoonrakerClientMock& c) {
        c.advance_medusa_swap();
    }

    // Simulation-loop iterations begun so far. Once it reads n + 1, iteration
    // n has pushed its status frame.
    static uint32_t tick_count(const MoonrakerClientMock& c) {
        return c.tick_count_.load();
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

    // Replace one registered method handler — the seam for refusal
    // injection, answering error_cb where the stock handler succeeds. The
    // registry is per-instance, so the override dies with the mock.
    // HELIX_MOCK_DRAGONBREATH_OFFLINE for a live client: the environment is
    // read once at construction, so a test crossing the transition flips it here.
    static void set_dragonbreath_offline(MoonrakerClientMock& c, bool offline) {
        c.dragonbreath_offline_ = offline;
    }

    static void set_method_handler(MoonrakerClientMock& c, const std::string& method,
                                   mock_internal::MethodHandler handler) {
        c.method_handlers_[method] = std::move(handler);
    }

    // The print state the mock's handlers check (0 standby, 1 printing,
    // 2 paused, 3 complete), without running a simulated print.
    static void set_print_state(MoonrakerClientMock& c, int state) {
        c.print_state_.store(state);
    }

    // Answer every directory listing with a disk that has no free space, as
    // Moonraker reports disk_usage beside any gcodes directory's entries.
    static void report_full_disk(MoonrakerClientMock& c) {
        set_method_handler(
            c, "server.files.get_directory",
            [](MoonrakerClientMock*, const nlohmann::json&,
               helix::SharedCallback<void(const nlohmann::json&)> success_cb,
               helix::SharedCallback<void(const MoonrakerError&)>) -> bool {
                if (success_cb) {
                    success_cb(nlohmann::json{
                        {"result",
                         {{"dirs", nlohmann::json::array()},
                          {"files", nlohmann::json::array()},
                          {"disk_usage",
                           {{"total", 1ULL << 30}, {"used", 1ULL << 30}, {"free", 0}}}}}});
                }
                return true;
            });
    }
};

} // namespace helix
