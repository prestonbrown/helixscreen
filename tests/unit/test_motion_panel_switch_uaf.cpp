// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motion_panel_switch_uaf.cpp
 * @brief Reopening Motion after a printer switch must not reuse the old overlay
 *
 * Run with: ./build/bin/helix-tests "[1707]"
 *
 * A printer switch (PrinterSession::switch_printer -> tear_down_printer_state)
 * destroys the panel objects registered with StaticPanelRegistry while their
 * overlay widgets are still on the screen; the widgets are freed a tick later.
 * Re-pushing that orphan fires its jog pad callbacks on the freed MotionPanel
 * (prestonbrown/helixscreen#1707). The reopen must push the live panel's own
 * root.
 *
 * These tests run that sequence through the real caller path: open via
 * OverlayBase::show, run the switch's panel teardown, reopen via the
 * same call. The second case pins the two-caller shape: a second caller
 * converges on the live panel's rebuilt widget, not a third one.
 */

#include "ui_nav_manager.h"
#include "ui_panel_motion.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "static_panel_registry.h"

#include <array>
#include <fstream>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

int g_orphan_deletes = 0;

void count_orphan_delete(lv_event_t* /*e*/) {
    ++g_orphan_deletes;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "reopening motion after a printer switch rebuilds the overlay",
                 "[motion][teardown][uaf][1707]") {
    // Seed NavigationManager the way the app does: the active root panel sits
    // at panel_stack_[0], which push_overlay() reads beneath each overlay.
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    // Open Motion through the real caller path (what MotionWidget::handle_click
    // does).
    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* orphan = cached;
    REQUIRE(orphan != nullptr);
    // The setup reached the hazard: the pushed widget carries a jog pad whose
    // callbacks are bound to the panel that created it.
    REQUIRE(lv_obj_find_by_name(orphan, "jog_pad") != nullptr);

    g_orphan_deletes = 0;
    lv_obj_add_event_cb(orphan, count_orphan_delete, LV_EVENT_DELETE, nullptr);

    // The switch's teardown step that matters here: panel objects die while
    // their widgets stay on the screen until a later tick.
    helix::ui::destroy_static_panels();
    CHECK(g_orphan_deletes == 0);

    // Reopen through the same path a user takes after the switch.
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    // The orphan must not be re-pushed: pushing it is what fires its jog pad
    // callbacks on the freed panel.
    REQUIRE(cached != nullptr);
    CHECK(cached != orphan);
    // What is shown now is the live panel's own widget, with a jog pad bound
    // to that live panel.
    CHECK(get_global_motion_panel().get_root() == cached);
    REQUIRE(lv_obj_find_by_name(cached, "jog_pad") != nullptr);

    // The orphan itself is freed, not left as a hidden screen child forever.
    process_lvgl(100);
    CHECK(g_orphan_deletes == 1);

    // Leave the registry clean for the next test: the reopened panel
    // re-registered itself with StaticPanelRegistry.
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "two cached callers converge on one live overlay after a switch",
                 "[motion][teardown][uaf][1707]") {
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    // Caller A opens; caller B's cache holds the same widget, which is what a
    // second caller that opened pre-switch is left with.
    lv_obj_t* cache_a = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cache_a = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* cache_b = cache_a;

    StaticPanelRegistry::instance().destroy_all();

    // Caller A reopens: the live panel has no widget, so a fresh one is built
    // for it.
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cache_a = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* rebuilt = get_global_motion_panel().get_root();
    REQUIRE(rebuilt != nullptr);
    REQUIRE(cache_a == rebuilt);

    // Caller B reopens still holding the pre-switch widget: it must adopt the
    // live root. Falling into create() here would overwrite the panel's root
    // and leave the rebuilt widget orphaned behind a third one.
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cache_b = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(cache_b == rebuilt);
    CHECK(get_global_motion_panel().get_root() == rebuilt);
    REQUIRE(lv_obj_find_by_name(rebuilt, "jog_pad") != nullptr);

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("motion panel fills the header slot and carries the portrait strip", "[motion][xml]") {
    // Markup-level pins for the Motion panel's coordinate placement: the
    // header slot is filled with the six bound labels in landscape, the
    // portrait branch replaces it with a full-width strip under the header,
    // and the deleted position card must not come back. Routing of slot
    // children is exercised end-to-end by tests/ui geometry (ctl resolves
    // header_pos_* under overlay_header/header_content on a live instance).
    std::ifstream file("ui_xml/motion_panel.xml");
    REQUIRE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string xml = buffer.str();

    CHECK(xml.find("<header_bar-header_content>") != std::string::npos);
    for (const char* axis : {"x", "y", "z"}) {
        std::string letter = std::string("<lv_label name=\"header_pos_") + axis + "_letter\"";
        std::string value =
            std::string("name=\"header_pos_") + axis + "\" bind_text=\"motion_pos_" + axis + "\"";
        CHECK(xml.find(letter) != std::string::npos);
        CHECK(xml.find(value) != std::string::npos);
    }

    // Portrait: the strip under the header binds the same subjects under its
    // own names, one row pairs the jog pad with a tall Z column whose buttons
    // reuse the landscape Z bindings, and a single bottom row carries the jog
    // modes plus the capability-gated leveling buttons.
    CHECK(xml.find("name=\"coord_row\"") != std::string::npos);
    CHECK(xml.find("name=\"row_pos_x\" bind_text=\"motion_pos_x\"") != std::string::npos);
    CHECK(xml.find("name=\"pad_row\"") != std::string::npos);
    CHECK(xml.find("name=\"z_column\"") != std::string::npos);
    CHECK(xml.find("name=\"bottom_row\"") != std::string::npos);
    CHECK(xml.find("name=\"btn_qgl\"") != std::string::npos);
    CHECK(xml.find("icon_position=\"top\"") != std::string::npos);
    const bool axis_label =
        xml.find("name=\"z_axis_label\" width=\"100%\" bind_text=\"motion_z_axis_label\"") !=
        std::string::npos;
    CHECK(axis_label);
    for (const char* btn : {"z_up_large", "z_up_small", "z_down_small", "z_down_large"}) {
        CHECK(xml.find(std::string("name=\"") + btn + "\"") != std::string::npos);
    }
    CHECK(xml.find(std::string("bind_text=\"motion_z_large_label\"")) != std::string::npos);
    CHECK(xml.find(std::string("bind_text=\"motion_z_small_label\"")) != std::string::npos);
    // The full-width Z row and its unit label must not come back: portrait
    // stacks the Z controls beside the pad, not under it.
    CHECK(xml.find("name=\"z_row\"") == std::string::npos);
    CHECK(xml.find("text=\"Z mm\"") == std::string::npos);

    // The title is the same "Motion" key the controls panel button uses.
    const auto title_needle = xml.find("title=\"Motion\"");
    REQUIRE(title_needle != std::string::npos);
    CHECK(xml.substr(title_needle, 40).find("title_tag=\"Motion\"") != std::string::npos);

    // The position card is gone; a resurrection would orphan the deleted
    // subjects (motion_x_homed & co.) that no C++ registers any more.
    CHECK(xml.find("motion_position_card") == std::string::npos);
}
