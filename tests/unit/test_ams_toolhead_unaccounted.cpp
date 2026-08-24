// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_toolhead_unaccounted.cpp
 * @brief Per-backend toolhead_filament_unaccounted() capability overrides.
 *
 * Print-start gate input ("unaccounted_toolhead_filament"): filament at the
 * toolhead that no lane/gate claims. Each backend answers for itself —
 *
 *  - AFC: true only when a physical toolhead sensor is tripped AND nothing
 *    accounts for the filament (not AFC.current, not an extruder's
 *    lane_loaded, not any lane's persisted tool_loaded). nullopt is never
 *    returned: hardware without sensors reports both false, which reads as
 *    "known, accounted" rather than "unknown" — no false positives.
 *  - Happy Hare: false when mmu.filament is not Loaded; otherwise true iff
 *    mmu.gate names no gate (-1). Gate -2 is bypass, which the gate layer
 *    silences separately anyway.
 *  - CFS: nullopt until the toolhead filament switch has EVER published a
 *    reading (filament_sensor_seen_, the #1199 pair); after that, true iff
 *    the switch detects filament while no bay letter (current_slot) names
 *    the seated lane.
 *  - AD5X IFS: the SWITCH pair only (head_switch_seen_/head_switch_present_),
 *    never head_filament_ alone (motion-sensor false negatives). nullopt
 *    until the switch has ever been seen; after that, true iff the switch
 *    reads present while current_slot names no channel.
 *
 * Helpers feed the production status paths (pattern:
 * test_ams_afc_per_slot_loaded.cpp / test_ams_happy_hare_per_slot_loaded.cpp).
 *
 * Run with: ./build/bin/helix-tests "[ams][toolhead-unaccounted]"
 */

#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/cfs_test_access.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

// File scope (NOT anonymous namespace): AfcHelper reaches private
// initialize_slots() via a friend declaration in ams_backend_afc.h, which
// names ::AfcHelper — an anonymous-namespace class would not match it.
class AfcHelper : public AmsBackendAfc {
  public:
    AfcHelper() : AmsBackendAfc(nullptr, nullptr) {
        initialize_slots({"lane1", "lane2", "lane3", "lane4"});
    }
    void feed_extruder(const std::string& lane_loaded, bool tool_start, bool tool_end) {
        json ext{{"tool_start_status", tool_start}, {"tool_end_status", tool_end}};
        if (!lane_loaded.empty())
            ext["lane_loaded"] = lane_loaded;
        json params;
        params["AFC_extruder extruder"] = ext;
        json notification;
        notification["params"] = json::array({params, 0.0});
        handle_status_update(notification);
    }
    void feed_stepper_tool_loaded(const std::string& lane, bool tool_loaded) {
        json params;
        params["AFC_stepper " + lane] = json{{"tool_loaded", tool_loaded}};
        json notification;
        notification["params"] = json::array({params, 0.0});
        handle_status_update(notification);
    }
};

namespace {
class HhHelper : public AmsBackendHappyHare {
  public:
    HhHelper() : AmsBackendHappyHare(nullptr, nullptr) {}
    void feed_mmu(const json& mmu) {
        json params;
        params["mmu"] = mmu;
        json notification;
        notification["params"] = json::array({params, 0.0});
        handle_status_update(notification);
    }
};
} // namespace

TEST_CASE("AFC unaccounted: sensors tripped, no lane claims -> true",
          "[ams][toolhead-unaccounted]") {
    AfcHelper afc;
    afc.feed_extruder(/*lane_loaded=*/"", /*tool_start=*/true, /*tool_end=*/true);
    REQUIRE(afc.toolhead_filament_unaccounted().has_value());
    CHECK(*afc.toolhead_filament_unaccounted() == true);
}

TEST_CASE("AFC unaccounted: lane_loaded accounts for it -> false", "[ams][toolhead-unaccounted]") {
    AfcHelper afc;
    afc.feed_extruder("lane1", true, true);
    REQUIRE(afc.toolhead_filament_unaccounted().has_value());
    CHECK(*afc.toolhead_filament_unaccounted() == false);
}

TEST_CASE("AFC unaccounted: persisted tool_loaded accounts even with sensors silent",
          "[ams][toolhead-unaccounted]") {
    AfcHelper afc;
    afc.feed_stepper_tool_loaded("lane2", true);
    REQUIRE(afc.toolhead_filament_unaccounted().has_value());
    CHECK(*afc.toolhead_filament_unaccounted() == false);
}

TEST_CASE("AFC unaccounted: idle backend -> false (never nullopt: absent sensors read false)",
          "[ams][toolhead-unaccounted]") {
    AfcHelper afc;
    REQUIRE(afc.toolhead_filament_unaccounted().has_value());
    CHECK(*afc.toolhead_filament_unaccounted() == false);
}

TEST_CASE("Happy Hare unaccounted: Loaded + no gate named -> true", "[ams][toolhead-unaccounted]") {
    HhHelper hh;
    hh.feed_mmu(json{{"filament", "Loaded"}, {"gate", -1}});
    REQUIRE(hh.toolhead_filament_unaccounted().has_value());
    CHECK(*hh.toolhead_filament_unaccounted() == true);
}

TEST_CASE("Happy Hare unaccounted: Loaded + gate named -> false", "[ams][toolhead-unaccounted]") {
    HhHelper hh;
    hh.feed_mmu(json{{"filament", "Loaded"}, {"gate", 2}});
    REQUIRE(hh.toolhead_filament_unaccounted().has_value());
    CHECK(*hh.toolhead_filament_unaccounted() == false);
}

TEST_CASE("Happy Hare unaccounted: Unloaded -> false", "[ams][toolhead-unaccounted]") {
    HhHelper hh;
    hh.feed_mmu(json{{"filament", "Unloaded"}, {"gate", -1}});
    REQUIRE(hh.toolhead_filament_unaccounted().has_value());
    CHECK(*hh.toolhead_filament_unaccounted() == false);
}

TEST_CASE("Default backends answer nullopt (mock, no scenario)", "[ams][toolhead-unaccounted]") {
    AmsBackendMock mock;
    CHECK_FALSE(mock.toolhead_filament_unaccounted().has_value());
}

namespace {
// CFS ctor needs an API handle (pattern: test_ams_backend_cfs.cpp).
struct CfsHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<helix::printer::AmsBackendCfs> cfs;
    CfsHarness() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        cfs = std::make_unique<helix::printer::AmsBackendCfs>(api.get(), nullptr);
    }
};
} // namespace

TEST_CASE("CFS unaccounted: sensor never published -> nullopt", "[ams][toolhead-unaccounted]") {
    CfsHarness h;
    CfsTestAccess::set_filament_sensor(*h.cfs, /*seen=*/false, /*detected=*/false);
    CHECK_FALSE(h.cfs->toolhead_filament_unaccounted().has_value());
}

TEST_CASE("CFS unaccounted: detected + no bay named -> true", "[ams][toolhead-unaccounted]") {
    CfsHarness h;
    CfsTestAccess::set_filament_sensor(*h.cfs, /*seen=*/true, /*detected=*/true);
    auto r = h.cfs->toolhead_filament_unaccounted();
    REQUIRE(r.has_value());
    CHECK(*r == true);
}

TEST_CASE("CFS unaccounted: detected but bay seated -> false", "[ams][toolhead-unaccounted]") {
    CfsHarness h;
    CfsTestAccess::set_filament_sensor(*h.cfs, /*seen=*/true, /*detected=*/true);
    CfsTestAccess::set_seated_bay(*h.cfs, 1);
    auto r = h.cfs->toolhead_filament_unaccounted();
    REQUIRE(r.has_value());
    CHECK(*r == false);
}

TEST_CASE("CFS unaccounted: seen + not detected -> false", "[ams][toolhead-unaccounted]") {
    CfsHarness h;
    CfsTestAccess::set_filament_sensor(*h.cfs, /*seen=*/true, /*detected=*/false);
    auto r = h.cfs->toolhead_filament_unaccounted();
    REQUIRE(r.has_value());
    CHECK(*r == false);
}

TEST_CASE("AD5X IFS unaccounted: switch never seen -> nullopt", "[ams][toolhead-unaccounted]") {
    AmsBackendAd5xIfs ifs(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_switch(ifs, /*seen=*/false, /*present=*/false);
    CHECK_FALSE(ifs.toolhead_filament_unaccounted().has_value());
}

TEST_CASE("AD5X IFS unaccounted: switch present + no channel -> true",
          "[ams][toolhead-unaccounted]") {
    AmsBackendAd5xIfs ifs(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_switch(ifs, /*seen=*/true, /*present=*/true);
    auto r = ifs.toolhead_filament_unaccounted();
    REQUIRE(r.has_value());
    CHECK(*r == true);
}

TEST_CASE("AD5X IFS unaccounted: switch present but channel seated -> false",
          "[ams][toolhead-unaccounted]") {
    AmsBackendAd5xIfs ifs(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_switch(ifs, /*seen=*/true, /*present=*/true);
    Ad5xIfsTestAccess::set_current_slot(ifs, 2);
    auto r = ifs.toolhead_filament_unaccounted();
    REQUIRE(r.has_value());
    CHECK(*r == false);
}

TEST_CASE("AD5X IFS unaccounted: switch seen + not present -> false",
          "[ams][toolhead-unaccounted]") {
    AmsBackendAd5xIfs ifs(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_switch(ifs, /*seen=*/true, /*present=*/false);
    auto r = ifs.toolhead_filament_unaccounted();
    REQUIRE(r.has_value());
    CHECK(*r == false);
}

TEST_CASE("Mock scenario 'unaccounted' drives the gate input", "[ams][toolhead-unaccounted]") {
    AmsBackendMock mock;
    mock.set_initial_state_scenario("unaccounted");
    mock.start();
    auto r = mock.toolhead_filament_unaccounted();
    REQUIRE(r.has_value());
    CHECK(*r == true);
    CHECK(mock.is_filament_loaded());
    mock.stop();
}

TEST_CASE("Mock scenario 'unaccounted' cleared on stop (no stale flag across restarts)",
          "[ams][toolhead-unaccounted]") {
    AmsBackendMock mock;
    mock.set_initial_state_scenario("unaccounted");
    mock.start();
    REQUIRE(mock.toolhead_filament_unaccounted().has_value());
    CHECK(*mock.toolhead_filament_unaccounted() == true);
    mock.stop();
    CHECK_FALSE(mock.toolhead_filament_unaccounted().has_value());
}
