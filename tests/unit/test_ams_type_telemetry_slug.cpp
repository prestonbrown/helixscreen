// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend.h"
#include "ams_backend_row.h"
#include "ams_types.h"

#include <iterator>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsType;

// hardware/expected entries are persisted in users' settings: an existing
// value must never change, and every one must map back to its own type.
TEST_CASE("Expected-hardware names are stable and round-trip", "[ams][hardware][validator]") {
    using helix::ams_type_for_expected_hardware;
    using helix::ams_type_traits;
    CHECK(std::string(ams_type_traits(AmsType::HAPPY_HARE).expected_hardware) == "mmu");
    CHECK(std::string(ams_type_traits(AmsType::AFC).expected_hardware) == "AFC");
    CHECK(std::string(ams_type_traits(AmsType::ACE).expected_hardware) == "ace");
    CHECK(std::string(ams_type_traits(AmsType::TOOL_CHANGER).expected_hardware) == "toolchanger");
    CHECK(std::string(ams_type_traits(AmsType::INDX).expected_hardware) == "indx");

    for (const auto& row : helix::kAmsTypeTraits) {
        CAPTURE(row.name);
        if (row.type == AmsType::NONE) {
            CHECK(std::string(row.expected_hardware).empty());
            continue;
        }
        CHECK(ams_type_for_expected_hardware(row.expected_hardware) == row.type);
    }
    CHECK(ams_type_for_expected_hardware("") == AmsType::NONE);
    CHECK(ams_type_for_expected_hardware("afc") == AmsType::NONE); // case matters: "AFC"
    CHECK(ams_type_for_expected_hardware("heater_bed") == AmsType::NONE);
}

TEST_CASE("Every AmsType has exactly one traits row", "[ams]") {
    for (int v = 0; v <= static_cast<int>(AmsType::INDX); ++v) {
        const auto type = static_cast<AmsType>(v);
        int rows = 0;
        for (const auto& row : helix::kAmsTypeTraits) {
            rows += row.type == type ? 1 : 0;
        }
        CAPTURE(v);
        CHECK(rows == 1);
        CHECK(helix::ams_type_traits(type).type == type);
    }
}

// Telemetry's ams.type and features are a published schema the dashboard groups by:
// an existing value must never change.
TEST_CASE("Each backend row keeps its classification, spellings and telemetry names",
          "[ams][traits][telemetry]") {
    struct Expected {
        AmsType type;
        const char* slug;
        bool http_poll, tool_changer, filament_system, names_sensors;
        const char* telemetry_feature;
        std::vector<std::string> aliases;
    };
    using helix::ams_backend_row;
    using helix::ams_type_from_string;
    // clang-format off
    const Expected rows[] = {
        {AmsType::NONE, "unknown", false, false, false, false, nullptr, {}},
        {AmsType::HAPPY_HARE, "happy_hare", false, false, true, true, "mmu_happy_hare", {"mmu", "happy_hare", "Happy Hare"}},
        {AmsType::AFC, "afc", false, false, true, true, "mmu_afc", {"afc", "AFC"}},
        {AmsType::ACE, "ace", true, false, true, false, nullptr, {"valgace", "ValgACE", "bunnyace", "BunnyACE", "duckace", "DuckACE", "ace", "ACE Pro"}},
        {AmsType::TOOL_CHANGER, "tool_changer", false, true, false, false, "tool_changer", {"toolchanger", "tool_changer", "Tool Changer"}},
        {AmsType::AD5X_IFS, "ifs", true, false, true, true, nullptr, {"ad5x_ifs", "ad5x ifs", "ad5x", "ifs"}},
        {AmsType::CFS, "cfs", false, false, true, true, nullptr, {"cfs", "CFS", "box"}},
        {AmsType::SNAPMAKER, "snapmaker", false, true, true, false, nullptr, {"snapmaker", "Snapmaker", "snapswap", "SnapSwap"}},
        {AmsType::QIDI_BOX, "qidi_box", false, false, true, false, nullptr, {"qidi_box", "QIDI Box", "qidibox"}},
        {AmsType::OPENAMS, "openams", false, false, true, false, nullptr, {"openams", "OpenAMS", "oams_manager"}},
        {AmsType::INDX, "indx", false, true, false, false, nullptr, {"indx", "INDX"}},
    };
    // clang-format on
    REQUIRE(std::size(rows) == std::size(helix::kAmsTypeTraits));
    for (const Expected& want : rows) {
        const helix::AmsBackendRow& row = ams_backend_row(want.type);
        CAPTURE(helix::ams_type_to_string(want.type));
        CHECK(row.type == want.type);
        CHECK(std::string(helix::ams_type_telemetry_slug(want.type)) == want.slug);
        CHECK(row.needs_http_poll == want.http_poll);
        CHECK(helix::is_tool_changer(want.type) == want.tool_changer);
        CHECK(helix::is_filament_system(want.type) == want.filament_system);
        CHECK((row.owns_filament_sensor != nullptr) == (row.construct && want.names_sensors));
        CHECK(std::string(row.telemetry_feature ? row.telemetry_feature : "") ==
              (want.telemetry_feature ? want.telemetry_feature : ""));
        for (const std::string& alias : want.aliases) {
            CAPTURE(alias);
            CHECK(ams_type_from_string(alias) == want.type);
        }
    }
    // A value with no row answers as NONE, and a spelling matches exactly.
    CHECK(ams_backend_row(static_cast<AmsType>(64)).type == AmsType::NONE);
    for (const char* unknown : {"", "ACE", "Mmu", "cfs ", "AD5X IFS"}) {
        CHECK(ams_type_from_string(unknown) == AmsType::NONE);
    }
}

TEST_CASE("Every compiled-in backend row builds its own type", "[ams][traits]") {
    for (const auto& type_row : helix::kAmsTypeTraits) {
        const helix::AmsBackendRow& row = helix::ams_backend_row(type_row.type);
        CAPTURE(type_row.name);
        CHECK((row.traits != nullptr) == (row.construct != nullptr));
        if (!row.construct) {
            continue;
        }
        const auto backend = row.construct(nullptr, nullptr, nullptr);
        REQUIRE(backend != nullptr);
        CHECK(backend->get_type() == type_row.type);
        CHECK(backend->traits().lane_noun == row.traits->lane_noun);
        CHECK(backend->traits().has_physical_tray == row.traits->has_physical_tray);
    }
    CHECK(helix::ams_backend_row(AmsType::NONE).construct == nullptr);
    CHECK(helix::ams_backend_row(AmsType::TOOL_CHANGER).construct != nullptr);
}
