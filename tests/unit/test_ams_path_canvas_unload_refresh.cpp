// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_path_canvas_unload_refresh.cpp
 * @brief Regression test for P2 (#1045) — idle-lane unload leaves stale path tube
 *
 * Bug: AmsPanel::refresh_slots() repainted spool colors but did NOT repaint the
 * filament path canvas. After an idle-lane unload (a non-active slot going EMPTY),
 * the canvas kept the now-empty lane's stale lane→hub tube segment.
 *
 * Fix: refresh_slots() now calls update_path_canvas_from_backend() at the end,
 * which re-reads each slot's PathSegment from the backend via
 * get_slot_filament_segment() and repaints the canvas (clearing tubes for
 * slots that now report PathSegment::NONE).
 *
 * This test builds a REAL AmsPanel (the exact production wiring), draws an
 * idle-lane tube, forces that lane EMPTY in the backend, calls the same public
 * refresh_slots() the slots_version observer calls, and asserts the canvas tube
 * is cleared. With the fix line commented out, step 6 fails (tube stays PREP).
 */

#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_panel_ams.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Replicate the production lazy-registration (ensure_ams_widgets_registered() is
// file-static in ui_panel_ams.cpp, so we register the same widgets + XML here).
// Guarded so repeated test runs don't double-register the LVGL XML components.
void register_ams_widgets_and_xml_once() {
    static bool done = false;
    if (done) {
        return;
    }

    // Custom widgets (order matters — dependencies first), as in production.
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();

    // Sidebar callbacks must exist before the XML parser sees them.
    helix::ui::AmsOperationSidebar::register_callbacks_static();

    // XML components the panel depends on.
    lv_xml_register_component_from_file("A:ui_xml/components/ams_unit_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_environment_indicator.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_panel.xml");

    done = true;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "AmsPanel::refresh_slots clears stale path tube after idle-lane unload",
                 "[ui_integration][ams][regression]") {
    // ------------------------------------------------------------------
    // 1. Install a started AFC mock backend BEFORE building the panel.
    //    AFC mode = single HUB unit, 4 slots: slot 0 LOADED (active),
    //    slots 1-3 AVAILABLE. Slot 1 is our idle, non-active, AVAILABLE
    //    lane → get_slot_filament_segment(1) returns PREP (tube drawn).
    // ------------------------------------------------------------------
    constexpr int IDLE_SLOT = 1;

    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_afc_mode(true);
    REQUIRE(mock->start().success()); // AmsError::success() == SUCCESS result
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true); // before XML creation so bindings resolve
    AmsState::instance().sync_from_backend();

    // Sanity: the backend reports a tube for the idle lane before any UI.
    auto* backend = static_cast<AmsBackendMock*>(AmsState::instance().get_backend());
    REQUIRE(backend != nullptr);
    REQUIRE(backend->get_slot_filament_segment(IDLE_SLOT) != PathSegment::NONE);

    // ------------------------------------------------------------------
    // 2. Build the real AmsPanel exactly as production does:
    //    register widgets/XML → create ams_panel XML → init_subjects → setup.
    // ------------------------------------------------------------------
    register_ams_widgets_and_xml_once();

    AmsPanel panel(state(), &api());
    panel.init_subjects();

    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(panel_obj != nullptr);

    panel.setup(panel_obj, test_screen());

    // Lay out + pump so the canvas widget exists and slot grid is measured.
    lv_obj_update_layout(test_screen());
    process_lvgl(50);

    // Populate the canvas from backend state.
    panel.refresh_slots();
    process_lvgl(20);

    // ------------------------------------------------------------------
    // 3. Locate the path canvas; assert the idle lane's tube is drawn.
    //    A null canvas or a NONE segment here means the harness is wrong —
    //    do NOT weaken this; the test must start from a real drawn tube.
    // ------------------------------------------------------------------
    lv_obj_t* canvas = lv_obj_find_by_name(panel.get_panel(), "path_canvas");
    REQUIRE(canvas != nullptr);

    REQUIRE(ui_filament_path_canvas_get_slot_filament(canvas, IDLE_SLOT) !=
            static_cast<int>(PathSegment::NONE));

    // ------------------------------------------------------------------
    // 4. Simulate the idle-lane unload: the lane goes EMPTY in the backend.
    // ------------------------------------------------------------------
    backend->force_slot_status(IDLE_SLOT, SlotStatus::EMPTY);
    AmsState::instance().sync_from_backend();
    AmsState::instance().bump_slots_version();

    // Backend now reports no tube for that lane.
    REQUIRE(backend->get_slot_filament_segment(IDLE_SLOT) == PathSegment::NONE);

    // ------------------------------------------------------------------
    // 5. Trigger the runtime path — the exact method the slots_version
    //    observer invokes on a per-slot status change.
    // ------------------------------------------------------------------
    panel.refresh_slots();
    process_lvgl(20);

    // ------------------------------------------------------------------
    // 6. THE REGRESSION ASSERTION. Pre-fix (without
    //    update_path_canvas_from_backend() in refresh_slots()) the canvas
    //    keeps the stale PREP tube and this fails.
    // ------------------------------------------------------------------
    REQUIRE(ui_filament_path_canvas_get_slot_filament(canvas, IDLE_SLOT) ==
            static_cast<int>(PathSegment::NONE));

    // Tear down panel UI before fixture destroys state/subjects.
    panel.clear_panel_reference();
    lv_obj_delete(panel_obj);
    process_lvgl(10);
    AmsState::instance().set_backend(nullptr);
}
