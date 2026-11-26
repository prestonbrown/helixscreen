// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AmsBackendAd5xIfs: which lane is seated, as the path view, the lanes'
// loaded marks and a cold boot's remembered lane each read it.

#include "../lvgl_test_fixture.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_types.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/registered_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsBackendAd5xIfs;
using helix::PathSegment;
using helix::SlotStatus;
using json = nlohmann::json;

namespace {

json save_variables(const json& variables) {
    return json{{"save_variables", {{"variables", variables}}}};
}

json port_sensor(int port_1based) {
    return json{
        {std::string("filament_switch_sensor _ifs_port_sensor_") + std::to_string(port_1based),
         {{"filament_detected", true}}}};
}

/// Two labelled lanes of Adventurer5M.json, to follow a `"channel": N`.
constexpr const char* kTwoLanes =
    R"(,"ffmColor1":"#FF0000","ffmType1":"PLA","ffmColor2":"#00FF00","ffmType2":"PETG")";

} // namespace

TEST_CASE("The path shows no lane while the active tool's own lane holds no filament",
          "[ams][ad5x_ifs][path][2044]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    backend.fire_status(save_variables(
        json{{"less_waste_tools", json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
             {"less_waste_current_tool", 0}}));
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
    REQUIRE(backend.get_tool_mapping() == std::vector<int>{0, 1, 2, 3});

    // Another lane holds filament; the one T0 feeds does not.
    backend.fire_status(port_sensor(2));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    CHECK(backend.get_filament_segment() == PathSegment::NONE);

    backend.fire_status(port_sensor(1));
    CHECK(backend.get_filament_segment() == PathSegment::LANE);
}

TEST_CASE_METHOD(LVGLTestFixture, "The loaded mark moves to the lane the file names as seated",
                 "[ams][ad5x_ifs][lane_truth][2044]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg{nullptr, nullptr};
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);

    Ad5xIfsTestAccess::parse_adventurer_json(backend, std::string(R"({"FFMInfo":{"channel":1)") +
                                                          kTwoLanes + "}}");
    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::LOADED);

    // The printer seated lane 2 with no frame to say so; the file is how it tells.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, std::string(R"({"FFMInfo":{"channel":2)") +
                                                          kTwoLanes + "}}");
    REQUIRE(backend.get_system_info().current_slot == 1);
    CHECK(backend.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "A remembered lane the frame's Ports do not reach is not restored after a power cycle",
    "[ams][ad5x_ifs][lane_truth][ifs_jacker][2044]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg{nullptr, nullptr};
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // An IFS Jacker rig: two IFS units on one bus. After the reboot Chan is 0,
    // and this frame's Ports array covers the first unit only.
    AmsBackendAd5xIfs::ZColorSilentResult frame;
    frame.saw_valid_response = true;
    frame.port_count = 8;
    frame.ifs_chan = 0;
    frame.ifs_ports = std::vector<bool>{true, false, false, false};

    // Lane 6 was seated, and nothing in the frame says it still holds filament.
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 5);
    Ad5xIfsTestAccess::apply_zcolor_result(backend, frame);
    REQUIRE(backend.get_system_info().total_slots == 8);
    CHECK(backend.get_system_info().current_slot == -1);
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);

    // The same boot with lane 1 remembered: the frame confirms it, so it is seated.
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 0);
    Ad5xIfsTestAccess::apply_zcolor_result(backend, frame);
    CHECK(backend.get_system_info().current_slot == 0);
}
