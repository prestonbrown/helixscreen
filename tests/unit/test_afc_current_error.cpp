// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_current_error.cpp
 * @brief AFC status-driven fault surfacing (#1171).
 *
 * AFC was the only backend not wired into AmsErrorBridge's status-driven error
 * path: it implemented neither current_error() nor anything that set
 * AmsAction::ERROR from its own error latch, so the bridge fired and found
 * nullopt every time.
 *
 * The trigger itself was never broken, contrary to the issue's premise.
 * Upstream AFC_error.py:151-157 is the ONLY writer of afc.error_state and
 * assigns current_state = State.ERROR on the adjacent line:
 *
 *     self.afc.error_state = state
 *     self.afc.current_state = State.ERROR if state else State.IDLE
 *
 * so "Error" reaching ams_action_from_string() and error_state being true are
 * the same event and cannot diverge. What was missing is only current_error().
 *
 * Also covers the dedup hazard this exposed in RecoveryModalPresenter:
 * AFC_logger.error() sends "!! {msg}" and appends that identical msg to the
 * message queue, so the line-arrival event and the later status-driven event
 * carry byte-identical detail while only the second carries the recovery set.
 */

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "error_event.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

class AfcCurrentErrorHelper : public AmsBackendAfc {
  public:
    AfcCurrentErrorHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2", "lane3", "lane4"};
        initialize_slots(names);
    }

    void feed(const nlohmann::json& params_inner) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    /// Drive AFC's global object the way a real status frame does.
    void feed_afc(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        feed(params);
    }

    void set_toolhead_sensor(bool state) {
        std::lock_guard<std::mutex> lock(mutex_);
        tool_start_sensor_ = state;
    }

    void set_current_lane(const std::string& lane) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_lane_name_ = lane;
    }

    [[nodiscard]] bool error_latched() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_state_;
    }

    [[nodiscard]] AmsAction action() const {
        return get_current_action();
    }
};

namespace {

/// The pair AFC always publishes together — set_error_state() writes both.
nlohmann::json faulted(const std::string& message) {
    return nlohmann::json{{"error_state", true},
                          {"current_state", "Error"},
                          {"message", nlohmann::json{{"message", message}, {"type", "error"}}}};
}

nlohmann::json cleared() {
    return nlohmann::json{{"error_state", false},
                          {"current_state", "Idle"},
                          {"message", nlohmann::json{{"message", ""}, {"type", ""}}}};
}

bool has_action(const helix::ErrorEvent& e, const std::string& gcode_prefix) {
    for (const auto& a : e.recovery_actions) {
        if (a.gcode.rfind(gcode_prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

// ============================================================================
// The trigger and the latch move together
// ============================================================================

TEST_CASE("AFC error_state and the ERROR action arrive together", "[ams][afc][1171]") {
    AfcCurrentErrorHelper afc;

    REQUIRE_FALSE(afc.error_latched());
    REQUIRE_FALSE(afc.current_error().has_value());

    afc.feed_afc(faulted("Lane 1 jammed"));

    // Both halves of upstream's set_error_state(True) landed. If this ever
    // fails, AFC has split the two assignments and the bridge's trigger
    // assumption needs revisiting.
    CHECK(afc.error_latched());
    CHECK(afc.action() == AmsAction::ERROR);
    CHECK(afc.current_error().has_value());

    afc.feed_afc(cleared());
    CHECK_FALSE(afc.error_latched());
    CHECK(afc.action() == AmsAction::IDLE);
    CHECK_FALSE(afc.current_error().has_value());
}

// ============================================================================
// current_error() contents
// ============================================================================

TEST_CASE("AFC current_error carries the fault text and a recovery set", "[ams][afc][1171]") {
    AfcCurrentErrorHelper afc;

    SECTION("no fault means no event") {
        CHECK_FALSE(afc.current_error().has_value());

        // A non-error message must not manufacture one.
        afc.feed_afc(
            nlohmann::json{{"current_state", "Idle"},
                           {"message", nlohmann::json{{"message", "Lane 2 ready"}, {"type", ""}}}});
        CHECK_FALSE(afc.current_error().has_value());
    }

    SECTION("fault text comes from AFC's queued message") {
        afc.feed_afc(faulted("Hub is already clear while trying to reset 'lane1'"));

        auto e = afc.current_error();
        REQUIRE(e.has_value());
        CHECK(e->source == helix::ErrorSource::AFC);
        CHECK(e->severity == helix::ErrorSeverity::CRITICAL);
        CHECK(e->sticky);
        CHECK(e->detail == "Hub is already clear while trying to reset 'lane1'");
        CHECK_FALSE(e->recovery_actions.empty());
    }

    SECTION("a warning-typed message is not used as the fault text") {
        // The message FIFO is a peek that RESET_FAILURE does not pop, so a
        // leftover non-error message must not be presented as this fault's
        // description.
        afc.feed_afc(nlohmann::json{
            {"error_state", true},
            {"current_state", "Error"},
            {"message", nlohmann::json{{"message", "Buffer nudged"}, {"type", "warning"}}}});

        auto e = afc.current_error();
        REQUIRE(e.has_value());
        CHECK(e->detail != "Buffer nudged");
        CHECK_FALSE(e->detail.empty());
    }

    SECTION("never empty — a button-less modal is a UI trap") {
        afc.feed_afc(nlohmann::json{{"error_state", true}, {"current_state", "Error"}});

        auto e = afc.current_error();
        REQUIRE(e.has_value());
        CHECK_FALSE(e->detail.empty());
        CHECK_FALSE(e->recovery_actions.empty());
    }
}

// ============================================================================
// Recovery set adapts to where the filament actually is
// ============================================================================

TEST_CASE("AFC recovery set matches the toolhead state", "[ams][afc][1171]") {
    SECTION("filament at the toolhead offers a heated unload, not a cold eject") {
        AfcCurrentErrorHelper afc;
        afc.set_toolhead_sensor(true);
        afc.feed_afc(faulted("Toolhead jam"));

        auto e = afc.current_error();
        REQUIRE(e.has_value());
        CHECK(has_action(*e, "TOOL_UNLOAD"));
        CHECK_FALSE(has_action(*e, "LANE_UNLOAD"));
    }

    SECTION("empty toolhead with a selected lane offers the cold lane eject") {
        AfcCurrentErrorHelper afc;
        afc.set_toolhead_sensor(false);
        afc.set_current_lane("lane3");
        afc.feed_afc(faulted("Lane error"));

        auto e = afc.current_error();
        REQUIRE(e.has_value());
        CHECK(has_action(*e, "LANE_UNLOAD LANE=lane3"));
        CHECK_FALSE(has_action(*e, "TOOL_UNLOAD"));
    }

    SECTION("every fault keeps a way out") {
        AfcCurrentErrorHelper afc;
        afc.feed_afc(faulted("Something went wrong"));

        auto e = afc.current_error();
        REQUIRE(e.has_value());
        // RESUME is always offered; without at least one button the recovery
        // modal renders as a non-dismissible trap (cf. #1041).
        CHECK(has_action(*e, "RESUME"));
        CHECK(e->recovery_actions.size() >= 2);
    }
}

// ============================================================================
// The stuck-action latch is ours, not AFC's
// ============================================================================

TEST_CASE("AFC current_error stays silent for a fault AFC does not claim", "[ams][afc][1171]") {
    AfcCurrentErrorHelper afc;

    // Our local stuck-action timeout drives the action to ERROR without AFC
    // ever setting error_state. Claiming a recovery set there would offer
    // AFC_RESET for a condition the firmware does not consider an error; that
    // path keeps its existing last-resort toast instead.
    afc.feed_afc(nlohmann::json{{"error_state", false}, {"current_state", "Loading"}});
    CHECK_FALSE(afc.current_error().has_value());

    afc.feed_afc(nlohmann::json{{"error_state", false}, {"current_state", "Idle"}});
    CHECK_FALSE(afc.current_error().has_value());
}
