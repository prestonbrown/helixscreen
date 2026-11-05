// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_air_settings.cpp
 * @brief Stored chamber air mode targets: clamping, the per-printer round trip,
 *        and the subjects the settings sliders bind.
 */

#include "../helix_test_fixture.h"
#include "chamber_air_settings.h"
#include "chamber_air_state.h"
#include "config.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ChamberAirState;
using helix::Config;
using helix::PrinterDiscovery;
using nlohmann::json;
namespace air = helix::chamber_air;

namespace {

struct Fixture : HelixTestFixture {
    Fixture() {
        erase();
    }
    ~Fixture() {
        erase();
    }
    /// As though this printer never stored targets: a null node reads as unset.
    static void erase() {
        Config* cfg = Config::get_instance();
        REQUIRE(cfg != nullptr);
        cfg->set<json>(cfg->df() + "chamber_air", json());
    }
};

PrinterDiscovery u1() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"purifier", "fm175xx_reader", "tool"}));
    return hw;
}

} // namespace

TEST_CASE("chamber air settings: targets are clamped to what the row offers",
          "[chamber_air][settings]") {
    const air::ModeTargets t = air::clamp_targets({120.0, 0, 250});
    CHECK(t.target_c == Catch::Approx(air::kMaxTargetC));
    CHECK(t.exhaust_pct == air::kMinTargetPct);
    CHECK(t.recirculate_pct == air::kMaxTargetPct);
    CHECK(air::clamp_targets({5.0, 50, 50}).target_c == Catch::Approx(air::kMinTargetC));
    CHECK(air::clamp_targets({std::nan(""), 50, 50}).target_c ==
          Catch::Approx(air::ModeTargets{}.target_c));
}

TEST_CASE_METHOD(Fixture, "chamber air settings: nothing stored reads the firmware defaults",
                 "[chamber_air][settings]") {
    const air::ModeTargets t = air::load_targets();
    CHECK(t.target_c == Catch::Approx(42.0));
    CHECK(t.exhaust_pct == 60);
    CHECK(t.recirculate_pct == 60);
}

TEST_CASE_METHOD(Fixture, "chamber air settings: stored targets round-trip, clamped",
                 "[chamber_air][settings]") {
    air::save_targets({36.0, 80, 999});
    const air::ModeTargets t = air::load_targets();
    CHECK(t.target_c == Catch::Approx(36.0));
    CHECK(t.exhaust_pct == 80);
    CHECK(t.recirculate_pct == 100);
    // A value written outside the range by hand is clamped on the way in.
    Config::get_instance()->set<int>(Config::get_instance()->df() + "chamber_air/exhaust_pct", 3);
    CHECK(air::load_targets().exhaust_pct == air::kMinTargetPct);
}

TEST_CASE_METHOD(Fixture, "chamber air settings: the sliders' subjects load and store the targets",
                 "[chamber_air][settings]") {
    air::save_targets({38.0, 70, 40});
    ChamberAirState state;
    state.init_subjects(false);
    state.set_hardware(u1());
    const air::ModeTargets loaded = state.targets();
    CHECK(loaded.target_c == Catch::Approx(38.0));
    CHECK(loaded.exhaust_pct == 70);
    CHECK(loaded.recirculate_pct == 40);
    state.deinit_subjects();
}
