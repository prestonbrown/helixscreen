// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_database_response.cpp
 * @brief AFC's server.database.get_item replies, applied through the backend.
 *
 * send_jsonrpc hands its callback the full JSON-RPC envelope, so the payload
 * lives at result.value (prestonbrown/helixscreen#1148). Reading the envelope
 * and the records is test_afc_status_parse.cpp's subject; the mock does not
 * implement server.database.get_item, so these cases drive the reply handlers
 * directly.
 */

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "test_helpers/afc_test_access.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

/// The envelope Moonraker actually sends for server.database.get_item.
json envelope(const char* ns, const json& value) {
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"namespace", ns}, {"value", value}}}};
}

bool apply_version(AmsBackendAfc& afc, const json& response) {
    return AfcTestAccess::apply_afc_version_response(afc, response);
}

bool apply_lanes(AmsBackendAfc& afc, const json& response) {
    return AfcTestAccess::apply_lane_data_response(afc, response);
}

std::string version(AmsBackendAfc& afc) {
    return AfcTestAccess::afc_version(afc);
}

} // namespace

TEST_CASE("AFC version is applied from a real database envelope", "[afc][database]") {
    AmsBackendAfc afc(nullptr, nullptr);
    REQUIRE(version(afc) == "unknown");

    REQUIRE(apply_version(afc, envelope("afc-install", {{"version", "1.0.40"}})));
    CHECK(version(afc) == "1.0.40");
}

// lane_data is not gated on a version: AFC stopped writing the afc-install
// namespace (#451), so the version string is absent or frozen. A real BoxTurtle
// reported "1.0.0" while its lane_data namespace was fully populated.
TEST_CASE("lane_data does not depend on the reported version", "[afc][database]") {
    AmsBackendAfc afc(nullptr, nullptr);
    REQUIRE(apply_version(afc, envelope("afc-install", {{"version", "1.0.0"}})));

    const json lanes = {{"lane1", {{"material", "ASA"}, {"color", "#c1c1c1"}, {"spool_id", 127}}}};
    REQUIRE(apply_lanes(afc, envelope("lane_data", lanes)));
    CHECK(afc.get_slot_info(0).material == "ASA");
    CHECK(afc.get_slot_info(0).color_rgb == 0xc1c1c1);
    CHECK(afc.get_slot_info(0).spoolman_id == 127);
}

TEST_CASE("a malformed version reply applies nothing", "[afc][database]") {
    AmsBackendAfc afc(nullptr, nullptr);
    CHECK_FALSE(apply_version(afc, envelope("afc-install", json::object())));
    CHECK_FALSE(apply_version(afc, envelope("afc-install", {{"version", nullptr}})));
    CHECK_FALSE(apply_version(afc, envelope("afc-install", "x")));
    CHECK_FALSE(apply_version(afc, json::object()));
    CHECK(version(afc) == "unknown");
}

// AFC's real lane_data payload carries no presence keys. Absent means unchanged:
// a default would clobber the status AFC_stepper derived.
TEST_CASE("lane_data does not clobber slot status when it carries none",
          "[afc][database][regression]") {
    AmsBackendAfc afc(nullptr, nullptr);
    REQUIRE(apply_lanes(afc, envelope("lane_data", {{"lane1", {{"empty", true}}}})));
    REQUIRE(afc.get_slot_info(0).status == SlotStatus::EMPTY);

    REQUIRE(apply_lanes(
        afc, envelope("lane_data", {{"lane1", {{"material", "ASA"}, {"spool_id", 86}}}})));
    CHECK(afc.get_slot_info(0).status == SlotStatus::EMPTY);
    CHECK(afc.get_slot_info(0).material == "ASA");

    REQUIRE(apply_lanes(afc, envelope("lane_data", {{"lane1", {{"tool_loaded", true}}}})));
    CHECK(afc.get_slot_info(0).status == SlotStatus::LOADED);
}

TEST_CASE("a non-object lane_data reply is rejected", "[afc][database]") {
    AmsBackendAfc afc(nullptr, nullptr);
    CHECK_FALSE(apply_lanes(afc, envelope("AFC", nullptr)));
    CHECK_FALSE(apply_lanes(afc, envelope("AFC", json::array())));
    CHECK_FALSE(apply_lanes(afc, json::object()));
}
