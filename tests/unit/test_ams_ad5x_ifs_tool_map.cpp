// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AmsBackendAd5xIfs's tool table as the lanes show it: each lane's tool badge
// (SlotInfo::mapped_tool) follows the table the firmware or a plugin holds,
// from the moment the table moves.

#include "ams_backend_ad5x_ifs.h"
#include "ams_types.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsBackendAd5xIfs;
using json = nlohmann::json;

namespace {

json save_variables(const json& variables) {
    return json{{"save_variables", {{"variables", variables}}}};
}

/// The lessWaste rows of a rig whose table is the identity: T<n> feeds lane
/// n+1, and T0 is active.
json identity_plugin_rows() {
    return save_variables(
        json{{"less_waste_tools", json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
             {"less_waste_current_tool", 0}});
}

} // namespace

TEST_CASE("A plugin-backed remap moves each lane's tool badge at once",
          "[ams][ad5x_ifs][tool_map]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    backend.fire_status(identity_plugin_rows());
    REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    REQUIRE(backend.get_tool_mapping() == std::vector<int>{0, 1, 2, 3});
    REQUIRE(backend.get_slot_info(0).mapped_tool == 0);
    REQUIRE(backend.get_slot_info(1).mapped_tool == 1);

    // T0 onto lane 2: lane 2 now carries T0, and lane 1 carries no tool.
    (void)backend.set_tool_mapping(0, 1);
    REQUIRE(backend.get_tool_mapping() == std::vector<int>{1, 1, 2, 3});
    CHECK(backend.get_slot_info(1).mapped_tool == 0);
    CHECK(backend.get_slot_info(0).mapped_tool == -1);
}

TEST_CASE("The module's first frame gives every lane its tool before any tool_map arrives",
          "[ams][ad5x_ifs][ifs_module][tool_map]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;

    // A module predating the tool_map field: it says which lanes hold filament
    // and which one it engaged, and nothing about tools. Its own macros map
    // T0..T3 onto lanes 1..4.
    backend.fire_status(json{{"ifs",
                              {{"connected", true},
                               {"channel_count", 4},
                               {"active_channel", 2},
                               {"loaded_channels", json::array({2})}}}});
    REQUIRE(Ad5xIfsTestAccess::module_live(backend));
    // No wire contract arrived: the table below is the module's own, not a frame's.
    REQUIRE(backend.get_tool_mapping().empty());

    const auto info = backend.get_system_info();
    REQUIRE(info.tool_to_slot_map.size() == 16);
    CHECK(std::vector<int>(info.tool_to_slot_map.begin(), info.tool_to_slot_map.begin() + 4) ==
          std::vector<int>{0, 1, 2, 3});
    CHECK(backend.get_slot_info(3).mapped_tool == 3);
    // T1 is the tool on the lane the module engaged.
    CHECK(Ad5xIfsTestAccess::active_tool(backend) == 1);
}
