// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Multi-color filaments on their way out: the lane record's `colors` and
// `color_mode`, the Spoolman filament's multi_color_hexes and
// multi_color_direction, and the mode names that join them.

#include "ams_types.h"
#include "filament_color_mode.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "spoolman_catalog_search.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::SlotInfo;
using helix::ams::FilamentSlotOverride;
using nlohmann::json;

namespace mode = helix::color_mode;

TEST_CASE("Color modes map between the lane spec and Spoolman", "[multi_color]") {
    CHECK(mode::to_spoolman_direction("gradient") == "longitudinal");
    CHECK(mode::to_spoolman_direction("coaxial") == "coaxial");
    CHECK(mode::to_spoolman_direction("").empty());
    CHECK(mode::normalize("longitudinal") == "gradient");
    CHECK(mode::normalize("Coaxial") == "coaxial");
    CHECK(mode::normalize("solid").empty());
    CHECK(mode::to_spec("") == "solid");
    CHECK(mode::to_spec("longitudinal") == "gradient");
}

TEST_CASE("A lane record carries a multi-color list and its mode, and reads them back",
          "[multi_color][lane_data]") {
    SlotInfo edited;
    edited.slot_index = 2;
    edited.color_rgb = 0xD4AF37;
    edited.multi_color_hexes = "#D4AF37,#C0C0C0,#B87333";
    edited.color_mode = "coaxial";
    edited.material = "PLA";
    const FilamentSlotOverride o = helix::ams::user_override_from_slot_info(edited, nullptr);
    CHECK(o.multi_color_hexes == edited.multi_color_hexes);
    CHECK(o.color_mode == "coaxial");

    const json record = helix::ams::to_lane_data_record(2, o);
    CHECK(record["color"] == "#D4AF37");
    CHECK(record["colors"] == json::array({"#D4AF37", "#C0C0C0", "#B87333"}));
    CHECK(record["color_mode"] == "coaxial");

    const auto back = helix::ams::from_lane_data_record(record);
    REQUIRE(back.has_value());
    CHECK(back->second.multi_color_hexes == "#D4AF37,#C0C0C0,#B87333");
    CHECK(back->second.color_mode == "coaxial");
}

TEST_CASE("A one-color lane record says solid and lists no colors", "[multi_color][lane_data]") {
    SlotInfo edited;
    edited.color_rgb = 0x123456;
    const json record = helix::ams::to_lane_data_record(
        0, helix::ams::user_override_from_slot_info(edited, nullptr));
    CHECK(record["colors"].is_null());
    CHECK(record["color_mode"] == "solid");

    // Another writer's single-entry list is one color, not a multi-color one.
    json one = record;
    one["colors"] = json::array({"#123456"});
    one["color_mode"] = "gradient";
    const auto back = helix::ams::from_lane_data_record(one);
    REQUIRE(back.has_value());
    CHECK(back->second.multi_color_hexes.empty());
    CHECK(back->second.color_mode.empty());
}

TEST_CASE("A Spoolman edit to several colors PATCHes the list and direction, no single color",
          "[multi_color][spoolman]") {
    SpoolInfo original;
    original.color_hex = "#1A1A1A";
    SpoolInfo edited = original;
    edited.multi_color_hexes = "#D4AF37,c0c0c0";
    edited.multi_color_direction = "gradient";
    json spool_patch;
    json filament_patch;
    helix::SpoolmanSlotSaver::build_spool_patches(original, edited, spool_patch, filament_patch);
    CHECK(filament_patch["multi_color_hexes"] == "D4AF37,C0C0C0");
    CHECK(filament_patch["multi_color_direction"] == "longitudinal");
    REQUIRE(filament_patch.contains("color_hex"));
    CHECK(filament_patch.at("color_hex").is_null());
}

TEST_CASE("Back to one color clears the Spoolman list and sends the color",
          "[multi_color][spoolman]") {
    SpoolInfo original;
    original.multi_color_hexes = "D4AF37,C0C0C0";
    original.multi_color_direction = "coaxial";
    SpoolInfo edited = original;
    edited.multi_color_hexes.clear();
    edited.multi_color_direction.clear();
    edited.color_hex = "#D4AF37";
    json spool_patch;
    json filament_patch;
    helix::SpoolmanSlotSaver::build_spool_patches(original, edited, spool_patch, filament_patch);
    // Present and null: Spoolman clears a field it is sent as null, and keeps one it is not sent.
    REQUIRE(filament_patch.contains("multi_color_hexes"));
    REQUIRE(filament_patch.contains("multi_color_direction"));
    CHECK(filament_patch.at("multi_color_hexes").is_null());
    CHECK(filament_patch.at("multi_color_direction").is_null());
    CHECK(filament_patch.at("color_hex") == "#D4AF37");
}

TEST_CASE("A slot edit to several colors files a multi-color Spoolman filament",
          "[multi_color][spoolman]") {
    SlotInfo original;
    original.color_rgb = 0x1A1A1A;
    original.material = "PLA";
    SlotInfo edited = original;
    edited.color_rgb = 0xD4AF37;
    edited.multi_color_hexes = "#D4AF37,#C0C0C0";
    edited.color_mode = "coaxial";
    const helix::NewFilament f = helix::SpoolmanSlotSaver::new_filament_for(original, edited);
    CHECK(f.multi_color_hexes == "D4AF37,C0C0C0");
    CHECK(f.multi_color_direction == "coaxial");
    CHECK(helix::SpoolmanSlotSaver::detect_changes(original, edited).filament_identity);

    // The same colors in another mode are another filament.
    SlotInfo gradient = edited;
    gradient.color_mode = "gradient";
    CHECK(helix::SpoolmanSlotSaver::detect_changes(edited, gradient).filament_identity);

    // An existing record with the same list and direction is reused; one with
    // the other direction is not.
    FilamentInfo same;
    same.id = 7;
    same.material = "PLA";
    same.filament_name = "Silk Duo";
    same.multi_color_hexes = "d4af37,c0c0c0";
    same.multi_color_direction = "coaxial";
    FilamentInfo other = same;
    other.id = 8;
    other.multi_color_direction = "longitudinal";
    const std::vector<FilamentInfo> filaments = {other, same};
    const FilamentInfo* found = helix::spoolman::find_matching_filament(
        filaments, "PLA", "Silk Duo", "D4AF37", f.multi_color_hexes, "coaxial");
    REQUIRE(found != nullptr);
    CHECK(found->id == 7);
}

TEST_CASE("A linked spool states its colors and their mode onto the slot",
          "[multi_color][spoolman]") {
    SpoolInfo spool;
    spool.id = 12;
    spool.multi_color_hexes = "D4AF37,C0C0C0";
    spool.multi_color_direction = "longitudinal";
    // The observation the lane resolver files for a linked lane.
    const helix::ams::Observation stated = helix::ams::spool_identity_observation(spool);
    CHECK(stated.multi_color_hexes == std::optional<std::string>("D4AF37,C0C0C0"));
    CHECK(stated.color_mode == std::optional<std::string>("gradient"));
    SlotInfo slot;
    apply_spool_to_slot(slot, spool);
    CHECK(slot.multi_color_hexes == "D4AF37,C0C0C0");
    CHECK(slot.color_mode == "gradient");
    // Spoolman keeps no color_hex for a multi-color filament; the first color is its color.
    CHECK(slot.color_rgb == 0xD4AF37);

    spool.multi_color_hexes.clear();
    spool.multi_color_direction.clear();
    spool.color_hex = "#112233";
    apply_spool_to_slot(slot, spool);
    CHECK(slot.multi_color_hexes.empty());
    CHECK(slot.color_mode.empty());
}
