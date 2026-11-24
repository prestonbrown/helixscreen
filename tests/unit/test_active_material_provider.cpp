// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "active_material_provider.h"
#include "ams_types.h"
#include "filament_database.h"
#include "material_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Fixture that wipes MaterialSettingsManager between tests so an override
// staged in one case doesn't leak into the next. Uses public clear_override
// for each known material so we don't need friend access. Config::get_instance()
// returns nullptr in unit-test scope so save_to_config side effects no-op.
struct OverrideFixture {
    ~OverrideFixture() {
        auto& mgr = MaterialSettingsManager::instance();
        for (const char* name : {"PETG", "PLA", "PA", "ABS"}) {
            if (mgr.has_override(name)) {
                mgr.clear_override(name);
            }
        }
    }
};

// ============================================================================
// build_active_material() — Unit tests (no singletons needed)
// ============================================================================

TEST_CASE("build_active_material: slot with known material uses DB temps",
          "[active_material][build]") {
    SlotInfo slot;
    slot.material = "PETG";
    slot.color_rgb = 0x00FF00;
    slot.brand = "Polymaker";

    auto result = build_active_material(slot);

    // Should use filament DB values for PETG (230-260°C nozzle, 80°C bed)
    CHECK(result.material_info.nozzle_min == 230);
    CHECK(result.material_info.nozzle_max == 260);
    CHECK(result.material_info.bed_temp == 80);
    CHECK(result.color_rgb == 0x00FF00);
    CHECK(result.brand == "Polymaker");
    CHECK(result.material_name == "PETG");
    CHECK(result.display_name == "Polymaker PETG");
}

TEST_CASE("build_active_material: slot with explicit temps overrides DB",
          "[active_material][build]") {
    SlotInfo slot;
    slot.material = "PETG";
    slot.nozzle_temp_min = 240;
    slot.nozzle_temp_max = 255;
    slot.bed_temp = 85;
    slot.color_rgb = 0xFF0000;

    auto result = build_active_material(slot);

    CHECK(result.material_info.nozzle_min == 240);
    CHECK(result.material_info.nozzle_max == 255);
    CHECK(result.material_info.bed_temp == 85);
}

TEST_CASE("build_active_material: unknown material with temps uses slot temps",
          "[active_material][build]") {
    SlotInfo slot;
    slot.material = "SuperCustomFilament";
    slot.nozzle_temp_min = 275;
    slot.nozzle_temp_max = 295;
    slot.bed_temp = 100;
    slot.color_rgb = 0x0000FF;

    auto result = build_active_material(slot);

    CHECK(result.material_info.nozzle_min == 275);
    CHECK(result.material_info.nozzle_max == 295);
    CHECK(result.material_info.bed_temp == 100);
    CHECK(result.material_name == "SuperCustomFilament");
}

TEST_CASE("build_active_material: unknown material without temps uses default",
          "[active_material][build]") {
    SlotInfo slot;
    slot.material = "UnknownStuff";
    slot.color_rgb = 0x808080;

    auto result = build_active_material(slot);

    CHECK(result.material_info.nozzle_min == 220); // kDefaultLoadPreheatTemp
    CHECK(result.material_info.nozzle_max == 220);
}

TEST_CASE("build_active_material: empty material uses default temp", "[active_material][build]") {
    SlotInfo slot;
    slot.color_rgb = 0x808080;

    auto result = build_active_material(slot);

    CHECK(result.material_info.nozzle_min == 220);
}

TEST_CASE("build_active_material: display_name includes brand when present",
          "[active_material][build]") {
    SlotInfo slot;
    slot.material = "PLA";
    slot.brand = "eSUN";

    auto result = build_active_material(slot);
    CHECK(result.display_name == "eSUN PLA");
}

TEST_CASE("build_active_material: display_name is material only when no brand",
          "[active_material][build]") {
    SlotInfo slot;
    slot.material = "PLA";

    auto result = build_active_material(slot);
    CHECK(result.display_name == "PLA");
}

TEST_CASE("build_active_material: spoolman IDs are carried through", "[active_material][build]") {
    SlotInfo slot;
    slot.material = "PLA";
    slot.spoolman_id = 42;
    slot.spoolman_filament_id = 7;
    slot.spoolman_vendor_id = 3;

    auto result = build_active_material(slot);
    CHECK(result.spoolman_id == 42);
    CHECK(result.spoolman_filament_id == 7);
    CHECK(result.spoolman_vendor_id == 3);
}

TEST_CASE("build_active_material: material alias resolved", "[active_material][build]") {
    SlotInfo slot;
    slot.material = "Nylon"; // Alias for PA

    auto result = build_active_material(slot);

    // Should resolve to PA temps (250-280°C nozzle)
    CHECK(result.material_info.nozzle_min == 250);
    CHECK(result.material_info.nozzle_max == 280);
    CHECK(result.material_name == "Nylon"); // Keep original name for display
}

// ============================================================================
// Three-tier precedence (#961): user_override > vendor_preset > db_default
// ============================================================================
// Decision (Preston, 2026-05-19): user override wins absolutely. Even
// RFID-attested vendor temps (Snapmaker, QIDI) do NOT override a user
// setting. Resolution is per-field — overriding only nozzle_min should
// still let vendor preset win on nozzle_max.

TEST_CASE_METHOD(OverrideFixture, "build_active_material: user override beats vendor slot preset",
                 "[active_material][build][precedence]") {
    // Stage a user override of PETG nozzle_min only.
    filament::MaterialOverride ovr;
    ovr.nozzle_min = 215;
    MaterialSettingsManager::instance().set_override("PETG", ovr);

    SlotInfo slot;
    slot.material = "PETG";
    slot.nozzle_temp_min = 240; // Vendor preset
    slot.nozzle_temp_max = 255;

    auto result = build_active_material(slot);

    // Tier 1 wins: user override on nozzle_min
    CHECK(result.material_info.nozzle_min == 215);
    // Tier 2 wins: vendor preset on nozzle_max (not user-overridden)
    CHECK(result.material_info.nozzle_max == 255);
}

TEST_CASE_METHOD(OverrideFixture,
                 "build_active_material: vendor preset beats DB default when no user override",
                 "[active_material][build][precedence]") {
    // No user override staged.
    SlotInfo slot;
    slot.material = "PETG";
    slot.nozzle_temp_min = 240; // Vendor preset
    slot.nozzle_temp_max = 255;

    auto result = build_active_material(slot);

    // Tier 2 wins both fields (no user override; vendor preset present).
    CHECK(result.material_info.nozzle_min == 240);
    CHECK(result.material_info.nozzle_max == 255);
}

TEST_CASE_METHOD(OverrideFixture,
                 "build_active_material: DB default wins when no override and no vendor preset",
                 "[active_material][build][precedence]") {
    SlotInfo slot;
    slot.material = "PETG";
    // No slot temps, no override.

    auto result = build_active_material(slot);

    // Tier 3 wins: DB default for PETG.
    CHECK(result.material_info.nozzle_min == 230);
    CHECK(result.material_info.nozzle_max == 260);
}

TEST_CASE_METHOD(OverrideFixture,
                 "build_active_material: user override of bed_temp beats vendor preset",
                 "[active_material][build][precedence]") {
    filament::MaterialOverride ovr;
    ovr.bed_temp = 75;
    MaterialSettingsManager::instance().set_override("PETG", ovr);

    SlotInfo slot;
    slot.material = "PETG";
    slot.bed_temp = 85; // vendor

    auto result = build_active_material(slot);
    CHECK(result.material_info.bed_temp == 75);
}

TEST_CASE_METHOD(OverrideFixture,
                 "build_active_material: per-field mixed precedence resolved correctly",
                 "[active_material][build][precedence]") {
    // User overrides nozzle_max only. Slot provides all three. DB has its own.
    filament::MaterialOverride ovr;
    ovr.nozzle_max = 270;
    MaterialSettingsManager::instance().set_override("PETG", ovr);

    SlotInfo slot;
    slot.material = "PETG";
    slot.nozzle_temp_min = 235; // vendor
    slot.nozzle_temp_max = 250; // vendor (will lose to override)
    slot.bed_temp = 78;         // vendor (no override, no slot conflict)

    auto result = build_active_material(slot);

    CHECK(result.material_info.nozzle_min == 235); // tier 2: vendor
    CHECK(result.material_info.nozzle_max == 270); // tier 1: user override
    CHECK(result.material_info.bed_temp == 78);    // tier 2: vendor
}

// ============================================================================
// The print temperature comes from the loaded spool: the catalog product
// picked for the slot (slot.catalog_id), else the lane's own temperature,
// else the middle of the material range. A user range (Material Temps) is
// limits: it clamps the temperature and never becomes one. The product is the
// bundled SUNLU "PLA+ 2.0": type PLA, prints at 220. The lane holds 215.

namespace {
SlotInfo lane_with_sunlu_pick() {
    SlotInfo slot;
    slot.material = "PLA";
    slot.brand = "SUNLU";
    slot.catalog_id = "sunlu-pla-plus-2-0";
    slot.nozzle_temp_min = 215;
    slot.nozzle_temp_max = 215;
    slot.bed_temp = 60;
    return slot;
}

void set_user_range(const char* material, int min, int max) {
    filament::MaterialOverride ovr;
    ovr.nozzle_min = min;
    ovr.nozzle_max = max;
    MaterialSettingsManager::instance().set_override(material, ovr);
}
} // namespace

TEST_CASE_METHOD(OverrideFixture, "build_active_material: a picked product sets the print temp",
                 "[active_material][build][print_temp]") {
    const auto result = build_active_material(lane_with_sunlu_pick());

    CHECK(result.material_info.nozzle_recommended() == 220);
    // The range is still the lane's.
    CHECK(result.material_info.nozzle_min == 215);
    CHECK(result.material_info.nozzle_max == 215);
}

TEST_CASE_METHOD(OverrideFixture, "build_active_material: a user range clamps the product temp",
                 "[active_material][build][print_temp]") {
    set_user_range("PLA", 200, 214);

    const auto result = build_active_material(lane_with_sunlu_pick());

    CHECK(result.material_info.nozzle_recommended() == 214);
    CHECK(result.material_info.nozzle_min == 200);
    CHECK(result.material_info.nozzle_max == 214);
}

TEST_CASE_METHOD(OverrideFixture,
                 "build_active_material: a staged product temp stands in without a catalog id",
                 "[active_material][build][print_temp]") {
    SlotInfo slot = lane_with_sunlu_pick();
    slot.catalog_id.clear();
    slot.nozzle_temp_recommended = 225;

    CHECK(build_active_material(slot).material_info.nozzle_recommended() == 225);
}

TEST_CASE_METHOD(OverrideFixture, "build_active_material: without a product the lane's temp prints",
                 "[active_material][build][print_temp]") {
    SlotInfo slot = lane_with_sunlu_pick();
    slot.catalog_id.clear();

    CHECK(build_active_material(slot).material_info.nozzle_recommended() == 215);

    SECTION("inside a user range, not the range's middle") {
        set_user_range("PLA", 200, 240);
        CHECK(build_active_material(slot).material_info.nozzle_recommended() == 215);
    }
    SECTION("clamped into a user range") {
        set_user_range("PLA", 200, 210);
        CHECK(build_active_material(slot).material_info.nozzle_recommended() == 210);
    }
}

TEST_CASE_METHOD(OverrideFixture,
                 "build_active_material: a product naming another material is not the loaded one",
                 "[active_material][build][print_temp]") {
    // A stale pick: the lane now holds PETG from the firmware.
    SlotInfo slot = lane_with_sunlu_pick();
    slot.material = "PETG";
    slot.nozzle_temp_min = 245;
    slot.nozzle_temp_max = 245;

    CHECK(build_active_material(slot).material_info.nozzle_recommended() == 245);
}

TEST_CASE_METHOD(OverrideFixture,
                 "build_active_material: with nothing stated the material range's middle prints",
                 "[active_material][build][print_temp]") {
    SlotInfo slot;
    slot.material = "PLA";
    const auto db = filament::find_material("PLA");
    REQUIRE(db.has_value());

    CHECK(build_active_material(slot).material_info.nozzle_recommended() ==
          (db->nozzle_min + db->nozzle_max) / 2);

    SECTION("the user's range over the database's") {
        set_user_range("PLA", 200, 230);
        CHECK(build_active_material(slot).material_info.nozzle_recommended() == 215);
    }
}

TEST_CASE("SlotInfo::nozzle_print_temp: a staged product temp inside the range wins",
          "[active_material][build]") {
    SlotInfo slot;
    slot.nozzle_temp_min = 190;
    slot.nozzle_temp_max = 240;
    CHECK(slot.nozzle_print_temp() == 215);
    slot.nozzle_temp_recommended = 220;
    CHECK(slot.nozzle_print_temp() == 220);
    slot.nozzle_temp_recommended = 250;
    CHECK(slot.nozzle_print_temp() == 215);
}

// ============================================================================
// Spool preset matching logic tests
// ============================================================================

TEST_CASE("build_active_material: nozzle_recommended() computes midpoint",
          "[active_material][preset]") {
    SlotInfo slot;
    slot.material = "PETG"; // DB: 230-260°C

    auto result = build_active_material(slot);
    CHECK(result.material_info.nozzle_recommended() == 245); // (230+260)/2
}

TEST_CASE("build_active_material: overridden temps affect nozzle_recommended()",
          "[active_material][preset]") {
    SlotInfo slot;
    slot.material = "PETG";
    slot.nozzle_temp_min = 240;
    slot.nozzle_temp_max = 260;

    auto result = build_active_material(slot);
    CHECK(result.material_info.nozzle_recommended() == 250); // (240+260)/2
}

TEST_CASE("build_active_material: partial temp override (min only) preserves DB max",
          "[active_material][preset]") {
    SlotInfo slot;
    slot.material = "PLA";
    slot.nozzle_temp_min = 200; // Override min only, DB max stays 220

    auto result = build_active_material(slot);
    CHECK(result.material_info.nozzle_min == 200);
    CHECK(result.material_info.nozzle_max == 220); // From DB
    CHECK(result.material_info.nozzle_recommended() == 210);
}

TEST_CASE("build_active_material: synthetic material has correct bed_temp default",
          "[active_material][preset]") {
    SlotInfo slot;
    slot.material = "MysteryFilament";
    // No temps set

    auto result = build_active_material(slot);
    CHECK(result.material_info.bed_temp == 60); // Sensible default
}

TEST_CASE("build_active_material: case-insensitive material lookup", "[active_material][preset]") {
    SlotInfo slot;
    slot.material = "petg"; // lowercase

    auto result = build_active_material(slot);
    CHECK(result.material_info.nozzle_min == 230); // Still finds PETG
    CHECK(result.material_info.bed_temp == 80);
}

TEST_CASE("build_active_material: display_name with empty brand and material",
          "[active_material][preset]") {
    SlotInfo slot;
    // Both empty

    auto result = build_active_material(slot);
    CHECK(result.display_name == "Unknown");
}
