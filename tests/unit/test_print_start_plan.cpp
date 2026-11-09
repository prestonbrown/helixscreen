// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_plan.cpp
 * @brief HELIX:PLAN parsing, merging and how declared durations price a start
 */

#include "../test_helpers/preprint_config_scope.h"
#include "config.h"
#include "preprint_predictor.h"
#include "print_start_plan.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

constexpr int kSoak = static_cast<int>(PrintStartPhase::SOAKING);
constexpr int kMesh = static_cast<int>(PrintStartPhase::BED_MESH);
constexpr int kBed = static_cast<int>(PrintStartPhase::HEATING_BED);
constexpr int kNozzle = static_cast<int>(PrintStartPhase::HEATING_NOZZLE);
constexpr int kHoming = static_cast<int>(PrintStartPhase::HOMING);

PrintStartPlanLine parsed(const std::string& line) {
    const auto result = parse_print_start_plan(line);
    REQUIRE(result.has_value());
    return *result;
}

bool drops_value(const std::string& value) {
    const auto line = parsed("HELIX:PLAN SOAKING=" + value + " BED_MESH=90");
    return line.durations.count(kSoak) == 0 && line.dropped.size() == 1 &&
           line.durations.at(kMesh) == 90;
}

PreprintHeating known_heating(float nozzle, float bed) {
    PreprintHeating heating;
    heating.nozzle_seconds = nozzle;
    heating.bed_seconds = bed;
    heating.nozzle_target_known = true;
    return heating;
}

PreprintEntry planned_entry(int total, int heating, std::map<int, int> phases,
                            PreprintDeclaredDurations declared) {
    PreprintEntry e;
    e.total_seconds = total;
    e.timestamp = 1;
    e.phase_durations = std::move(phases);
    e.heating_seconds = heating;
    e.declared = std::move(declared);
    return e;
}

float durations_sum(const PreprintEstimate& estimate) {
    float sum = 0.0f;
    for (const auto& [_, d] : estimate.durations) {
        sum += d;
    }
    return sum;
}

} // namespace

TEST_CASE("HELIX:PLAN accepts the line forms a printer emits", "[print_start_plan]") {
    const auto echoed = parsed("echo: HELIX:PLAN SOAKING=600 BED_MESH=90");
    CHECK(echoed.durations == PreprintDeclaredDurations{{kSoak, 600}, {kMesh, 90}});
    CHECK(echoed.dropped.empty());

    CHECK(parsed("// HELIX:PLAN soak=600").durations == PreprintDeclaredDurations{{kSoak, 600}});
    CHECK(parsed("  HELIX:PLAN\tSOAKING=600").durations.at(kSoak) == 600);
    CHECK(parsed("HELIX:PLAN SOAKING=600.0").durations.at(kSoak) == 600);
    CHECK(parsed("HELIX:PLAN SOAKING=599.6").durations.at(kSoak) == 600);
    CHECK(parsed("HELIX:PLAN SOAKING=0").durations.at(kSoak) == 0);
    CHECK(parsed("HELIX:PLAN SOAKING=21600").durations.at(kSoak) == 21600);
    CHECK(parsed("HELIX:PLAN SOAKING=600 SOAKING=300").durations.at(kSoak) == 300);
}

TEST_CASE("HELIX:PLAN needs its anchor", "[print_start_plan]") {
    CHECK_FALSE(parse_print_start_plan("M117 HELIX:PLAN SOAKING=600"));
    CHECK_FALSE(parse_print_start_plan("HELIX:PLANX SOAKING=1"));
    CHECK_FALSE(parse_print_start_plan("HELIX:PHASE:SOAKING"));
    CHECK_FALSE(parse_print_start_plan("echo: echo: HELIX:PLAN SOAKING=1"));
    CHECK_FALSE(parse_print_start_plan(""));

    const auto bare = parsed("HELIX:PLAN");
    CHECK(bare.durations.empty());
    CHECK_FALSE(bare.rejected_whole);
}

TEST_CASE("HELIX:PLAN drops only the bad token", "[print_start_plan]") {
    for (const char* value : {"-5", "abc", "1e9", "600x", "", "21601", "1.2.3", "."}) {
        INFO(value);
        CHECK(drops_value(value));
    }
    for (const char* token : {"IDLE=5", "COMPLETE=5", "INITIALIZING=5", "FOO=5", "SOAKING", "=5"}) {
        INFO(token);
        const auto line = parsed(std::string("HELIX:PLAN ") + token + " BED_MESH=90");
        CHECK(line.durations == PreprintDeclaredDurations{{kMesh, 90}});
        CHECK(line.dropped.size() == 1);
    }
    const auto spaced = parsed("HELIX:PLAN SOAKING= 600");
    CHECK(spaced.durations.empty());
    CHECK(spaced.dropped.size() == 2);

    const auto many = parsed("HELIX:PLAN A=1 B=2 C=3 D=4 E=5 F=6");
    CHECK(many.dropped.size() == 4);
}

TEST_CASE("HELIX:PLAN refuses an oversize line whole", "[print_start_plan]") {
    std::string at_limit = "HELIX:PLAN SOAKING=600 ";
    at_limit.append(kMaxPlanLineBytes - at_limit.size(), ' ');
    CHECK_FALSE(parsed(at_limit).rejected_whole);
    CHECK(parsed(at_limit).durations.at(kSoak) == 600);

    const auto over = parsed(at_limit + " ");
    CHECK(over.rejected_whole);
    CHECK(over.durations.empty());

    std::string tokens = "HELIX:PLAN";
    for (size_t i = 0; i < kMaxPlanTokens; ++i) {
        tokens += " SOAKING=1";
    }
    CHECK_FALSE(parsed(tokens).rejected_whole);
    const auto too_many = parsed(tokens + " SOAKING=1");
    CHECK(too_many.rejected_whole);
    CHECK(too_many.durations.empty());
}

TEST_CASE("merge_declared follows where the run is", "[print_start_plan]") {
    PreprintDeclaredDurations plan{{kMesh, 90}};

    SECTION("a finished phase is left alone") {
        merge_declared(plan, {{kMesh, 500}}, kSoak, 10, {kMesh});
        CHECK(plan.at(kMesh) == 90);
    }
    SECTION("the current phase counts from now") {
        merge_declared(plan, {{kSoak, 600}}, kSoak, 120, {});
        CHECK(plan.at(kSoak) == 720);
    }
    SECTION("a zero for the current phase stays zero") {
        merge_declared(plan, {{kSoak, 0}}, kSoak, 120, {});
        CHECK(plan.at(kSoak) == 0);
    }
    SECTION("a later phase is replaced") {
        merge_declared(plan, {{kMesh, 30}, {kSoak, 5}}, kHoming, 7, {});
        CHECK(plan.at(kMesh) == 30);
        CHECK(plan.at(kSoak) == 5);
    }
}

TEST_CASE("overtime balance is signed", "[print_start_plan]") {
    const std::map<int, int> expected{{kHoming, 40}, {kMesh, 100}, {kSoak, 600}};
    const std::map<int, int> actual{{kHoming, 100}, {kMesh, 80}};
    CHECK(overtime_balance_seconds(expected, actual, kSoak, 630) == 70);
    CHECK(overtime_balance_seconds(expected, actual, kSoak, 10) == 40);
    CHECK(overtime_balance_seconds({}, actual, kSoak, 630) == 0);
}

TEST_CASE("declared_phase_running_over needs a real overrun", "[print_start_plan]") {
    const PreprintDeclaredDurations plan{{kSoak, 600}, {kMesh, 0}};
    CHECK_FALSE(declared_phase_running_over(plan, kSoak, 600));
    CHECK(declared_phase_running_over(plan, kSoak, 601));
    CHECK_FALSE(declared_phase_running_over(plan, kMesh, 9000));
    CHECK_FALSE(declared_phase_running_over(plan, kHoming, 9000));
}

TEST_CASE("a plan overlays the checklist inputs", "[print_start_plan]") {
    std::vector<PrintStartPhase> order{PrintStartPhase::HOMING, PrintStartPhase::BED_MESH,
                                       PrintStartPhase::PURGING};
    std::map<int, int> predicted{{kHoming, 30}, {kMesh, 90}};
    apply_declared_to_checklist({{kSoak, 600}, {kMesh, 0}, {kHoming, 45}}, order, predicted);
    CHECK(order == std::vector<PrintStartPhase>{PrintStartPhase::HOMING, PrintStartPhase::SOAKING,
                                                PrintStartPhase::PURGING});
    CHECK(predicted == std::map<int, int>{{kHoming, 45}, {kSoak, 600}});
}

TEST_CASE("estimate prices declared durations without history", "[print_start_plan]") {
    PreprintConfigScope scope;
    PreprintPredictor p;

    SECTION("a declared phase joins the durations and the total is their sum") {
        const auto e = p.estimate(known_heating(0, 0), {}, {{kSoak, 600}});
        CHECK(e.durations.at(kSoak) == Catch::Approx(600.0f));
        CHECK(e.total_seconds == Catch::Approx(durations_sum(e)));
    }
    SECTION("declared heating replaces the thermal price and counts once") {
        const auto without = p.estimate(known_heating(0, 200), {}, {{kSoak, 1}});
        const auto with = p.estimate(known_heating(0, 200), {}, {{kBed, 300}, {kSoak, 1}});
        CHECK(with.durations.at(kBed) == Catch::Approx(300.0f));
        CHECK(with.total_seconds - without.total_seconds == Catch::Approx(100.0f));
    }
    SECTION("a nozzle declared 0 needs no placeholder") {
        const auto e = p.estimate(PreprintHeating{}, {}, {{kNozzle, 0}});
        CHECK(e.durations.count(kNozzle) == 0);
    }
    SECTION("zero removes the phase") {
        const auto e = p.estimate(known_heating(0, 0), {}, {{kHoming, 0}});
        CHECK(e.durations.count(kHoming) == 0);
    }
    SECTION("a declared duration beats an override that skips the phase") {
        const auto e = p.estimate(known_heating(0, 0), {{kSoak, false}}, {{kSoak, 600}});
        CHECK(e.durations.at(kSoak) == Catch::Approx(600.0f));
    }
}

TEST_CASE("with_declared_heating substitutes only what was declared", "[print_start_plan]") {
    PreprintHeating heating;
    heating.nozzle_seconds = 50.0f;
    heating.bed_seconds = 70.0f;

    const auto none = PreprintPredictor::with_declared_heating(heating, {{kSoak, 9}});
    CHECK(none.nozzle_seconds == Catch::Approx(50.0f));
    CHECK(none.bed_seconds == Catch::Approx(70.0f));
    CHECK_FALSE(none.nozzle_target_known);

    const auto both = PreprintPredictor::with_declared_heating(heating, {{kBed, 5}, {kNozzle, 6}});
    CHECK(both.bed_seconds == Catch::Approx(5.0f));
    CHECK(both.nozzle_seconds == Catch::Approx(6.0f));
    CHECK(both.nozzle_target_known);
}

TEST_CASE("a plan never double counts the wall clock", "[print_start_plan]") {
    PreprintConfigScope scope;
    PreprintPredictor p;

    SECTION("unplanned history leaves the sum of the durations") {
        PreprintEntry e;
        e.total_seconds = 2000; // a silent soak sits inside this
        e.timestamp = 1;
        e.phase_durations = {{kMesh, 90}};
        e.heating_seconds = 100;
        p.load_entries({e}, 0);
        const auto estimate = p.estimate(known_heating(0, 0), {}, {{kSoak, 600}});
        CHECK(estimate.total_seconds == Catch::Approx(durations_sum(estimate)));
        CHECK(estimate.total_seconds < 2000.0f);
    }
    SECTION("planned entries give a residual") {
        p.load_entries({planned_entry(1200, 300, {{kSoak, 600}}, {{kSoak, 600}})}, 0);
        const auto e = p.estimate(known_heating(0, 120), {}, {{kSoak, 900}});
        CHECK(e.total_seconds == Catch::Approx(300.0f + 120.0f + 900.0f));
    }
    SECTION("a declared phase that never signalled subtracts its declared value") {
        p.load_entries({planned_entry(1200, 300, {}, {{kSoak, 600}})}, 0);
        const auto e = p.estimate(known_heating(0, 0), {}, {{kSoak, 900}});
        CHECK(e.total_seconds == Catch::Approx(300.0f + 900.0f));
    }
    SECTION("an overrun stays out of the residual") {
        p.load_entries({planned_entry(1200, 300, {{kSoak, 700}}, {{kSoak, 600}})}, 0);
        const auto e = p.estimate(known_heating(0, 0), {}, {{kSoak, 900}});
        CHECK(e.total_seconds == Catch::Approx(200.0f + 900.0f));
    }
    SECTION("declared heating is priced once, not as a declared phase") {
        p.load_entries({planned_entry(1200, 300, {}, {{kBed, 300}})}, 0);
        const auto e = p.estimate(known_heating(0, 0), {}, {{kBed, 500}});
        CHECK(e.total_seconds == Catch::Approx(900.0f + 500.0f));
    }
    SECTION("a planned entry with no priced heating cannot give a residual") {
        auto entry = planned_entry(1200, 300, {{kSoak, 600}}, {{kSoak, 600}});
        entry.heating_seconds.reset();
        p.load_entries({entry}, 0);
        const auto e = p.estimate(known_heating(0, 0), {}, {{kSoak, 900}});
        CHECK(e.total_seconds == Catch::Approx(durations_sum(e)));
    }
}

TEST_CASE("an estimate without a plan treats planned entries as ordinary runs",
          "[print_start_plan]") {
    PreprintConfigScope scope;
    PreprintEntry plain;
    plain.total_seconds = 400;
    plain.timestamp = 1;
    plain.phase_durations = {{kMesh, 80}};
    plain.heating_seconds = 100;
    PreprintEntry planned = plain;
    planned.declared = PreprintDeclaredDurations{{kSoak, 200}};

    PreprintPredictor mixed;
    mixed.load_entries({plain, planned}, 0);
    PreprintPredictor unmarked;
    unmarked.load_entries({plain, plain}, 0);

    const auto a = mixed.estimate(known_heating(30, 40));
    const auto b = unmarked.estimate(known_heating(30, 40));
    CHECK(a.total_seconds == Catch::Approx(b.total_seconds));
    CHECK(a.durations == b.durations);
}

TEST_CASE("declared durations survive the history file", "[print_start_plan]") {
    PreprintConfigScope scope;
    PreprintEntry e = planned_entry(100, 10, {{kMesh, 20}}, {{kSoak, 600}, {kMesh, 0}});
    e.timestamp = 12;
    PreprintEntry plain;
    plain.total_seconds = 50;
    plain.timestamp = 13;

    const auto stored = PreprintPredictor::entries_to_json({e, plain});
    CHECK(stored[0]["declared"]["SOAKING"].get<int>() == 600);
    CHECK(stored[0]["declared"]["BED_MESH"].get<int>() == 0);
    CHECK_FALSE(stored[1].contains("declared"));

    nlohmann::json doctored = stored;
    doctored[0]["declared"]["NOT_A_PHASE"] = 5;
    doctored[0]["declared"]["IDLE"] = 5;
    doctored[0]["declared"]["QGL"] = -3;
    doctored[0]["declared"]["PURGING"] = "oops";
    doctored[0]["declared"]["CLEANING"] = 99999;
    Config::get_instance()->set<nlohmann::json>("/print_start_history/entries", doctored);

    const auto loaded = PreprintPredictor::load_entries_from_config();
    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded[0].declared);
    CHECK(*loaded[0].declared == PreprintDeclaredDurations{{kSoak, 600}, {kMesh, 0}});
    CHECK_FALSE(loaded[1].declared);
}
