// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mock backend AmsBackend::create() builds from HELIX_MOCK_AMS: a mode that
// models no firmware reaches the mock's own topology table, and what the mock
// becomes there is what a --test run then shows.

#include "ams_backend.h"
#include "ams_types.h"
#include "test_helpers/scoped_env.h"
#include "test_helpers/scoped_runtime_config.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("HELIX_MOCK_AMS=afc_direct builds the direct-tool mock", "[ams][mock][factory][2044]") {
    // No persona and no mode left over from another case in this shard.
    helix::ScopedEnv printer{"HELIX_MOCK_PRINTER", nullptr};
    ScopedRuntimeConfig scoped_config;
    auto* config = get_runtime_config();
    config->test_mode = true;
    config->use_real_ams = false;
    REQUIRE(config->should_mock_ams());

    helix::ScopedEnv ams{"HELIX_MOCK_AMS", "afc_direct"};
    auto backend = helix::AmsBackend::create(helix::AmsType::NONE, nullptr, nullptr);
    REQUIRE(backend != nullptr);
    const auto info = backend->get_system_info();
    CHECK(info.type == helix::AmsType::AFC);
    REQUIRE(info.units.size() == 6);
    CHECK(info.total_slots == 19);
    // Lane counts unit by unit: a four-lane box, then one lane straight into a
    // toolhead with no hub in between.
    const int lanes[] = {4, 1, 4, 2, 4, 4};
    for (std::size_t unit = 0; unit < info.units.size(); ++unit) {
        INFO(unit);
        CHECK(info.units[unit].slot_count == lanes[unit]);
    }
    CHECK_FALSE(info.units[1].has_hub_sensor);
}
