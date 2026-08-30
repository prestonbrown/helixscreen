// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_endless_status_visibility.cpp
 * @brief Visibility gating of the ams_endless_status component.
 *
 * The component has two call sites with different contracts:
 *  - the context menu always shows the line when there is something truthful
 *    to say (its job is explaining the greyed backup dropdown), and
 *  - the AMS panel passes hide_when_healthy so the healthy sentence ("will
 *    switch") is not permanently on screen; only attention kinds (Off, Unknown,
 *    NeedsPlugin, OnWithoutBackup) surface there.
 *
 * Hidden(0) hides the line everywhere: a printer without the mechanism has no
 * truthful sentence in either site.
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "ams_state.h"
#include "ams_types.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::printer::EndlessSpoolStatusKind;

namespace {

constexpr int to_int(EndlessSpoolStatusKind kind) {
    return static_cast<int>(kind);
}

// The endless subjects live on the AmsState singleton, which registers them
// into the global XML scope separately from the fixture's PrinterState.
lv_subject_t* endless_state_subject() {
    AmsState::instance().init_subjects(true); // before XML creation so bindings resolve
    lv_subject_t* subj = AmsState::instance().get_endless_state_subject();
    REQUIRE(subj != nullptr);
    return subj;
}

void set_endless_state(EndlessSpoolStatusKind kind) {
    lv_subject_set_int(endless_state_subject(), to_int(kind));
    helix::ui::UpdateQueue::instance().drain();
}

lv_obj_t* create_endless_status(XMLTestFixture& fixture, const char** attrs) {
    // register_component() only resolves top-level ui_xml/, and this component
    // lives in ui_xml/components/ — register the file directly.
    lv_result_t reg =
        lv_xml_register_component_from_file("A:ui_xml/components/ams_endless_status.xml");
    REQUIRE(reg == LV_RESULT_OK);
    lv_obj_t* root = fixture.create_component("ams_endless_status", attrs);
    REQUIRE(root != nullptr);
    return root;
}

} // namespace

TEST_CASE("ams_endless_status hides healthy state only when asked", "[ams][xml]") {
    XMLTestFixture fixture;

    // Subject init must precede XML creation so the component's bindings
    // resolve (each SECTION re-runs this body; init is idempotent).
    endless_state_subject();

    SECTION("without hide_when_healthy (context-menu shape)") {
        lv_obj_t* root = create_endless_status(fixture, nullptr);

        set_endless_state(EndlessSpoolStatusKind::Hidden);
        REQUIRE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        set_endless_state(EndlessSpoolStatusKind::On);
        REQUIRE_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        set_endless_state(EndlessSpoolStatusKind::Off);
        REQUIRE_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("with hide_when_healthy (panel shape)") {
        const char* attrs[] = {"hide_when_healthy", "1", nullptr};
        lv_obj_t* root = create_endless_status(fixture, attrs);

        // Healthy: nothing to demand of the user, so the panel hides the line.
        set_endless_state(EndlessSpoolStatusKind::On);
        REQUIRE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        // Attention kinds stay visible - they are the reason the prop exists.
        set_endless_state(EndlessSpoolStatusKind::Off);
        REQUIRE_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        set_endless_state(EndlessSpoolStatusKind::Unknown);
        REQUIRE_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        set_endless_state(EndlessSpoolStatusKind::NeedsPlugin);
        REQUIRE_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        // The honest degraded state (#1391) must NOT hide with the healthy
        // line: it is an attention kind, and hiding it here is exactly the
        // silence the fix exists to remove.
        set_endless_state(EndlessSpoolStatusKind::OnWithoutBackup);
        REQUIRE_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        // No mechanism: hidden with or without the prop.
        set_endless_state(EndlessSpoolStatusKind::Hidden);
        REQUIRE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        // Reactive both directions, not just at creation.
        set_endless_state(EndlessSpoolStatusKind::On);
        REQUIRE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("prop defaults to off") {
        // Empty-string prop = unset = the context-menu contract: visible when On.
        const char* attrs[] = {"hide_when_healthy", "", nullptr};
        lv_obj_t* root = create_endless_status(fixture, attrs);

        set_endless_state(EndlessSpoolStatusKind::On);
        REQUIRE_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
    }
}
