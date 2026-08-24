// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_panel_material_refresh.cpp
 * @brief Regression test for #981 — stale slot material label after nav-away/back
 *
 * Bug: the slot MATERIAL label has no backing subject (unlike color, which
 * self-refreshes via the color observer / sync_from_backend()). The color
 * observer only re-reads material as a SIDE EFFECT when the color subject
 * actually changes. An in-app material edit that leaves the color unchanged,
 * followed by navigating away and back, therefore left the slot label showing
 * the OLD material until the next slots_version bump (an IFS event).
 *
 * Fix (79caeec2a): AmsPanel::on_activate() now calls refresh_slots(), which
 * unconditionally re-reads each slot's material from the backend. With that line
 * removed, the final assertion below fails: the label stays "PLA".
 *
 * The test builds a real AmsPanel, renders slot 0 = PLA, edits ONLY the
 * material to PETG in the backend (color unchanged, no slots_version bump, so
 * only on_activate() can refresh it), calls on_activate(), and asserts the
 * label updated.
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
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Replicate the production lazy-registration (mirrors
// test_ams_path_canvas_unload_refresh.cpp). Guarded against double-registration.
void register_ams_widgets_and_xml_once() {
    static bool done = false;
    if (done) {
        return;
    }
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    lv_xml_register_component_from_file("A:ui_xml/components/ams_unit_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_environment_indicator.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_panel.xml");
    done = true;
}

// Read slot 0's material-label text. Scoped to slot_grid so we don't pick up the
// "Currently Loaded" card's material text — slot 0 is the first slot widget in
// the grid, so the first material_label under slot_grid is its label.
std::string slot0_material_text(AmsPanel& panel) {
    lv_obj_t* slot_grid = lv_obj_find_by_name(panel.get_panel(), "slot_grid");
    REQUIRE(slot_grid != nullptr);
    lv_obj_t* material_label = lv_obj_find_by_name(slot_grid, "material_label");
    REQUIRE(material_label != nullptr);
    return std::string(lv_label_get_text(material_label));
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "AmsPanel slot material stays correct across nav-away/back (#981)",
                 "[ui_integration][ams][regression][981]") {
    // 1. AFC mock backend (single HUB unit, 4 slots, slot 0 LOADED/present).
    //    Pin slot 0's material to PLA, leaving its color at the mock default.
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_afc_mode(true);
    REQUIRE(mock->start().success());

    {
        SlotInfo info = mock->get_slot_info(0);
        info.material = "PLA";
        REQUIRE(mock->set_slot_info(0, info).success());
    }

    auto* backend = mock.get();
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true); // before XML creation so bindings resolve
    AmsState::instance().sync_from_backend();

    // 2. Build the real AmsPanel exactly as production does.
    register_ams_widgets_and_xml_once();

    AmsPanel panel(state(), &api());
    panel.init_subjects();

    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(panel_obj != nullptr);

    panel.setup(panel_obj, test_screen());
    lv_obj_update_layout(test_screen());
    process_lvgl(50);
    panel.refresh_slots();
    process_lvgl(20);

    // 3. Baseline: the label reads PLA. This also confirms we located slot 0's
    //    material label (only slot 0 carries PLA here).
    REQUIRE(slot0_material_text(panel) == "PLA");

    // 4. #981 root fix: the slot material label is now backed by a per-slot
    //    subject the ams_slot widget observes directly — no imperative re-read.
    //    Nav away, change ONLY the material while the panel is inactive (color
    //    unchanged, exactly the #981 scenario), then nav back. The label must
    //    reflect the new material, driven by the reactive subject rather than an
    //    on_activate() imperative re-read.
    panel.on_deactivate();
    {
        SlotInfo info = backend->get_slot_info(0);
        info.material = "PETG"; // color_rgb left untouched — the #981 case
        REQUIRE(backend->set_slot_info(0, info).success());
    }
    AmsState::instance().sync_from_backend();
    process_lvgl(20);

    panel.on_activate();
    process_lvgl(20);

    // THE REGRESSION ASSERTION: material tracked across nav-away/back without
    // any imperative re-read — the widget observed its material subject.
    REQUIRE(slot0_material_text(panel) == "PETG");

    // Tear down panel UI before the fixture destroys state/subjects.
    panel.clear_panel_reference();
    lv_obj_delete(panel_obj);
    process_lvgl(10);
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "sync_from_backend refreshes a material-only slot change immediately (#1065)",
                 "[ui_integration][ams][regression][1065]") {
    // #1065 (native ZMOD AD5X): a lane whose MATERIAL type changes while its
    // color stays the same failed to refresh the MF-menu material label —
    // color updated, material stuck — until some unrelated action bumped
    // slots_version. Root cause: the sync loop never compared material, so a
    // material-only delta produced no slots_version bump and refresh_slots()
    // never re-read the label. Fix: sync_from_backend()/update_slot() now treat
    // a material delta like a color/status delta and bump slots_version, so the
    // label refreshes on the SAME sync frame — no nav-away/on_activate needed.
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_afc_mode(true);
    REQUIRE(mock->start().success());

    {
        SlotInfo info = mock->get_slot_info(0);
        info.material = "PLA";
        REQUIRE(mock->set_slot_info(0, info).success());
    }

    auto* backend = mock.get();
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    register_ams_widgets_and_xml_once();

    AmsPanel panel(state(), &api());
    panel.init_subjects();

    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(panel_obj != nullptr);

    panel.setup(panel_obj, test_screen());
    lv_obj_update_layout(test_screen());
    process_lvgl(50);
    panel.refresh_slots();
    process_lvgl(20);

    REQUIRE(slot0_material_text(panel) == "PLA");

    // Change ONLY the material (color untouched), then sync. No on_activate,
    // no unrelated slot change — the material delta itself must drive the label.
    {
        SlotInfo info = backend->get_slot_info(0);
        info.material = "PETG"; // color_rgb left untouched
        REQUIRE(backend->set_slot_info(0, info).success());
    }
    AmsState::instance().sync_from_backend();
    process_lvgl(20);

    // THE #1065 ASSERTION: the label tracks the new material on the sync frame.
    REQUIRE(slot0_material_text(panel) == "PETG");

    panel.clear_panel_reference();
    lv_obj_delete(panel_obj);
    process_lvgl(10);
    AmsState::instance().set_backend(nullptr);
}
