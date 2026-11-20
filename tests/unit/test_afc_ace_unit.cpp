// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_ace_unit.cpp
 * @brief An ACE Pro unit run through AFC (AFC DEV's AFC_ACE / AFC_ACE2 objects)
 *        reports its dryer and climate, and its dryer is driven with AFC_ACE_DRY.
 */

#include "../lvgl_test_fixture.h"
#include "afc_status_parse.h"
#include "ams_backend_afc.h"
#include "ams_dryer.h"
#include "ams_state.h"
#include "json_utils.h"
#include "moonraker_client_mock.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_env.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {

class AfcAceHelper : public AmsBackendAfc {
  public:
    explicit AfcAceHelper(const std::vector<std::string>& names) : AmsBackendAfc(nullptr, nullptr) {
        // The unit objects build the slots from the discovered lanes.
        set_discovered_lanes(names, {});
        running_ = true;
    }

    void feed(const nlohmann::json& params) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    std::vector<std::string> gcodes;
    AmsError execute_gcode(const std::string& gcode) override {
        gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    [[nodiscard]] AmsUnit unit(int index) const {
        return get_system_info().units.at(static_cast<size_t>(index));
    }
};

} // namespace helix

namespace {

nlohmann::json ace_and_turtle(const nlohmann::json& ace_fields) {
    nlohmann::json ace = {{"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                          {"extruders", {"extruder"}},
                          {"hubs", {"Ace_1"}},
                          {"buffers", nlohmann::json::array()}};
    ace.update(ace_fields);
    return {{"AFC", {{"units", {"ACE Ace_1", "Box_Turtle Turtle_1"}}}},
            {"AFC_ACE Ace_1", ace},
            {"AFC_BoxTurtle Turtle_1",
             {{"lanes", {"lane5", "lane6", "lane7", "lane8"}},
              {"extruders", {"extruder"}},
              {"hubs", {"Turtle_1"}},
              {"buffers", nlohmann::json::array()}}}};
}

const nlohmann::json kDrying = {{"ace_dryer", "drying"},    {"ace_dryer_target", 50},
                                {"ace_dryer_remain", 7200}, {"ace_dryer_duration", 240},
                                {"ace_temp", 44.5},         {"ace_humidity", 18.0}};

std::vector<std::string> eight_lanes() {
    return {"lane1", "lane2", "lane3", "lane4", "lane5", "lane6", "lane7", "lane8"};
}

} // namespace

TEST_CASE("AFC unit object: the ACE hardware fields parse only where published", "[afc][afc_ace]") {
    const auto plain = afc::parse_unit_object({{"lanes", {"lane1"}}});
    CHECK_FALSE(plain.ace.has_value());

    const auto ace = afc::parse_unit_object(kDrying);
    REQUIRE(ace.ace.has_value());
    CHECK(ace.ace->dryer_status == std::optional<std::string>("drying"));
    CHECK(ace.ace->dryer_target_c == std::optional<float>(50.0f));
    CHECK(ace.ace->dryer_remaining_min == std::optional<int>(120));
    CHECK(ace.ace->dryer_duration_min == std::optional<int>(240));
    CHECK(ace.ace->temperature_c == std::optional<float>(44.5f));
    CHECK(ace.ace->humidity_pct == std::optional<float>(18.0f));

    // A disconnected ACE publishes nulls: they state nothing.
    const auto offline = afc::parse_unit_object({{"ace_dryer", ""}, {"ace_temp", nullptr}});
    REQUIRE(offline.ace.has_value());
    CHECK_FALSE(offline.ace->temperature_c.has_value());
}

TEST_CASE("ACE dryer status words: AFC's idle words are not a cycle", "[afc][afc_ace]") {
    for (const char* idle : {"", "stop", "Stopped", "stopping", "idle", "off", "none"}) {
        CHECK_FALSE(afc::ace_dryer_running(idle));
    }
    CHECK(afc::ace_dryer_running("drying"));
    CHECK(afc::ace_dryer_running("heater_err"));
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC ACE unit: the dryer and climate reach the unit",
                 "[afc][afc_ace][ams]") {
    helix::test::RegisteredBackend<AfcAceHelper> reg{eight_lanes()};
    AfcAceHelper& afc = *reg;
    afc.feed(ace_and_turtle(kDrying));

    REQUIRE(afc.get_system_info().units.size() == 2);
    const DryerInfo dryer = helix::dryer_info(afc, 0);
    CHECK(dryer.supported);
    CHECK(dryer.active);
    CHECK(dryer.target_temp_c == 50.0f);
    CHECK(dryer.remaining_min == 120);
    CHECK(dryer.duration_min == 240);
    CHECK(dryer.current_temp_c == 44.5f);

    REQUIRE(afc.unit(0).environment.has_value());
    CHECK(afc.unit(0).environment->temperature_c == 44.5f);
    CHECK(afc.unit(0).environment->has_humidity);
    CHECK(afc.unit(0).environment->humidity_pct == 18.0f);
    CHECK(afc.traits().has_environment_sensors);

    // Two units: the header names the system.
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.sync_from_backend();
    CHECK(std::string(lv_subject_get_string(ams.get_ams_system_name_subject())) == "AFC");

    // The Box Turtle beside it has neither.
    CHECK_FALSE(helix::dryer_info(afc, 1).supported);
    CHECK_FALSE(afc.unit(1).environment.has_value());

    // A later delta carrying only the stop keeps the climate.
    afc.feed({{"AFC_ACE Ace_1", {{"ace_dryer", "stop"}}}});
    CHECK_FALSE(helix::dryer_info(afc, 0).active);
    CHECK(afc.unit(0).environment.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC ACE unit: no ace_dryer, no dryer", "[afc][afc_ace][ams]") {
    helix::test::RegisteredBackend<AfcAceHelper> reg{eight_lanes()};
    AfcAceHelper& afc = *reg;
    afc.feed(ace_and_turtle(nlohmann::json::object()));

    CHECK_FALSE(helix::dryer_info(afc, 0).supported);
    CHECK_FALSE(afc.start_drying(50.0f, 60, -1, 0).success());
    CHECK(afc.gcodes.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC ACE unit: start and stop name the unit",
                 "[afc][afc_ace][ams]") {
    helix::test::RegisteredBackend<AfcAceHelper> reg{eight_lanes()};
    AfcAceHelper& afc = *reg;
    afc.feed(ace_and_turtle({{"ace_dryer", "stop"}, {"ace_temp", 25.0}}));

    REQUIRE(afc.start_drying(50.0f, 240, -1, 0).success());
    REQUIRE(afc.stop_drying(0).success());
    REQUIRE(afc.gcodes.size() == 2);
    CHECK(afc.gcodes[0] == "AFC_ACE_DRY UNIT=Ace_1 TEMP=50 DURATION=240");
    CHECK(afc.gcodes[1] == "AFC_ACE_DRY_STOP UNIT=Ace_1");

    SECTION("out of the ACE Pro's range is refused before anything is sent") {
        afc.gcodes.clear();
        CHECK_FALSE(afc.start_drying(80.0f, 240, -1, 0).success());
        CHECK(afc.gcodes.empty());
    }
    SECTION("the Box Turtle has no dryer to start") {
        afc.gcodes.clear();
        CHECK_FALSE(afc.start_drying(50.0f, 240, -1, 1).success());
        CHECK(afc.gcodes.empty());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC ACE unit: the subscription carries the climate",
                 "[afc][afc_ace][ams]") {
    // An idle ACE 2 Pro under AFC DEV, as the V0 rig publishes it, cut down to the
    // fields the subscription asks for: Moonraker sends nothing else after the
    // first snapshot.
    const nlohmann::json published = {{"lanes", {"lane0", "lane1", "lane2", "lane3"}},
                                      {"extruders", {"extruder"}},
                                      {"hubs", {"Ace2_1_hub"}},
                                      {"buffers", nlohmann::json::array()},
                                      {"ace_connected", true},
                                      {"ace_status", "ready"},
                                      {"ace_temp", 28},
                                      {"ace_humidity", 39},
                                      {"ace_dryer", "stop"},
                                      {"ace_dryer_target", 0},
                                      {"ace_dryer_remain", 0},
                                      {"ace_dryer_duration", 0}};
    const auto subscribed =
        AmsBackendAfc::required_status_objects(std::vector<std::string>{"AFC", "AFC_ACE2 Ace2_1"});
    nlohmann::json delivered = nlohmann::json::object();
    for (const auto& field : subscribed.at("AFC_ACE2 Ace2_1")) {
        const auto key = field.get<std::string>();
        if (published.contains(key)) {
            delivered[key] = published[key];
        }
    }

    helix::test::RegisteredBackend<AfcAceHelper> reg{
        std::vector<std::string>{"lane0", "lane1", "lane2", "lane3"}};
    AfcAceHelper& afc = *reg;
    afc.feed({{"AFC", {{"units", {"ACE2 Ace2_1"}}}}, {"AFC_ACE2 Ace2_1", delivered}});

    REQUIRE(afc.get_system_info().units.size() == 1);
    CHECK(helix::dryer_info(afc, 0).supported);
    REQUIRE(afc.unit(0).environment.has_value());
    CHECK(afc.unit(0).environment->temperature_c == 28.0f);
    CHECK(afc.unit(0).environment->humidity_pct == 39.0f);
    CHECK(afc.traits().has_environment_sensors);

    // One named unit: the AMS panel's header names it rather than the system.
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.sync_from_backend();
    CHECK(std::string(lv_subject_get_string(ams.get_ams_system_name_subject())) == "Ace2 1");
    // The dryer page titles the box the same way.
    const auto zones = afc.get_environment_zones(-1);
    REQUIRE(zones.size() == 1);
    CHECK(zones[0].label == "Ace2 1");
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC ACE mock: its dryer reads as the firmware's would",
                 "[afc][afc_ace][ams]") {
    // HELIX_MOCK_AMS=afc_ace starts mid cycle, 205 of 240 minutes left
    // (MOCK_ENVIRONMENT_VARIABLES.md); the backend must read the same.
    ScopedEnv mock_ams{"HELIX_MOCK_AMS", "afc_ace"};
    MoonrakerClientMock mock;
    nlohmann::json status;
    mock.send_jsonrpc("printer.objects.query",
                      {{"objects", {{"AFC", nullptr}, {"AFC_ACE Ace_1", nullptr}}}},
                      [&status](const nlohmann::json& reply) {
                          if (const auto* s = json_util::find_path(reply, "result", "status"))
                              status = *s;
                      });
    REQUIRE(status.contains("AFC_ACE Ace_1"));

    helix::test::RegisteredBackend<AfcAceHelper> reg{
        std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"}};
    AfcAceHelper& afc = *reg;
    afc.feed(status);
    const DryerInfo dryer = helix::dryer_info(afc, 0);
    REQUIRE(dryer.supported);
    CHECK(dryer.active);
    CHECK(dryer.duration_min == 240);
    CHECK(dryer.remaining_min == 205);
}
