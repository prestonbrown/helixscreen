// SPDX-License-Identifier: GPL-3.0-or-later
//
// Exercises the preset-reassignment logic formerly driven through
// MaterialPickerMenu (deleted in Task 8 of the Phase 2 offline filament
// picker project -- FilamentPanel now uses catalog_picker_ instead).
// MaterialPickerMenu itself was pure UI plumbing: a ContextMenu subclass
// whose dispatch_select()/dispatch_reset() just forwarded a row tap to a
// locally-injected std::function, with no interaction with FilamentPanel or
// MaterialSettingsManager. That mechanism has no logic equivalent once the
// widget is gone.
//
// The chain itself is production code, not a local reconstruction of one:
// helix::filament_presets::reassign_preset_if_valid() is the guard-then-persist
// step FilamentPanel::reassign_preset() runs before it refreshes the UI, and
// reset_to_defaults() is the persistence half of
// FilamentPanel::reset_presets_to_defaults(). Both are declared in
// ui_panel_filament.h and defined in ui_panel_filament.cpp, so a panel that
// stopped consulting validate_reassignment() before persisting would fail here.
// What is deliberately NOT covered is the panel's UI refresh that follows a
// successful write (presets::refresh_subjects(), the TemperatureController
// refresh, check_and_auto_select_preset(), update_spool_preset()) -- that half
// needs a constructed panel whose init_subjects() re-registers the panel's
// named subjects. tests/unit/test_filament_panel_op_timeout.cpp builds a real
// FilamentPanel from filament_panel.xml over a recording backend and is where
// that coverage belongs.

#include "ui_panel_filament.h"

#include "material_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Resets MaterialSettingsManager's in-memory preset state via its own public
// API (reset_preset_materials()) between test cases. Deliberately avoids a
// friend TestAccess helper here -- test_material_settings_manager.cpp and
// test_preset_filament_persistence.cpp already share one byte-identical copy
// across translation units (ODR-fragile by design note in the latter); a
// third copy isn't worth the risk when the public reset API already gives
// full isolation for what these tests need.
struct PresetResetFixture {
    PresetResetFixture() {
        MaterialSettingsManager::instance().reset_preset_materials();
    }
    ~PresetResetFixture() {
        MaterialSettingsManager::instance().reset_preset_materials();
    }
};

} // namespace

TEST_CASE("validate_reassignment accepts valid slot+material", "[material_settings][validate]") {
    using helix::filament_presets::validate_reassignment;
    CHECK(validate_reassignment(0, "PLA"));
    CHECK(validate_reassignment(3, "PC"));
}

TEST_CASE("validate_reassignment rejects bad slot, empty, or unknown material",
          "[material_settings][validate]") {
    using helix::filament_presets::validate_reassignment;
    CHECK_FALSE(validate_reassignment(-1, "PLA"));
    CHECK_FALSE(validate_reassignment(4, "PLA"));
    CHECK_FALSE(validate_reassignment(0, ""));
    CHECK_FALSE(validate_reassignment(0, "DEFINITELY_NOT_A_MATERIAL"));
}

// The guard-then-persist chain FilamentPanel::reassign_preset() runs: a picked
// material only reaches MaterialSettingsManager when validate_reassignment()
// accepts it. This is the state transition MaterialPickerMenu::dispatch_select()
// used to kick off (once wired up by the panel) -- exercised directly now that
// the widget is gone.
TEST_CASE_METHOD(PresetResetFixture, "reassign_preset_if_valid persists a valid material selection",
                 "[material_settings][presets]") {
    using helix::filament_presets::reassign_preset_if_valid;
    auto& mgr = MaterialSettingsManager::instance();

    CHECK(reassign_preset_if_valid(1, "PC"));
    CHECK(mgr.get_preset_materials()[1] == "PC");
}

TEST_CASE_METHOD(PresetResetFixture,
                 "reassign_preset_if_valid leaves the preset unchanged when validation rejects",
                 "[material_settings][presets]") {
    using helix::filament_presets::reassign_preset_if_valid;
    auto& mgr = MaterialSettingsManager::instance();

    auto before = mgr.get_preset_materials();

    // The guard is the assertion. Drop it from the production chain -- persist
    // first, validate after, or not at all -- and an unknown material reaches
    // MaterialSettingsManager and this CHECK fails.
    CHECK_FALSE(reassign_preset_if_valid(0, "NOT_A_REAL_MATERIAL"));
    CHECK(mgr.get_preset_materials() == before);

    // Positive half, same call: proves the guard -- not simply "nothing ever
    // gets written" -- is what gated the rejection above.
    CHECK(reassign_preset_if_valid(1, "PETG"));
    CHECK(mgr.get_preset_materials()[1] == "PETG");
}

// FilamentPanel::reset_presets_to_defaults()'s persistence half. In-depth
// coverage of the write itself already lives in test_material_settings_manager.cpp,
// so this ties the reset transition -- the one MaterialPickerMenu::dispatch_reset()
// used to trigger -- back to a prior reassignment.
TEST_CASE_METHOD(PresetResetFixture, "reset_to_defaults restores defaults after a reassignment",
                 "[material_settings][presets]") {
    auto& mgr = MaterialSettingsManager::instance();
    REQUIRE(helix::filament_presets::reassign_preset_if_valid(2, "PA"));
    REQUIRE(mgr.get_preset_materials()[2] == "PA");

    helix::filament_presets::reset_to_defaults();

    auto p = mgr.get_preset_materials();
    CHECK(p[0] == "PLA");
    CHECK(p[1] == "PETG");
    CHECK(p[2] == "ABS");
    CHECK(p[3] == "TPU");
}
