// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_ams_mini_status.cpp
 * @brief Tests for the ams_mini_status home dashboard widget (wide spool view)
 */

#include "ui_ams_mini_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "config.h"
#include "grid_layout.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "ams registry: scalable to 4x wide",
                 "[ui][ams_mini][registry]") {
    const PanelWidgetDef* def = find_widget_def("ams");
    REQUIRE(def != nullptr);
    // Spans are authored in tracks, and a track is half a cell, so four cells
    // of width is 4 * TRACKS_PER_CELL. Derived rather than written out so the
    // widget's authored reach stays four cells if the track resolution changes.
    REQUIRE(def->max_colspan == 4 * helix::GridLayout::TRACKS_PER_CELL);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: set_width applies its width argument",
                 "[ui][ams_mini]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 40);
    REQUIRE(w != nullptr);
    // Flush the AmsState auto-sync queued at create time before seeding slots
    // (see the mode test below for why).
    helix::ui::UpdateQueue::instance().drain();

    ui_ams_mini_status_set_slot_count(w, 2);
    ui_ams_mini_status_set_slot_full(w, 0, 0xFF0000, 70, true, "PLA", 70);
    ui_ams_mini_status_set_slot_full(w, 1, 0x00FF00, 40, true, "PETG", 40);

    // Note: set_width does NOT resize the widget object itself (the manager
    // owns the grid cell); width_px is a render hint. The observable effect of
    // the call is therefore the rebuild it triggers, and the width is what
    // picks the render mode. Drive it high->low->high so a set_width that
    // ignored its argument (or only ever built one mode) cannot pass.
    ui_ams_mini_status_set_width(w, 260);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(spools, LV_OBJ_FLAG_HIDDEN));

    // Dropping below w_normal() hides the spool view rather than deleting it (the
    // container is built once and recycled), so assert on the flag.
    ui_ams_mini_status_set_width(w, 130);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(UITest::find_by_name(w, "ams_spools_container") == spools);
    REQUIRE(lv_obj_has_flag(spools, LV_OBJ_FLAG_HIDDEN));

    // Bar mode renders ams_lane_bar widgets: each bar carries the widget's
    // named children, so a regression back to hand-rolled slot columns shows
    // up here as a missing bar_bg.
    lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
    REQUIRE(bars != nullptr);
    lv_obj_t* bar0 = lv_obj_get_child(bars, 0);
    REQUIRE(bar0 != nullptr);
    REQUIRE(UITest::find_by_name(bar0, "bar_bg") != nullptr);

    // ...and coming back up shows it again.
    ui_ams_mini_status_set_width(w, 260);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(lv_obj_has_flag(spools, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: width_px >= w_normal() selects spool mode",
                 "[ui][ams_mini][mode]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    // Flush any initial AmsState auto-sync queued at create time (a prior test in
    // the shard may have left AmsState with slots). The widget auto-binds to
    // AmsState and sync_from_ams_state() would otherwise clobber slot_count below.
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_slot_count(w, 2);
    ui_ams_mini_status_set_slot_full(w, 0, 0xFF0000, 70, true, "PLA", 70);
    ui_ams_mini_status_set_slot_full(w, 1, 0x00FF00, 40, true, "PETG", 40);

    ui_ams_mini_status_set_width(w, 130); // narrow -> bar mode, no spools container
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(UITest::find_by_name(w, "ams_spools_container") == nullptr);

    ui_ams_mini_status_set_width(w, 260); // wide -> spool mode
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(spools, LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: cell has spool + material + pct",
                 "[ui][ams_mini][spool]") {
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain(); // flush stray auto-sync
    ui_ams_mini_status_set_slot_count(w, 1);
    ui_ams_mini_status_set_slot_full(w, 0, 0xFF0000, 73, true, "PLA", 73);
    ui_ams_mini_status_set_width(w, 260);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* mat = UITest::find_by_name(w, "spool_material_0");
    lv_obj_t* pct = UITest::find_by_name(w, "spool_pct_0");
    REQUIRE(mat != nullptr);
    REQUIRE(pct != nullptr);
    REQUIRE(std::string(lv_label_get_text(mat)) == "PLA");
    REQUIRE(std::string(lv_label_get_text(pct)) == "73%");
    REQUIRE(UITest::find_by_name(w, "spool_cell_0") != nullptr);
    lv_obj_delete(w);
}

// An empty lane with no assigned identity names its purpose ("Empty"), matching
// the ams_slot material label — "--" reads as "assigned, unknown material".
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: unassigned empty slot reads Empty",
                 "[ui][ams_mini][spool]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_slot_count(w, 1);
    ui_ams_mini_status_set_slot_full(w, 0, 0x808080, 0, false, "", -1);
    ui_ams_mini_status_set_width(w, 260);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(std::string(lv_label_get_text(UITest::find_by_name(w, "spool_material_0"))) ==
            std::string(lv_tr("Empty")));
    REQUIRE(std::string(lv_label_get_text(UITest::find_by_name(w, "spool_pct_0"))) == "");
    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: all slots present incl multi-unit",
                 "[ui][ams_mini][spool]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_slot_count(w, 12); // > MAX_VISIBLE(8): uncapped vector
    for (int i = 0; i < 12; ++i)
        ui_ams_mini_status_set_slot_full(w, i, 0xFF0000, 50, true, "PLA", 50);
    ui_ams_mini_status_set_width(w, 520);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    REQUIRE(lv_obj_get_child_count(spools) == 12); // single row, all slots; overflow scrolls
    REQUIRE(UITest::find_by_name(w, "spool_cell_11") != nullptr);
    lv_obj_delete(w);
}

// Drives the SPOOL view through the REAL canonical data path: a mock backend is
// installed into AmsState, sync_from_backend() bumps slots_version, the widget's
// observer fires sync_from_ams_state(), and the spool labels must reflect the
// backend's material + remaining percent (not external set_slot_full). This is the
// path that guards bar/spool cache divergence on every AmsState event.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: sync_from_ams_state feeds spool cells",
                 "[ui][ams_mini][spool][sync]") {
    using namespace helix;
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(false);

    auto mock = std::make_unique<helix::AmsBackendMock>(2);
    auto* mock_ptr = mock.get();
    ams.set_backend(std::move(mock));
    mock_ptr->start();

    // Deterministic known data: slot 0 = PETG @ 60%, slot 1 = ABS @ 25%.
    helix::SlotInfo s0 = mock_ptr->get_slot_info(0);
    s0.status = helix::SlotStatus::AVAILABLE;
    s0.material = "PETG";
    s0.total_weight_g = 1000.0f;
    s0.remaining_weight_g = 600.0f; // -> 60%
    helix::test::apply_edit(*mock_ptr, 0, s0);

    helix::SlotInfo s1 = mock_ptr->get_slot_info(1);
    s1.status = helix::SlotStatus::AVAILABLE;
    s1.material = "ABS";
    s1.total_weight_g = 1000.0f;
    s1.remaining_weight_g = 250.0f; // -> 25%
    helix::test::apply_edit(*mock_ptr, 1, s1);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    ui_ams_mini_status_set_width(w, 260);       // spool mode
    helix::ui::UpdateQueue::instance().drain(); // flush create-time auto-sync

    // Canonical path: bump slots_version -> widget observer -> sync_from_ams_state.
    ams.sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* mat0 = UITest::find_by_name(w, "spool_material_0");
    lv_obj_t* pct0 = UITest::find_by_name(w, "spool_pct_0");
    lv_obj_t* mat1 = UITest::find_by_name(w, "spool_material_1");
    lv_obj_t* pct1 = UITest::find_by_name(w, "spool_pct_1");
    REQUIRE(mat0 != nullptr);
    REQUIRE(pct0 != nullptr);
    REQUIRE(mat1 != nullptr);
    REQUIRE(pct1 != nullptr);
    REQUIRE(std::string(lv_label_get_text(mat0)) == "PETG");
    REQUIRE(std::string(lv_label_get_text(pct0)) == "60%");
    REQUIRE(std::string(lv_label_get_text(mat1)) == "ABS");
    REQUIRE(std::string(lv_label_get_text(pct1)) == "25%");

    lv_obj_delete(w);
    mock_ptr->stop();
    ams.clear_backends();
    ams.deinit_subjects();
}

// The loaded (active) lane's badge must use the "success" accent color; non-active
// lanes keep the neutral "ams_badge_bg". Drives the canonical AmsState path: the mock
// starts with slot 0 loaded (current_slot=0), sync_from_backend() propagates that to
// the current_slot subject, and rebuild_spools() colors the active badge.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: active lane badge uses success color",
                 "[ui][ams_mini][spool]") {
    using namespace helix;
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(false);

    auto mock = std::make_unique<helix::AmsBackendMock>(2);
    auto* mock_ptr = mock.get();
    ams.set_backend(std::move(mock));
    mock_ptr->start();

    // Mock initializes slot 0 LOADED with current_slot=0 -> lane 0 is the active lane.
    helix::SlotInfo s0 = mock_ptr->get_slot_info(0);
    s0.material = "PETG";
    s0.total_weight_g = 1000.0f;
    s0.remaining_weight_g = 600.0f;
    helix::test::apply_edit(*mock_ptr, 0, s0);

    helix::SlotInfo s1 = mock_ptr->get_slot_info(1);
    s1.status = helix::SlotStatus::AVAILABLE;
    s1.material = "ABS";
    s1.total_weight_g = 1000.0f;
    s1.remaining_weight_g = 250.0f;
    helix::test::apply_edit(*mock_ptr, 1, s1);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    ui_ams_mini_status_set_width(w, 260); // spool mode
    helix::ui::UpdateQueue::instance().drain();

    ams.sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(lv_subject_get_int(ams.get_current_slot_subject()) == 0);

    lv_obj_t* b0 = UITest::find_by_name(w, "spool_badge_0");
    lv_obj_t* b1 = UITest::find_by_name(w, "spool_badge_1");
    REQUIRE(b0 != nullptr);
    REQUIRE(b1 != nullptr);

    lv_color_t success = theme_manager_get_color("success");
    lv_color_t neutral = theme_manager_get_color("ams_badge_bg");
    REQUIRE(lv_color_eq(lv_obj_get_style_bg_color(b0, LV_PART_MAIN), success)); // active lane
    REQUIRE(lv_color_eq(lv_obj_get_style_bg_color(b1, LV_PART_MAIN), neutral)); // non-active lane

    lv_obj_delete(w);
    mock_ptr->stop();
    ams.clear_backends();
    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: unchanged sync skips cell rebuild",
                 "[ui][ams_mini][spool]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_slot_count(w, 2);
    ui_ams_mini_status_set_slot_full(w, 0, 0xFF0000, 70, true, "PLA", 70);
    ui_ams_mini_status_set_slot_full(w, 1, 0x00FF00, 40, true, "PETG", 40);
    ui_ams_mini_status_set_width(w, 260);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* sc = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(sc != nullptr);
    lv_obj_t* cell0_before = UITest::find_by_name(w, "spool_cell_0");
    REQUIRE(cell0_before != nullptr);

    // Re-feed IDENTICAL data + refresh -> should NOT recreate cells.
    ui_ams_mini_status_set_slot_full(w, 0, 0xFF0000, 70, true, "PLA", 70);
    ui_ams_mini_status_set_slot_full(w, 1, 0x00FF00, 40, true, "PETG", 40);
    ui_ams_mini_status_refresh(w);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(UITest::find_by_name(w, "spool_cell_0") == cell0_before); // same object => no rebuild

    // Change data -> cells rebuilt (pointer differs or content updates).
    ui_ams_mini_status_set_slot_full(w, 0, 0x0000FF, 20, true, "ABS", 20);
    ui_ams_mini_status_refresh(w);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* mat0 = UITest::find_by_name(w, "spool_material_0");
    REQUIRE(mat0 != nullptr);
    REQUIRE(std::string(lv_label_get_text(mat0)) == "ABS");
    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: 2x->1x restores bar view", "[ui][ams_mini][mode]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_slot_count(w, 2);
    ui_ams_mini_status_set_slot_full(w, 0, 0xFF0000, 70, true, "PLA", 70);
    ui_ams_mini_status_set_slot_full(w, 1, 0x00FF00, 40, true, "PETG", 40);

    ui_ams_mini_status_set_width(w, 260); // -> SPOOL (hides bars_container)
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_width(w, 130); // -> back to BAR
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    REQUIRE(lv_obj_has_flag(spools, LV_OBJ_FLAG_HIDDEN)); // spools hidden in bar mode
    lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
    REQUIRE(bars != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(bars, LV_OBJ_FLAG_HIDDEN)); // bars visible again
    lv_obj_delete(w);
}

// Both observers the widget attaches watch AmsState-owned subjects, and
// AmsState::deinit_subjects() frees every observer node on them at once. A
// guard that never learned its subject died stays truthy with a dangling
// lv_observer_t*, so on_delete()'s reset() removes an observer that no longer
// exists. The widget is deleted AFTER the owner's teardown here because that is
// the order a test binary produces: one test tears the singleton down, a later
// one destroys a widget built before it.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: observers survive AmsState's subject teardown",
                 "[ui][ams_mini][teardown][observer]") {
    struct SubjectOwnerRestore {
        ~SubjectOwnerRestore() {
            helix::AmsState::instance().init_subjects(true);
        }
    } restore_ams_subjects;

    helix::AmsState::instance().init_subjects(true);
    REQUIRE(helix::AmsState::instance().get_current_slot_subject() != nullptr);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    REQUIRE(w != nullptr);
    helix::ui::UpdateQueue::instance().drain();

    helix::AmsState::instance().deinit_subjects();

    lv_obj_delete(w);
    helix::ui::UpdateQueue::instance().drain();

    SUCCEED("widget teardown completed without walking freed observer nodes");
}
