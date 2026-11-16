// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cold_pull_flow.cpp
 * @brief The guided cold pull's decisions (prestonbrown/helixscreen#2011).
 *
 * Run with: ./build/bin/helix-tests "[cold_pull]"
 */

#include "cold_pull_flow.h"
#include "cold_pull_runner.h"
#include "config.h"
#include "filament_op_router.h"
#include "material_settings_manager.h"

#include <chrono>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace std::chrono_literals;

namespace {

using TP = ColdPullFlow::TimePoint;
const TP t0{};

ColdPullParams params() {
    ColdPullParams p;
    p.temps = cold_pull_default_temps(ColdPullMaterial::Pla); // 230 / 90
    p.heater = "extruder1";
    p.part_fan = "fan";
    p.prior_fan_percent = 35;
    p.floor_c = 170;
    p.homed = true;
    p.has_park_macro = true;
    return p;
}

bool moves_axes(const std::string& gcode) {
    return gcode.find('X') != std::string::npos || gcode.find('Y') != std::string::npos ||
           gcode.find('Z') != std::string::npos || gcode.find("G28") != std::string::npos;
}

/// Drives a flow to @p target, asserting each step on the way.
void drive_to(ColdPullFlow& f, ColdPullStage target) {
    if (f.stage() == target) {
        return;
    }
    (void)f.start(t0);
    if (target == ColdPullStage::Preparing) {
        return;
    }
    (void)f.toolhead_ready(t0);
    REQUIRE(f.stage() == ColdPullStage::Heating);
    if (target == ColdPullStage::Heating) {
        return;
    }
    (void)f.temperature(228, 230, t0 + 1s);
    REQUIRE(f.stage() == ColdPullStage::Feed);
    if (target == ColdPullStage::Feed) {
        return;
    }
    (void)f.extrude(t0 + 2s);
    (void)f.command_finished(f.in_flight_command(), true, t0 + 3s);
    (void)f.fed(t0 + 4s);
    REQUIRE(f.stage() == ColdPullStage::Cooling);
    if (target == ColdPullStage::Cooling) {
        return;
    }
    (void)f.temperature(225, 90, t0 + 5s);
    (void)f.temperature(92, 90, t0 + 6s);
    REQUIRE(f.stage() == ColdPullStage::Pull);
    if (target == ColdPullStage::Pull) {
        return;
    }
    (void)f.pulled(t0 + 7s);
    REQUIRE(f.stage() == ColdPullStage::Inspect);
}

} // namespace

TEST_CASE("cold pull: each material starts at its own soak and pull", "[cold_pull]") {
    CHECK(cold_pull_default_temps(ColdPullMaterial::Pla) == ColdPullTemps{230, 90});
    CHECK(cold_pull_default_temps(ColdPullMaterial::Nylon) == ColdPullTemps{250, 140});
    CHECK(cold_pull_default_temps(ColdPullMaterial::Cleaning) == ColdPullTemps{260, 100});
}

TEST_CASE("cold pull: an edit stays inside the printer's range", "[cold_pull]") {
    CHECK(clamp_cold_pull_temps({320, 90}, 170, 300).soak_c == 300);
    CHECK(clamp_cold_pull_temps({180, 90}, 170, 300).soak_c == 190);
    CHECK(clamp_cold_pull_temps({230, 200}, 170, 300).pull_c == 170);
    CHECK(clamp_cold_pull_temps({230, 10}, 170, 300).pull_c == kMinPullC);
    CHECK(clamp_cold_pull_temps({230, 90}, 170, 300) == ColdPullTemps{230, 90});
}

TEST_CASE("cold pull: extrude_gcode is relative, and negative retracts", "[cold_pull]") {
    CHECK(ui::extrude_gcode(60, 300) == "M83\nG1 E60 F300");
    CHECK(ui::extrude_gcode(-10, 1200) == "M83\nG1 E-10 F1200");
    CHECK(ui::filament_purge_fallback_gcode() == "M83\nG1 E50 F600");
}

TEST_CASE("cold pull: the happy path sends each step's commands in order", "[cold_pull]") {
    ColdPullFlow f(params());

    auto a = f.start(t0);
    CHECK(f.stage() == ColdPullStage::Preparing);
    CHECK(a.run_park);
    CHECK_FALSE(a.heater_target_c);

    a = f.toolhead_ready(t0);
    CHECK(f.stage() == ColdPullStage::Heating);
    CHECK(a.heater_target_c == 230);
    CHECK(a.keep_previous_hot);
    CHECK(f.goal_c() == 230);

    CHECK(f.temperature(150, 230, t0 + 1s).empty());
    CHECK(f.stage() == ColdPullStage::Heating);
    (void)f.temperature(226, 230, t0 + 2s);
    CHECK(f.stage() == ColdPullStage::Feed);
    CHECK(f.feed_stage() == GuidedLoadStage::AwaitPush);

    a = f.extrude(t0 + 3s);
    CHECK(a.gcode == "M83\nG1 E60 F300");
    CHECK(f.extrude(t0 + 3s).empty()); // one in flight
    (void)f.command_finished(f.in_flight_command(), true, t0 + 4s);
    CHECK(f.feed_stage() == GuidedLoadStage::ConfirmOutput);
    CHECK(f.extrude(t0 + 5s).gcode == "M83\nG1 E20 F300");
    (void)f.command_finished(f.in_flight_command(), true, t0 + 6s);

    a = f.fed(t0 + 7s);
    CHECK(f.stage() == ColdPullStage::Cooling);
    CHECK(a.heater_target_c == 90);
    CHECK_FALSE(a.keep_previous_hot);
    CHECK(a.fan_percent == 100);
    CHECK(f.goal_c() == 90);

    (void)f.temperature(225, 90, t0 + 8s);
    a = f.temperature(92, 90, t0 + 9s);
    CHECK(f.stage() == ColdPullStage::Pull);
    CHECK(a.fan_percent == 35);
    CHECK(f.hold_awake());

    a = f.pulled(t0 + 10s);
    CHECK(f.stage() == ColdPullStage::Inspect);
    CHECK(a.heater_target_c == 0);
    CHECK_FALSE(f.hold_awake());

    a = f.done(t0 + 11s);
    CHECK(f.stage() == ColdPullStage::Finished);
    CHECK(a.reset_meter);
    CHECK_FALSE(a.heater_target_c); // already off
    CHECK_FALSE(a.fan_percent);     // already restored
}

TEST_CASE("cold pull: Pull fires on the nozzle temperature, not on the target change",
          "[cold_pull]") {
    ColdPullFlow f(params());
    drive_to(f, ColdPullStage::Cooling);
    (void)f.temperature(225, 90, t0 + 10s);
    CHECK(f.stage() == ColdPullStage::Cooling);
    (void)f.temperature(94, 90, t0 + 11s);
    CHECK(f.stage() == ColdPullStage::Cooling);
    (void)f.temperature(93, 90, t0 + 12s);
    CHECK(f.stage() == ColdPullStage::Pull);
}

TEST_CASE("cold pull: a nozzle already at the pull temperature never came from above",
          "[cold_pull]") {
    ColdPullFlow f(params());
    drive_to(f, ColdPullStage::Cooling);
    (void)f.temperature(80, 90, t0 + 10s);
    CHECK(f.stage() == ColdPullStage::Cooling);
}

TEST_CASE("cold pull: Repeat heats again and counts the pass", "[cold_pull]") {
    ColdPullFlow f(params());
    drive_to(f, ColdPullStage::Inspect);
    CHECK(f.pass() == 1);
    auto a = f.repeat(t0 + 20s);
    CHECK(f.stage() == ColdPullStage::Heating);
    CHECK(f.pass() == 2);
    CHECK(a.heater_target_c == 230);
    CHECK(a.keep_previous_hot);
    // A fresh target: the old pull target reading is not a foreign one.
    (void)f.temperature(60, 0, t0 + 21s);
    CHECK(f.stage() == ColdPullStage::Heating);
}

TEST_CASE("cold pull: cancel from every live stage leaves the heater and fan as found",
          "[cold_pull]") {
    struct Row {
        ColdPullStage stage;
        bool heater_off;
        bool fan_restored;
    };
    const Row rows[] = {
        {ColdPullStage::Preparing, false, false}, {ColdPullStage::Heating, true, false},
        {ColdPullStage::Feed, true, false},       {ColdPullStage::Cooling, true, true},
        {ColdPullStage::Pull, true, false},       {ColdPullStage::Inspect, false, false},
    };
    for (const Row& row : rows) {
        CAPTURE(static_cast<int>(row.stage));
        ColdPullFlow f(params());
        drive_to(f, row.stage);
        const auto a = f.cancel();
        CHECK(f.stage() == ColdPullStage::Cancelled);
        CHECK(f.end_reason() == ColdPullEndReason::UserCancel);
        CHECK(a.heater_target_c.has_value() == row.heater_off);
        if (row.heater_off) {
            CHECK(*a.heater_target_c == 0);
        }
        CHECK(a.fan_percent.has_value() == row.fan_restored);
        if (row.fan_restored) {
            CHECK(*a.fan_percent == 35);
        }
        CHECK_FALSE(a.reset_meter);
        // Idempotent: nothing more after the first cleanup.
        CHECK(f.cancel().empty());
        CHECK(f.tick(t0 + 1h).empty());
    }
}

TEST_CASE("cold pull: a job taking the machine gets the heater and fan untouched", "[cold_pull]") {
    ColdPullFlow f(params());
    drive_to(f, ColdPullStage::Cooling);
    CHECK(f.exit_actions(/*job_holds_machine=*/true).empty());
    CHECK(f.job_started().empty());
    CHECK(f.stage() == ColdPullStage::Abandoned);
    CHECK(f.end_reason() == ColdPullEndReason::JobStarted);
    CHECK(f.cancel().empty());
}

TEST_CASE("cold pull: a disconnect ends the flow and remembers nothing to send", "[cold_pull]") {
    ColdPullFlow f(params());
    drive_to(f, ColdPullStage::Heating);
    CHECK(f.owned_target_c() == 230);
    CHECK(f.disconnected().empty());
    CHECK(f.end_reason() == ColdPullEndReason::Disconnected);
}

TEST_CASE("cold pull: a target set elsewhere stands; only the fan is restored", "[cold_pull]") {
    SECTION("during Feed") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Feed);
        const auto a = f.temperature(229, 250, t0 + 5s);
        CHECK(f.end_reason() == ColdPullEndReason::ForeignTarget);
        CHECK_FALSE(a.heater_target_c);
    }
    SECTION("during Cooling") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Cooling);
        (void)f.temperature(200, 90, t0 + 10s);
        const auto a = f.temperature(180, 0, t0 + 11s);
        CHECK(f.end_reason() == ColdPullEndReason::ForeignTarget);
        CHECK_FALSE(a.heater_target_c);
        CHECK(a.fan_percent == 35);
    }
    SECTION("the old soak target before the pull target lands is not foreign") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Cooling);
        (void)f.temperature(229, 230, t0 + 10s);
        CHECK(f.stage() == ColdPullStage::Cooling);
    }
    SECTION("the swap latch raising the soak is the soak") {
        ColdPullFlow f(params());
        (void)f.start(t0);
        (void)f.toolhead_ready(t0);
        (void)f.temperature(100, 250, t0 + 1s);
        CHECK(f.stage() == ColdPullStage::Heating);
        CHECK(f.goal_c() == 250);
        (void)f.temperature(240, 250, t0 + 2s);
        CHECK(f.stage() == ColdPullStage::Heating);
        (void)f.temperature(246, 250, t0 + 3s);
        CHECK(f.stage() == ColdPullStage::Feed);
    }
}

TEST_CASE("cold pull: every stage times out at its limit and not before", "[cold_pull]") {
    SECTION("the target never lands") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Heating);
        CHECK(f.tick(t0 + 19s).empty());
        CHECK(f.tick(t0 + 20s).heater_target_c == 0);
        CHECK(f.end_reason() == ColdPullEndReason::TargetNeverLanded);
    }
    SECTION("heating") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Heating);
        (void)f.temperature(100, 230, t0 + 1s);
        (void)f.tick(t0 + 10min - 1s);
        CHECK(f.stage() == ColdPullStage::Heating);
        (void)f.tick(t0 + 10min);
        CHECK(f.end_reason() == ColdPullEndReason::Timeout);
    }
    SECTION("feed idle, reset by a tap") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Feed); // entered at t0 + 1s
        (void)f.tick(t0 + 1s + 8min - 1s);
        CHECK(f.stage() == ColdPullStage::Feed);
        (void)f.extrude(t0 + 5min);
        (void)f.command_finished(f.in_flight_command(), true, t0 + 5min);
        (void)f.tick(t0 + 9min);
        CHECK(f.stage() == ColdPullStage::Feed);
        (void)f.tick(t0 + 13min);
        CHECK(f.end_reason() == ColdPullEndReason::Timeout);
    }
    SECTION("feed idle, with a command that never answers") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Feed);
        (void)f.extrude(t0 + 2s);
        REQUIRE(f.busy());
        const auto a = f.tick(t0 + 2s + 8min);
        CHECK(f.end_reason() == ColdPullEndReason::Timeout);
        CHECK(a.heater_target_c == 0);
    }
    SECTION("cooling with a fan, and twice as long without one") {
        for (const bool fan : {true, false}) {
            auto p = params();
            if (!fan) {
                p.part_fan.clear();
            }
            ColdPullFlow f(p);
            drive_to(f, ColdPullStage::Cooling); // entered at t0 + 4s
            const auto limit = fan ? 10min : 20min;
            (void)f.tick(t0 + 4s + limit - 1s);
            CHECK(f.stage() == ColdPullStage::Cooling);
            (void)f.tick(t0 + 4s + limit);
            CHECK(f.end_reason() == ColdPullEndReason::Timeout);
        }
    }
    SECTION("pull hold turns the heater off") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Pull); // entered at t0 + 6s
        CHECK(f.tick(t0 + 6s + 5min - 1s).empty());
        CHECK(f.tick(t0 + 6s + 5min).heater_target_c == 0);
        CHECK(f.end_reason() == ColdPullEndReason::Timeout);
    }
}

TEST_CASE("cold pull: an unhomed machine is never moved on its own", "[cold_pull]") {
    auto p = params();
    p.homed = false;
    ColdPullFlow f(p);
    const auto a = f.start(t0);
    CHECK_FALSE(a.run_park);
    CHECK_FALSE(a.home_then_park);
    CHECK_FALSE(moves_axes(a.gcode));
    const auto tapped = f.home_and_park(t0);
    CHECK(tapped.home_then_park);
    CHECK(f.home_and_park(t0).empty()); // once

    p.has_park_macro = false;
    ColdPullFlow no_macro(p);
    CHECK(no_macro.start(t0).empty());
    CHECK(no_macro.home_and_park(t0).empty());
}

TEST_CASE("cold pull: a homed machine without a park macro stays where it is", "[cold_pull]") {
    auto p = params();
    p.has_park_macro = false;
    ColdPullFlow f(p);
    CHECK(f.start(t0).empty());
}

TEST_CASE("cold pull: hold pressure stops for good near the extrusion floor", "[cold_pull]") {
    auto p = params();
    p.hold_pressure = true;
    ColdPullFlow f(p);
    drive_to(f, ColdPullStage::Cooling); // entered at t0 + 4s
    (void)f.temperature(220, 90, t0 + 5s);
    CHECK(f.tick(t0 + 6s).gcode.empty()); // first one 3 s in
    CHECK(f.tick(t0 + 7s).gcode == "M83\nG1 E1 F60");
    CHECK(f.tick(t0 + 10s).gcode.empty()); // one in flight
    (void)f.command_finished(f.in_flight_command(), true, t0 + 10s);
    CHECK(f.tick(t0 + 10s).gcode == "M83\nG1 E1 F60");
    (void)f.command_finished(f.in_flight_command(), true, t0 + 11s);
    (void)f.temperature(185, 90, t0 + 12s);
    CHECK_FALSE(f.tick(t0 + 13s).gcode.empty());
    (void)f.command_finished(f.in_flight_command(), true, t0 + 13s);
    (void)f.temperature(184, 90, t0 + 14s);
    (void)f.temperature(200, 90, t0 + 15s); // a bump back up does not restart it
    CHECK(f.tick(t0 + 20s).gcode.empty());

    ColdPullFlow off(params());
    drive_to(off, ColdPullStage::Cooling);
    (void)off.temperature(220, 90, t0 + 5s);
    CHECK(off.tick(t0 + 30s).gcode.empty());
}

TEST_CASE("cold pull: no part fan, no fan command anywhere", "[cold_pull]") {
    auto p = params();
    p.part_fan.clear();
    ColdPullFlow f(p);
    drive_to(f, ColdPullStage::Cooling);
    CHECK_FALSE(f.cancel().fan_percent);
    ColdPullFlow g(p);
    drive_to(g, ColdPullStage::Inspect);
    CHECK_FALSE(g.done(t0 + 1min).fan_percent);
}

/// A flow on a seated lane, driven through its first Feed.
ColdPullFlow seated_at_feed() {
    auto p = params();
    p.toolhead_loaded = true;
    ColdPullFlow f(p);
    (void)f.start(t0);
    REQUIRE(f.stage() == ColdPullStage::Preparing);
    const auto heat = f.toolhead_ready(t0);
    REQUIRE(f.stage() == ColdPullStage::Heating);
    REQUIRE(heat.heater_target_c == 230);
    (void)f.temperature(228, 230, t0 + 1s);
    REQUIRE(f.stage() == ColdPullStage::Feed);
    return f;
}

/// Feed done through a hand pull, back at Inspect.
void pull_to_inspect(ColdPullFlow& f, TP at) {
    (void)f.fed(at);
    (void)f.temperature(200, 90, at + 1s);
    (void)f.temperature(92, 90, at + 2s);
    REQUIRE(f.stage() == ColdPullStage::Pull);
    (void)f.pulled(at + 3s);
    REQUIRE(f.stage() == ColdPullStage::Inspect);
}

TEST_CASE("cold pull: a seated lane's filament is purged, never pushed", "[cold_pull]") {
    auto f = seated_at_feed();
    CHECK(f.feed_mode() == ColdPullFeedMode::Loaded);
    CHECK(f.feed_stage() == GuidedLoadStage::ConfirmOutput);
    // Already at the nozzle: the 60 mm hand-feed length would be waste.
    CHECK(f.filament_detected(t0 + 2s).empty());
    CHECK(f.extrude(t0 + 2s).gcode == "M83\nG1 E20 F300");
    (void)f.command_finished(f.in_flight_command(), false, t0 + 3s);
    CHECK(f.feed_stage() == GuidedLoadStage::ConfirmOutput); // a refused purge stays a purge
    CHECK(f.extrude(t0 + 3s).gcode == "M83\nG1 E20 F300");
    (void)f.command_finished(f.in_flight_command(), true, t0 + 3s);
    CHECK(f.feed_stage() == GuidedLoadStage::ConfirmOutput);
    CHECK(f.fed(t0 + 3s).heater_target_c == 90);
    CHECK(f.stage() == ColdPullStage::Cooling);
    CHECK_FALSE(f.lane_needs_reseat());
}

TEST_CASE("cold pull: after a hand pull the lane is owed a re-seat", "[cold_pull]") {
    auto f = seated_at_feed();
    pull_to_inspect(f, t0 + 2s);
    CHECK(f.lane_needs_reseat());

    SECTION("Done carries it to the end screen") {
        (void)f.done(t0 + 1min);
        CHECK(f.stage() == ColdPullStage::Finished);
        CHECK(f.lane_needs_reseat());
    }
    SECTION("Repeat feeds by hand, and feeding it back settles the debt") {
        (void)f.repeat(t0 + 1min);
        (void)f.temperature(228, 230, t0 + 2min);
        REQUIRE(f.stage() == ColdPullStage::Feed);
        CHECK(f.feed_mode() == ColdPullFeedMode::Refeed);
        CHECK(f.feed_stage() == GuidedLoadStage::AwaitPush);
        CHECK(f.lane_needs_reseat());
        CHECK(f.extrude(t0 + 3min).gcode == "M83\nG1 E60 F300");
        CHECK(f.lane_needs_reseat()); // not back until the extrude has run
        (void)f.command_finished(f.in_flight_command(), true, t0 + 3min + 1s);
        CHECK_FALSE(f.lane_needs_reseat());
        CHECK(f.feed_mode() == ColdPullFeedMode::Loaded);
    }
    SECTION("Done without feeding it back leaves it owed") {
        (void)f.repeat(t0 + 1min);
        (void)f.temperature(228, 230, t0 + 2min);
        REQUIRE(f.stage() == ColdPullStage::Feed);
        (void)f.fed(t0 + 3min);
        CHECK(f.stage() == ColdPullStage::Cooling);
        CHECK(f.lane_needs_reseat());
    }
    SECTION("a refused extrude does not settle it, and the retry is the full feed") {
        (void)f.repeat(t0 + 1min);
        (void)f.temperature(228, 230, t0 + 2min);
        (void)f.extrude(t0 + 3min);
        (void)f.command_finished(f.in_flight_command(), false, t0 + 3min + 1s);
        CHECK(f.lane_needs_reseat());
        CHECK(f.feed_stage() == GuidedLoadStage::AwaitPush);
        CHECK(f.extrude(t0 + 3min + 2s).gcode == "M83\nG1 E60 F300");
    }
    SECTION("cancelling a repeat before the feed leaves it owed") {
        (void)f.repeat(t0 + 1min);
        (void)f.cancel();
        CHECK(f.stage() == ColdPullStage::Cancelled);
        CHECK(f.lane_needs_reseat());
    }
}

TEST_CASE("cold pull: no seated lane feeds by hand and owes nothing", "[cold_pull]") {
    ColdPullFlow f(params());
    drive_to(f, ColdPullStage::Feed);
    CHECK(f.feed_mode() == ColdPullFeedMode::ByHand);
    CHECK(f.feed_stage() == GuidedLoadStage::AwaitPush);
    (void)f.extrude(t0 + 2s);
    (void)f.command_finished(f.in_flight_command(), true, t0 + 3s);
    pull_to_inspect(f, t0 + 4s);
    CHECK_FALSE(f.lane_needs_reseat());
}

TEST_CASE("cold pull: the feed runs once per push and a refused one costs nothing", "[cold_pull]") {
    ColdPullFlow f(params());
    drive_to(f, ColdPullStage::Feed);
    CHECK(f.filament_detected(t0 + 2s).gcode == "M83\nG1 E60 F300");
    CHECK(f.filament_detected(t0 + 2s).empty());
    CHECK(f.extrude(t0 + 2s).empty());
    CHECK(f.command_finished(f.in_flight_command(), false, t0 + 3s).empty());
    CHECK(f.stage() == ColdPullStage::Feed);
    CHECK_FALSE(f.busy());
    CHECK(f.feed_stage() == GuidedLoadStage::AwaitPush); // the retry is the full feed
    CHECK(f.extrude(t0 + 4s).gcode == "M83\nG1 E60 F300");
    (void)f.command_finished(f.in_flight_command(), false, t0 + 5s);
    CHECK(f.fed(t0 + 6s).heater_target_c == 90);
}

TEST_CASE("cold pull: a clogged head reaches the pull with nothing extruded", "[cold_pull]") {
    SECTION("seated lane, Done without an extrude") {
        auto f = seated_at_feed();
        CHECK(f.fed(t0 + 2s).heater_target_c == 90);
        CHECK(f.stage() == ColdPullStage::Cooling);
    }
    SECTION("hand feed, Done before any extrude") {
        ColdPullFlow f(params());
        drive_to(f, ColdPullStage::Feed);
        REQUIRE(f.feed_stage() == GuidedLoadStage::AwaitPush);
        CHECK(f.fed(t0 + 2s).heater_target_c == 90);
        CHECK(f.stage() == ColdPullStage::Cooling);
    }
    SECTION("Done while an extrude never answers; its late reply changes nothing") {
        auto p = params();
        p.hold_pressure = true;
        ColdPullFlow f(p);
        drive_to(f, ColdPullStage::Feed);
        const uint32_t feed_id = f.extrude(t0 + 2s).command_id;
        REQUIRE(f.busy());
        (void)f.fed(t0 + 3s);
        CHECK(f.stage() == ColdPullStage::Cooling);
        CHECK_FALSE(f.busy());
        (void)f.temperature(225, 90, t0 + 4s);
        const auto hold = f.tick(t0 + 7s);
        CHECK(hold.gcode == "M83\nG1 E1 F60"); // the feed's slot is free again
        CHECK(f.command_finished(feed_id, false, t0 + 8s).empty());
        CHECK(f.stage() == ColdPullStage::Cooling);
        CHECK(f.in_flight_command() == hold.command_id); // the hold is still the one out
        (void)f.command_finished(hold.command_id, true, t0 + 8s);
        CHECK_FALSE(f.busy());
        CHECK(f.tick(t0 + 10s).gcode == "M83\nG1 E1 F60"); // the late refusal stopped nothing
        (void)f.temperature(92, 90, t0 + 9s);
        CHECK(f.stage() == ColdPullStage::Pull);
    }
    SECTION("refused hold extrusions and one that hangs do not hold up the pull") {
        auto p = params();
        p.hold_pressure = true;
        ColdPullFlow f(p);
        drive_to(f, ColdPullStage::Cooling);
        (void)f.temperature(225, 90, t0 + 5s);
        REQUIRE_FALSE(f.tick(t0 + 9s).gcode.empty());
        (void)f.temperature(92, 90, t0 + 10s);
        CHECK(f.stage() == ColdPullStage::Pull);
        CHECK(f.busy());
        (void)f.pulled(t0 + 11s);
        CHECK(f.stage() == ColdPullStage::Inspect);
    }
}

TEST_CASE("cold pull: the ring reads the climb and is full once the goal is reached",
          "[cold_pull]") {
    CHECK(cold_pull_ring_percent(ColdPullStage::Heating, 30, 130, 230) == 50);
    CHECK(cold_pull_ring_percent(ColdPullStage::Cooling, 230, 160, 90) == 50);
    CHECK(cold_pull_ring_percent(ColdPullStage::Heating, 30, 20, 230) == 0);
    CHECK(cold_pull_ring_percent(ColdPullStage::Heating, 30, 240, 230) == 100);
    CHECK(cold_pull_ring_percent(ColdPullStage::Heating, 230, 230, 230) == 100);
    // Feed and Pull start within a few degrees of the goal: no span to measure.
    CHECK(cold_pull_ring_percent(ColdPullStage::Feed, 226, 228, 230) == 100);
    CHECK(cold_pull_ring_percent(ColdPullStage::Feed, 228, 227, 230) == 100);
    CHECK(cold_pull_ring_percent(ColdPullStage::Pull, 93, 92, 90) == 100);
}

TEST_CASE("cold pull: edits persist per material; unset and corrupt read as defaults",
          "[cold_pull][material_settings]") {
    auto& msm = MaterialSettingsManager::instance();
    Config* config = Config::get_instance();
    REQUIRE(config);
    for (int i = 0; i < kColdPullMaterialCount; ++i) {
        msm.reset_cold_pull_temps(static_cast<ColdPullMaterial>(i));
    }
    CHECK(msm.get_cold_pull_temps(ColdPullMaterial::Nylon) == ColdPullTemps{250, 140});

    msm.set_cold_pull_temps(ColdPullMaterial::Nylon, {255, 150});
    CHECK(msm.get_cold_pull_temps(ColdPullMaterial::Nylon) == ColdPullTemps{255, 150});
    CHECK(msm.get_cold_pull_temps(ColdPullMaterial::Pla) == ColdPullTemps{230, 90});

    msm.set_cold_pull_material(ColdPullMaterial::Cleaning);
    CHECK(msm.get_cold_pull_material() == ColdPullMaterial::Cleaning);

    config->get_json("/cold_pull")["pla"] = {{"soak", 100}, {"pull", 90}};
    CHECK(msm.get_cold_pull_temps(ColdPullMaterial::Pla) == ColdPullTemps{230, 90});
    config->get_json("/cold_pull")["material"] = 7;
    CHECK(msm.get_cold_pull_material() == ColdPullMaterial::Pla);

    msm.reset_cold_pull_temps(ColdPullMaterial::Nylon);
    CHECK(msm.get_cold_pull_temps(ColdPullMaterial::Nylon) == ColdPullTemps{250, 140});
    config->get_json("/cold_pull") = nlohmann::json::object();
}

TEST_CASE("cold pull: Start is refused while anything else owns the printer", "[cold_pull]") {
    using ui::cold_pull_refusal;
    using ui::ColdPullRefusal;
    for (const PrintState job : {PrintState::Printing, PrintState::Paused, PrintState::Preparing}) {
        CHECK(cold_pull_refusal(job, false, true, true, false) == ColdPullRefusal::JobHoldsMachine);
    }
    CHECK(cold_pull_refusal(PrintState::Idle, true, true, true, false) ==
          ColdPullRefusal::PreheatPending);
    CHECK(cold_pull_refusal(PrintState::Idle, false, false, true, false) ==
          ColdPullRefusal::NotReady);
    CHECK(cold_pull_refusal(PrintState::Idle, false, true, false, false) ==
          ColdPullRefusal::NoHeater);
    CHECK(cold_pull_refusal(PrintState::Complete, false, true, true, true) ==
          ColdPullRefusal::Running);
    CHECK(cold_pull_refusal(PrintState::Complete, false, true, true, false) == ColdPullRefusal::Ok);
}
