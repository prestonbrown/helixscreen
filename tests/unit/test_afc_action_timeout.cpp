// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// AFC stuck-action timeout (#1188).
//
// AFC's busy state is re-derived from the firmware-echoed state string on every
// status frame, so when the terminating frame never arrives — a macro that
// silently never completes, a WebSocket bounce mid-toolchange, a Klipper
// shutdown — the UI stays pinned in a busy state forever. A per-action ERROR
// budget unsticks it.
//
// The latch is the AFC-specific part: setting ERROR alone would be undone by the
// very next frame, whose "Loading" maps straight back to LOADING and restarts
// the clock, flapping between busy and ERROR indefinitely. These tests pin the
// latch, its release conditions, and the fact that the clock is stamped once per
// frame rather than inside apply_state_string().
//
// The clock is faked by back-dating action_start_time_ — no sleeps.

#include "ams_backend_afc.h"
#include "ams_types.h"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace std::chrono;

class AfcActionTimeoutHelper : public AmsBackendAfc {
  public:
    /// @param with_lanes false leaves the slot registry uninitialized, which is
    ///        the shape get_system_info() short-circuits on.
    explicit AfcActionTimeoutHelper(bool with_lanes = true) : AmsBackendAfc(nullptr, nullptr) {
        if (with_lanes) {
            std::vector<std::string> lanes{"lane1", "lane2"};
            initialize_slots(lanes);
        }
    }

    /// Feed a status frame carrying only AFC.current_state, the authoritative
    /// state field. Goes through the real handle_status_update() path.
    void feed_state(const std::string& current_state) {
        nlohmann::json afc;
        afc["current_state"] = current_state;
        feed_afc(afc);
    }

    void feed_afc(const nlohmann::json& afc_data) {
        nlohmann::json params;
        params["AFC"] = afc_data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    /// Back-date the action clock so the budget has "elapsed" without sleeping.
    void age_action(seconds elapsed) {
        std::lock_guard<std::mutex> lock(mutex_);
        action_start_time_ = steady_clock::now() - elapsed;
    }

    [[nodiscard]] AmsAction action() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return system_info_.action;
    }

    [[nodiscard]] std::string detail() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return system_info_.operation_detail;
    }

    [[nodiscard]] bool latched() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timed_out_state_.has_value();
    }

    [[nodiscard]] steady_clock::time_point action_stamp() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return action_start_time_;
    }
};

namespace {

bool ends_with_timed_out(const std::string& s) {
    const std::string suffix = " (timed out)";
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

// ============================================================================
// Budgets
// ============================================================================

TEST_CASE("AFC busy state past its budget surfaces ERROR", "[ams][afc][timeout]") {
    AfcActionTimeoutHelper h;

    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::LOADING);

    // LOADING budget is 180s.
    h.age_action(seconds(200));
    h.feed_state("Loading");

    REQUIRE(h.action() == AmsAction::ERROR);
    // The detail names what was happening when the budget blew, so the error
    // modal is not a bare "something failed".
    REQUIRE(ends_with_timed_out(h.detail()));
}

TEST_CASE("AFC busy state under its budget stays busy", "[ams][afc][timeout]") {
    AfcActionTimeoutHelper h;

    h.feed_state("Loading");
    h.age_action(seconds(60));
    h.feed_state("Loading");

    REQUIRE(h.action() == AmsAction::LOADING);
    REQUIRE_FALSE(h.latched());
}

TEST_CASE("AFC UNLOADING shares the load/unload budget", "[ams][afc][timeout]") {
    AfcActionTimeoutHelper h;

    h.feed_state("Unloading");
    REQUIRE(h.action() == AmsAction::UNLOADING);

    h.age_action(seconds(120)); // past the 120s default, under the 180s pair budget
    h.feed_state("Unloading");
    REQUIRE(h.action() == AmsAction::UNLOADING);

    h.age_action(seconds(200));
    h.feed_state("Unloading");
    REQUIRE(h.action() == AmsAction::ERROR);
}

TEST_CASE("AFC SELECTING gets the long toolchange budget", "[ams][afc][timeout]") {
    // A real BoxTurtle toolchange (cut + poop + kick + brush + purge) measured
    // 67s; AFC's ToolSwap/ToolDock/ToolPickup/Moving/Restoring all land on
    // SELECTING. AD5X's busy list omits SELECTING entirely, so this budget is
    // the AFC-specific half of the port.
    AfcActionTimeoutHelper h;

    h.feed_state("ToolSwap");
    REQUIRE(h.action() == AmsAction::SELECTING);

    // 200s is past every other budget except SELECTING's 300s.
    h.age_action(seconds(200));
    h.feed_state("ToolSwap");
    REQUIRE(h.action() == AmsAction::SELECTING);

    h.age_action(seconds(400));
    h.feed_state("ToolSwap");
    REQUIRE(h.action() == AmsAction::ERROR);
}

TEST_CASE("AFC PAUSED never times out", "[ams][afc][timeout]") {
    // PAUSED means AFC is waiting on the user (clear a jam, swap a spool).
    // That is legitimately indefinite; failing it into ERROR would discard a
    // prompt the user is in the middle of answering.
    AfcActionTimeoutHelper h;

    h.feed_state("Paused");
    REQUIRE(h.action() == AmsAction::PAUSED);

    h.age_action(hours(4));
    h.feed_state("Paused");
    REQUIRE(h.action() == AmsAction::PAUSED);

    h.age_action(hours(4));
    REQUIRE(h.get_system_info().action == AmsAction::PAUSED);
    REQUIRE_FALSE(h.latched());
}

TEST_CASE("AFC IDLE never times out", "[ams][afc][timeout]") {
    AfcActionTimeoutHelper h;

    h.feed_state("Idle");
    h.age_action(hours(1));
    h.feed_state("Idle");

    REQUIRE(h.action() == AmsAction::IDLE);
    REQUIRE_FALSE(h.latched());
}

// ============================================================================
// Latch — the AFC-specific anti-flap
// ============================================================================

TEST_CASE("AFC repeating the timed-out state keeps ERROR and does not restart the clock",
          "[ams][afc][timeout]") {
    // Without the latch, the next frame's "Loading" maps straight back to
    // LOADING, the frame-level action change restarts the clock, and the
    // backend flaps busy -> ERROR -> busy forever: a modal storm.
    AfcActionTimeoutHelper h;

    h.feed_state("Loading");
    h.age_action(seconds(200));
    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::ERROR);
    REQUIRE(h.latched());

    const auto stamp_after_timeout = h.action_stamp();
    const std::string detail_after_timeout = h.detail();

    // A real feed repeats the state on every delta.
    for (int i = 0; i < 6; ++i) {
        h.feed_state("Loading");
        REQUIRE(h.action() == AmsAction::ERROR);
    }

    REQUIRE(h.latched());
    REQUIRE(h.action_stamp() == stamp_after_timeout);
    REQUIRE(h.detail() == detail_after_timeout);
}

TEST_CASE("AFC reporting a different state releases the timeout latch", "[ams][afc][timeout]") {
    AfcActionTimeoutHelper h;

    h.feed_state("Loading");
    h.age_action(seconds(200));
    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::ERROR);
    REQUIRE(h.latched());

    // AFC moved on — the stuck operation resolved (or was replaced). The
    // backend must follow AFC again rather than pinning ERROR.
    h.feed_state("Idle");
    REQUIRE_FALSE(h.latched());
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC latch release re-arms a full budget for the next busy state",
          "[ams][afc][timeout]") {
    AfcActionTimeoutHelper h;

    h.feed_state("Loading");
    h.age_action(seconds(200));
    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::ERROR);

    // A different busy state must get its own clock, not inherit the blown one.
    h.feed_state("Unloading");
    REQUIRE_FALSE(h.latched());
    REQUIRE(h.action() == AmsAction::UNLOADING);

    h.feed_state("Unloading");
    REQUIRE(h.action() == AmsAction::UNLOADING);
}

TEST_CASE("AFC clear_fault drops the timeout latch", "[ams][afc][timeout]") {
    // The user dismissed the error modal; AFC gets RESET_FAILURE +
    // AFC_CLEAR_MESSAGE. If AFC is genuinely still stuck the clock simply
    // restarts and re-fires after a full budget, which is bounded.
    AfcActionTimeoutHelper h;

    h.feed_state("Loading");
    h.age_action(seconds(200));
    h.feed_state("Loading");
    REQUIRE(h.latched());

    h.clear_fault(0);
    REQUIRE_FALSE(h.latched());

    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::LOADING);
}

// ============================================================================
// Where the clock is stamped
// ============================================================================

TEST_CASE("AFC repeating the same state does not restart the timeout clock",
          "[ams][afc][timeout]") {
    // The stamp lives in parse_afc_state(), once per frame, gated on the action
    // actually changing across the whole frame. Moving it into
    // apply_state_string() — which runs on every frame and up to twice within
    // one — would restart the clock on every AFC delta and the budget could
    // never elapse.
    AfcActionTimeoutHelper h;

    h.feed_state("Loading");

    // 40s, 80s, 120s, 160s: four repeats, all under the 180s budget.
    for (int i = 1; i <= 4; ++i) {
        h.age_action(seconds(40 * i));
        h.feed_state("Loading");
        REQUIRE(h.action() == AmsAction::LOADING);
    }

    // 200s of aging with the same state repeating throughout: the budget must
    // still blow.
    h.age_action(seconds(200));
    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::ERROR);
}

TEST_CASE("AFC legacy 'status' and authoritative 'current_state' in one frame share a clock",
          "[ams][afc][timeout]") {
    // parse_afc_state() applies "status" then "current_state" within a single
    // frame. A per-apply_state_string() stamp would see those two disagree and
    // reset the clock on every frame; the frame-level comparison does not.
    AfcActionTimeoutHelper h;

    nlohmann::json afc;
    afc["status"] = "Idle";
    afc["current_state"] = "Loading";
    h.feed_afc(afc);
    REQUIRE(h.action() == AmsAction::LOADING);

    h.age_action(seconds(200));
    h.feed_afc(afc);
    REQUIRE(h.action() == AmsAction::ERROR);
}

// ============================================================================
// Watchdog path — no status update at all
// ============================================================================

TEST_CASE("AFC get_system_info surfaces the timeout with no status update", "[ams][afc][timeout]") {
    // The sidebar's 1.5s stall watchdog polls get_system_info() whenever the
    // action is non-IDLE. That is the only clock AFC has when the printer goes
    // silent mid-operation, so the check must run there too.
    AfcActionTimeoutHelper h;

    h.feed_state("Loading");
    h.age_action(seconds(200));

    auto info = h.get_system_info();
    REQUIRE(info.action == AmsAction::ERROR);
    REQUIRE(ends_with_timed_out(info.operation_detail));
    REQUIRE(h.latched());
}

TEST_CASE("AFC get_system_info runs the check before the uninitialized-slots early return",
          "[ams][afc][timeout]") {
    // get_system_info() returns system_info_ verbatim when the slot registry is
    // empty. Putting the check after that early return would skip the watchdog
    // path entirely on a system whose lanes never got discovered.
    AfcActionTimeoutHelper h(/*with_lanes=*/false);

    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::LOADING);

    h.age_action(seconds(200));
    REQUIRE(h.get_system_info().action == AmsAction::ERROR);
}
