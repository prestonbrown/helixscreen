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
// The real logic lives in helix::filament_presets::validate_reassignment
// (declared in ui_panel_filament.h, defined in ui_panel_filament.cpp) and
// MaterialSettingsManager::set_preset_material / reset_preset_materials
// (material_settings_manager.cpp) -- both exercised directly below, with no
// LVGL widget involved. FilamentPanel::reassign_preset()/
// reset_presets_to_defaults() themselves are thin wrappers around this same
// validate-then-persist chain, and the chain is mirrored here at the same level
// test_material_settings_manager.cpp and test_preset_filament_persistence.cpp
// already exercise it.
//
// The claim that used to stand here -- "no existing unit test constructs a
// FilamentPanel" -- is false. tests/unit/test_filament_panel_op_timeout.cpp
// builds a real one from filament_panel.xml over a recording backend, and both
// reassign_preset() and reset_presets_to_defaults() are PUBLIC, so the mirror
// below can be retired by driving the panel directly. Its three remaining
// callees are unit-safe with no widgets: presets::refresh_subjects() returns
// early when subjects were never initialized, update_spool_preset() guards on
// spool_preset_button_, and check_and_auto_select_preset() writes nothing while
// the targets are 0. What that costs is a per-test-case panel whose
// init_subjects() re-registers the panel's named subjects, which is the part
// that needs proving against a build.

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

// Mirrors FilamentPanel::reassign_preset()'s guard-then-persist chain: a
// picked material is only forwarded to MaterialSettingsManager when
// validate_reassignment() accepts it. This is the state transition
// MaterialPickerMenu::dispatch_select() used to kick off (once wired up by
// the panel) -- exercised directly now that the widget is gone.
TEST_CASE_METHOD(PresetResetFixture, "reassign_preset chain persists a valid material selection",
                 "[material_settings][presets]") {
    using helix::filament_presets::validate_reassignment;
    auto& mgr = MaterialSettingsManager::instance();

    const int slot = 1;
    const std::string material = "PC";
    REQUIRE(validate_reassignment(slot, material));
    mgr.set_preset_material(slot, material);

    CHECK(mgr.get_preset_materials()[slot] == "PC");
}

TEST_CASE_METHOD(PresetResetFixture,
                 "reassign_preset chain leaves preset unchanged when validation rejects",
                 "[material_settings][presets]") {
    using helix::filament_presets::validate_reassignment;
    auto& mgr = MaterialSettingsManager::instance();

    auto before = mgr.get_preset_materials();

    // Mirrors FilamentPanel::reassign_preset()'s guard verbatim: only
    // persist when validate_reassignment() accepts. If this guard were
    // missing (or if set_preset_material() were called unconditionally),
    // the CHECK below would fail -- unlike the old version of this test,
    // set_preset_material() is actually reachable here.
    if (validate_reassignment(0, "NOT_A_REAL_MATERIAL")) {
        mgr.set_preset_material(0, "NOT_A_REAL_MATERIAL");
    }
    CHECK(mgr.get_preset_materials() == before);

    // Positive half, same guarded pattern: proves the guard -- not simply
    // "nothing ever gets written" -- is what gated the rejection above.
    REQUIRE(validate_reassignment(1, "PETG"));
    if (validate_reassignment(1, "PETG")) {
        mgr.set_preset_material(1, "PETG");
    }
    CHECK(mgr.get_preset_materials()[1] == "PETG");
}

// Mirrors FilamentPanel::reset_presets_to_defaults(), a thin wrapper around
// reset_preset_materials(). MaterialPickerMenu::dispatch_reset() used to
// trigger this transition; in-depth persistence coverage already lives in
// test_material_settings_manager.cpp, so this is a smoke check tying the
// reset transition back to a prior reassignment.
TEST_CASE_METHOD(PresetResetFixture, "reset_preset_materials restores defaults after reassignment",
                 "[material_settings][presets]") {
    auto& mgr = MaterialSettingsManager::instance();
    mgr.set_preset_material(2, "PA");
    REQUIRE(mgr.get_preset_materials()[2] == "PA");

    mgr.reset_preset_materials();

    auto p = mgr.get_preset_materials();
    CHECK(p[0] == "PLA");
    CHECK(p[1] == "PETG");
    CHECK(p[2] == "ABS");
    CHECK(p[3] == "TPU");
}
