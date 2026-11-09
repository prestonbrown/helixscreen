// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_plan_collector.cpp
 * @brief How PrintStartCollector consumes a declared plan, through its public paths
 */

#include "../test_helpers/armed_timers.h"
#include "../test_helpers/preprint_config_scope.h"
#include "../test_helpers/print_start_collector_fixture.h"
#include "preprint_checklist.h"
#include "print_start_analyzer.h"
#include "printer_discovery.h"
#include "simulated_clock.h"
#include "thermal_rate_model.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int kSoak = static_cast<int>(PrintStartPhase::SOAKING);

using ManualClock = helix::sim::SimulatedClock::ManualScope;

class PlanFixture : public PrintStartCollectorHeaterFixture {
  public:
    void begin() {
        set_all_temps(250, 0, 250, 0); // no heating to price
        collector().start();
        drain_async_updates();
    }

    /// One pass of the collector's periodic ETA timer.
    void tick_eta() {
        for (lv_timer_t* timer : helix::test::armed_timers_for(&collector())) {
            lv_timer_ready(timer);
        }
        lv_timer_handler();
        drain_async_updates();
    }

    int remaining() {
        return lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
    }

    std::string time_left() {
        return lv_subject_get_string(state().print_state().get_print_start_time_left_subject());
    }

    PreprintConfigScope config_scope_;
};

PrintStartAnalysis plan_declaring_analysis() {
    PrintStartAnalysis analysis;
    analysis.found = true;
    analysis.declares_plan = true;
    return analysis;
}

} // namespace

TEST_CASE_METHOD(PlanFixture, "A plan line reprices the countdown and is not a phase",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    begin();
    tick_eta();
    const int before = remaining();
    REQUIRE(before > 0);
    REQUIRE(before < 6000);

    send_gcode_response("echo: HELIX:PLAN SOAKING=6000 BED_MESH=90 QGL=40 HOMING=30");
    CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);

    tick_eta();
    CHECK(remaining() >= 6000); // above the anchor the earlier estimate left
}

TEST_CASE_METHOD(PlanFixture, "A plan line with nothing valid changes nothing",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    begin();
    tick_eta();
    const int before = remaining();

    send_gcode_response("HELIX:PLAN SOAKING=abc FOO=5");
    CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
    tick_eta();
    CHECK(remaining() == before);
}

TEST_CASE_METHOD(PlanFixture, "A plan outside a pre-print leaves the next one alone",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    send_gcode_response("HELIX:PLAN SOAKING=6000");
    begin();
    tick_eta();
    CHECK(remaining() < 6000);
}

TEST_CASE_METHOD(PlanFixture, "A plan for the phase under way counts from now",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    begin();
    send_gcode_response("HELIX:PHASE:SOAKING");
    clock.advance(std::chrono::seconds(120));

    send_gcode_response("HELIX:PLAN SOAKING=100");
    tick_eta();
    const int low = remaining();
    send_gcode_response("HELIX:PLAN SOAKING=700");
    tick_eta();
    CHECK(remaining() - low == 600);
}

TEST_CASE_METHOD(PlanFixture, "A declared plan holds back the first estimate",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    collector().set_macro_analysis(plan_declaring_analysis());
    begin();
    tick_eta();
    REQUIRE(remaining() == 0);
    REQUIRE(time_left().empty());

    SECTION("until the plan arrives") {
        send_gcode_response("HELIX:PLAN SOAKING=6000");
        tick_eta();
        CHECK(remaining() >= 6000);
    }
    SECTION("until a phase signal") {
        send_gcode_response("HELIX:PHASE:HOMING");
        tick_eta();
        CHECK(remaining() > 0);
    }
    SECTION("until the wait runs out") {
        clock.advance(std::chrono::seconds(9));
        tick_eta();
        REQUIRE(remaining() == 0);
        clock.advance(std::chrono::seconds(2));
        tick_eta();
        CHECK(remaining() > 0);
    }
}

TEST_CASE_METHOD(PlanFixture, "A declared analysis is spent by one start",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    collector().set_macro_analysis(plan_declaring_analysis());
    begin();
    collector().stop();
    drain_async_updates();

    begin();
    tick_eta();
    CHECK(remaining() > 0);
}

TEST_CASE_METHOD(PlanFixture, "A declared phase holds the timeouts for its length",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    begin();
    collector().enable_fallbacks();
    send_gcode_response("HELIX:PLAN HOMING=60 SOAKING=600");
    send_gcode_response("HELIX:PHASE:HOMING");
    clock.advance(std::chrono::seconds(800)); // homing ran long: the deadline is near
    send_gcode_response("HELIX:PHASE:SOAKING");
    set_all_temps(1000, 1000, 2000, 2000);

    clock.advance(std::chrono::seconds(300));
    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    clock.advance(std::chrono::seconds(400)); // hold over and silent past the quiet gate
    tick_fallbacks();
    CHECK(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PlanFixture, "A phase past its declared length says so",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    begin();
    send_gcode_response("HELIX:PLAN SOAKING=60");
    send_gcode_response("HELIX:PHASE:SOAKING");
    clock.advance(std::chrono::seconds(30));
    tick_eta();
    REQUIRE(time_left().find("Running over") == std::string::npos);

    clock.advance(std::chrono::seconds(40));
    tick_eta();
    CHECK(time_left().find("Running over") != std::string::npos);
}

TEST_CASE_METHOD(PlanFixture, "The checklist uses declared seconds", "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    begin();
    send_gcode_response("HELIX:PLAN SOAKING=600");
    const auto input = collector().checklist_input();
    CHECK(input.predicted_seconds.at(kSoak) == 600);
    CHECK(std::find(input.expected_order.begin(), input.expected_order.end(),
                    PrintStartPhase::SOAKING) != input.expected_order.end());
}

TEST_CASE_METHOD(PlanFixture, "A finished run records its plan", "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    begin();

    SECTION("with a plan") {
        send_gcode_response("HELIX:PLAN SOAKING=600");
        clock.advance(std::chrono::seconds(30));
        send_gcode_response("HELIX:PHASE:COMPLETE");
        drain_async_updates();
        const auto entries = PreprintPredictor::load_entries_from_config();
        REQUIRE(entries.size() == 1);
        REQUIRE(entries[0].declared);
        CHECK(entries[0].declared->at(kSoak) == 600);
    }
    SECTION("without one") {
        clock.advance(std::chrono::seconds(30));
        send_gcode_response("HELIX:PHASE:COMPLETE");
        drain_async_updates();
        const auto entries = PreprintPredictor::load_entries_from_config();
        REQUIRE(entries.size() == 1);
        CHECK_FALSE(entries[0].declared);
    }
}

TEST_CASE_METHOD(PlanFixture, "A plan stored on the printer survives a screen restart",
                 "[print][collector][plan]") {
    ManualClock clock(helix::sim::SimSpeed::of(1.0));
    client().set_object_status("gcode_macro _HELIX_STATE",
                               json{{"plan", "SOAKING=6000 BED_MESH=90"}});
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(json::array({"gcode_macro _HELIX_PLAN"}));

    SECTION("restored at start") {
        state().set_hardware(discovery);
        begin();
        tick_eta();
        CHECK(remaining() >= 6000);
    }
    SECTION("a live line replaces the stored copy") {
        state().set_hardware(discovery);
        begin();
        send_gcode_response("HELIX:PLAN BED_MESH=10");
        tick_eta();
        CHECK(remaining() < 6000);
    }
    SECTION("a printer without the macro is not asked") {
        begin();
        tick_eta();
        CHECK(remaining() < 6000);
    }
}

TEST_CASE("PrintStartAnalyzer notices a _HELIX_PLAN call", "[print][collector][plan]") {
    const auto declares = [](const std::string& gcode) {
        return PrintStartAnalyzer::parse_macro("PRINT_START", gcode).declares_plan;
    };
    CHECK(declares("_HELIX_PLAN SOAKING=600\nG28\n"));
    CHECK(declares("  _helix_plan SOAKING={soak}\n"));
    CHECK(declares("{% if x %} _HELIX_PLAN Q=1 {% endif %}\n"));
    CHECK_FALSE(declares("_HELIX_PLANX SOAKING=1\n"));
    CHECK_FALSE(declares("MY_HELIX_PLAN SOAKING=1\n"));
    CHECK_FALSE(declares("# _HELIX_PLAN SOAKING=1\n; _HELIX_PLAN Q=1\n"));
    CHECK_FALSE(declares("G28 ; _HELIX_PLAN SOAKING=1\n"));
}
