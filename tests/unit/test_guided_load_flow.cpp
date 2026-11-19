// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_guided_load_flow.cpp
 * @brief The stock load wizards and the flow that walks them.
 *
 * Run with: ./build/bin/helix-tests "[guided_load]"
 */

#include "filament_macro_profiles.h"
#include "guided_load_flow.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::GuidedLoadFlow;
using helix::GuidedLoadStage;
using helix::filament_macros::guided_load;
using helix::filament_macros::guided_load_example;
using helix::filament_macros::guided_load_status_query;
using helix::filament_macros::guided_load_waits;
using helix::filament_macros::GuidedLoad;
using helix::filament_macros::restore_after;

namespace {

GuidedLoad steps() {
    auto g = guided_load("M604", {{"S", "230"}});
    REQUIRE(g.has_value());
    return *g;
}

} // namespace

// ============================================================================
// The table
// ============================================================================

TEST_CASE("guided_load: QIDI's M604 is a wizard, rendered for the temperature", "[guided_load]") {
    const GuidedLoad g = steps();
    CHECK(g.waiting_object == "print_stats_manager");
    CHECK(g.waiting_field == "sub_status");
    CHECK(g.waiting_value == "push_filamenting");
    CHECK(g.feed == "SET_PRINT_SUB_STATUS SUB_STATUS=bit_filamenting\nM604 T1 S230");
    CHECK(g.retry == "M604 T1 S230");
    CHECK(g.flush == "M604 T2 S230");
    CHECK(g.release == "SET_PRINT_MAIN_STATUS MAIN_STATUS=ready");
    CHECK(g.cleanup == "M104 S0\nENABLE_ALL_SENSOR");
}

TEST_CASE("guided_load: case-insensitive, and nothing for a macro without one", "[guided_load]") {
    CHECK(guided_load("m604", {}).has_value());
    CHECK_FALSE(guided_load("LOAD_FILAMENT", {}).has_value());
    CHECK_FALSE(guided_load("M603", {}).has_value());
    CHECK_FALSE(guided_load("", {}).has_value());
}

TEST_CASE("guided_load: the profile picks its own temperature out of what was sent",
          "[guided_load]") {
    SECTION("nothing sent: the macro's default") {
        auto g = guided_load("M604", {});
        REQUIRE(g.has_value());
        CHECK(g->retry == "M604 T1 S250");
        CHECK(g->flush == "M604 T2 S250");
    }
    SECTION("its parameter, in any case") {
        auto g = guided_load("M604", {{"s", "215"}});
        REQUIRE(g.has_value());
        CHECK(g->retry == "M604 T1 S215");
    }
    SECTION("another macro's temperature parameter is not its own") {
        auto g = guided_load("M604", {{"EXTRUDER_TEMP", "215"}, {"NOZZLE_TEMP", "215"}});
        REQUIRE(g.has_value());
        CHECK(g->retry == "M604 T1 S250");
    }
    SECTION("an unreadable or zero value keeps the default") {
        CHECK(guided_load("M604", {{"S", "hot"}})->retry == "M604 T1 S250");
        CHECK(guided_load("M604", {{"S", "0"}})->retry == "M604 T1 S250");
    }
}

TEST_CASE("guided_load_example: a known wizard at its default", "[guided_load]") {
    auto g = guided_load_example();
    REQUIRE(g.has_value());
    CHECK(g->flush == "M604 T2 S250");
}

TEST_CASE("guided_load_status_query: asks for the waiting field only", "[guided_load]") {
    CHECK(guided_load_status_query(steps()) ==
          nlohmann::json{{"objects", {{"print_stats_manager", {"sub_status"}}}}});
}

TEST_CASE("guided_load_waits: reads the reported field from a query reply", "[guided_load]") {
    const GuidedLoad g = steps();
    const auto reply = [](const char* sub) {
        return nlohmann::json{
            {"result", {{"status", {{"print_stats_manager", {{"sub_status", sub}}}}}}}};
    };
    CHECK(guided_load_waits(reply("push_filamenting"), g));
    CHECK_FALSE(guided_load_waits(reply(""), g));
    CHECK_FALSE(guided_load_waits(reply("bit_filamenting"), g));
    SECTION("firmware without the object answers no") {
        CHECK_FALSE(guided_load_waits(
            nlohmann::json{{"result", {{"status", nlohmann::json::object()}}}}, g));
        CHECK_FALSE(guided_load_waits(nlohmann::json(), g));
        CHECK_FALSE(guided_load_waits(nlohmann::json::array(), g));
    }
    SECTION("a non-string field answers no") {
        CHECK_FALSE(guided_load_waits(
            nlohmann::json{
                {"result", {{"status", {{"print_stats_manager", {{"sub_status", 3}}}}}}}},
            g));
    }
}

TEST_CASE("restore_after: the stock unload gets its sensor back, nothing else does",
          "[guided_load]") {
    CHECK(restore_after("M603") == "ENABLE_ALL_SENSOR");
    CHECK(restore_after("m603") == "ENABLE_ALL_SENSOR");
    CHECK(restore_after("M604").empty());
    CHECK(restore_after("UNLOAD_FILAMENT").empty());
}

// ============================================================================
// Firmware that stops for a hand push
// ============================================================================

TEST_CASE("GuidedLoadFlow: a waiting printer feeds once on the sensor edge", "[guided_load]") {
    GuidedLoadFlow flow(steps(), /*waits_for_push=*/true);
    REQUIRE(flow.stage() == GuidedLoadStage::AwaitPush);

    CHECK(flow.filament_detected() == steps().feed);
    CHECK(flow.stage() == GuidedLoadStage::Feeding);
    // A second edge or a tap while the bite runs must not stack a second feed.
    CHECK(flow.filament_detected().empty());
    CHECK(flow.extrude().empty());
    CHECK(flow.done().empty());

    CHECK(flow.command_finished(true).empty());
    CHECK(flow.stage() == GuidedLoadStage::ConfirmOutput);
}

TEST_CASE("GuidedLoadFlow: Extrude from the push step sends the feed", "[guided_load]") {
    GuidedLoadFlow flow(steps(), true);
    CHECK(flow.extrude() == steps().feed);
    CHECK(flow.stage() == GuidedLoadStage::Feeding);
}

TEST_CASE("GuidedLoadFlow: the sensor only matters while waiting for the push", "[guided_load]") {
    GuidedLoadFlow flow(steps(), true);
    (void)flow.extrude();
    (void)flow.command_finished(true);
    REQUIRE(flow.stage() == GuidedLoadStage::ConfirmOutput);
    CHECK(flow.filament_detected().empty());
    CHECK(flow.stage() == GuidedLoadStage::ConfirmOutput);
}

TEST_CASE("GuidedLoadFlow: Extrude more retries, Done flushes then cleans up", "[guided_load]") {
    GuidedLoadFlow flow(steps(), true);
    (void)flow.extrude();
    (void)flow.command_finished(true);

    CHECK(flow.extrude() == steps().retry);
    CHECK(flow.stage() == GuidedLoadStage::Feeding);
    CHECK(flow.command_finished(true).empty());

    CHECK(flow.done() == steps().flush + "\n" + steps().cleanup);
    CHECK(flow.stage() == GuidedLoadStage::Finishing);
    CHECK(flow.cancel() == steps().release + "\n" + steps().cleanup);
}

TEST_CASE("GuidedLoadFlow: finishing lands on Finished and stays quiet", "[guided_load]") {
    GuidedLoadFlow flow(steps(), true);
    (void)flow.extrude();
    (void)flow.command_finished(true);
    (void)flow.done();
    CHECK(flow.command_finished(true).empty());
    CHECK(flow.stage() == GuidedLoadStage::Finished);
    CHECK(flow.ended());
    CHECK(flow.cancel().empty());
    CHECK(flow.extrude().empty());
    CHECK(flow.command_finished(false).empty());
}

TEST_CASE("GuidedLoadFlow: cancelling a waiting printer releases it", "[guided_load]") {
    GuidedLoadFlow flow(steps(), true);
    CHECK(flow.cancel() == steps().release + "\n" + steps().cleanup);
    CHECK(flow.stage() == GuidedLoadStage::Cancelled);
    CHECK(flow.cancel().empty());
}

TEST_CASE("GuidedLoadFlow: a failed step cancels and cleans up once", "[guided_load]") {
    GuidedLoadFlow flow(steps(), true);
    (void)flow.extrude();
    CHECK(flow.command_finished(false) == steps().release + "\n" + steps().cleanup);
    CHECK(flow.stage() == GuidedLoadStage::Cancelled);
    CHECK(flow.command_finished(false).empty());
}

TEST_CASE("GuidedLoadFlow: a reply with nothing in flight changes nothing", "[guided_load]") {
    GuidedLoadFlow flow(steps(), true);
    CHECK(flow.command_finished(true).empty());
    CHECK(flow.stage() == GuidedLoadStage::AwaitPush);
}

// ============================================================================
// Firmware whose load macro already extruded
// ============================================================================

TEST_CASE("GuidedLoadFlow: an extruding printer opens on the question", "[guided_load]") {
    GuidedLoadFlow flow(steps(), /*waits_for_push=*/false);
    CHECK(flow.stage() == GuidedLoadStage::ConfirmOutput);
    CHECK(flow.filament_detected().empty());
    CHECK(flow.extrude() == steps().retry);
}

TEST_CASE("GuidedLoadFlow: older firmware is never sent the waiting-only commands",
          "[guided_load]") {
    SECTION("Done is the cleanup alone") {
        GuidedLoadFlow flow(steps(), false);
        CHECK(flow.done() == steps().cleanup);
        CHECK(flow.command_finished(true).empty());
        CHECK(flow.stage() == GuidedLoadStage::Finished);
    }
    SECTION("Cancel is the cleanup alone") {
        GuidedLoadFlow flow(steps(), false);
        CHECK(flow.cancel() == steps().cleanup);
    }
    SECTION("A failure is the cleanup alone") {
        GuidedLoadFlow flow(steps(), false);
        (void)flow.extrude();
        CHECK(flow.command_finished(false) == steps().cleanup);
    }
}

TEST_CASE("GuidedLoadFlow: steps with no flush, release or cleanup send nothing at the end",
          "[guided_load][cold_pull]") {
    GuidedLoad bare;
    bare.feed = "M83\nG1 E60 F300";
    bare.retry = "M83\nG1 E20 F300";
    GuidedLoadFlow flow(bare, /*waits_for_push=*/true);
    (void)flow.extrude();
    (void)flow.command_finished(true);
    CHECK(flow.done().empty());
    CHECK(flow.stage() == GuidedLoadStage::Finished);

    GuidedLoadFlow cancelled(bare, true);
    CHECK(cancelled.cancel().empty());
    CHECK(cancelled.stage() == GuidedLoadStage::Cancelled);
}

// ============================================================================
// A manual load: the screen heats first (prestonbrown/helixscreen#2000)
// ============================================================================

TEST_CASE("GuidedLoadFlow: a manual load sends nothing until the nozzle is hot",
          "[guided_load][manual_load]") {
    GuidedLoadFlow flow = GuidedLoadFlow::after_preheat(helix::hand_feed_steps());
    CHECK(flow.stage() == GuidedLoadStage::Heating);
    CHECK(flow.extrude().empty());
    CHECK(flow.filament_detected().empty());
    CHECK(flow.done().empty());
    CHECK(flow.command_finished(true).empty());
    CHECK(flow.stage() == GuidedLoadStage::Heating);

    SECTION("heated, then the hand push, the feed, more, and Done") {
        flow.heated();
        CHECK(flow.stage() == GuidedLoadStage::AwaitPush);
        CHECK(flow.extrude() == "M83\nG1 E60 F300");
        CHECK(flow.command_finished(true).empty());
        CHECK(flow.stage() == GuidedLoadStage::ConfirmOutput);
        CHECK(flow.extrude() == "M83\nG1 E20 F300");
        CHECK(flow.command_finished(true).empty());
        // Nothing to flush or clean up: Done ends it at once.
        CHECK(flow.done().empty());
        CHECK(flow.stage() == GuidedLoadStage::Finished);
    }
    SECTION("a cancel while heating ends it with nothing to send") {
        CHECK(flow.cancel().empty());
        CHECK(flow.stage() == GuidedLoadStage::Cancelled);
        flow.heated();
        CHECK(flow.stage() == GuidedLoadStage::Cancelled);
    }
}
