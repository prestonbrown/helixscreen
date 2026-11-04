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

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_mapping_modal_test_access.h"
#include "core/update_queue.h"
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

// The start-macro note is not part of Cancel, but it shares this fixture: the
// modal needs every XML component registered to build at all.
TEST_CASE_METHOD(MappingCancelFixture,
                 "The start macro note shows only while the picks move a tool",
                 "[filament_mapping][modal][note]") {
    auto find = [](FilamentMappingModal& modal, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(modal.dialog(), name);
        REQUIRE(obj != nullptr);
        return obj;
    };
    auto identity = [](FilamentMappingModal& modal) {
        helix::ToolMapping t0;
        t0.tool_index = 0;
        t0.mapped_slot = 0;
        t0.mapped_backend = 0;
        helix::ToolMapping t1 = t0;
        t1.tool_index = 1;
        t1.mapped_slot = 1;
        modal.set_mappings({t0, t1});
    };
    auto note_hidden = [&](FilamentMappingModal& modal) {
        return lv_obj_has_flag(find(modal, "start_macro_note_row"), LV_OBJ_FLAG_HIDDEN);
    };

    {
        FilamentMappingModal modal;
        seed(modal);
        identity(modal);
        modal.set_start_macro_note("Your PRINT_START line passes EXTRUDER1_TEMP");
        REQUIRE(modal.show(test_screen()));
        CHECK(std::string(lv_label_get_text(find(modal, "start_macro_note"))) ==
              "Your PRINT_START line passes EXTRUDER1_TEMP");
        // Identity picks rewrite nothing, so nothing is left behind.
        CHECK(note_hidden(modal));

        // Tool 0 moved onto slot 1: the rewrite now changes a tool number.
        FilamentMappingModalTestAccess::select_slot(modal, 0, {1, 0, false});
        CHECK_FALSE(note_hidden(modal));

        // Back to its own slot.
        FilamentMappingModalTestAccess::select_slot(modal, 0, {0, 0, false});
        CHECK(note_hidden(modal));

        FilamentMappingModalTestAccess::select_slot(modal, 0, {1, 0, false});
        REQUIRE_FALSE(note_hidden(modal));
        FilamentMappingModalTestAccess::cancel(modal);
        helix::ui::UpdateQueue::instance().drain();

        // Cancel restored identity picks, so a reopen starts hidden.
        REQUIRE(modal.show(test_screen()));
        CHECK(note_hidden(modal));
        FilamentMappingModalTestAccess::cancel(modal);
        helix::ui::UpdateQueue::instance().drain();
    }

    // A second caller that never sets a note must not inherit the first one's,
    // even with a move: the subjects behind the label are shared by every instance.
    FilamentMappingModal other;
    seed(other);
    identity(other);
    REQUIRE(other.show(test_screen()));
    FilamentMappingModalTestAccess::select_slot(other, 0, {1, 0, false});
    CHECK(note_hidden(other));
    FilamentMappingModalTestAccess::cancel(other);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(MappingCancelFixture, "A close no button answered reverts the toggle like Cancel",
                 "[filament_mapping][modal][cancel][dismiss]") {
    set_persisted(true);

    FilamentMappingModal modal;
    seed(modal);
    REQUIRE(modal.show(test_screen()));

    FilamentMappingModalTestAccess::toggle(modal, false);
    REQUIRE_FALSE(persisted());

    SECTION("backdrop tap") {
        lv_obj_send_event(modal.backdrop(), LV_EVENT_CLICKED, nullptr);
    }
    SECTION("programmatic hide") {
        modal.hide();
    }
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(modal.is_visible());
    CHECK(persisted());
}
