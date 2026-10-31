// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_apply.h"
#include "lane_translation.h"
#include "moonraker_spoolman_api.h"
#include "spoolman_types.h"

#include "../catch_amalgamated.hpp"

using nlohmann::json;
using namespace helix::ams;

namespace {
json spool_record(const json& extra) {
    return {{"id", 42}, {"filament", {{"id", 55}, {"material", "ABS"}, {"extra", extra}}}};
}

FilamentSlotOverride assign(const json& record, helix::SlotInfo& slot) {
    const helix::SlotInfo original = slot;
    apply_spool_to_slot(slot, helix::spoolman_detail::parse_spool_info(record));
    return user_override_from_slot_info(original, slot, nullptr);
}
} // namespace

TEST_CASE("Spoolman Orca id reaches lane_data through the picker", "[spoolman][orca_id]") {
    helix::SlotInfo slot;
    const auto ovr = assign(spool_record({{"orca_filament_id", "\"P123abcd\""}}), slot);
    CHECK(slot.spoolman_orca_filament_id == "P123abcd");
    const auto wire = to_lane_data_record(2, ovr);
    REQUIRE(wire.contains("filament_id"));
    CHECK(wire["filament_id"].is_string());
    CHECK(wire["filament_id"] == "P123abcd");
    CHECK(wire["lane"] == "2");
    CHECK(wire["helix_spoolman_filament_id"] == 55);

    const auto loaded = from_lane_data_record(wire);
    REQUIRE(loaded);
    CHECK(to_lane_data_record(2, loaded->second)["filament_id"] == "P123abcd");
    json cache = to_json(ovr);
    const auto cached = from_json(cache);
    CHECK(to_lane_data_record(2, cached)["filament_id"] == "P123abcd");
    CHECK(to_lane_data_record(2, external_lane_record(slot))["filament_id"] == "P123abcd");

    const auto sources = sources_from_record(loaded->second, wire, LegacyLockKeys::LaneData);
    helix::SlotInfo reopened;
    apply_resolved(reopened, resolve(sources));
    CHECK(reopened.spoolman_orca_filament_id == "P123abcd");
    CHECK(to_lane_data_record(2, user_override_from_slot_info(
                                     reopened, reopened, &cached))["filament_id"] == "P123abcd");
}

TEST_CASE("Only a JSON-encoded nonnumeric Orca token is accepted", "[spoolman][orca_id]") {
    const json invalid[] = {
        nullptr,    55,     1.5,       true,        json::array(), json::object(), "",   "55",
        "\"55\"",   "1.5",  "\"1.5\"", "null",      "true",        "{}",           "[]", "{",
        "P123abcd", "\"\"", "\" \"",   "\"P bad\"", "\"P\\n123\"", "\"P123abcd"};
    for (const auto& value : invalid) {
        CAPTURE(value.dump());
        helix::SlotInfo slot;
        const auto ovr = assign(spool_record({{"orca_filament_id", value}}), slot);
        CHECK(slot.spoolman_orca_filament_id.empty());
        CHECK_FALSE(to_lane_data_record(0, ovr).contains("filament_id"));
    }
    for (const json& extra : {json::object(), json::array(), json(nullptr), json("wrong")}) {
        helix::SlotInfo slot;
        CHECK_FALSE(
            to_lane_data_record(0, assign(spool_record(extra), slot)).contains("filament_id"));
    }
}

TEST_CASE("An explicit linked Spoolman id wins the catalog with fallback", "[spoolman][orca_id]") {
    helix::SlotInfo slot;
    slot.catalog_id = "bambu-lab-bambu-abs";
    SECTION("explicit Spoolman id") {
        const auto ovr = assign(spool_record({{"orca_filament_id", "\"P123abcd\""}}), slot);
        CHECK(to_lane_data_record(0, ovr)["filament_id"] == "P123abcd");
    }
    SECTION("missing extra") {
        const auto ovr = assign(spool_record(json::object()), slot);
        CHECK(to_lane_data_record(0, ovr)["filament_id"] == "OFhuaUQB");
    }
    SECTION("invalid extra") {
        const auto ovr = assign(spool_record({{"orca_filament_id", 55}}), slot);
        CHECK(to_lane_data_record(0, ovr)["filament_id"] == "OFhuaUQB");
    }
    SECTION("unlinked stale id") {
        FilamentSlotOverride ovr;
        ovr.catalog_id = slot.catalog_id;
        ovr.spoolman_orca_filament_id = "P123abcd";
        CHECK(to_lane_data_record(0, ovr)["filament_id"] == "OFhuaUQB");
        CHECK_FALSE(to_lane_data_record(0, ovr).contains("helix_spoolman_orca_filament_id"));
    }
}

TEST_CASE("A replacement or unlinked spool cannot retain another Orca id", "[spoolman][orca_id]") {
    helix::SlotInfo slot;
    auto prior = assign(spool_record({{"orca_filament_id", "\"P123abcd\""}}), slot);
    SECTION("replacing the spool with one without an id") {
        auto record = spool_record(json::object());
        record["id"] = 43;
        const auto ovr = assign(record, slot);
        CHECK(slot.spoolman_orca_filament_id.empty());
        CHECK_FALSE(to_lane_data_record(0, ovr).contains("filament_id"));
    }
    SECTION("unlinking") {
        const auto original = slot;
        slot.clear_spoolman_link();
        CHECK(slot.spoolman_orca_filament_id.empty());
        const auto ovr = user_override_from_slot_info(original, slot, &prior);
        CHECK_FALSE(to_lane_data_record(0, ovr).contains("filament_id"));
    }
    SECTION("server removes the extra from the same spool") {
        const auto observed = spool_identity_observation(
            helix::spoolman_detail::parse_spool_info(spool_record(json::object())));
        std::unordered_map<int, FilamentSlotOverride> records{{0, prior}};
        REQUIRE(persist_override_external_identity(nullptr, records, 0, observed, "[test]"));
        CHECK_FALSE(to_lane_data_record(0, records.at(0)).contains("filament_id"));
        auto sources =
            sources_from_record(prior, to_lane_data_record(0, prior), LegacyLockKeys::LaneData);
        sources.apply(observed);
        apply_resolved(slot, resolve(sources));
        CHECK(slot.spoolman_orca_filament_id.empty());
    }
    SECTION("an update for another spool is ignored") {
        auto record = spool_record({{"orca_filament_id", "\"Pother\""}});
        record["id"] = 43;
        const auto observed =
            spool_identity_observation(helix::spoolman_detail::parse_spool_info(record));
        std::unordered_map<int, FilamentSlotOverride> records{{0, prior}};
        CHECK_FALSE(persist_override_external_identity(nullptr, records, 0, observed, "[test]"));
        CHECK(to_lane_data_record(0, records.at(0))["filament_id"] == "P123abcd");
    }
}

TEST_CASE("Foreign numeric lane ids and old caches are not Orca ids", "[spoolman][orca_id]") {
    for (const json& value : {json(55), json("55"), json("P123abcd")}) {
        auto wire = to_lane_data_record(0, FilamentSlotOverride{});
        wire["filament_id"] = value;
        wire["spool_id"] = 42;
        const auto loaded = from_lane_data_record(wire);
        REQUIRE(loaded);
        CHECK(loaded->second.spoolman_orca_filament_id.empty());
        CHECK_FALSE(to_lane_data_record(0, loaded->second).contains("filament_id"));
    }
    json cache = to_json(FilamentSlotOverride{});
    cache.erase("spoolman_orca_filament_id");
    CHECK(from_json(cache).spoolman_orca_filament_id.empty());
}
