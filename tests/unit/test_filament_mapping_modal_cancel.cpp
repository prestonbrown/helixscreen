// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_mapping_modal_cancel.cpp
 * @brief Cancel must revert the PERSISTED auto-color preference, not just the
 *        in-memory mappings.
 *
 * Run with: ./build/bin/helix-tests "[filament_mapping][modal][cancel]"
 *
 * The toggle row writes straight through to SettingsManager the moment it
 * changes, because the modal recomputes its rows from the new mode
 * immediately. on_cancel() restored mappings_ from its snapshot and left that
 * write standing — so opening the picker, flipping "Map to closest colors with
 * matching material" to see what it would do, and pressing Cancel silently
 * changed the printer's stored preference for every future print. It affects
 * every backend that can reach the picker.
 *
 * These assert on SettingsManager, NOT on the modal's auto_color_map_ member.
 * That distinction is the whole bug: a revert that only fixed the member would
 * leave the persisted value wrong and reproduce the same blind spot, while
 * every in-memory assertion still passed.
 */

#include "ui_filament_mapping_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_mapping_modal_test_access.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentMappingModal;

namespace {

/// LVGLUITestFixture, not XMLTestFixture: only this one registers ALL XML
/// components, and the modal cannot construct filament_mapping_modal.xml
/// without that — show() returns false and every case fails in setup.
///
/// Restores the persisted preference on the way out — it is process-wide and
/// survives the fixture, so a test that leaves it flipped poisons every later one.
struct MappingCancelFixture : public LVGLUITestFixture {
    MappingCancelFixture() {
        // get_auto_color_map() reads an lv_subject and set_auto_color_map() writes
        // one; without this the setter silently does not stick and every
        // assertion below would compare two constant zeroes and pass.
        helix::SettingsManager::instance().init_subjects();
        prev_ = helix::SettingsManager::instance().get_auto_color_map();
    }

    ~MappingCancelFixture() override {
        helix::SettingsManager::instance().set_auto_color_map(prev_);
        helix::ui::UpdateQueue::instance().drain();
    }

    static bool persisted() {
        return helix::SettingsManager::instance().get_auto_color_map();
    }

    static void set_persisted(bool on) {
        helix::SettingsManager::instance().set_auto_color_map(on);
        REQUIRE(helix::SettingsManager::instance().get_auto_color_map() == on);
    }

    /// A two-tool print against two loaded lanes — enough for the modal to build
    /// rows and for recalculate_mappings() to have something to chew on.
    static void seed(FilamentMappingModal& modal) {
        modal.set_tool_info({{0, 0xE72F1D, "PLA"}, {1, 0x080A0D, "PLA"}});
        modal.set_available_slots(
            {{0, 0, 0x080A0D, "PLA", false, -1}, {1, 0, 0xE72F1D, "PLA", false, -1}});
        modal.set_mappings({});
    }

    bool prev_ = false;
};

} // namespace

TEST_CASE_METHOD(MappingCancelFixture, "Cancel reverts a toggled auto-color preference",
                 "[filament_mapping][modal][cancel]") {
    set_persisted(true);

    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    FilamentMappingModalTestAccess::toggle(modal, false);
    // Load-bearing: proves the toggle really does write through. Without it the
    // final assertion would pass even if the toggle had become a no-op, testing
    // nothing at all.
    REQUIRE_FALSE(persisted());

    FilamentMappingModalTestAccess::cancel(modal);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(persisted()); // reverted — cancel means cancel
}

TEST_CASE_METHOD(MappingCancelFixture, "Cancel reverts a toggle in the other direction too",
                 "[filament_mapping][modal][cancel]") {
    // The mirror case, so the fix cannot be a hardcoded restore-to-true.
    set_persisted(false);

    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    FilamentMappingModalTestAccess::toggle(modal, true);
    REQUIRE(persisted());

    FilamentMappingModalTestAccess::cancel(modal);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(persisted());
}

TEST_CASE_METHOD(MappingCancelFixture, "OK keeps the toggled preference",
                 "[filament_mapping][modal][cancel]") {
    // The other half of the contract: the revert must be scoped to Cancel. A fix
    // that reverted unconditionally would make the toggle permanently unusable,
    // and every Cancel-side assertion above would still pass.
    set_persisted(true);

    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    FilamentMappingModalTestAccess::toggle(modal, false);
    FilamentMappingModalTestAccess::ok(modal);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(persisted()); // the user's choice stands
}

TEST_CASE_METHOD(MappingCancelFixture, "Cancel without touching the toggle changes nothing",
                 "[filament_mapping][modal][cancel]") {
    // Guards the restore itself: snapshotting the wrong value (or snapshotting
    // after the first toggle instead of at show time) would show up here as a
    // cancel that flips a preference the user never touched.
    set_persisted(true);

    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    FilamentMappingModalTestAccess::cancel(modal);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(persisted());
}
