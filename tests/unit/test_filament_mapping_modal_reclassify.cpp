// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_mapping_modal_reclassify.cpp
 * @brief Hand-picking a lane must re-answer BOTH mismatch questions.
 *
 * Run with: ./build/bin/helix-tests "[filament_mapping][modal][colour_mismatch]"
 *
 * The picker replaces the pairing, so every warning the seeding computed about
 * the previous lane is stale. Material was already recomputed here; colour is
 * the one that draws the chip's surround, and a flag carried over from a lane
 * the user has just replaced surrounds (or fails to surround) a chip on the
 * strength of a mapping that no longer exists.
 *
 * The modal has no mappings getter - it reports through on_mappings_updated at
 * OK - so that callback is how these read the result, which is also the path
 * the print-detail view consumes.
 */

#include "ui_filament_mapping_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_mapping_modal_test_access.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ToolMapping;
using helix::ui::FilamentMappingModal;
using helix::ui::FilamentSlotPicker;

namespace {

/// LVGLUITestFixture, not XMLTestFixture: only this one registers ALL XML
/// components, and the modal cannot construct filament_mapping_modal.xml
/// without that - show() returns false and every case fails in setup.
struct MappingReclassifyFixture : public LVGLUITestFixture {
    /// One red PLA tool against three lanes: a matching red PLA, a near-black
    /// PLA, and a red PETG. Between them every combination of the two warnings
    /// is reachable by picking a different lane and nothing else.
    static void seed(FilamentMappingModal& modal) {
        modal.set_tool_info({{0, 0xE72F1D, "PLA"}});
        modal.set_available_slots({
            {0, 0, 0x080A0D, "PLA", false, -1},  // right material, wrong colour
            {1, 0, 0xE72F1D, "PLA", false, -1},  // the lane the file asked for
            {2, 0, 0xE72F1D, "PETG", false, -1}, // right colour, wrong material
        });

        // Seeded with BOTH warnings standing, so "cleared" is a real observation
        // rather than a default that was never written.
        ToolMapping stale;
        stale.tool_index = 0;
        stale.mapped_slot = 0;
        stale.mapped_backend = 0;
        stale.material_mismatch = true;
        stale.color_mismatch = true;
        modal.set_mappings({stale});
    }

    /// The mappings the modal hands back at OK.
    static std::vector<ToolMapping> pick(FilamentMappingModal& modal,
                                         const FilamentSlotPicker::Selection& selection) {
        std::vector<ToolMapping> out;
        modal.set_on_mappings_updated([&out](std::vector<ToolMapping> m) { out = std::move(m); });
        FilamentMappingModalTestAccess::select_slot(modal, 0, selection);
        FilamentMappingModalTestAccess::ok(modal);
        helix::ui::UpdateQueue::instance().drain();
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(MappingReclassifyFixture,
                 "Picking the lane the file asked for clears both warnings",
                 "[filament_mapping][modal][colour_mismatch]") {
    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    const auto result = pick(modal, {1, 0, false});
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 1);
    CHECK_FALSE(result[0].material_mismatch);
    CHECK_FALSE(result[0].color_mismatch);
}

TEST_CASE_METHOD(MappingReclassifyFixture,
                 "Picking a wrong-coloured lane warns about the colour only",
                 "[filament_mapping][modal][colour_mismatch]") {
    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    // Same PLA, nowhere near the file's red. The two flags are independent
    // answers, so the material warning has to come DOWN in the same step.
    const auto result = pick(modal, {0, 0, false});
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 0);
    CHECK_FALSE(result[0].material_mismatch);
    CHECK(result[0].color_mismatch);
}

TEST_CASE_METHOD(MappingReclassifyFixture,
                 "Picking a wrong-material lane warns about the material only",
                 "[filament_mapping][modal][colour_mismatch]") {
    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    const auto result = pick(modal, {2, 0, false});
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 2);
    CHECK(result[0].material_mismatch);
    CHECK_FALSE(result[0].color_mismatch);
}

TEST_CASE_METHOD(MappingReclassifyFixture, "Choosing auto leaves no pairing to warn about",
                 "[filament_mapping][modal][colour_mismatch]") {
    // "Auto" is the firmware picking at print time, so there is no lane yet and
    // nothing to compare. A warning surviving here would surround a chip that
    // shows no lane colour at all.
    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    const auto result = pick(modal, {-1, -1, true});
    REQUIRE(result.size() == 1);
    CHECK(result[0].is_auto);
    CHECK_FALSE(result[0].material_mismatch);
    CHECK_FALSE(result[0].color_mismatch);
}
