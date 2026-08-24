// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_load_preheat.cpp
 * @brief Tests for resolve_load_preheat_material() — the nozzle temperature a
 *        filament LOAD heats to, shared by FilamentPanel and AmsOperationSidebar.
 *
 * Run with: ./build/bin/helix-tests "[filament][preheat]"
 *
 * Two silent bugs live here, both reported on a 5-tool toolchanger with a PETG
 * external spool assigned:
 *
 *   (a) FilamentPanel::resolve_preheat_temp() took NO slot argument. It read
 *       AmsSystemInfo::get_active_slot() — the LOADED lane. Select tool 3 (PETG)
 *       while tool 1 (PLA) is loaded, tap Load, and it preheated to PLA.
 *   (b) Before that it consulted the external spool UNCONDITIONALLY, so on any
 *       printer with a spool assigned, every panel load preheated to that
 *       spool's material no matter which lane was picked.
 *
 * Neither produces an error — you get a jam. AmsOperationSidebar always resolved
 * from the slot being loaded; the equality section below is the guard that the
 * two surfaces cannot drift apart again.
 */

#include "app_constants.h"
#include "filament_database.h"
#include "filament_op_slot_resolver.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::EXTERNAL_SPOOL_SLOT;
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

// ---------------------------------------------------------------------------
// Mirrors of the two surfaces' tails.
//
// FilamentPanel and AmsOperationSidebar are both LVGL/Moonraker-coupled and not
// unit-instantiable (cf. test_filament_op_button_state_char.cpp), so these pin
// the few lines each surface adds on top of the shared resolver. Everything
// above them is the shared function itself — which is the point.
// ---------------------------------------------------------------------------

/// AmsOperationSidebar::get_load_temp_for_slot()
int sidebar_load_temp(int target_slot, const SlotInfo* slot, const SlotInfo* ext) {
    auto resolved = resolve_load_preheat_material(target_slot, slot, ext);
    return resolved ? resolved->temp_c : AppConstants::Ams::DEFAULT_LOAD_PREHEAT_TEMP;
}

/// FilamentPanel::resolve_preheat_temp(). @p preset is the panel's selected
/// material preset ("" for none), @p min_extrude its Klipper min_extrude_temp.
PreheatTarget panel_preheat(int target_slot, const SlotInfo* slot, const SlotInfo* ext,
                            const std::string& preset, int min_extrude) {
    if (auto resolved = resolve_load_preheat_material(target_slot, slot, ext)) {
        return *resolved;
    }
    if (!preset.empty()) {
        if (auto mat = filament::find_material(preset)) {
            return {load_preheat_temp(*mat), preset};
        }
    }
    return {min_extrude, ""};
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
    (void)loaded; // deliberately never handed to the resolver — that is the fix

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
            resolve_load_preheat_material(EXTERNAL_SPOOL_SLOT, &lane_that_must_not_win, &ext);
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
            resolve_load_preheat_material(EXTERNAL_SPOOL_SLOT, nullptr, nullptr).has_value());
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

// ===========================================================================
// The regression guard: both surfaces answer the same for the same state
// ===========================================================================
//
// This is the equality the bug broke. For every (selected lane, loaded lane,
// external spool) combination where the selected lane names a material, the
// filament panel and the AMS sidebar must compute the SAME preheat target for
// the same slot. The loaded lane is present in each case precisely to prove it
// changes nothing.
TEST_CASE("FilamentPanel and AmsOperationSidebar agree on the load temp",
          "[filament][preheat][parity]") {
    struct Case {
        const char* selected;
        const char* loaded;
        const char* external; // "" == no spool assigned
    };
    const std::vector<Case> cases = {
        {"PETG", "PLA", "PETG"}, // the reported toolchanger state
        {"PETG", "PLA", ""},     // same, no spool assigned
        {"PLA", "PETG", "ABS"},  // spool disagreeing with both lanes
        {"ABS", "ABS", "PLA"},   // selected == loaded
        {"PLA", "PLA", ""},      // the boring case
        {"TPU", "PETG", "PETG"}, // spool matching the loaded lane, not the target
    };

    for (const auto& c : cases) {
        CAPTURE(c.selected, c.loaded, c.external);
        const SlotInfo selected = lane(c.selected);
        const SlotInfo ext = lane(c.external);
        const SlotInfo* ext_ptr = *c.external ? &ext : nullptr;
        constexpr int SELECTED_SLOT = 3;

        const int sidebar = sidebar_load_temp(SELECTED_SLOT, &selected, ext_ptr);
        // The panel is given a preset and a min_extrude_temp deliberately
        // DIFFERENT from every material in the table above: if either of the
        // panel's own tiers is reachable the equality breaks loudly instead of
        // coinciding by luck. PC is 260-300 (recommended 280); no case uses it.
        const PreheatTarget panel = panel_preheat(SELECTED_SLOT, &selected, ext_ptr,
                                                  /*preset=*/"PC", /*min_extrude=*/170);

        CHECK(panel.temp_c == sidebar);
        CHECK(panel.temp_c == db_recommended(c.selected));
        CHECK(panel.material_name == c.selected);
    }
}

// The one place the two surfaces legitimately differ, pinned so it is a decision
// and not a drift: the panel has a material-preset tier the sidebar has no UI
// for. It is only reachable when NEITHER the target lane nor an external spool
// names anything — exactly where the sidebar falls back to a blind default.
TEST_CASE("Only where nothing is named do the surfaces diverge, and deliberately",
          "[filament][preheat][parity]") {
    const SlotInfo nameless = lane("");

    CHECK(sidebar_load_temp(0, &nameless, nullptr) == AppConstants::Ams::DEFAULT_LOAD_PREHEAT_TEMP);

    const PreheatTarget with_preset =
        panel_preheat(0, &nameless, nullptr, /*preset=*/"PETG", /*min_extrude=*/170);
    CHECK(with_preset.temp_c == db_recommended("PETG"));
    CHECK(with_preset.material_name == "PETG");

    const PreheatTarget no_preset =
        panel_preheat(0, &nameless, nullptr, /*preset=*/"", /*min_extrude=*/170);
    CHECK(no_preset.temp_c == 170);
    CHECK(no_preset.material_name.empty());
}
