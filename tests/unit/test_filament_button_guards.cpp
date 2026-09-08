// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_button_guards.cpp
 * @brief Every reason to disable a filament button survives the others
 *
 * The five filament action buttons each answer to more than one guard.
 * filament_safety_warning_visible is on screen when the nozzle is too cold to
 * move filament; filament_operation_in_progress covers a load or unload already
 * running; filament_load_disabled and filament_unload_disabled carry the
 * per-direction verdict. Any one of them is reason enough for the button to be
 * dead.
 *
 * Two bindings on one LVGL state do not compose. Each observer adds or removes
 * LV_STATE_DISABLED unconditionally from its own subject, so with several of
 * them the last to notify decides alone and the others are silently discarded.
 * A button can then be live while the safety warning is on screen. One
 * expression binding per button is what makes the union hold.
 *
 * These drive the shipped ui_xml/filament_panel.xml, so they fail if the union
 * is ever expressed as separate bindings again, however it is spelled.
 */

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/xml_bind_test_utils.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using helix::test::PanelSubjectOwner;
using helix::test::require_named;
using helix::test::set_xml_subject;

namespace {

using FilamentPanelSubjects = PanelSubjectOwner<FilamentPanel>;

constexpr const char* kAllButtons[] = {"btn_load", "btn_unload", "btn_purge", "btn_extrude",
                                       "btn_retract"};

/// Put every guard in its permissive state. filament_unload_disabled defaults
/// to 1, so "no guard asserted" has to be established explicitly rather than
/// assumed from a freshly built panel.
void clear_all_guards() {
    set_xml_subject("filament_safety_warning_visible", 0);
    set_xml_subject("filament_operation_in_progress", 0);
    set_xml_subject("filament_load_disabled", 0);
    set_xml_subject("filament_unload_disabled", 0);
}

bool is_disabled(lv_obj_t* panel, const char* name) {
    return lv_obj_has_state(require_named(panel, name), LV_STATE_DISABLED);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "a filament safety warning disables every action button",
                 "[ui][filament_panel][bind_state][filament_guard]") {
    FilamentPanelSubjects owner(state());
    REQUIRE(register_component("filament_panel"));

    clear_all_guards();
    // The reason this guard exists: filament must not move while the warning
    // is up, whatever the per-direction verdicts say.
    set_xml_subject("filament_safety_warning_visible", 1);

    lv_obj_t* panel = create_component("filament_panel");
    REQUIRE(panel != nullptr);

    for (const char* name : kAllButtons) {
        INFO("button: " << name);
        CHECK(is_disabled(panel, name));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "an in-flight filament operation disables every action button",
                 "[ui][filament_panel][bind_state][filament_guard]") {
    FilamentPanelSubjects owner(state());
    REQUIRE(register_component("filament_panel"));

    clear_all_guards();
    set_xml_subject("filament_operation_in_progress", 1);

    lv_obj_t* panel = create_component("filament_panel");
    REQUIRE(panel != nullptr);

    for (const char* name : kAllButtons) {
        INFO("button: " << name);
        CHECK(is_disabled(panel, name));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "the per-direction guards reach only their own buttons",
                 "[ui][filament_panel][bind_state][filament_guard]") {
    FilamentPanelSubjects owner(state());
    REQUIRE(register_component("filament_panel"));

    SECTION("load_disabled") {
        clear_all_guards();
        set_xml_subject("filament_load_disabled", 1);
        lv_obj_t* panel = create_component("filament_panel");
        REQUIRE(panel != nullptr);

        CHECK(is_disabled(panel, "btn_load"));
        // Asserting the negatives is what keeps the compound from being
        // widened into an unrelated button's guard.
        CHECK_FALSE(is_disabled(panel, "btn_unload"));
        CHECK_FALSE(is_disabled(panel, "btn_purge"));
        CHECK_FALSE(is_disabled(panel, "btn_extrude"));
        CHECK_FALSE(is_disabled(panel, "btn_retract"));
    }

    SECTION("unload_disabled") {
        clear_all_guards();
        set_xml_subject("filament_unload_disabled", 1);
        lv_obj_t* panel = create_component("filament_panel");
        REQUIRE(panel != nullptr);

        CHECK(is_disabled(panel, "btn_unload"));
        CHECK(is_disabled(panel, "btn_purge"));
        CHECK_FALSE(is_disabled(panel, "btn_load"));
        CHECK_FALSE(is_disabled(panel, "btn_extrude"));
        CHECK_FALSE(is_disabled(panel, "btn_retract"));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "a settling filament operation does not release the safety guard",
                 "[ui][filament_panel][bind_state][filament_guard]") {
    FilamentPanelSubjects owner(state());
    REQUIRE(register_component("filament_panel"));

    clear_all_guards();
    set_xml_subject("filament_safety_warning_visible", 1);
    set_xml_subject("filament_operation_in_progress", 1);

    lv_obj_t* panel = create_component("filament_panel");
    REQUIRE(panel != nullptr);

    // An operation finishing publishes a 0 while the nozzle stays too cold.
    // Being the most recent notifier must not let it decide alone.
    set_xml_subject("filament_operation_in_progress", 0);
    UpdateQueue::instance().drain();

    for (const char* name : kAllButtons) {
        INFO("button: " << name);
        CHECK(is_disabled(panel, name));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "filament buttons are live once no guard is asserted",
                 "[ui][filament_panel][bind_state][filament_guard]") {
    FilamentPanelSubjects owner(state());
    REQUIRE(register_component("filament_panel"));

    clear_all_guards();
    set_xml_subject("filament_safety_warning_visible", 1);

    lv_obj_t* panel = create_component("filament_panel");
    REQUIRE(panel != nullptr);

    // Without this the guard could be a constant rather than a binding, and
    // every assertion above would hold for the wrong reason.
    clear_all_guards();
    UpdateQueue::instance().drain();

    for (const char* name : kAllButtons) {
        INFO("button: " << name);
        CHECK_FALSE(is_disabled(panel, name));
    }
}
