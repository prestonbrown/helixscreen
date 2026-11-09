// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_load_preheat.cpp
 * @brief Tests for resolve_load_preheat_material() — the material a filament op
 *        heats for, which FilamentOpHeater resolves for every surface.
 *
 * Run with: ./build/bin/helix-tests "[filament][preheat]"
 *
 * The slot the op targets decides, never the slot that happens to be loaded,
 * and the external spool only stands in when that slot names nothing. Either
 * mistake is silent: the nozzle heats for the wrong material and the user gets
 * a jam, not an error.
 */

#include "filament_database.h"
#include "filament_op_slot_resolver.h"

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::SlotInfo;
using helix::SlotStatus;

using helix::ui::kExternalSpoolSlot;
using helix::ui::load_preheat_temp;
using helix::ui::PreheatTarget;
using helix::ui::resolve_load_preheat_material;

namespace {

SlotInfo lane(const std::string& material) {
    SlotInfo s;
    s.material = material;
    s.status = material.empty() ? SlotStatus::EMPTY : SlotStatus::AVAILABLE;
    return s;
}

/// The DB's own recommended temp for a material, derived rather than hardcoded
/// so the expectations track the catalog instead of pinning a magic number.
int db_recommended(const std::string& material) {
    auto m = filament::find_material(material);
    REQUIRE(m.has_value());
    return m->nozzle_recommended();
}

int db_min(const std::string& material) {
    auto m = filament::find_material(material);
    REQUIRE(m.has_value());
    return m->nozzle_min;
}

} // namespace

// ===========================================================================
// (a) The slot being loaded decides — not the slot that happens to be loaded
// ===========================================================================
//
// Mutation check: make resolve_load_preheat_material() read a "current slot"
// instead of target_slot_info and this section goes red.
TEST_CASE("Load preheats for the SELECTED lane, not the loaded one", "[filament][preheat]") {
    const SlotInfo selected = lane("PETG"); // tool 3, the one being loaded
    const SlotInfo loaded = lane("PLA");    // tool 1, currently at the toolhead
    (void)loaded;                           // deliberately never handed to the resolver

    auto resolved = resolve_load_preheat_material(/*target_slot=*/3, &selected,
                                                  /*external_spool=*/nullptr);
    REQUIRE(resolved.has_value());
    CHECK(resolved->temp_c == db_recommended("PETG"));
    CHECK(resolved->temp_c != db_recommended("PLA"));
    CHECK(resolved->material_name == "PETG");
}

// ===========================================================================
// (b) An assigned external spool must not outrank an explicit AMS lane
// ===========================================================================
//
// Mutation check: move the external-spool branch above the target-slot branch
// (the old panel order) and every CHECK here fails.
TEST_CASE("External spool loses to the AMS lane the load targets", "[filament][preheat]") {
    const SlotInfo selected = lane("PLA");
    const SlotInfo ext = lane("PETG"); // the reporter's assigned external spool

    auto resolved = resolve_load_preheat_material(/*target_slot=*/0, &selected, &ext);
    REQUIRE(resolved.has_value());
    CHECK(resolved->temp_c == db_recommended("PLA"));
    CHECK(resolved->material_name == "PLA");
}

TEST_CASE("External spool is consulted when the load has no AMS lane of its own",
          "[filament][preheat]") {
    const SlotInfo ext = lane("PETG");

    SECTION("bypass sentinel goes straight to the spool, ignoring any lane") {
        const SlotInfo lane_that_must_not_win = lane("ABS");
        auto resolved =
            resolve_load_preheat_material(kExternalSpoolSlot, &lane_that_must_not_win, &ext);
        REQUIRE(resolved.has_value());
        CHECK(resolved->temp_c == db_recommended("PETG"));
    }

    SECTION("no backend / nothing resolved falls through to the spool") {
        auto resolved = resolve_load_preheat_material(/*target_slot=*/-1, nullptr, &ext);
        REQUIRE(resolved.has_value());
        CHECK(resolved->temp_c == db_recommended("PETG"));
    }

    SECTION("bypass with no spool assigned resolves nothing") {
        CHECK_FALSE(
            resolve_load_preheat_material(kExternalSpoolSlot, nullptr, nullptr).has_value());
    }

    SECTION("a lane that names nothing does not mask the spool") {
        // build_active_material() would hand back a synthetic 220°C for this
        // lane. Answering with it would bury both the spool and the panel's
        // material preset under a number nobody chose.
        const SlotInfo nameless = lane("");
        auto resolved = resolve_load_preheat_material(/*target_slot=*/2, &nameless, &ext);
        REQUIRE(resolved.has_value());
        CHECK(resolved->temp_c == db_recommended("PETG"));
    }

    SECTION("a lane with a vendor temp but no material name still counts as named") {
        SlotInfo vendor_only = lane("");
        vendor_only.nozzle_temp_min = 245;
        vendor_only.nozzle_temp_max = 265;
        auto resolved = resolve_load_preheat_material(/*target_slot=*/2, &vendor_only, &ext);
        REQUIRE(resolved.has_value());
        CHECK(resolved->temp_c == (245 + 265) / 2);
    }
}

TEST_CASE("Nothing named anywhere resolves to nullopt so the caller's tail runs",
          "[filament][preheat]") {
    const SlotInfo nameless = lane("");
    CHECK_FALSE(resolve_load_preheat_material(/*target_slot=*/0, &nameless, nullptr).has_value());
    CHECK_FALSE(resolve_load_preheat_material(/*target_slot=*/-1, nullptr, nullptr).has_value());
}

// ===========================================================================
// nozzle_recommended(), not nozzle_min
// ===========================================================================
//
// A load pushes cold filament through the melt zone and usually purges behind
// it — the highest-viscosity demand the hotend sees. nozzle_min is the edge
// below which the material will not flow at all, so loading at it is how you
// grind and jam. Recommended is inside the same window by construction.
//
// Mutation check: change load_preheat_temp() back to `mat.nozzle_min` and the
// first two sections fail.
TEST_CASE("Load preheat uses the recommended temp, not the bottom of the window",
          "[filament][preheat]") {
    SECTION("a real material heats above its minimum") {
        auto petg = filament::find_material("PETG");
        REQUIRE(petg.has_value());
        CHECK(load_preheat_temp(*petg) == petg->nozzle_recommended());
        CHECK(load_preheat_temp(*petg) > petg->nozzle_min);
        CHECK(load_preheat_temp(*petg) <= petg->nozzle_max);
    }

    SECTION("the resolver hands back that temp, not the DB minimum") {
        const SlotInfo slot = lane("PETG");
        auto resolved = resolve_load_preheat_material(0, &slot, nullptr);
        REQUIRE(resolved.has_value());
        CHECK(resolved->temp_c == db_recommended("PETG"));
        CHECK(resolved->temp_c != db_min("PETG"));
    }

    SECTION("an unknown material is unaffected — build_active_material makes max == min") {
        SlotInfo unknown = lane("Unobtainium-9000");
        unknown.nozzle_temp_min = 235;
        auto resolved = resolve_load_preheat_material(0, &unknown, nullptr);
        REQUIRE(resolved.has_value());
        CHECK(resolved->temp_c == 235);
    }
}

TEST_CASE("A filament op heats to its material, raised to what the nozzle is already set to",
          "[filament][preheat]") {
    CHECK(helix::ui::filament_op_nozzle_temp(240, 260) == 260);
    CHECK(helix::ui::filament_op_nozzle_temp(240, 200) == 240);
    CHECK(helix::ui::filament_op_nozzle_temp(240, 0) == 240);
}
