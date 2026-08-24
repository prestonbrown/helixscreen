// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Preset-material unification tests.
//
// The codebase used to carry four independent notions of "preset materials":
// MaterialSettingsManager (user-configurable, the real one), a PRESET_NAMES[]
// literal in the preheat widget, material-named fields on HeaterPresets, and
// hardcoded text="PLA" in XML. They disagreed the moment a user reassigned a
// preset slot. These tests pin the unified behavior; each fails if its part of
// the unification is reverted.

#include "ui_ams_environment_overlay.h"

#include "../helix_test_fixture.h"
#include "filament_database.h"
#include "material_settings_manager.h"
#include "preheat_widget.h"
#include "preset_materials.h"
#include "temperature_controller.h"

#include <algorithm>
#include <array>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::HeaterType;
using helix::MaterialSettingsManager;
using helix::PreheatWidget;
namespace presets = helix::presets;

namespace {

/// Reassigns preset slots for the duration of a test and restores the defaults
/// afterwards, so slot reassignment can't leak into other tests.
struct PresetSlotGuard {
    PresetSlotGuard() {
        MaterialSettingsManager::instance().init();
        // Establish the default baseline rather than ASSUMING it. Preset slots
        // are process-global and persisted, and other suites sharing the
        // [presets] tag (test_material_settings_manager,
        // test_filament_preset_reassign) reassign them too. Under Catch2's
        // randomized ordering this test can run after one of those, so a test
        // that merely asserts slot 0 == "PLA" on entry passes or fails
        // depending on order — and the sharded full-suite run hides it by
        // landing them in different shards.
        MaterialSettingsManager::instance().reset_preset_materials();
    }
    ~PresetSlotGuard() {
        MaterialSettingsManager::instance().reset_preset_materials();
    }
    static void assign(int slot, const std::string& material) {
        MaterialSettingsManager::instance().set_preset_material(slot, material);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// THE LIVE BUG
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(HelixTestFixture,
                 "Preheat widget label and applied temps follow a reassigned preset slot",
                 "[presets][preheat]") {
    PresetSlotGuard guard;

    // Baseline: slot 0 is PLA by default.
    REQUIRE(presets::name(0) == "PLA");

    PresetSlotGuard::assign(0, "ASA");
    REQUIRE(presets::name(0) == "ASA");

    auto asa = filament::find_material("ASA");
    REQUIRE(asa.has_value());

    SECTION("label follows the reassigned material") {
        // Wide form shows "Preheat <material> (<nozzle>/<bed>)".
        const std::string wide = PreheatWidget::label_for_slot(0, /*heaters_active=*/false, 400);
        REQUIRE(wide.find("ASA") != std::string::npos);
        REQUIRE(wide.find("PLA") == std::string::npos);

        // Narrow form is just the material name.
        const std::string narrow = PreheatWidget::label_for_slot(0, /*heaters_active=*/false, 200);
        REQUIRE(narrow == "ASA");
    }

    SECTION("applied temperatures follow the reassigned material") {
        const auto t = PreheatWidget::targets_for_slot(0);
        REQUIRE(t.nozzle == asa->nozzle_recommended());
        REQUIRE(t.bed == asa->bed_temp);

        // And are genuinely different from PLA's, so this cannot pass by accident.
        auto pla = filament::find_material("PLA");
        REQUIRE(pla.has_value());
        REQUIRE(t.nozzle != pla->nozzle_recommended());
    }

    SECTION("label temps agree with the temps that get applied") {
        const auto t = PreheatWidget::targets_for_slot(0);
        const std::string wide = PreheatWidget::label_for_slot(0, false, 400);
        REQUIRE(wide.find(std::to_string(t.nozzle)) != std::string::npos);
        REQUIRE(wide.find(std::to_string(t.bed)) != std::string::npos);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "Filament panel and preheat widget agree on all slots after reconfiguration",
                 "[presets][preheat]") {
    PresetSlotGuard guard;

    // Reassign every slot away from its default so a stale hardcoded list cannot
    // coincidentally match.
    const std::array<const char*, presets::PRESET_COUNT> reassigned{"ASA", "PC", "PA", "PETG-CF"};
    for (int i = 0; i < presets::PRESET_COUNT; ++i) {
        PresetSlotGuard::assign(i, reassigned[static_cast<size_t>(i)]);
    }

    for (int i = 0; i < presets::PRESET_COUNT; ++i) {
        const std::string expected = reassigned[static_cast<size_t>(i)];

        // The filament panel renders presets::display_label(); the preheat widget
        // renders presets::name(). For an unbranded slot they must be the same
        // material, and both must be what the user assigned.
        REQUIRE(presets::name(i) == expected);
        REQUIRE(presets::display_label(i) == expected);

        // The preheat widget's narrow label is the bare material name.
        REQUIRE(PreheatWidget::label_for_slot(i, false, 200) == expected);

        // And the temps both surfaces show come from that same material.
        auto mat = filament::find_material(expected);
        REQUIRE(mat.has_value());
        const auto t = PreheatWidget::targets_for_slot(i);
        REQUIRE(t.nozzle == mat->nozzle_recommended());
        REQUIRE(t.bed == mat->bed_temp);
    }
}

// ---------------------------------------------------------------------------
// TEMP PANELS / HEATER PRESETS
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(HelixTestFixture, "Temp panel presets reflect the user's configured materials",
                 "[presets][temp_panel]") {
    PresetSlotGuard guard;

    PresetSlotGuard::assign(0, "ASA");
    PresetSlotGuard::assign(1, "PC");

    const auto nozzle = helix::compute_heater_presets(HeaterType::Nozzle);
    const auto bed = helix::compute_heater_presets(HeaterType::Bed);

    auto asa = filament::find_material("ASA");
    auto pc = filament::find_material("PC");
    REQUIRE(asa.has_value());
    REQUIRE(pc.has_value());

    REQUIRE(nozzle.off == 0);
    REQUIRE(nozzle.material[0] == asa->nozzle_recommended());
    REQUIRE(nozzle.material[1] == pc->nozzle_recommended());
    REQUIRE(bed.material[0] == asa->bed_temp);
    REQUIRE(bed.material[1] == pc->bed_temp);

    // The DATA layer covers every configured slot, independently of how many
    // buttons any given screen has room to render. The nozzle/bed/chamber temp
    // panels and the temp graph overlay display only slots 0-2 (a layout
    // constraint documented in those XML files and in TEMP_GRAPH_VISIBLE_PRESETS);
    // the filament and PID panels display all 4. compute_heater_presets() must
    // populate all of them regardless, so no screen is ever forced to show a
    // stale or zero target for a slot it does have room for.
    REQUIRE(nozzle.material.size() == static_cast<size_t>(presets::PRESET_COUNT));
    for (int i = 0; i < presets::PRESET_COUNT; ++i) {
        auto mat = filament::find_material(presets::name(i));
        REQUIRE(mat.has_value());
        REQUIRE(nozzle.material[static_cast<size_t>(i)] == mat->nozzle_recommended());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "PID presets resolve the right material's temps by index",
                 "[presets][pid]") {
    PresetSlotGuard guard;

    PresetSlotGuard::assign(2, "ASA");

    // The PID panel parses a slot from the button name and resolves it through
    // presets::name(), then looks the temperature up in the filament database —
    // the same two steps asserted here. Previously slot 2 was hardwired to ABS.
    const std::string material = presets::name(2);
    REQUIRE(material == "ASA");

    auto asa = filament::find_material(material);
    REQUIRE(asa.has_value());

    // Extruder preset uses the nozzle recommendation, bed preset the bed temp.
    REQUIRE(helix::compute_heater_presets(HeaterType::Nozzle).material[2] ==
            asa->nozzle_recommended());
    REQUIRE(helix::compute_heater_presets(HeaterType::Bed).material[2] == asa->bed_temp);
}

// ---------------------------------------------------------------------------
// AMS ENVIRONMENT FALLBACK
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(HelixTestFixture, "AMS environment fallback uses the unified preset list",
                 "[presets][ams]") {
    PresetSlotGuard guard;

    PresetSlotGuard::assign(3, "ASA");

    const auto fallback = helix::ui::AmsEnvironmentOverlay::fallback_comfort_materials();

    REQUIRE(fallback.size() == static_cast<size_t>(presets::PRESET_COUNT));
    for (int i = 0; i < presets::PRESET_COUNT; ++i) {
        REQUIRE(fallback[static_cast<size_t>(i)] == presets::name(i));
    }

    // The old hardcoded fallback ended in "PA" and ignored the user entirely.
    REQUIRE(fallback.back() == "ASA");
    REQUIRE(std::find(fallback.begin(), fallback.end(), "PA") == fallback.end());
}

// ---------------------------------------------------------------------------
// CHAMBER — deliberately NOT material-derived
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(HelixTestFixture,
                 "Chamber presets are a sane enclosure ladder, not inline literals",
                 "[presets][chamber]") {
    PresetSlotGuard guard;

    const auto chamber = helix::compute_heater_presets(HeaterType::Chamber);

    REQUIRE(chamber.off == 0);

    // Sane: every rung is a usable enclosure temperature and the ladder ascends.
    for (int i = 0; i < presets::PRESET_COUNT; ++i) {
        const int v = chamber.material[static_cast<size_t>(i)];
        REQUIRE(v > 0);
        REQUIRE(v <= 80); // chamber keypad ceiling
        if (i > 0) {
            REQUIRE(v > chamber.material[static_cast<size_t>(i - 1)]);
        }
    }

    // Deliberately independent of material identity. Deriving chamber from the
    // filament database would give PLA=0, PETG=0, TPU=0, ABS=50 — collapsing
    // three of four buttons into duplicates of "Off". Reassigning a slot must
    // therefore NOT move the chamber ladder.
    const auto before = chamber.material;
    PresetSlotGuard::assign(0, "ASA");
    PresetSlotGuard::assign(1, "PC");
    const auto after = helix::compute_heater_presets(HeaterType::Chamber).material;
    REQUIRE(after == before);

    // Specifically: slot 0 is an enclosure temperature, not PLA's chamber_temp_c (0).
    auto pla = filament::find_material("PLA");
    REQUIRE(pla.has_value());
    REQUIRE(pla->chamber_temp_c == 0);
    REQUIRE(chamber.material[0] > 0);
}
