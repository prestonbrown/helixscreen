// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_slot_override.h"
#include "helix_test_fixture.h"
#include "lane_apply.h"
#include "lane_observation.h"
#include "lane_source_store.h"

#include "../catch_amalgamated.hpp"

using helix::SlotInfo;
using helix::SlotStatus;
using helix::ams::apply_resolved;
using helix::ams::copy_resolver_owned_identity;
using helix::ams::FilamentSlotOverride;
using helix::ams::narrow_status;
using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::ams::resolved_lane;
using helix::ams::ResolvedLane;

TEST_CASE("Resolved presence is the only thing that decides whether a lane is occupied",
          "[lane][apply]") {
    const SlotStatus every[] = {SlotStatus::UNKNOWN, SlotStatus::EMPTY,   SlotStatus::AVAILABLE,
                                SlotStatus::LOADED,  SlotStatus::BLOCKED, SlotStatus::FROM_BUFFER};

    SECTION("no backend status can make an absent lane read occupied") {
        for (SlotStatus s : every) {
            INFO("backend status " << helix::slot_status_to_string(s));
            CHECK(narrow_status(s, false) == SlotStatus::EMPTY);
        }
    }

    SECTION("no backend status can make a present lane read absent") {
        for (SlotStatus s : every) {
            INFO("backend status " << helix::slot_status_to_string(s));
            const SlotStatus out = narrow_status(s, true);
            CHECK(out != SlotStatus::EMPTY);
            CHECK(out != SlotStatus::UNKNOWN);
        }
    }

    SECTION("a present lane keeps the flavour its backend state machine owns") {
        // LOADED, BLOCKED and FROM_BUFFER are not presence answers: they say
        // which lane is at the extruder, that a lane is jammed, and Happy Hare's
        // gate_status 2 respectively. The resolver has no opinion on any of them.
        CHECK(narrow_status(SlotStatus::LOADED, true) == SlotStatus::LOADED);
        CHECK(narrow_status(SlotStatus::BLOCKED, true) == SlotStatus::BLOCKED);
        CHECK(narrow_status(SlotStatus::FROM_BUFFER, true) == SlotStatus::FROM_BUFFER);
        CHECK(narrow_status(SlotStatus::AVAILABLE, true) == SlotStatus::AVAILABLE);
    }

    SECTION("a present lane whose backend has no flavour yet reads available") {
        CHECK(narrow_status(SlotStatus::UNKNOWN, true) == SlotStatus::AVAILABLE);
        CHECK(narrow_status(SlotStatus::EMPTY, true) == SlotStatus::AVAILABLE);
    }
}

TEST_CASE("apply_resolved writes the resolver's fields and leaves the rest alone",
          "[lane][apply]") {
    SlotInfo slot;
    slot.slot_index = 3;
    slot.global_index = 7;
    slot.status = SlotStatus::LOADED;
    slot.mapped_tool = 2;
    slot.extruder_name = "extruder1";
    slot.endless_spool_group = 4;
    slot.remaining_length_m = 12.5F;
    slot.nozzle_temp_min = 210;
    slot.material = "PETG";
    slot.color_rgb = 0xFFFFFF;
    // Fields apply_resolved must leave alone because ResolvedLane has no
    // equivalent of them: a later task must not quietly start writing these.
    slot.multi_color_hexes = "#111111,#222222";

    ResolvedLane r;
    r.present = true;
    r.color_rgb = 0xBCBCBC;
    r.color_name = "Gunmetal";
    r.material = "PLA";
    r.brand = "Kingroon";
    r.spool_name = "Reel 9";
    r.catalog_id = "cat-kingroon-pla";
    r.product_name = "PLA Silk";
    r.spoolman_id = 7;
    r.spoolman_filament_id = 31;
    r.spoolman_vendor_id = 12;
    r.remaining_weight_g = 218.0F;
    r.total_weight_g = 950.0F;

    apply_resolved(slot, r);

    // The resolver's 12 fields are replaced wholesale, each with a value
    // distinctive enough that a dropped assignment cannot pass by accident.
    CHECK(slot.color_rgb == 0xBCBCBC);
    CHECK(slot.color_name == "Gunmetal");
    CHECK(slot.material == "PLA");
    CHECK(slot.brand == "Kingroon");
    CHECK(slot.spool_name == "Reel 9");
    CHECK(slot.catalog_id == "cat-kingroon-pla");
    CHECK(slot.product_name == "PLA Silk");
    CHECK(slot.spoolman_id == 7);
    CHECK(slot.spoolman_filament_id == 31);
    CHECK(slot.spoolman_vendor_id == 12);
    CHECK(slot.remaining_weight_g == Catch::Approx(218.0F));
    CHECK(slot.total_weight_g == Catch::Approx(950.0F));

    // Presence narrows the status; LOADED is a flavour, so it survives.
    CHECK(slot.status == SlotStatus::LOADED);

    // Everything SlotInfo carries that the resolver does not own is untouched,
    // including the field ResolvedLane has no member for at all.
    CHECK(slot.slot_index == 3);
    CHECK(slot.global_index == 7);
    CHECK(slot.mapped_tool == 2);
    CHECK(slot.extruder_name == "extruder1");
    CHECK(slot.endless_spool_group == 4);
    CHECK(slot.remaining_length_m == Catch::Approx(12.5F));
    CHECK(slot.nozzle_temp_min == 210);
    CHECK(slot.multi_color_hexes == "#111111,#222222");
}

TEST_CASE("A lane the resolver reports absent is emptied, not merely dimmed", "[lane][apply]") {
    SlotInfo slot;
    slot.status = SlotStatus::LOADED;
    slot.material = "PETG";

    ResolvedLane r;
    r.present = false;
    r.material = "PETG"; // identity survives an eject by design (#1071)

    apply_resolved(slot, r);

    CHECK(slot.status == SlotStatus::EMPTY);
    CHECK_FALSE(slot.is_present());
    // Identity is deliberately retained so the lane ghosts rather than blanking.
    CHECK(slot.material == "PETG");
}

TEST_CASE_METHOD(HelixTestFixture, "resolved_lane reflects what was ingested for that lane",
                 "[lane][apply]") {
    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    helix::ams::ingest(helix::ams::lane_id_for(0, 0), sensed);

    Observation cache(ObservationSource::Firmware);
    cache.color_rgb = 0xED2C2C;
    cache.material = "PETG";
    helix::ams::ingest(helix::ams::lane_id_for(0, 0), cache);

    const ResolvedLane r = resolved_lane(helix::ams::lane_id_for(0, 0));
    CHECK(r.present == true);
    CHECK(r.color_rgb == 0xED2C2C);
    CHECK(r.material == "PETG");
}

TEST_CASE_METHOD(HelixTestFixture, "an unwritten lane resolves to nothing observed",
                 "[lane][apply]") {
    // Not "empty and grey". Nobody has looked at this lane, and the difference
    // is what lets apply_resolved leave a backend's own values in place.
    const ResolvedLane r = resolved_lane(helix::ams::lane_id_for(0, 5));
    CHECK_FALSE(r.present.has_value());
    CHECK_FALSE(r.material.has_value());
    CHECK_FALSE(r.color_rgb.has_value());
}

TEST_CASE("A field no source observed leaves the backend's own value standing", "[lane][apply]") {
    // The backend parsed material, brand and colour out of its own firmware
    // and no lane source speaks to any of them. Snapmaker's print_task_config
    // is exactly this shape: it states material, brand and colour, and files
    // no record on purpose, because it is a write surface rather than a sensor.
    SlotInfo slot;
    slot.status = SlotStatus::AVAILABLE;
    slot.material = "PLA";
    slot.brand = "Snapmaker";
    slot.color_rgb = 0xED2C2C;
    slot.spool_name = "T0";
    slot.remaining_weight_g = 612.0F;

    // Only presence was observed, which is what a lane on a live printer holds
    // when nothing has declared an identity for it.
    ResolvedLane r;
    r.present = true;

    apply_resolved(slot, r);

    CHECK(slot.material == "PLA");
    CHECK(slot.brand == "Snapmaker");
    CHECK(slot.color_rgb == 0xED2C2C);
    CHECK(slot.spool_name == "T0");
    CHECK(slot.remaining_weight_g == Catch::Approx(612.0F));
    CHECK(slot.status == SlotStatus::AVAILABLE);
}

TEST_CASE("A lane with no presence reading keeps the status its backend stamped", "[lane][apply]") {
    // Presence narrows the status only when a sensor has spoken. AFC files a
    // sensed record only when a frame carries a presence signal, and ACE only
    // when the frame states a status, so a lane holding identity and no sensor
    // reading is ordinary. Narrowing on an unobserved presence would stamp
    // EMPTY over a lane the backend knows is loaded.
    SlotInfo slot;
    slot.status = SlotStatus::LOADED;

    ResolvedLane r;
    r.material = "PETG";

    apply_resolved(slot, r);

    CHECK(slot.status == SlotStatus::LOADED);
    CHECK(slot.material == "PETG");
}

TEST_CASE("copy_resolver_owned_identity carries every field a paint can write",
          "[lane][apply][1672]") {
    // The set has to track apply_resolved()'s, field for field. A caller
    // snapshots before a paint and copies back after; one field short and the
    // paint's answer survives where the caller's value was meant to.
    SlotInfo src;
    src.color_rgb = 0x1E5AA8;
    src.color_name = "Cobalt";
    src.material = "PETG";
    src.brand = "Polymaker";
    src.spool_name = "PolyLite PETG";
    src.catalog_id = "polymaker-polylite-petg";
    src.product_name = "PolyLite PETG";
    src.spoolman_id = 7;
    src.spoolman_filament_id = 55;
    src.spoolman_vendor_id = 3;
    src.remaining_weight_g = 812.5F;
    src.total_weight_g = 1000.0F;

    SlotInfo dst;
    dst.status = SlotStatus::LOADED;
    dst.mapped_tool = 2;

    copy_resolver_owned_identity(dst, src);

    CHECK(dst.color_rgb == 0x1E5AA8u);
    CHECK(dst.color_name == "Cobalt");
    CHECK(dst.material == "PETG");
    CHECK(dst.brand == "Polymaker");
    CHECK(dst.spool_name == "PolyLite PETG");
    CHECK(dst.catalog_id == "polymaker-polylite-petg");
    CHECK(dst.product_name == "PolyLite PETG");
    CHECK(dst.spoolman_id == 7);
    CHECK(dst.spoolman_filament_id == 55);
    CHECK(dst.spoolman_vendor_id == 3);
    CHECK(dst.remaining_weight_g == Catch::Approx(812.5F));
    CHECK(dst.total_weight_g == Catch::Approx(1000.0F));

    // Outside the set: the fields apply_resolved() never writes, which a
    // caller wants the paint's recomputation of, not its own stale copy.
    CHECK(dst.status == SlotStatus::LOADED);
    CHECK(dst.mapped_tool == 2);
}

TEST_CASE("clear_lane_only_identity blanks every field only a lane record states",
          "[lane][apply][1632]") {
    // The set has to mirror the fields no firmware states, field for field.
    // A repaint narrows the cached struct through this; one field short and a
    // dropped record's value keeps standing beside a binding it no longer
    // describes.
    const auto make_painted = [] {
        SlotInfo slot;
        slot.brand = "Polymaker";
        slot.spool_name = "PolyLite PETG";
        slot.catalog_id = "polymaker-polylite-petg";
        slot.product_name = "PolyLite PETG";
        slot.spoolman_filament_id = 55;
        slot.spoolman_vendor_id = 3;
        return slot;
    };

    SECTION("no record left: every lane-only field blanks") {
        SlotInfo slot = make_painted();
        helix::ams::clear_lane_only_identity(slot, nullptr);
        CHECK(slot.brand.empty());
        CHECK(slot.spool_name.empty());
        CHECK(slot.catalog_id.empty());
        CHECK(slot.product_name.empty());
        CHECK(slot.spoolman_filament_id == 0);
        CHECK(slot.spoolman_vendor_id == 0);
    }

    SECTION("a kept record restates its own values") {
        SlotInfo slot = make_painted();
        FilamentSlotOverride kept;
        kept.brand = "Bambu";
        kept.spoolman_filament_id = 61;
        kept.spoolman_vendor_id = 8;
        helix::ams::clear_lane_only_identity(slot, &kept);
        CHECK(slot.brand == "Bambu");
        CHECK(slot.spool_name.empty());
        CHECK(slot.spoolman_filament_id == 61);
        CHECK(slot.spoolman_vendor_id == 8);
    }
}
