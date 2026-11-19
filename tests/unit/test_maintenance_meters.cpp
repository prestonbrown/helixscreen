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
constexpr int kRails = static_cast<int>(MeterId::XyAxis);
constexpr int kZ = static_cast<int>(MeterId::ZAxis);
constexpr int kFans = static_cast<int>(MeterId::ElectronicsFans);
constexpr int kBelt = static_cast<int>(MeterId::BeltTension);
constexpr int64_t kNow = 1800000000;
constexpr int64_t kDay = 86400;

Meter& meter(State& s, int i) {
    return s.meters[static_cast<size_t>(i)];
}

/// Every meter seeded with its generic default, as on an enclosed printer with a board fan.
State seeded_state() {
    State s = default_state();
    seed_defaults(s, Applicability{true, false, false, true}, ModelDefaults{});
    return s;
}
} // namespace

TEST_CASE("the meters: keys, order and generic defaults", "[maintenance][meters]") {
    CHECK(std::vector<std::string>(kMeterKeys.begin(), kMeterKeys.end()) ==
          std::vector<std::string>{"rails", "z_axis", "nozzle", "extruder", "filter",
                                   "electronics_fans", "belt_tension"});
    State s = seeded_state();
    CHECK(meter(s, kRails).interval_h == 300);
    CHECK(meter(s, kZ).interval_h == 500);
    CHECK(meter(s, kNozzle).interval_h == 500);
    CHECK(meter(s, kFilter).interval_h == 200);
    CHECK(meter(s, kFans).interval_h == 500);
    CHECK(meter(s, kBelt).interval_h == 0);
    CHECK(s.total_h == 0.0);
    for (const Meter& m : s.meters) {
        CHECK(m.used_h == 0.0);
        CHECK_FALSE(m.reset_at_h.has_value());
        CHECK_FALSE(m.notified);
    }
}

TEST_CASE("a fresh state is unseeded and off", "[maintenance][meters]") {
    for (const Meter& m : default_state().meters) {
        CHECK_FALSE(m.seeded);
        CHECK_FALSE(enabled(m));
    }
}

TEST_CASE("accrue adds to the total and to enabled meters only", "[maintenance][meters]") {
    State s = seeded_state();
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

TEST_CASE("take_pending_notices names each due meter once, until a reset re-arms it",
          "[maintenance][meters]") {
    State s = seeded_state();
    set_interval(s, kFilter, 25);
    accrue(s, 24.9);
    CHECK(take_pending_notices(s).empty());
    CHECK_FALSE(meter(s, kFilter).notified);

    accrue(s, 0.1);
    CHECK(meter(s, kFilter).notified == false);
    CHECK(take_pending_notices(s) == std::vector<int>{kFilter});
    CHECK(meter(s, kFilter).notified);
    CHECK(meter_status(meter(s, kFilter)) == ValueStatus::Danger);

    accrue(s, 10.0);
    CHECK(take_pending_notices(s).empty());

    reset_meter(s, kFilter);
    accrue(s, 24.0);
    CHECK(take_pending_notices(s).empty());
    accrue(s, 1.0);
    CHECK(take_pending_notices(s) == std::vector<int>{kFilter});
}

TEST_CASE("reset starts a new interval stamped with the total", "[maintenance][meters]") {
    State s = seeded_state();
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
    State s = seeded_state();
    for (int i = 0; i < kMeterCount; ++i)
        set_interval(s, i, 0);
    set_interval(s, kNozzle, 100);
    accrue(s, 150.0);
    take_pending_notices(s);
    REQUIRE(meter(s, kNozzle).notified);

    set_interval(s, kNozzle, 150);
    CHECK(meter(s, kNozzle).notified);

    set_interval(s, kNozzle, 200);
    CHECK_FALSE(meter(s, kNozzle).notified);
    accrue(s, 50.0);
    CHECK(take_pending_notices(s) == std::vector<int>{kNozzle});
}

TEST_CASE("an interval of 0 turns a meter off: no accrual, not most due", "[maintenance][meters]") {
    State s = seeded_state();
    accrue(s, 190.0);
    REQUIRE(most_due(s) == kFilter);
    set_interval(s, kFilter, 0);
    CHECK_FALSE(enabled(meter(s, kFilter)));
    CHECK(used_fraction(meter(s, kFilter)) == 0.0);
    CHECK(hours_left(meter(s, kFilter)) == 0);
    CHECK(most_due(s) == kRails);

    for (int i = 0; i < kMeterCount; ++i)
        set_interval(s, i, 0);
    CHECK_FALSE(most_due(s).has_value());
    accrue(s, 5.0);
    CHECK(take_pending_notices(s).empty());
    CHECK(s.total_h == Catch::Approx(195.0));
}

TEST_CASE("hours left round up and stop at zero; status bands follow the used share",
          "[maintenance][meters]") {
    State s = seeded_state();
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
    State s = seeded_state();
    set_interval(s, kNozzle, 0);
    set_interval(s, kFilter, 25);
    accrue(s, 30.25);
    take_pending_notices(s);
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
    CHECK_FALSE(s.meters[kFilter].seeded);
    CHECK(s.meters[kFilter].interval_h == 0);
    CHECK(s.meters[kFilter].used_h == 0.0);
    CHECK_FALSE(s.meters[kFilter].notified);
    CHECK(s.meters[kNozzle].interval_h == 250);
    CHECK(s.meters[kNozzle].used_h == 0.0);
    CHECK_FALSE(s.meters[kNozzle].reset_at_h.has_value());
    CHECK_FALSE(s.meters[kRails].seeded);

    CHECK_FALSE(from_json(json()).meters[kNozzle].seeded);
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

TEST_CASE("accrue can leave the filter to a firmware counter", "[maintenance][meters]") {
    State s = seeded_state();
    accrue(s, 2.0, true);
    CHECK(s.total_h == Catch::Approx(2.0));
    CHECK(meter(s, kFilter).used_h == Catch::Approx(0.0));
    CHECK(meter(s, kRails).used_h == Catch::Approx(2.0));
    CHECK(meter(s, kNozzle).used_h == Catch::Approx(2.0));
}

TEST_CASE("firmware filter hours replace the filter's used hours and fall due once",
          "[maintenance][meters]") {
    State s = seeded_state();
    REQUIRE(meter(s, kFilter).interval_h == 200);
    apply_firmware_filter_hours(s, 12.5);
    CHECK(meter(s, kFilter).used_h == Catch::Approx(12.5));
    CHECK(s.total_h == Catch::Approx(0.0));
    apply_firmware_filter_hours(s, 200.0);
    CHECK(take_pending_notices(s) == std::vector<int>{kFilter});
    apply_firmware_filter_hours(s, 210.0);
    CHECK(take_pending_notices(s).empty());
    // The counter going back (reset on the printer) lowers the hours.
    reset_meter(s, kFilter);
    apply_firmware_filter_hours(s, 0.1);
    CHECK(meter(s, kFilter).used_h == Catch::Approx(0.1));
    apply_firmware_filter_hours(s, -1.0);
    CHECK(meter(s, kFilter).used_h == Catch::Approx(0.1));
}

TEST_CASE("an Off filter meter takes no firmware hours", "[maintenance][meters]") {
    State s = seeded_state();
    set_interval(s, kFilter, 0);
    apply_firmware_filter_hours(s, 500.0);
    CHECK(meter(s, kFilter).used_h == Catch::Approx(0.0));
}

TEST_CASE("a firmware filter counter reset elsewhere re-arms the due notice",
          "[maintenance][meters]") {
    State s = seeded_state();
    apply_firmware_filter_hours(s, 200.0);
    CHECK(take_pending_notices(s) == std::vector<int>{kFilter});
    // Reset on the printer's console: the hours fall without reset_meter() running here.
    apply_firmware_filter_hours(s, 0.0);
    CHECK_FALSE(meter(s, kFilter).notified);
    apply_firmware_filter_hours(s, 200.0);
    CHECK(take_pending_notices(s) == std::vector<int>{kFilter});
}

TEST_CASE("a record from before the new meters keeps its three and leaves the rest unseeded",
          "[maintenance][meters][migration]") {
    const State s = from_json(json::parse(R"({"total_h": 900, "meters": {
        "rails": {"interval_h": 300, "used_h": 340, "reset_at_h": 560},
        "nozzle": {"interval_h": 500, "used_h": 12},
        "filter": {"interval_h": 200, "used_h": 80}}})"));
    CHECK(s.meters[kRails].seeded);
    CHECK(s.meters[kRails].interval_h == 300);
    CHECK(s.meters[kFilter].interval_h == 200);
    CHECK_FALSE(s.meters[kZ].seeded);
    CHECK_FALSE(s.meters[kFans].seeded);

    SECTION("Z starts from the rails hours") {
        CHECK(s.meters[kZ].used_h == Catch::Approx(340.0));
        REQUIRE(s.meters[kZ].reset_at_h.has_value());
        CHECK(*s.meters[kZ].reset_at_h == Catch::Approx(560.0));
    }
    SECTION("every other new meter starts at zero") {
        CHECK(s.meters[static_cast<int>(MeterId::Extruder)].used_h == 0.0);
        CHECK(s.meters[kFans].used_h == 0.0);
    }
    SECTION("the seed does not repeat once z_axis exists") {
        const State back = from_json(json::parse(R"({"meters": {
            "rails": {"interval_h": 300, "used_h": 340},
            "z_axis": {"used_h": 3}}})"));
        CHECK(back.meters[kZ].used_h == Catch::Approx(3.0));
    }
}

TEST_CASE("an unseeded meter round-trips without an interval", "[maintenance][meters]") {
    State s = default_state();
    meter(s, kZ).used_h = 7.0;
    const json j = to_json(s);
    CHECK_FALSE(j["meters"]["z_axis"].contains("interval_h"));
    const State back = from_json(j);
    CHECK_FALSE(back.meters[kZ].seeded);
    CHECK(back.meters[kZ].used_h == Catch::Approx(7.0));

    seed_defaults(s, {}, {});
    CHECK(to_json(s)["meters"]["z_axis"]["interval_h"] == 500);
}

TEST_CASE("seed_defaults decides from what the printer reports", "[maintenance][meters]") {
    const auto seeded = [](Applicability a, ModelDefaults model = {}) {
        State s = default_state();
        REQUIRE(seed_defaults(s, a, model));
        return s;
    };
    SECTION("an open printer with no filter fan starts the filter Off") {
        CHECK(seeded({}).meters[kFilter].interval_h == 0);
    }
    SECTION("an enclosure starts it at 200 h") {
        CHECK(seeded({true, false, false, false}).meters[kFilter].interval_h == 200);
    }
    SECTION("a chamber filter fan alone turns it on") {
        CHECK(seeded({false, true, false, false}).meters[kFilter].interval_h == 200);
    }
    SECTION("a firmware filter counter alone turns it on") {
        CHECK(seeded({false, false, true, false}).meters[kFilter].interval_h == 200);
    }
    SECTION("no controller_fan starts the fans meter Off, one starts it at 500 h") {
        CHECK(seeded({}).meters[kFans].interval_h == 0);
        CHECK(seeded({false, false, false, true}).meters[kFans].interval_h == 500);
    }
    SECTION("belt tension needs a model row") {
        CHECK(seeded({true, true, true, true}).meters[kBelt].interval_h == 0);
        ModelDefaults model{};
        model[kBelt].hours = 300;
        CHECK(seeded({}, model).meters[kBelt].interval_h == 300);
    }
    SECTION("a model row overrides both ways") {
        ModelDefaults model{};
        model[kNozzle].hours = 0;
        model[kFilter].hours = 150;
        model[kFilter].days = 90;
        const State s = seeded({}, model);
        CHECK(s.meters[kNozzle].interval_h == 0);
        CHECK(s.meters[kFilter].interval_h == 150);
        CHECK(s.meters[kFilter].interval_d == 90);
    }
    SECTION("a seeded meter is never changed and an idle seed reports no change") {
        State s = seeded({true, false, false, true});
        set_interval(s, kNozzle, 750);
        CHECK_FALSE(seed_defaults(s, {}, {}));
        CHECK(s.meters[kNozzle].interval_h == 750);
        CHECK(s.meters[kFilter].interval_h == 200);
    }
}

TEST_CASE("the calendar limit", "[maintenance][meters]") {
    State s = seeded_state();
    set_interval(s, kNozzle, 500);
    set_interval_days(s, kNozzle, 30, kNow);
    Meter& m = meter(s, kNozzle);
    REQUIRE(m.reset_at_s == kNow);

    SECTION("a day limit makes a meter due before its hours") {
        CHECK(used_fraction(m, kNow + 15 * kDay) == Catch::Approx(0.5));
        CHECK(used_fraction(m, kNow + 30 * kDay) == 1.0);
        CHECK(take_pending_notices(s, kNow + 30 * kDay) == std::vector<int>{kNozzle});
    }
    SECTION("status bands use the larger share") {
        CHECK(meter_status(m, kNow + 23 * kDay) == ValueStatus::Warn);
        accrue(s, 490.0);
        CHECK(meter_status(m, kNow) == ValueStatus::Warn);
    }
    SECTION("the card says days only when they are the closer limit") {
        CHECK(days_are_closer(m, kNow + 15 * kDay));
        CHECK(days_left(m, kNow + 15 * kDay) == 15);
        accrue(s, 400.0);
        CHECK_FALSE(days_are_closer(m, kNow + 15 * kDay));
    }
    SECTION("an untrusted now never makes it due") {
        CHECK(used_fraction(m, std::nullopt) == 0.0);
        CHECK(used_fraction(m, int64_t{1000}) == 0.0);
        CHECK_FALSE(days_left(m, std::nullopt).has_value());
        CHECK(take_pending_notices(s, std::nullopt).empty());
    }
    SECTION("an untrusted reset leaves the limit idle until a trusted one") {
        reset_meter(s, kNozzle, std::nullopt);
        CHECK_FALSE(m.reset_at_s.has_value());
        CHECK(used_fraction(m, kNow + 60 * kDay) == 0.0);
        reset_meter(s, kNozzle, kNow);
        CHECK(m.reset_at_s == kNow);
    }
    SECTION("a clock that went back holds at zero") {
        CHECK(used_fraction(m, kNow - 10 * kDay) == 0.0);
    }
    SECTION("the day fields round-trip; a stamp from an untrusted clock does not load") {
        const State back = from_json(json::parse(to_json(s).dump()));
        CHECK(back.meters[kNozzle].interval_d == 30);
        CHECK(back.meters[kNozzle].reset_at_s == kNow);
        json j = to_json(s);
        j["meters"]["nozzle"]["reset_at_s"] = 1000;
        CHECK_FALSE(from_json(j).meters[kNozzle].reset_at_s.has_value());
    }
    SECTION("a late trusted clock starts a limit that had none") {
        m.reset_at_s.reset();
        CHECK_FALSE(stamp_calendar_starts(s, std::nullopt));
        CHECK(stamp_calendar_starts(s, kNow));
        CHECK(m.reset_at_s == kNow);
        CHECK_FALSE(stamp_calendar_starts(s, kNow));
    }
    SECTION("raising the limit re-arms a notice the limit raised") {
        take_pending_notices(s, kNow + 30 * kDay);
        REQUIRE(m.notified);
        set_interval_days(s, kNozzle, 60, kNow + 30 * kDay);
        CHECK_FALSE(m.notified);
    }
}

TEST_CASE("day choices snap", "[maintenance][meters]") {
    CHECK(snap_days(-3) == 0);
    CHECK(snap_days(0) == 0);
    CHECK(snap_days(45) == 30);
    CHECK(snap_days(46) == 60);
    CHECK(snap_days(4000) == 365);
    CHECK(day_choice_index(180) == 4);
}

TEST_CASE("due_order: most due first, ties in display order, Off meters absent",
          "[maintenance][meters]") {
    State s = seeded_state();
    set_interval(s, kNozzle, 0);
    meter(s, kFilter).used_h = 100.0;
    meter(s, kRails).used_h = 100.0;
    const auto order = due_order(s);
    REQUIRE(order.size() == 5u);
    CHECK(order[0] == kFilter);
    CHECK(order[1] == kRails);
    CHECK(order[2] == kZ);
    CHECK(std::find(order.begin(), order.end(), kNozzle) == order.end());
}

TEST_CASE("model_defaults_from_json takes only good fields", "[maintenance][meters]") {
    const json j = json::parse(R"({
        "belt_tension": {"hours": 300},
        "filter": {"hours": 150, "days": 90},
        "nozzle": {"hours": "x", "days": -4},
        "unknown": {"hours": 7},
        "z_axis": 3
    })");
    const ModelDefaults m = model_defaults_from_json(j);
    CHECK(m[kBelt].hours == 300);
    CHECK(m[kFilter].days == 90);
    CHECK_FALSE(m[kNozzle].hours.has_value());
    CHECK_FALSE(m[kNozzle].days.has_value());
    CHECK_FALSE(m[kZ].hours.has_value());
    CHECK_FALSE(model_defaults_from_json(json::array())[kBelt].hours.has_value());
}
