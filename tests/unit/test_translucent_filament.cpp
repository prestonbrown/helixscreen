// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A translucent filament (SlotInfo::translucent, prestonbrown/helixscreen#2129):
// read from an RRGGBBAA alpha, carried through the lane sources and records,
// and written back as Spoolman's 8-digit color_hex.

#include "ams_types.h"
#include "color_utils.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_resolver.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_catalog_search.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::ams::FilamentSlotOverride;
using nlohmann::json;

TEST_CASE("An RRGGBBAA color below FF is translucent", "[translucent]") {
    CHECK(parse_hex_alpha("#D32F2F80") == std::optional<uint8_t>(0x80));
    CHECK(parse_hex_alpha("D32F2F") == std::optional<uint8_t>(0xFF));
    CHECK(parse_hex_alpha("0xD32F2F") == std::optional<uint8_t>(0xFF));
    CHECK(parse_hex_alpha("#abc") == std::optional<uint8_t>(0xFF));
    CHECK_FALSE(parse_hex_alpha("nope").has_value());
    CHECK(hex_color_is_translucent("e8e8ff7f"));
    CHECK_FALSE(hex_color_is_translucent("E8E8FFFF"));
    CHECK_FALSE(hex_color_is_translucent("E8E8FF"));

    const ams::ColorReading reading = ams::read_lane_color("#E8E8FF80");
    CHECK(reading.kind == ams::ColorReadingKind::Observed);
    CHECK(reading.rgb == 0xE8E8FFu);
    CHECK(reading.translucent);
    CHECK_FALSE(ams::read_lane_color("#E8E8FF").translucent);
}

TEST_CASE("A linked Spoolman spool's 8-digit color states translucency",
          "[translucent][spoolman]") {
    SpoolInfo spool;
    spool.id = 7;
    spool.color_hex = "D32F2F80";
    const ams::Observation stated = ams::spool_identity_observation(spool);
    CHECK(stated.color_rgb == std::optional<uint32_t>(0xD32F2Fu));
    CHECK(stated.translucent == std::optional<bool>(true));

    spool.color_hex = "D32F2F";
    CHECK(ams::spool_identity_observation(spool).translucent == std::optional<bool>(false));

    // A multi-color filament is translucent when any of its colors is.
    spool.color_hex.clear();
    spool.multi_color_hexes = "D4AF37,C0C0C080";
    CHECK(spool.is_translucent());
}

TEST_CASE("Spoolman color spelling carries the alpha and drops an opaque one",
          "[translucent][spoolman]") {
    CHECK(spoolman::color_hex_for(0xD32F2F, true) == "D32F2F80");
    CHECK(spoolman::color_hex_for(0xD32F2F, false) == "D32F2F");
    // FF says nothing a 6-digit color does not, so the two match one filament.
    CHECK(spoolman::normalize_color_hex("#d32f2fff") == "D32F2F");
    CHECK(spoolman::normalize_color_hex("d32f2f80") == "D32F2F80");
    CHECK(spoolman::color_list_with_alpha("#D4AF37,C0C0C0", true) == "D4AF3780,C0C0C080");
    CHECK(spoolman::color_list_with_alpha("D4AF3780,C0C0C080", false) == "D4AF37,C0C0C0");
    CHECK(spoolman::normalize_color_list("D4AF3780,C0C0C080") == "D4AF3780,C0C0C080");
}

TEST_CASE("Saving a translucent lane to Spoolman writes RRGGBBAA, and reads back translucent",
          "[translucent][spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& spools = client.spoolman_mock().get_mock_spools();
    spools.clear();
    SpoolInfo linked;
    linked.id = 42;
    spools.push_back(linked);
    client.spoolman_mock().add_vendor(1, "Polymaker");
    client.spoolman_mock().next_created_filament_id = 999;

    SlotInfo original;
    original.slot_index = 0;
    original.spoolman_id = 42;
    original.spoolman_filament_id = 100;
    original.brand = "Polymaker";
    original.material = "PETG";
    original.color_rgb = 0xD32F2F;
    original.color_name = "Red";

    // Toggling the flag alone is a change to the filament's identity.
    SlotInfo edited = original;
    edited.translucent = true;
    const auto changes = SpoolmanSlotSaver::detect_changes(original, edited);
    CHECK(changes.filament_identity);

    SpoolmanSlotSaver saver(&api);
    bool done = false;
    saver.save(original, edited, [&](const SaveResult&) { done = true; });
    REQUIRE(done);
    REQUIRE(client.spoolman_mock().created_filaments.size() == 1);
    const std::string hex = client.spoolman_mock().created_filaments[0]["color_hex"];
    CHECK(hex == "D32F2F80");

    // The filament Spoolman now holds reads back as a translucent lane.
    SpoolInfo back;
    back.id = 42;
    back.color_hex = hex;
    SlotInfo slot;
    apply_spool_to_slot(slot, back);
    CHECK(slot.translucent);
    CHECK(slot.color_rgb == 0xD32F2Fu);
}

TEST_CASE("A lane record keeps translucency beside an #RRGGBB color", "[translucent][lane_data]") {
    SlotInfo edited;
    edited.slot_index = 1;
    edited.color_rgb = 0xE8E8FF;
    edited.material = "PETG";
    edited.translucent = true;
    const FilamentSlotOverride o = ams::user_override_from_slot_info(edited, nullptr);
    CHECK(o.translucent == std::optional<bool>(true));

    const json record = ams::to_lane_data_record(1, o);
    // Every other reader takes `color` as #RRGGBB.
    CHECK(record["color"] == "#E8E8FF");
    CHECK(record["helix_translucent"] == true);

    const auto back = ams::from_lane_data_record(record);
    REQUIRE(back.has_value());
    CHECK(back->second.translucent == std::optional<bool>(true));
    CHECK(ams::record_observation(back->second).translucent == std::optional<bool>(true));

    // Another writer's #RRGGBBAA color states it too.
    json other = record;
    other.erase("helix_translucent");
    other["color"] = "#E8E8FF80";
    const auto theirs = ams::from_lane_data_record(other);
    REQUIRE(theirs.has_value());
    CHECK(theirs->second.color_rgb == 0xE8E8FFu);
    CHECK(theirs->second.translucent == std::optional<bool>(true));

    // A color alpha below FF outranks our own key's stale false.
    other["color"] = "#E8E8FF7F";
    other["helix_translucent"] = false;
    const auto newer = ams::from_lane_data_record(other);
    REQUIRE(newer.has_value());
    CHECK(newer->second.translucent == std::optional<bool>(true));
    other.erase("helix_translucent");

    // A record without either says nothing, leaving another source's answer.
    other["color"] = "#E8E8FF";
    const auto silent = ams::from_lane_data_record(other);
    REQUIRE(silent.has_value());
    CHECK_FALSE(ams::record_observation(silent->second).translucent.has_value());
}

TEST_CASE("The external spool read from its record keeps the record's translucency",
          "[translucent][lane_data][external_spool]") {
    FilamentSlotOverride record;
    record.material = "PETG";
    record.color_rgb = 0xE8E8FF;
    record.color_set = true;
    record.translucent = true;
    CHECK(ams::external_spool_from_record(record).translucent);

    record.translucent = std::nullopt;
    CHECK_FALSE(ams::external_spool_from_record(record).translucent);

    // Translucency belongs to a color: a record with none states no paint.
    record.translucent = true;
    record.color_set = false;
    CHECK_FALSE(ams::external_spool_from_record(record).translucent);
}

TEST_CASE("The external spool's record keeps a translucent spool's translucency",
          "[translucent][lane_data][external_spool]") {
    SlotInfo spool;
    spool.material = "PETG";
    spool.color_rgb = 0xE8E8FF;
    spool.translucent = true;
    const FilamentSlotOverride record = ams::external_lane_record(spool);
    CHECK(record.translucent == std::optional<bool>(true));

    // Through the database and back.
    const auto stored = ams::from_lane_data_record(ams::to_lane_data_record(0, record));
    REQUIRE(stored.has_value());
    CHECK(ams::external_spool_from_record(stored->second).translucent);

    spool.translucent = false;
    CHECK(ams::external_lane_record(spool).translucent == std::optional<bool>(false));
}

TEST_CASE("Translucency goes with the color its source stated", "[translucent][lane_resolver]") {
    ams::Observation record(ams::ObservationSource::Record);
    record.color_rgb = 0xE8E8FF;
    record.translucent = true;

    SECTION("a stronger source naming the same color keeps it") {
        ams::Observation firmware(ams::ObservationSource::Firmware);
        firmware.color_rgb = 0xE8E8FF;
        ams::LaneSources sources;
        sources.record = record;
        sources.firmware = firmware;
        CHECK(ams::resolve(sources).translucent == std::optional<bool>(true));
    }
    SECTION("a stronger source naming another color is another filament") {
        ams::Observation firmware(ams::ObservationSource::Firmware);
        firmware.color_rgb = 0x101010;
        ams::LaneSources sources;
        sources.record = record;
        sources.firmware = firmware;
        CHECK(ams::resolve(sources).translucent == std::optional<bool>(false));
    }
    SECTION("an edit unchecking it wins") {
        ams::Observation pending(ams::ObservationSource::Pending);
        pending.translucent = false;
        ams::LaneSources sources;
        sources.record = record;
        sources.pending = pending;
        CHECK(ams::resolve(sources).translucent == std::optional<bool>(false));
    }
}

TEST_CASE("An edit that toggles translucency declares it", "[translucent][lane_translation]") {
    SlotInfo original;
    original.color_rgb = 0xE8E8FF;
    SlotInfo edited = original;
    CHECK_FALSE(ams::user_edit_observation(original, edited, false).translucent.has_value());
    edited.translucent = true;
    CHECK(ams::user_edit_observation(original, edited, false).translucent ==
          std::optional<bool>(true));
}
