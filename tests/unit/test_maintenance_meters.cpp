// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_maintenance_meters.cpp
 * @brief The maintenance meters' rules: accrual, the due latch, reset, intervals, the
 * persisted form, the plugin-store migration and the print_duration tracker.
 *
 * Run with: ./build/bin/helix-tests "[maintenance][meters]"
 */

#include "maintenance_meters.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::maintenance;
using json = nlohmann::json;

namespace {
constexpr int kFilter = static_cast<int>(MeterId::Filter);
constexpr int kNozzle = static_cast<int>(MeterId::Nozzle);
constexpr int kRails = static_cast<int>(MeterId::Rails);

Meter& meter(State& s, int i) {
    return s.meters[static_cast<size_t>(i)];
}
} // namespace

TEST_CASE("maintenance defaults: filter 200, nozzle 500, rails 300, nothing used",
          "[maintenance][meters]") {
    State s = default_state();
    CHECK(meter(s, kFilter).interval_h == 200);
    CHECK(meter(s, kNozzle).interval_h == 500);
    CHECK(meter(s, kRails).interval_h == 300);
    CHECK(s.total_h == 0.0);
    for (const Meter& m : s.meters) {
        CHECK(m.used_h == 0.0);
        CHECK_FALSE(m.reset_at_h.has_value());
        CHECK_FALSE(m.notified);
    }
}

TEST_CASE("accrue adds to the total and to enabled meters only", "[maintenance][meters]") {
    State s = default_state();
    set_interval(s, kNozzle, 0);
    accrue(s, 1.5);
    CHECK(s.total_h == Catch::Approx(1.5));
    CHECK(meter(s, kFilter).used_h == Catch::Approx(1.5));
    CHECK(meter(s, kNozzle).used_h == 0.0);
    CHECK(meter(s, kRails).used_h == Catch::Approx(1.5));

    SECTION("zero, negative and non-finite hours change nothing") {
        accrue(s, 0.0);
        accrue(s, -3.0);
        accrue(s, std::numeric_limits<double>::quiet_NaN());
        accrue(s, std::numeric_limits<double>::infinity());
        CHECK(s.total_h == Catch::Approx(1.5));
        CHECK(meter(s, kFilter).used_h == Catch::Approx(1.5));
    }
}

TEST_CASE("crossing the interval reports the meter once, until a reset re-arms it",
          "[maintenance][meters]") {
    State s = default_state();
    set_interval(s, kFilter, 25);
    CHECK(accrue(s, 24.9).empty());
    CHECK_FALSE(meter(s, kFilter).notified);

    const auto due = accrue(s, 0.1);
    REQUIRE(due.size() == 1);
    CHECK(due[0] == kFilter);
    CHECK(meter(s, kFilter).notified);
    CHECK(meter_status(meter(s, kFilter)) == ValueStatus::Danger);

    CHECK(accrue(s, 10.0).empty());

    reset_meter(s, kFilter);
    CHECK(accrue(s, 24.0).empty());
    CHECK(accrue(s, 1.0) == std::vector<int>{kFilter});
}

TEST_CASE("reset starts a new interval stamped with the total", "[maintenance][meters]") {
    State s = default_state();
    accrue(s, 120.4);
    reset_meter(s, kRails);
    CHECK(meter(s, kRails).used_h == 0.0);
    REQUIRE(meter(s, kRails).reset_at_h.has_value());
    CHECK(*meter(s, kRails).reset_at_h == Catch::Approx(120.4));
    CHECK(meter(s, kFilter).used_h == Catch::Approx(120.4));

    SECTION("an out-of-range meter is ignored") {
        reset_meter(s, -1);
        reset_meter(s, kMeterCount);
        CHECK(meter(s, kFilter).used_h == Catch::Approx(120.4));
    }
}

TEST_CASE("a raised interval re-arms the due notice, a lowered one does not",
          "[maintenance][meters]") {
    State s = default_state();
    set_interval(s, kFilter, 0);
    set_interval(s, kRails, 0);
    set_interval(s, kNozzle, 100);
    accrue(s, 150.0);
    REQUIRE(meter(s, kNozzle).notified);

    set_interval(s, kNozzle, 150);
    CHECK(meter(s, kNozzle).notified);

    set_interval(s, kNozzle, 200);
    CHECK_FALSE(meter(s, kNozzle).notified);
    CHECK(accrue(s, 50.0) == std::vector<int>{kNozzle});
}

TEST_CASE("an interval of 0 turns a meter off: no accrual, not most due", "[maintenance][meters]") {
    State s = default_state();
    accrue(s, 190.0);
    REQUIRE(most_due(s) == kFilter);
    set_interval(s, kFilter, 0);
    CHECK_FALSE(enabled(meter(s, kFilter)));
    CHECK(used_fraction(meter(s, kFilter)) == 0.0);
    CHECK(hours_left(meter(s, kFilter)) == 0);
    CHECK(most_due(s) == kRails);

    set_interval(s, kNozzle, 0);
    set_interval(s, kRails, 0);
    CHECK_FALSE(most_due(s).has_value());
    CHECK(accrue(s, 5.0).empty());
    CHECK(s.total_h == Catch::Approx(195.0));
}

TEST_CASE("hours left round up and stop at zero; status bands follow the used share",
          "[maintenance][meters]") {
    State s = default_state();
    set_interval(s, kFilter, 100);
    accrue(s, 74.2);
    CHECK(hours_left(meter(s, kFilter)) == 26);
    CHECK(hours_used(meter(s, kFilter)) == 74);
    CHECK(meter_status(meter(s, kFilter)) == ValueStatus::Normal);
    accrue(s, 0.8);
    CHECK(meter_status(meter(s, kFilter)) == ValueStatus::Warn);
    accrue(s, 40.0);
    CHECK(hours_left(meter(s, kFilter)) == 0);
    CHECK(used_fraction(meter(s, kFilter)) == 1.0);
}

TEST_CASE("intervals snap to the offered choices", "[maintenance][meters]") {
    CHECK(snap_interval(-5) == 0);
    CHECK(snap_interval(0) == 0);
    CHECK(snap_interval(1) == 25);
    CHECK(snap_interval(200) == 200);
    CHECK(snap_interval(225) == 200);
    CHECK(snap_interval(226) == 250);
    CHECK(snap_interval(5000) == 2000);
    CHECK(interval_choice_index(0) == 0);
    CHECK(interval_choice_index(200) == 5);
    CHECK(kIntervalChoices[static_cast<size_t>(interval_choice_index(2000))] == 2000);
}

TEST_CASE("the persisted form round-trips", "[maintenance][meters]") {
    State s = default_state();
    set_interval(s, kNozzle, 0);
    set_interval(s, kFilter, 25);
    accrue(s, 30.25);
    reset_meter(s, kRails);
    accrue(s, 2.0);

    const State back = from_json(json::parse(to_json(s).dump()));
    CHECK(back.total_h == Catch::Approx(32.25));
    for (int i = 0; i < kMeterCount; ++i) {
        INFO(i);
        const Meter& a = s.meters[static_cast<size_t>(i)];
        const Meter& b = back.meters[static_cast<size_t>(i)];
        CHECK(b.interval_h == a.interval_h);
        CHECK(b.used_h == Catch::Approx(a.used_h));
        CHECK(b.notified == a.notified);
        CHECK(b.reset_at_h.has_value() == a.reset_at_h.has_value());
    }
    CHECK(*back.meters[kRails].reset_at_h == Catch::Approx(30.25));
    CHECK(back.meters[kFilter].notified);
}

TEST_CASE("a corrupt record falls back field by field", "[maintenance][meters]") {
    const json j = json::parse(R"({
        "total_h": "lots",
        "meters": {
            "filter": {"interval_h": "x", "used_h": -4, "notified": "yes"},
            "nozzle": {"interval_h": 260, "used_h": 1e12, "reset_at_h": true},
            "rails": 7
        }
    })");
    const State s = from_json(j);
    CHECK(s.total_h == 0.0);
    CHECK(s.meters[kFilter].interval_h == 200);
    CHECK(s.meters[kFilter].used_h == 0.0);
    CHECK_FALSE(s.meters[kFilter].notified);
    CHECK(s.meters[kNozzle].interval_h == 250);
    CHECK(s.meters[kNozzle].used_h == 0.0);
    CHECK_FALSE(s.meters[kNozzle].reset_at_h.has_value());
    CHECK(s.meters[kRails].interval_h == 300);

    CHECK(from_json(json()).meters[kNozzle].interval_h == 500);
    CHECK(from_json(json::array()).total_h == 0.0);
}

TEST_CASE("the plugin's store and settings migrate", "[maintenance][meters][migration]") {
    const json storage = json::parse(R"({
        "state": {
            "total_h": 412.5,
            "meters": {
                "filter": {"used": 210.0, "reset_at": 202.5, "notified": true},
                "nozzle": {"used": 12.25, "notified": false},
                "rails": {"used": "bad"}
            }
        }
    })");
    const json settings = json::parse(R"({"filter_interval_h": 200, "nozzle_interval_h": 0,
                                          "rails_interval_h": 333})");
    const auto migrated = from_plugin(storage, settings);
    REQUIRE(migrated.has_value());
    const State& s = *migrated;
    CHECK(s.total_h == Catch::Approx(412.5));
    CHECK(s.meters[kFilter].interval_h == 200);
    CHECK(s.meters[kFilter].used_h == Catch::Approx(210.0));
    REQUIRE(s.meters[kFilter].reset_at_h.has_value());
    CHECK(*s.meters[kFilter].reset_at_h == Catch::Approx(202.5));
    CHECK(s.meters[kFilter].notified);
    CHECK(s.meters[kNozzle].interval_h == 0);
    CHECK(s.meters[kNozzle].used_h == Catch::Approx(12.25));
    CHECK(s.meters[kRails].interval_h == 300);
    CHECK(s.meters[kRails].used_h == 0.0);

    SECTION("only a store: the plugin's default intervals stay") {
        const auto only_store = from_plugin(storage, json());
        REQUIRE(only_store.has_value());
        CHECK(only_store->meters[kNozzle].interval_h == 500);
        CHECK(only_store->meters[kNozzle].used_h == Catch::Approx(12.25));
    }
    SECTION("only settings: intervals carry, no hours") {
        const auto only_settings = from_plugin(json(), settings);
        REQUIRE(only_settings.has_value());
        CHECK(only_settings->meters[kNozzle].interval_h == 0);
        CHECK(only_settings->total_h == 0.0);
    }
    SECTION("nothing to carry over") {
        CHECK_FALSE(from_plugin(json(), json()).has_value());
        CHECK_FALSE(from_plugin(json::object(), json::object()).has_value());
        CHECK_FALSE(from_plugin(json::parse(R"({"other": 1})"), json()).has_value());
    }
    SECTION("a latch whose interval snapped above the used hours re-arms") {
        const json raised = json::parse(R"({"filter_interval_h": 240})");
        const auto r = from_plugin(storage, raised);
        REQUIRE(r.has_value());
        CHECK(r->meters[kFilter].interval_h == 250);
        CHECK_FALSE(r->meters[kFilter].notified);
    }
}

TEST_CASE("DurationTracker: the first reading seeds, rises accrue, a drop re-seeds",
          "[maintenance][meters]") {
    DurationTracker t;
    CHECK(t.observe(3600) == 0);
    CHECK(t.observe(3600) == 0);
    CHECK(t.observe(3630) == 30);
    CHECK(t.observe(3631) == 1);
    CHECK(t.observe(5) == 0);
    CHECK(t.observe(65) == 0);
    CHECK(t.observe(125) == 60);
    CHECK(t.observe(-1) == 0);
    CHECK(t.observe(130) == 5);

    t.reset();
    CHECK(t.observe(9000) == 0);
    CHECK(t.observe(9010) == 10);
}

TEST_CASE("DurationTracker: a local zero before Klipper's running value counts nothing twice",
          "[maintenance][meters]") {
    DurationTracker t;
    CHECK(t.observe(1000) == 0);
    CHECK(t.observe(1060) == 60);
    // The screen zeroes its copy at a print start; Klipper's value is still 1060.
    CHECK(t.observe(0) == 0);
    CHECK(t.observe(1060) == 0);
    CHECK(t.observe(1120) == 60);
}
