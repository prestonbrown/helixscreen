// tests/unit/test_ams_backend_snapmaker_prefs.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"
#include "test_helpers/backend_harness.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {
/// An unwrapped status object carrying only print_task_config — the shape the
/// initial query response sends and handle_status() takes.
nlohmann::json frame(const nlohmann::json& ptc_fields) {
    nlohmann::json params = nlohmann::json::object();
    params["print_task_config"] = ptc_fields;
    return params;
}
} // namespace

TEST_CASE("snapmaker backend keeps the preferences it is told about", "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_led_turn_off", true}, {"filament_entangle_sen", "low"}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == true);
    REQUIRE(backend.print_preferences().filament_entangle_sen.value() == "low");
}

TEST_CASE("a later frame that omits a preference does not clear it", "[ams][snapmaker][prefs]") {
    // The delta trap: a frame carrying only filament colours says nothing about
    // the preferences, and replacing rather than merging would switch them off.
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_led_turn_off", true}}));
    backend.fire_status(frame({{"filament_type", {"PLA"}}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == true);
}

TEST_CASE("a later frame that changes a preference wins", "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_led_turn_off", true}}));
    backend.fire_status(frame({{"end_led_turn_off", false}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == false);
}

TEST_CASE("every preference survives a later frame that carries none of them",
          "[ams][snapmaker][prefs]") {
    // One assertion per merge arm: deleting any single arm stops its field
    // surviving here, so no arm can lose coverage quietly.
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"auto_replenish_filament", true},
                               {"replenish_ignore_color", false},
                               {"filament_entangle_detect", true},
                               {"end_led_turn_off", false},
                               {"filament_entangle_sen", "high"},
                               {"end_unload_filament", {true, false, true, false}}}));
    backend.fire_status(frame({{"filament_type", {"PLA"}}}));
    const auto prefs = backend.print_preferences();
    REQUIRE(prefs.auto_replenish.value() == true);
    REQUIRE(prefs.replenish_ignore_color.value() == false);
    REQUIRE(prefs.filament_entangle_detect.value() == true);
    REQUIRE(prefs.end_led_turn_off.value() == false);
    REQUIRE(prefs.filament_entangle_sen.value() == "high");
    REQUIRE(prefs.end_unload_filament == std::vector<bool>{true, false, true, false});
}

TEST_CASE("a backend that has seen no preference field reports an empty set",
          "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"filament_type", {"PLA"}}}));
    REQUIRE(backend.print_preferences().empty());
}

TEST_CASE("one preference field is enough to make the held set non-empty",
          "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"auto_replenish_filament", false}}));
    REQUIRE_FALSE(backend.print_preferences().empty());
}
