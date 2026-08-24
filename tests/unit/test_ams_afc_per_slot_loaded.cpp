// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_afc_per_slot_loaded.cpp
 * @brief AFC per-lane load authority (#1194).
 *
 * AmsBackend::slot_is_actively_loaded()'s legacy default derives a per-slot
 * answer from the aggregate pair (current_slot, filament_loaded). AFC publishes
 * the truth per lane — AFC_stepper.<lane>.tool_loaded — and the two disagree
 * whenever our derivation of the aggregate lags or names the wrong lane. Every
 * affordance built on the predicate then inherits the wrong answer: Load stayed
 * enabled on a lane AFC already considered loaded (#1183), and the context menu
 * offered Recover on a lane that only reached the hub.
 *
 * Backends now declare per-slot authority explicitly; AFC claims it, so the
 * predicate reads SlotStatus::LOADED — which parse_afc_stepper already derives
 * from tool_loaded — instead of the aggregate.
 *
 * Fixture values are the live BoxTurtle at 192.168.1.112: lane1 tool_loaded
 * true / status "Tooled", lanes 2-4 tool_loaded false / status "None" with
 * prep+load set, AFC_extruder.lane_loaded "lane1" with both toolhead sensors
 * tripped.
 */

#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_types.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

/// Feeds the real Moonraker status path so the parse chain under test is the
/// production one (parse_afc_state -> parse_afc_stepper -> parse_afc_extruder).
class AfcPerSlotLoadedHelper : public AmsBackendAfc {
  public:
    AfcPerSlotLoadedHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2", "lane3", "lane4"};
        initialize_slots(names);
    }

    void feed(const nlohmann::json& params_inner) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    void feed_stepper(const std::string& lane, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane] = data;
        feed(params);
    }

    /// The aggregate pair the legacy default reads. Set directly so a test can
    /// stage a disagreement without needing a status frame that produces one.
    void force_aggregate(int slot, bool loaded) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.current_slot = slot;
        system_info_.filament_loaded = loaded;
    }
};

namespace {

/// A lane AFC has seated at the extruder.
nlohmann::json tooled_lane() {
    return nlohmann::json{{"tool_loaded", true},
                          {"status", "Tooled"},
                          {"prep", true},
                          {"load", true},
                          {"hub", "Turtle_1"}};
}

/// A lane holding filament that has NOT reached the extruder. AFC's own
/// "Loaded" means loaded-to-hub, which is why tool_loaded is the only signal
/// that answers "is this lane at the toolhead".
nlohmann::json hub_loaded_lane() {
    return nlohmann::json{{"tool_loaded", false}, {"status", "Loaded"},    {"prep", true},
                          {"load", true},         {"loaded_to_hub", true}, {"hub", "Turtle_1"}};
}

/// An idle lane with filament parked at prep, as the live BoxTurtle reports it.
nlohmann::json idle_lane() {
    return nlohmann::json{{"tool_loaded", false},
                          {"status", "None"},
                          {"prep", true},
                          {"load", true},
                          {"hub", "Turtle_1"}};
}

} // namespace

// ============================================================================
// The authority seam
// ============================================================================

TEST_CASE("Per-slot load authority is opt-in at the AmsBackend seam", "[ams][afc][1194]") {
    SECTION("AFC claims it — its parse carries tool_loaded per lane") {
        AfcPerSlotLoadedHelper afc;
        CHECK(afc.has_per_slot_loaded_authority());
    }

    SECTION("backends without per-slot truth keep the aggregate default") {
        // The mock never marks a non-active slot LOADED, so switching it to the
        // per-slot rule would silently blank its active-lane highlight. The
        // default must stay false for every backend that has not opted in.
        AmsBackendMock mock;
        CHECK_FALSE(mock.has_per_slot_loaded_authority());

        mock.start();
        REQUIRE(mock.is_filament_loaded());
        REQUIRE(mock.get_current_slot() == 0);
        CHECK(mock.slot_is_actively_loaded(0));
        CHECK_FALSE(mock.slot_is_actively_loaded(1));
        mock.stop();
    }
}

// ============================================================================
// slot_is_actively_loaded — per-lane tool_loaded outranks the aggregate
// ============================================================================

TEST_CASE("AFC slot_is_actively_loaded follows the lane AFC seated, not current_slot",
          "[ams][afc][1194][1183]") {
    AfcPerSlotLoadedHelper afc;

    afc.feed_stepper("lane1", tooled_lane());
    afc.feed_stepper("lane2", idle_lane());
    afc.feed_stepper("lane3", idle_lane());
    afc.feed_stepper("lane4", idle_lane());

    REQUIRE(afc.get_slot_info(0).status == SlotStatus::LOADED);
    REQUIRE(afc.get_slot_info(1).status == SlotStatus::AVAILABLE);

    SECTION("aggregate agreeing changes nothing") {
        afc.force_aggregate(0, true);
        CHECK(afc.slot_is_actively_loaded(0));
        CHECK_FALSE(afc.slot_is_actively_loaded(1));
        CHECK_FALSE(afc.slot_is_actively_loaded(2));
        CHECK_FALSE(afc.slot_is_actively_loaded(3));
    }

    SECTION("aggregate naming the WRONG lane loses — the #1183 trigger") {
        // AFC has lane1 at the toolhead; our derived aggregate says lane3.
        // The legacy default answered false for lane1, which left Load enabled
        // on a lane AFC would refuse to load.
        afc.force_aggregate(2, true);
        CHECK(afc.slot_is_actively_loaded(0));
        CHECK_FALSE(afc.slot_is_actively_loaded(2));
    }

    SECTION("aggregate gone stale-empty loses too") {
        // current_load goes null mid-toolchange while the lane stays seated.
        afc.force_aggregate(-1, false);
        CHECK(afc.slot_is_actively_loaded(0));
    }

    SECTION("hub-loaded is not toolhead-loaded even when the aggregate says so") {
        // AFC's lane status "Loaded" means loaded-to-hub. Answering true here is
        // what offered Recover/Reset on a lane that never reached the extruder.
        afc.feed_stepper("lane1", hub_loaded_lane());
        REQUIRE(afc.get_slot_info(0).status == SlotStatus::AVAILABLE);

        afc.force_aggregate(0, true);
        CHECK_FALSE(afc.slot_is_actively_loaded(0));
    }

    SECTION("unload clears it on the lane, not just on the aggregate") {
        afc.feed_stepper("lane1", idle_lane());
        afc.force_aggregate(0, true); // aggregate not yet caught up
        CHECK_FALSE(afc.slot_is_actively_loaded(0));
    }

    SECTION("out-of-range slots are false, not a crash") {
        CHECK_FALSE(afc.slot_is_actively_loaded(-1));
        CHECK_FALSE(afc.slot_is_actively_loaded(99));
    }
}

// ============================================================================
// slot_has_filament_at_toolhead — the extruder sensors, attributed by lane
// ============================================================================

TEST_CASE("AFC slot_has_filament_at_toolhead reports the extruder sensors for the loaded lane",
          "[ams][afc][1194]") {
    AfcPerSlotLoadedHelper afc;

    afc.feed_stepper("lane1", tooled_lane());
    afc.feed_stepper("lane2", idle_lane());

    auto extruder = [](bool start, bool end, const nlohmann::json& lane) {
        nlohmann::json params;
        params["AFC_extruder extruder"] = nlohmann::json{
            {"tool_start_status", start}, {"tool_end_status", end}, {"lane_loaded", lane}};
        return params;
    };

    SECTION("tripped sensor attributes to the lane the extruder names") {
        afc.feed(extruder(true, true, "lane1"));
        CHECK(afc.slot_has_filament_at_toolhead(0));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(1));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(2));
    }

    SECTION("entry sensor alone is enough — filament is past the extruder inlet") {
        afc.feed(extruder(true, false, "lane1"));
        CHECK(afc.slot_has_filament_at_toolhead(0));
    }

    SECTION("exit sensor alone is enough — a start sensor may not be configured") {
        afc.feed(extruder(false, true, "lane1"));
        CHECK(afc.slot_has_filament_at_toolhead(0));
    }

    SECTION("both sensors clear means nothing is at the head") {
        afc.feed(extruder(false, false, "lane1"));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(0));
    }

    SECTION("no lane named — never fabricate an attribution") {
        // A tripped sensor with no owning lane could be any lane's filament.
        // Reporting it on all of them (or on slot 0) would be a fabrication;
        // the base default of false is the honest answer.
        afc.feed(extruder(true, true, nullptr));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(0));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(1));
    }

    SECTION("a lane AFC does not know about attributes to nobody") {
        afc.feed(extruder(true, true, "lane9"));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(0));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(1));
    }

    SECTION("attribution follows the extruder across a lane change") {
        afc.feed(extruder(true, true, "lane1"));
        REQUIRE(afc.slot_has_filament_at_toolhead(0));

        afc.feed(extruder(true, true, "lane2"));
        CHECK(afc.slot_has_filament_at_toolhead(1));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(0));
    }

    SECTION("no AFC_extruder object at all — stays at the safe default") {
        AfcPerSlotLoadedHelper bare;
        bare.feed_stepper("lane1", tooled_lane());
        CHECK_FALSE(bare.slot_has_filament_at_toolhead(0));
    }

    SECTION("out-of-range slots are false, not a crash") {
        afc.feed(extruder(true, true, "lane1"));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(-1));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(99));
    }
}
