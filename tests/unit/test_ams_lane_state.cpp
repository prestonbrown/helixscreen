// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_lane_state.cpp
 * @brief The AMS lane classifier (pure).
 *
 * Three base states. Loaded-ness and error-ness are NOT here — they ride their
 * own subjects and are applied by the chrome, because a blocked lane still has
 * filament and an active lane is still Present.
 */

#include "ams_lane_state.h"

#include "../catch_amalgamated.hpp"

using helix::SlotInfo;
using helix::SlotStatus;

using helix::ui::ASSUMED_FILL_LEVEL;
using helix::ui::classify_lane;
using helix::ui::lane_fill_level;
using helix::ui::lane_has_identity;
using helix::ui::LaneState;

namespace {
/// Every value SlotStatus can take, so "exhaustive" below is literal.
/// name_of()'s switch has no default arm, so adding a status makes the
/// compiler point here rather than letting it go untested.
constexpr SlotStatus ALL_STATUSES[] = {
    SlotStatus::UNKNOWN, SlotStatus::EMPTY,   SlotStatus::AVAILABLE,
    SlotStatus::LOADED,  SlotStatus::BLOCKED, SlotStatus::FROM_BUFFER,
};

const char* name_of(SlotStatus s) {
    switch (s) {
    case SlotStatus::UNKNOWN:
        return "UNKNOWN";
    case SlotStatus::EMPTY:
        return "EMPTY";
    case SlotStatus::AVAILABLE:
        return "AVAILABLE";
    case SlotStatus::LOADED:
        return "LOADED";
    case SlotStatus::BLOCKED:
        return "BLOCKED";
    case SlotStatus::FROM_BUFFER:
        return "FROM_BUFFER";
    }
    return "?";
}
} // namespace

static_assert(classify_lane(SlotStatus::LOADED, false) == LaneState::Present);
static_assert(classify_lane(SlotStatus::EMPTY, true) == LaneState::Ghosted);
static_assert(classify_lane(SlotStatus::EMPTY, false) == LaneState::Empty);

TEST_CASE("Lane classification is exhaustively pinned", "[ams][lane_state]") {
    for (SlotStatus status : ALL_STATUSES) {
        for (bool identity : {false, true}) {
            for (bool weights : {false, true}) {
                INFO("status=" << name_of(status) << " identity=" << identity
                               << " weights=" << weights);
                const bool absent = (status == SlotStatus::EMPTY || status == SlotStatus::UNKNOWN);
                const LaneState expected = (status == SlotStatus::UNKNOWN && weights)
                                               ? LaneState::Ghosted
                                           : !absent  ? LaneState::Present
                                           : identity ? LaneState::Ghosted
                                                      : LaneState::Empty;
                CHECK(classify_lane(status, identity, weights) == expected);
                // The SlotInfo overload reads the same signals off the slot.
                SlotInfo s;
                s.status = status;
                s.material = identity ? "PLA" : "";
                s.total_weight_g = weights ? 1000.0f : 0.0f;
                CHECK(classify_lane(s) == expected);
            }
        }
    }
}

TEST_CASE("UNKNOWN is classified exactly as EMPTY", "[ams][lane_state]") {
    // UNKNOWN is the skeleton value every backend writes before firmware data
    // lands (ams_backend_qidi.cpp:72 and friends). Treating it as Present would
    // briefly show filament in a lane that has none; treating it as EMPTY makes
    // it inherit the identity split, so a lane we already have a material for
    // dims instead of blanking.
    for (bool identity : {false, true}) {
        INFO("identity=" << identity);
        CHECK(classify_lane(SlotStatus::UNKNOWN, identity) ==
              classify_lane(SlotStatus::EMPTY, identity));
    }
}

TEST_CASE("UNKNOWN with weights is ghosted, not blanked", "[ams][lane_state]") {
    // Presence unconfirmed but the scale is configured: the weight is real
    // user data, and hiding a weighed spool reads worse than showing it
    // dimmed. This is the deliberate resolution of the second #1368 UNKNOWN
    // outcome (weights without identity used to split the surfaces).
    SECTION("no identity, weights configured") {
        SlotInfo s;
        s.status = SlotStatus::UNKNOWN;
        s.total_weight_g = 1000.0f;
        s.remaining_weight_g = 400.0f;
        CHECK(classify_lane(s) == LaneState::Ghosted);
        CHECK(lane_fill_level(s) == Catch::Approx(0.4f));
    }
    SECTION("EMPTY outranks the scale even with weights") {
        SlotInfo s;
        s.status = SlotStatus::EMPTY;
        s.total_weight_g = 1000.0f;
        s.remaining_weight_g = 400.0f;
        CHECK(classify_lane(s) == LaneState::Empty);
        CHECK(lane_fill_level(s) == Catch::Approx(0.0f));
    }
}

TEST_CASE("Any one identity field retains the lane", "[ams][lane_state]") {
    SECTION("nothing set") {
        SlotInfo s;
        CHECK_FALSE(lane_has_identity(s));
    }
    SECTION("spoolman_id alone") {
        SlotInfo s;
        s.spoolman_id = 7;
        CHECK(lane_has_identity(s));
    }
    SECTION("material alone") {
        SlotInfo s;
        s.material = "PLA";
        CHECK(lane_has_identity(s));
    }
    SECTION("brand alone") {
        SlotInfo s;
        s.brand = "Polymaker";
        CHECK(lane_has_identity(s));
    }
    SECTION("spool_name alone") {
        SlotInfo s;
        s.spool_name = "Galaxy Black #3";
        CHECK(lane_has_identity(s));
    }
    SECTION("a zero or negative spoolman id is not a handle") {
        SlotInfo s;
        s.spoolman_id = 0;
        CHECK_FALSE(lane_has_identity(s));
        s.spoolman_id = -1;
        CHECK_FALSE(lane_has_identity(s));
    }
}

TEST_CASE("Fill level: tracked ratio, else the assumed constant", "[ams][lane_state][fill]") {
    SECTION("tracked weights give the real ratio") {
        SlotInfo s;
        s.status = SlotStatus::AVAILABLE;
        s.total_weight_g = 1000.0f;
        s.remaining_weight_g = 250.0f;
        CHECK(lane_fill_level(s) == Catch::Approx(0.25f));
    }
    SECTION("present but weightless uses the shared assumed constant") {
        // The case that must NOT be a bare 1.0f duplicated per call site.
        SlotInfo s;
        s.status = SlotStatus::AVAILABLE;
        s.material = "PETG";
        CHECK(lane_fill_level(s) == Catch::Approx(ASSUMED_FILL_LEVEL));
    }
    SECTION("a ghosted lane keeps its last known fill") {
        // Deliberate reversal of a106413f6 (#1071). Safe only because the whole
        // cell is ghosted — see the wiring test in Task 5.
        SlotInfo s;
        s.status = SlotStatus::EMPTY;
        s.material = "PETG";
        s.total_weight_g = 1000.0f;
        s.remaining_weight_g = 600.0f;
        CHECK(lane_fill_level(s) == Catch::Approx(0.6f));
    }
    SECTION("a ghosted lane with no weights falls back to the assumed constant") {
        SlotInfo s;
        s.status = SlotStatus::EMPTY;
        s.material = "PETG";
        CHECK(lane_fill_level(s) == Catch::Approx(ASSUMED_FILL_LEVEL));
    }
    SECTION("an unassigned empty lane has no fill") {
        SlotInfo s;
        s.status = SlotStatus::EMPTY;
        CHECK(lane_fill_level(s) == Catch::Approx(0.0f));
    }
}
