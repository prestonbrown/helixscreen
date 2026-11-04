// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_switcher_widget.cpp
 * @brief Tests for ToolSwitcherWidget registration and metadata
 *
 * Verifies that the tool_switcher widget is registered correctly in the
 * panel widget registry with expected metadata and hardware gating, and that
 * the rail, the face and the sheet it draws keep every target finger-sized and
 * route each tap where it belongs.
 */

#include "ui_modal.h"
#include "ui_swatch.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "../test_helpers/tool_topologies.h"
#include "helix/ui/text_metrics.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "spool_latch_gate.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("ToolSwitcherWidget: widget def exists in registry", "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);

    SECTION("has correct id") {
        REQUIRE(std::strcmp(def->id, "tool_switcher") == 0);
    }

    SECTION("has display name") {
        REQUIRE(def->display_name != nullptr);
        REQUIRE(std::strlen(def->display_name) > 0);
    }

    SECTION("has icon") {
        REQUIRE(def->icon != nullptr);
        REQUIRE(std::strlen(def->icon) > 0);
    }

    SECTION("has description") {
        REQUIRE(def->description != nullptr);
        REQUIRE(std::strlen(def->description) > 0);
    }
}

TEST_CASE("ToolSwitcherWidget: no hardware gate (visible to all printers)",
          "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);
    REQUIRE(def->hardware_gate_subject == nullptr);
}

TEST_CASE("ToolSwitcherWidget: scales from one track to a strip four cells wide",
          "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);

    // Tracks, not cells — a track is half a cell (GridLayout::TRACKS_PER_CELL).
    // Default is one whole cell (compact).
    REQUIRE(def->colspan == 2);
    REQUIRE(def->rowspan == 2);

    // Up to eight tracks wide, so a six-head changer's rail with Park and the
    // feeder fits one row of touch-sized cells; up to 2 cells tall.
    REQUIRE(def->effective_max_colspan() == 8);
    REQUIRE(def->effective_max_rowspan() == 4);

    // Minimum is one track: every box has a mode, so fits_at refuses none
    REQUIRE(def->effective_min_colspan() == 1);
    REQUIRE(def->effective_min_rowspan() == 1);

    REQUIRE(def->is_scalable());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ToolSwitcherWidget: draws its own layout, so it sits outside the tile ladder",
                 "[tool_switcher][panel_widget][1792]") {
    ToolSwitcherWidget widget(state());
    CHECK(widget.tile_sizing() == nullptr);
    CHECK(widget.xml_attrs() == nullptr);
}

TEST_CASE("ToolSwitcherWidget: not enabled by default (requires multi-tool)",
          "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);
    REQUIRE_FALSE(def->default_enabled);
}

// ============================================================================
// The rail, the face and the sheet as drawn (prestonbrown/helixscreen#1784)
// ============================================================================

namespace {

int mode() {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_mode"));
}

bool shown(lv_obj_t* obj) {
    return obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* named(PanelWidgetHarness<ToolSwitcherWidget>& h, const std::string& name) {
    return h.child(name.c_str());
}

void tap(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
}

/// The picker's head cells and its Park button, while it is open.
struct Sheet {
    std::vector<lv_obj_t*> rows;
    lv_obj_t* park = nullptr;
};

Sheet open_sheet(ToolSwitcherWidget& widget) {
    Sheet sheet;
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(widget);
    REQUIRE(list != nullptr);
    lv_obj_update_layout(lv_obj_get_parent(list));
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
        sheet.rows.push_back(lv_obj_get_child(list, static_cast<int32_t>(i)));
    }
    sheet.park = lv_obj_find_by_name(lv_obj_get_parent(list), "tool_park_btn");
    return sheet;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the rail draws a finger-sized segment per head, then Park",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const int floor = resolve_px("touch_target_min", 48);

    // 214x93: four segments fit at the floor, a fifth cell for Park does not.
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));
    lv_obj_update_layout(h.root());
    CHECK(shown(named(h, "tool_switcher_rail_view")));
    CHECK_FALSE(shown(named(h, "tool_switcher_face_view")));
    int last_x = -1;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* seg = named(h, "tool_switcher_seg_" + std::to_string(i));
        REQUIRE(shown(seg));
        CHECK(lv_obj_get_width(seg) >= floor);
        CHECK(lv_obj_get_height(seg) >= floor);
        // Side by side in head order, not stacked.
        CHECK(lv_obj_get_x(seg) > last_x);
        last_x = lv_obj_get_x(seg);
        CHECK_FALSE(shown(named(h, "tool_switcher_rail_park_" + std::to_string(i))));
    }
    CHECK(named(h, "tool_switcher_seg_4") == nullptr);

    // A wide tile has a cell for Park too: after the last head, finger-sized.
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    lv_obj_update_layout(h.root());
    lv_obj_t* park = named(h, "tool_switcher_rail_park_3");
    REQUIRE(shown(park));
    CHECK(lv_obj_get_width(park) >= floor);
    CHECK(lv_obj_get_height(park) >= floor);
    CHECK(lv_obj_get_x(park) > lv_obj_get_x(named(h, "tool_switcher_seg_3")));
    for (int i = 0; i < 3; ++i) {
        CHECK_FALSE(shown(named(h, "tool_switcher_rail_park_" + std::to_string(i))));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a segment tap mounts its head; the mounted one opens the sheet",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));
    REQUIRE(ToolState::instance().active_tool_index() == 0);

    tap(named(h, "tool_switcher_seg_1"));
    CHECK(rig.backend->change_tool_calls == 1);
    CHECK(rig.backend->last_tool == 1);
    CHECK_FALSE(h.widget().has_overlay_open());

    tap(named(h, "tool_switcher_seg_0"));
    CHECK(h.widget().has_overlay_open());
    CHECK(rig.backend->change_tool_calls == 1);
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ToolSwitcherWidget: a rail cell raises its own widget's picker",
                 "[tool_switcher][panel_widget][from_event]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> a(test_screen(), state());
    PanelWidgetHarness<ToolSwitcherWidget> b(test_screen(), state());
    process_lvgl(30);
    resize_content(a, 214, 93);
    resize_content(b, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));

    tap(named(a, "tool_switcher_seg_0"));
    CHECK(a.widget().has_overlay_open());
    CHECK_FALSE(b.widget().has_overlay_open());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the Pair face opens the sheet, and Park beside it needs a head",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(5);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const int floor = resolve_px("touch_target_min", 48);

    // 214x93: five heads do not fit a rail, so the mounted head beside Park.
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Pair));
    lv_obj_update_layout(h.root());
    CHECK_FALSE(shown(named(h, "tool_switcher_rail_view")));
    REQUIRE(shown(named(h, "tool_switcher_face_view")));
    lv_obj_t* face = named(h, "tool_switcher_face");
    lv_obj_t* park_cell = named(h, "tool_switcher_face_park");
    lv_obj_t* park = named(h, "tool_switcher_face_park_btn");
    REQUIRE(shown(face));
    REQUIRE(shown(park_cell));
    CHECK(lv_obj_get_width(face) >= floor);
    CHECK(lv_obj_get_height(face) >= floor);
    CHECK(lv_obj_get_width(park) >= floor);
    CHECK(lv_obj_get_x(park_cell) > lv_obj_get_x(face));

    // A head is mounted, so Park is live; an empty carriage disables it.
    CHECK_FALSE(lv_obj_has_state(park, LV_STATE_DISABLED));
    ToolChangerRig::unmount();
    process_lvgl(30);
    REQUIRE(ToolState::instance().active_tool_index() == -1);
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
    const std::string face_label =
        lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_face_label"));
    CHECK((face_label == lv_tr("Choose head") || face_label == lv_tr("None")));

    tap(face);
    CHECK(h.widget().has_overlay_open());
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the sheet lists every head and Park, each a finger tall",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const int floor = resolve_px("touch_target_min", 48);
    h.resize(2, 2, 160, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    Sheet sheet = open_sheet(h.widget());
    REQUIRE(sheet.rows.size() == 3);
    for (size_t i = 0; i < sheet.rows.size(); ++i) {
        CAPTURE(i);
        REQUIRE(sheet.rows[i] != nullptr);
        CHECK(lv_obj_get_height(sheet.rows[i]) >= floor);
    }
    REQUIRE(shown(sheet.park));
    CHECK(lv_obj_get_height(sheet.park) >= floor);
    CHECK_FALSE(lv_obj_has_state(sheet.park, LV_STATE_DISABLED));

    // Nothing mounted: nothing to park.
    ToolChangerRig::unmount();
    process_lvgl(30);
    REQUIRE(ToolState::instance().active_tool_index() == -1);
    CHECK(lv_obj_has_state(sheet.park, LV_STATE_DISABLED));
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a segment shows its second line only where the cell holds it",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(2);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int lines = lv_font_get_line_height(body) + lv_font_get_line_height(small);
    lv_subject_t* two_line = lv_xml_get_subject(nullptr, "tool_switcher_two_line");
    REQUIRE(two_line != nullptr);
    auto cell_h = [] {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_cell_h"));
    };
    auto band_h = [] {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_band_h"));
    };

    // Tall cells: the label and its sub both show.
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(cell_h() - band_h() >= lines);
    CHECK(lv_subject_get_int(two_line) == 1);
    lv_obj_t* sub = lv_obj_find_by_name(h.child("tool_switcher_seg_0"), "tool_switcher_seg_sub");
    REQUIRE(sub != nullptr);
    CHECK_FALSE(lv_obj_has_flag(sub, LV_OBJ_FLAG_HIDDEN));

    // Cells on the touch floor are too short for two lines over the band.
    const int floor = resolve_px("touch_target_min", 48);
    resize_content(h, 3 * floor + 2 * 2, floor);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    REQUIRE(cell_h() - band_h() < lines);
    CHECK(lv_subject_get_int(two_line) == 0);
    CHECK(lv_obj_has_flag(sub, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a backend that cannot park gets no Park cell anywhere",
                 "[tool_switcher][1784]") {
    SECTION("a rail with room for Park is a rail of heads") {
        ToolChangerRig rig(4);
        rig.backend->can_park = false;
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        resize_content(h, 600, 60);
        process_lvgl(30);
        CHECK(mode() == static_cast<int>(ToolSwitcherMode::Rail));
        lv_obj_update_layout(h.root());
        for (int i = 0; i < 4; ++i) {
            CAPTURE(i);
            CHECK(shown(named(h, "tool_switcher_seg_" + std::to_string(i))));
            CHECK_FALSE(shown(named(h, "tool_switcher_rail_park_" + std::to_string(i))));
        }
        // The heads take the whole width: no cell is held back for Park.
        const int floor = resolve_px("touch_target_min", 48);
        lv_obj_t* last = named(h, "tool_switcher_seg_3");
        CHECK(lv_obj_get_x(last) + lv_obj_get_width(last) + floor > 600);
    }
    SECTION("a box for a face beside Park is the face alone") {
        ToolChangerRig rig(5);
        rig.backend->can_park = false;
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        resize_content(h, 214, 93);
        process_lvgl(30);
        CHECK(mode() == static_cast<int>(ToolSwitcherMode::Single));
        lv_obj_update_layout(h.root());
        CHECK_FALSE(shown(named(h, "tool_switcher_face_park")));
        CHECK(lv_obj_get_width(named(h, "tool_switcher_face")) == 214);
    }
    SECTION("a backend that learns to park re-decides the tile it already sized") {
        ToolChangerRig rig(4);
        rig.backend->can_park = false;
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        resize_content(h, 600, 60);
        process_lvgl(30);
        REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));

        rig.backend->can_park = true;
        AmsState::instance().bump_slots_version();
        process_lvgl(30);
        CHECK(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
        CHECK(shown(named(h, "tool_switcher_rail_park_3")));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a picker cell shows its head's temperature and colour band",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(3);
    // Each head sources its own lane, which is where its colour comes from.
    ToolTopology topo;
    topo.tool_count = 3;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2};
    ToolState::instance().set_ams_topology(topo);
    state().temperature_state().init_extruders({"extruder", "extruder1", "extruder2"});
    process_lvgl(30);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    state().update_from_status(nlohmann::json{{"extruder1", {{"temperature", 215.0}}}});
    process_lvgl(30);
    h.resize(2, 2, 160, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 3);
    lv_obj_t* row1 = lv_obj_get_child(list, 1);
    lv_obj_t* temp = lv_obj_find_by_name(row1, "tool_switcher_seg_sub");
    REQUIRE(temp != nullptr);
    CHECK(std::string(lv_label_get_text(temp)) == "215°C");

    // The band's fill follows the head's own colour subject, shown exactly
    // when the head's filament colour is known.
    int known = 0;
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        const std::string base = "tool_switcher_seg_" + std::to_string(i);
        const int has_color =
            lv_subject_get_int(lv_xml_get_subject(nullptr, (base + "_has_color").c_str()));
        lv_obj_t* strip = lv_obj_find_by_name(lv_obj_get_child(list, i), "tool_switcher_band_fill");
        REQUIRE(strip != nullptr);
        CHECK(lv_obj_has_flag(strip, LV_OBJ_FLAG_HIDDEN) == (has_color == 0));
        if (has_color) {
            ++known;
            CHECK((lv_color_to_u32(lv_obj_get_style_bg_color(strip, LV_PART_MAIN)) & 0xFFFFFF) ==
                  (lv_color_to_u32(lv_subject_get_color(
                       lv_xml_get_subject(nullptr, (base + "_color").c_str()))) &
                   0xFFFFFF));
        }
    }
    CHECK(known > 0);

    lv_obj_update_layout(list);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(lv_obj_get_height(lv_obj_get_child(list, i)) >= resolve_px("touch_target_min", 48));
    }
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture, "tool_switcher: spools on the bed block every head and Park",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    lv_obj_t* seg = named(h, "tool_switcher_seg_1");
    lv_obj_t* park = named(h, "tool_switcher_rail_park_btn_3");
    REQUIRE(shown(named(h, "tool_switcher_rail_park_3")));
    REQUIRE_FALSE(lv_obj_has_state(seg, LV_STATE_DISABLED));
    REQUIRE_FALSE(lv_obj_has_state(park, LV_STATE_DISABLED));

    state().print_state().set_spool_latch(true);
    process_lvgl(30);
    CHECK(lv_obj_has_state(seg, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(named(h, "tool_switcher_face"), LV_STATE_DISABLED));

    // A tap that reaches them anyway is refused, and says why.
    tap(seg);
    tap(park);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(rig.backend->park_calls == 0);
    REQUIRE(rig.warnings.size() == 2);
    for (const std::string& w : rig.warnings) {
        CHECK(w.find(lv_tr(spool_latch_message())) != std::string::npos);
    }

    // The sheet's head rows too.
    ToolSwitcherTestAccess::show_picker(h.widget());
    Sheet sheet = open_sheet(h.widget());
    REQUIRE_FALSE(sheet.rows.empty());
    for (lv_obj_t* row : sheet.rows) {
        CHECK(lv_obj_has_state(row, LV_STATE_DISABLED));
    }
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);

    state().print_state().set_spool_latch(false);
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a paused print still lets a head change through its confirmation",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    lv_obj_t* seg = named(h, "tool_switcher_seg_1");
    lv_obj_t* park = named(h, "tool_switcher_rail_park_btn_3");
    helix::test::set_wire_state(state(), PrintJobState::PAUSED);
    process_lvgl(30);
    REQUIRE(ModalStack::instance().empty());

    SECTION("a head is offered, behind the paused-change confirmation") {
        CHECK_FALSE(lv_obj_has_state(seg, LV_STATE_DISABLED));
        CHECK_FALSE(lv_obj_has_state(named(h, "tool_switcher_face"), LV_STATE_DISABLED));
        tap(seg);
        process_lvgl(30);
        CHECK_FALSE(ModalStack::instance().empty());
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(rig.warnings.empty());
    }
    SECTION("Park is not: resuming with the head docked prints nothing") {
        CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
        tap(park);
        CHECK(rig.backend->park_calls == 0);
        REQUIRE(rig.warnings.size() == 1);
        CHECK(rig.warnings[0].find("Cannot park while a print is active") != std::string::npos);
    }
    SECTION("spools on the bed refuse the head change as well") {
        state().print_state().set_spool_latch(true);
        process_lvgl(30);
        CHECK(lv_obj_has_state(seg, LV_STATE_DISABLED));
        tap(seg);
        process_lvgl(30);
        CHECK(ModalStack::instance().empty());
        CHECK(rig.backend->change_tool_calls == 0);
        REQUIRE(rig.warnings.size() == 1);
        CHECK(rig.warnings[0].find(lv_tr(spool_latch_message())) != std::string::npos);
        state().print_state().set_spool_latch(false);
        process_lvgl(30);
    }
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    process_lvgl(30);
}

namespace {

/// Each head sources its own lane, which is where its colour comes from.
void map_heads_to_lanes(int heads) {
    ToolTopology topo;
    topo.tool_count = heads;
    topo.active_tool = 0;
    for (int i = 0; i < heads; ++i) {
        topo.tool_to_slot.push_back(i);
    }
    ToolState::instance().set_ams_topology(topo);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the picker reaches every head where the rail cannot",
                 "[tool_switcher][1784]") {
    constexpr int kHeads = ToolSwitcherWidget::kMaxRailHeads + 2;
    ToolChangerRig rig(kHeads);
    map_heads_to_lanes(kHeads);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(2, 2, 160, 60);
    process_lvgl(30);

    // More heads than the rail draws: the face, and no rail cells behind it.
    CHECK(mode() >= static_cast<int>(ToolSwitcherMode::Pair));
    lv_obj_t* rail = named(h, "tool_switcher_rail");
    REQUIRE(rail != nullptr);
    CHECK(lv_obj_get_child_count(rail) == 0);

    // The picker has a cell for every head, the last one named for its head.
    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    CHECK(lv_obj_get_child_count(list) == static_cast<uint32_t>(kHeads));
    const std::string last = "tool_picker_cell_" + std::to_string(kHeads - 1);
    CHECK(lv_obj_find_by_name(list, last.c_str()) != nullptr);
    // Its G-code name is on one of its two lines, whichever form they read in.
    const std::string seg = "tool_switcher_seg_" + std::to_string(kHeads - 1);
    lv_subject_t* label = lv_xml_get_subject(nullptr, (seg + "_pick").c_str());
    lv_subject_t* sub = lv_xml_get_subject(nullptr, (seg + "_pick_sub").c_str());
    REQUIRE(label != nullptr);
    REQUIRE(sub != nullptr);
    const std::string text =
        std::string(lv_subject_get_string(label)) + " " + lv_subject_get_string(sub);
    CHECK(text.find(ToolState::instance().tools()[kHeads - 1].name) != std::string::npos);
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: every surface sizes its colour band by tool_switcher_band()",
                 "[tool_switcher][band]") {
    lv_display_t* disp = lv_display_get_default();
    for (const auto& [w, hpx] : {std::pair{480, 272}, std::pair{800, 480}}) {
        CAPTURE(w, hpx);
        ScopedResolution res(disp, w, hpx);
        theme_manager_refresh_layout_constants(disp);
        ToolChangerRig rig(4);
        map_heads_to_lanes(4);
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        const int small_px = lv_font_get_line_height(theme_manager_get_font("font_small"));
        const int pad = resolve_px("space_xxs", 2);
        auto check_band = [&](lv_obj_t* cell) {
            REQUIRE(cell != nullptr);
            const int cell_h = lv_obj_get_height(cell);
            lv_obj_t* band = lv_obj_find_by_name(cell, "tool_switcher_band_fill");
            REQUIRE(band != nullptr);
            const ToolSwitcherBand want = tool_switcher_band(cell_h, small_px, pad);
            CAPTURE(cell_h, want.px, want.labelled);
            CHECK(lv_obj_get_height(lv_obj_get_parent(band)) == want.px);
            CHECK(want.px >= tool_switcher_strip_px(cell_h));
            // The material shows on a labelled band with a known colour.
            lv_obj_t* text = lv_obj_find_by_name(cell, "tool_switcher_band_text");
            REQUIRE(text != nullptr);
            if (!lv_obj_has_flag(band, LV_OBJ_FLAG_HIDDEN)) {
                CHECK(lv_obj_has_flag(text, LV_OBJ_FLAG_HIDDEN) == !want.labelled);
            } else {
                CHECK(lv_obj_has_flag(text, LV_OBJ_FLAG_HIDDEN));
            }
        };

        // A rail of tall segments, then one on the touch floor.
        for (const auto& [cw, ch] : {std::pair{214, 93}, std::pair{4 * 48 + 3 * 2, 48}}) {
            resize_content(h, cw, ch);
            process_lvgl(30);
            REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));
            lv_obj_update_layout(h.root());
            check_band(named(h, "tool_switcher_seg_1"));
        }

        // The face, alone in its box.
        resize_content(h, 90, 90);
        process_lvgl(30);
        REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Single));
        lv_obj_update_layout(h.root());
        check_band(named(h, "tool_switcher_face"));

        // A picker cell, never under the touch floor.
        ToolSwitcherTestAccess::show_picker(h.widget());
        lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
        REQUIRE(list != nullptr);
        lv_obj_update_layout(lv_obj_get_parent(list));
        lv_obj_t* cell = lv_obj_get_child(list, 1);
        REQUIRE(lv_obj_get_height(cell) >= resolve_px("touch_target_min", 48));
        check_band(cell);
        ToolSwitcherTestAccess::hide_picker(h.widget());
        process_lvgl(30);
    }
    theme_manager_refresh_layout_constants(disp);
}

namespace {

lv_area_t coords_of(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a stacked Pair puts a finger-sized Park under the face",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(5);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const int floor = resolve_px("touch_target_min", 48);

    // 95x211 is the 1x2 content box at 800x480: too narrow for a rail of five.
    resize_content(h, 95, 211);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Pair));
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_rows")) == 2);
    lv_obj_update_layout(h.root());

    const lv_area_t tile = coords_of(h.root());
    const lv_area_t face = coords_of(named(h, "tool_switcher_face"));
    lv_obj_t* park_btn = named(h, "tool_switcher_face_park_btn");
    REQUIRE(shown(named(h, "tool_switcher_face_park")));
    const lv_area_t park = coords_of(park_btn);
    CAPTURE(face.x1, face.y1, face.x2, face.y2, park.x1, park.y1, park.x2, park.y2);
    CHECK(lv_obj_get_width(park_btn) >= floor);
    CHECK(lv_obj_get_height(park_btn) >= floor);
    CHECK(park.x1 >= tile.x1);
    CHECK(park.x2 <= tile.x2);
    CHECK(park.y2 <= tile.y2);
    CHECK(park.y1 > face.y2);
    // Face and Park share the column's width.
    CHECK(lv_obj_get_width(named(h, "tool_switcher_face")) == lv_obj_get_width(park_btn));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the face and Park fill the same cross axis whatever box the "
                 "tile was told",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(5);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Pair));

    // The grid can report a box a pixel smaller than the tile it lays out.
    const auto [w, hpx] = tile_for_content(214, 93);
    h.widget().notify_size_changed(1, 1, w - 1, hpx - 1);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Pair));
    lv_obj_update_layout(h.root());
    CHECK(lv_obj_get_height(named(h, "tool_switcher_face")) ==
          lv_obj_get_height(named(h, "tool_switcher_face_park_btn")));
    CHECK(lv_obj_get_height(named(h, "tool_switcher_face")) == lv_obj_get_content_height(h.root()));
    // Stacked, the cross axis is the width.
    resize_content(h, 95, 211);
    process_lvgl(30);
    const auto [sw, sh] = tile_for_content(95, 211);
    h.widget().notify_size_changed(1, 1, sw - 1, sh - 1);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Pair));
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_rows")) == 2);
    lv_obj_update_layout(h.root());
    CHECK(lv_obj_get_width(named(h, "tool_switcher_face")) ==
          lv_obj_get_width(named(h, "tool_switcher_face_park_btn")));
    CHECK(lv_obj_get_width(named(h, "tool_switcher_face")) == lv_obj_get_content_width(h.root()));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a picker cell's colour band sits below its temperature",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(3);
    map_heads_to_lanes(3);
    state().temperature_state().init_extruders({"extruder", "extruder1", "extruder2"});
    process_lvgl(30);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    state().update_from_status(nlohmann::json{{"extruder1", {{"temperature", 150.0}}}});
    process_lvgl(30);
    h.resize(2, 2, 160, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    lv_obj_update_layout(lv_obj_get_parent(list));
    lv_obj_t* row = lv_obj_get_child(list, 1);
    lv_obj_t* temp = lv_obj_find_by_name(row, "tool_switcher_seg_sub");
    lv_obj_t* strip = lv_obj_find_by_name(row, "tool_switcher_band_fill");
    lv_obj_t* btn = row;
    REQUIRE(std::string(lv_label_get_text(temp)) == "150°C");
    REQUIRE_FALSE(lv_obj_has_flag(strip, LV_OBJ_FLAG_HIDDEN));
    const lv_area_t t = coords_of(temp);
    const lv_area_t s = coords_of(strip);
    const lv_area_t b = coords_of(btn);
    CAPTURE(t.y1, t.y2, s.y1, s.y2, b.y1, b.y2);
    CHECK(s.y1 > t.y2);
    CHECK(s.y2 <= b.y2);
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture, "tool_switcher: the empty-carriage detail line fits its block",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(5);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    ToolChangerRig::unmount();
    process_lvgl(30);
    REQUIRE(ToolState::instance().active_tool_index() == -1);

    // The 2x2 content box at 800x480.
    resize_content(h, 214, 211);
    process_lvgl(30);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_detail")) == 1);
    const lv_font_t* body = theme_manager_get_font("font_body");
    REQUIRE(ui::text_width(lv_tr("Carriage empty · tap a head"), body) > 214);
    const std::string line =
        lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_detail_0_text"));
    CHECK(line == lv_tr("Carriage empty"));
    CHECK(ui::text_width(line.c_str(), body) <= 214);

    // Where the whole line fits, it is kept.
    resize_content(h, 600, 211);
    process_lvgl(30);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(
              nullptr, "tool_switcher_detail_0_text"))) == lv_tr("Carriage empty · tap a head"));
}

namespace {

std::string published(const std::string& name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name.c_str());
    REQUIRE(s != nullptr);
    return lv_subject_get_string(s);
}

int published_int(const std::string& name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name.c_str());
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a temperature tick republishes the temperatures alone",
                 "[tool_switcher][1784]") {
    // Extruder temperatures notify several times a second per head; reading
    // every head's slot on each one holds the backend's mutex for nothing.
    ToolChangerRig rig(3);
    map_heads_to_lanes(3);
    state().temperature_state().init_extruders({"extruder", "extruder1", "extruder2"});
    process_lvgl(30);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);

    // A rail with its detail block.
    resize_content(h, 214, 211);
    process_lvgl(30);
    REQUIRE(published_int("tool_switcher_detail") == 1);
    REQUIRE(published_int("tool_switcher_detail_rows") >= 2);
    const std::string label = published("tool_switcher_seg_1_label");
    const int reads = rig.backend->slot_info_calls;

    state().update_from_status(nlohmann::json{{"extruder1", {{"temperature", 150.0}}}});
    process_lvgl(30);
    CHECK(published("tool_switcher_seg_1_temp") == "150°C");
    if (published_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Code)) {
        CHECK(published("tool_switcher_seg_1_sub") == "150°C");
    }
    // Head 0 is mounted, so head 1 is the second detail row.
    CHECK(published("tool_switcher_detail_1_text").find("150°C") != std::string::npos);
    CHECK(published_int("tool_switcher_detail_1_hot") == 1);
    CHECK(published("tool_switcher_seg_1_label") == label);
    CHECK(rig.backend->slot_info_calls == reads);

    // The face's second line is the mounted head's temperature.
    h.resize(2, 2, 60, 60);
    process_lvgl(30);
    REQUIRE(published_int("tool_switcher_mode") == static_cast<int>(ToolSwitcherMode::Single));
    const int face_reads = rig.backend->slot_info_calls;
    state().update_from_status(nlohmann::json{{"extruder", {{"temperature", 200.0}}}});
    process_lvgl(30);
    CHECK(published("tool_switcher_face_sub") == "200°C");
    CHECK(rig.backend->slot_info_calls == face_reads);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: with no tools the face names no head and is not filled",
                 "[tool_switcher][1784]") {
    // Before discovery, or while disconnected.
    ToolState::instance().deinit_subjects();
    ToolState::instance().init_subjects(true);
    REQUIRE(ToolState::instance().tool_count() == 0);
    register_tool_switcher_widget();
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(2, 2, 60, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Single));
    lv_obj_t* face = named(h, "tool_switcher_face");
    REQUIRE(face != nullptr);
    const uint32_t primary = lv_color_to_u32(theme_manager_get_color("primary"));

    CHECK(published("tool_switcher_face_label").empty());
    CHECK(lv_color_to_u32(lv_obj_get_style_bg_color(face, LV_PART_MAIN)) != primary);

    // A mounted head fills the face, so the check above can see a fill.
    ToolTopology topo;
    topo.tool_count = 2;
    topo.active_tool = 0;
    ToolState::instance().set_ams_topology(topo);
    process_lvgl(30);
    CHECK_FALSE(published("tool_switcher_face_label").empty());
    CHECK(lv_color_to_u32(lv_obj_get_style_bg_color(face, LV_PART_MAIN)) == primary);

    ToolState::instance().deinit_subjects();
}

// ============================================================================
// The feeder and the changer fault
// ============================================================================

namespace {

/// An eight-track strip one cell tall at 800x480, which holds eight cells of
/// 54 px: six heads, Park and the feeder.
constexpr int kStripW = 214 + 2 * 119;
constexpr int kStripH = 93;

int subject_value(const char* name) {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a backend with a feeder gets a feeder cell after Park",
                 "[tool_switcher][feeder]") {
    ToolChangerRig rig(6);
    const bool feeder = GENERATE(true, false);
    CAPTURE(feeder);
    rig.backend->has_feeder = feeder;
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, kStripW, kStripH);
    process_lvgl(30);

    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    CHECK(subject_value("tool_switcher_cols") == (feeder ? 8 : 7));
    CHECK(subject_value("tool_switcher_cell_w") >= resolve_px("touch_target_min", 48));
    CHECK(shown(named(h, "tool_switcher_rail_park_5")));
    CHECK(shown(named(h, "tool_switcher_rail_feeder_5")) == feeder);
    // Only after the last head.
    CHECK_FALSE(shown(named(h, "tool_switcher_rail_feeder_4")));

    lv_obj_update_layout(h.root());
    if (feeder) {
        lv_obj_t* cell = named(h, "tool_switcher_rail_feeder_btn_5");
        REQUIRE(cell != nullptr);
        CHECK(lv_obj_get_width(cell) >= resolve_px("touch_target_min", 48));
        CHECK(lv_obj_get_height(cell) >= resolve_px("touch_target_min", 48));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a feeder tap opens a closed feeder and closes an open one",
                 "[tool_switcher][feeder]") {
    ToolChangerRig rig(6);
    rig.backend->has_feeder = true;
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, kStripW, kStripH);
    process_lvgl(30);
    lv_obj_t* cell = named(h, "tool_switcher_rail_feeder_btn_5");
    REQUIRE(shown(named(h, "tool_switcher_rail_feeder_5")));

    tap(cell);
    rig.backend->feeder_is_open = true;
    tap(cell);
    CHECK(rig.backend->feeder_requests == std::vector<bool>{true, false});
    CHECK(rig.warnings.empty());

    // The cell shows the state AmsState publishes: the open lock and the
    // filled look while open, the closed lock while closed.
    lv_subject_t* open = AmsState::instance().get_feeder_open_subject();
    lv_obj_t* closed_icon = lv_obj_find_by_name(cell, "tool_switcher_feeder_icon_closed");
    lv_obj_t* open_icon = lv_obj_find_by_name(cell, "tool_switcher_feeder_icon_open");
    REQUIRE(closed_icon != nullptr);
    REQUIRE(open_icon != nullptr);
    const uint32_t primary = lv_color_to_u32(theme_manager_get_color("primary"));
    lv_subject_set_int(open, 0);
    process_lvgl(30);
    CHECK(shown(closed_icon));
    CHECK_FALSE(shown(open_icon));
    CHECK(lv_obj_get_style_bg_opa(cell, LV_PART_MAIN) == LV_OPA_TRANSP);
    lv_subject_set_int(open, 1);
    process_lvgl(30);
    CHECK_FALSE(shown(closed_icon));
    CHECK(shown(open_icon));
    CHECK(lv_obj_get_style_bg_opa(cell, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_color_to_u32(lv_obj_get_style_bg_color(cell, LV_PART_MAIN)) == primary);
    lv_subject_set_int(open, 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the feeder is refused while a print holds the machine",
                 "[tool_switcher][feeder]") {
    ToolChangerRig rig(6);
    rig.backend->has_feeder = true;
    helix::test::set_wire_state(state(), PrintJobState::PRINTING);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, kStripW, kStripH);
    process_lvgl(30);
    lv_obj_t* cell = named(h, "tool_switcher_rail_feeder_btn_5");
    REQUIRE(cell != nullptr);
    CHECK(lv_obj_has_state(cell, LV_STATE_DISABLED));

    // A tap that lands before the binding catches up is refused with a reason.
    tap(cell);
    CHECK(rig.backend->feeder_requests.empty());
    CHECK(rig.warnings.size() == 1);
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the picker footer offers the feeder only where there is one",
                 "[tool_switcher][feeder]") {
    ToolChangerRig rig(6);
    const bool feeder = GENERATE(true, false);
    CAPTURE(feeder);
    rig.backend->has_feeder = feeder;
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(1, 1, 60, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    lv_obj_t* card = lv_obj_get_parent(list);
    lv_obj_update_layout(card);
    lv_obj_t* park = lv_obj_find_by_name(card, "tool_park_btn");
    lv_obj_t* feeder_btn = lv_obj_find_by_name(card, "tool_feeder_btn");
    REQUIRE(park != nullptr);
    REQUIRE(feeder_btn != nullptr);
    CHECK(shown(park));
    CHECK(shown(feeder_btn) == feeder);
    const int floor = resolve_px("touch_target_min", 48);
    CHECK(lv_obj_get_height(park) >= floor);
    if (feeder) {
        CHECK(lv_obj_get_height(feeder_btn) >= floor);
        // The footer's buttons split the grid's width.
        CHECK(std::abs(lv_obj_get_width(park) - lv_obj_get_width(feeder_btn)) <= 1);
    }
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture, "tool_switcher: the rail feeder cell never truncates its label",
                 "[tool_switcher][feeder]") {
    lv_display_t* disp = lv_display_get_default();
    for (const auto& [w, hpx] : {std::pair{800, 480}, std::pair{1024, 600}}) {
        CAPTURE(w, hpx);
        ScopedResolution res(disp, w, hpx);
        theme_manager_refresh_layout_constants(disp);
        ToolChangerRig rig(4);
        rig.backend->has_feeder = true;
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);

        // Four heads, Park and the feeder in one row, from the touch floor up.
        const int floor = resolve_px("touch_target_min", 48);
        std::vector<int> forms_seen;
        for (int cell = floor; cell <= 3 * floor; cell += 3) {
            CAPTURE(cell);
            resize_content(h, 6 * cell + 5 * 2, 84);
            process_lvgl(30);
            REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
            REQUIRE(subject_value("tool_switcher_cols") == 6);
            lv_obj_update_layout(h.root());
            const int form = subject_value("tool_switcher_feeder_label");
            forms_seen.push_back(form);
            lv_obj_t* btn = named(h, "tool_switcher_rail_feeder_btn_3");
            REQUIRE(btn != nullptr);
            int labels_shown = 0;
            for (const char* name :
                 {"tool_switcher_feeder_label", "tool_switcher_feeder_open_label",
                  "tool_switcher_feeder_close_label"}) {
                lv_obj_t* label = lv_obj_find_by_name(btn, name);
                REQUIRE(label != nullptr);
                if (!shown(label))
                    continue;
                ++labels_shown;
                // Truncation rewrites the label's text with dots.
                const std::string text = lv_label_get_text(label);
                CAPTURE(name, text);
                CHECK(text.find("...") == std::string::npos);
                CHECK(
                    ui::text_width(text.c_str(), lv_obj_get_style_text_font(label, LV_PART_MAIN)) <=
                    lv_obj_get_content_width(label));
            }
            const bool labelled = subject_value("tool_switcher_two_line") == 1 &&
                                  form != static_cast<int>(FeederLabelForm::Icon);
            CHECK(labels_shown == (labelled ? 1 : 0));
        }
        // The sweep crosses from the lock alone to the action's name.
        CHECK(forms_seen.front() < static_cast<int>(FeederLabelForm::Long));
        CHECK(forms_seen.back() == static_cast<int>(FeederLabelForm::Long));
    }
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "tool_switcher: six heads lay out three columns of square cells that do not scroll",
    "[tool_switcher][picker]") {
    ToolChangerRig rig(6);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(1, 1, 60, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    lv_obj_update_layout(lv_obj_get_parent(list));
    REQUIRE(lv_obj_get_child_count(list) == 6);
    const int floor = resolve_px("touch_target_min", 48);
    lv_area_t first;
    lv_obj_get_coords(lv_obj_get_child(list, 0), &first);
    for (uint32_t i = 0; i < 6; ++i) {
        lv_obj_t* cell = lv_obj_get_child(list, static_cast<int32_t>(i));
        CAPTURE(i);
        CHECK(lv_obj_get_width(cell) >= floor);
        CHECK(lv_obj_get_width(cell) == lv_obj_get_height(cell));
        lv_area_t a;
        lv_obj_get_coords(cell, &a);
        // Row i / 3: the fourth cell starts the second row.
        CHECK((a.y1 > first.y2) == (i >= 3));
    }
    CHECK(lv_obj_get_scroll_bottom(list) <= 0);

    // The card stays on screen, however wide the grid made it.
    lv_area_t card;
    lv_obj_get_coords(lv_obj_get_parent(list), &card);
    CHECK(card.x1 >= 0);
    CHECK(card.x2 < lv_display_get_horizontal_resolution(lv_display_get_default()));
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a changer fault marks every head but the mounted one",
                 "[tool_switcher][band]") {
    ToolChangerRig rig(3);
    // A machine whose heads dock, against a shared toolhead whose filament
    // system errors: only the first has heads that can be out of place.
    const bool docked_heads = GENERATE(true, false);
    CAPTURE(docked_heads);
    rig.backend->set_tool_changer_mode(docked_heads);
    REQUIRE(rig.backend->carriage_can_be_empty() == docked_heads);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(ToolState::instance().active_tool_index() == 0);
    auto state_of = [](int i) {
        return subject_value(("tool_switcher_seg_" + std::to_string(i) + "_state").c_str());
    };
    using S = ToolSwitcherCellState;
    CHECK(state_of(0) == static_cast<int>(S::Mounted));
    CHECK(state_of(1) == static_cast<int>(S::Idle));

    lv_subject_set_int(AmsState::instance().get_ams_action_subject(),
                       static_cast<int>(AmsAction::ERROR));
    process_lvgl(30);
    const S others = docked_heads ? S::Fault : S::Idle;
    CHECK(state_of(0) == static_cast<int>(S::Mounted));
    CHECK(state_of(1) == static_cast<int>(others));
    CHECK(state_of(2) == static_cast<int>(others));

    // The faulted cell fills its text area red and rules it off from the band
    // in the cell's own background, so a red band still reads.
    lv_obj_t* area = lv_obj_find_by_name(named(h, "tool_switcher_seg_1"), "tool_switcher_seg_text");
    REQUIRE(area != nullptr);
    const uint32_t danger = lv_color_to_u32(theme_manager_get_color("danger"));
    const bool red = lv_obj_get_style_bg_opa(area, LV_PART_MAIN) == LV_OPA_COVER &&
                     lv_color_to_u32(lv_obj_get_style_bg_color(area, LV_PART_MAIN)) == danger;
    CHECK(red == docked_heads);
    if (docked_heads) {
        CHECK(lv_obj_get_style_border_width(area, LV_PART_MAIN) == 2);
        CHECK(lv_obj_get_style_border_side(area, LV_PART_MAIN) == LV_BORDER_SIDE_BOTTOM);
        CHECK(lv_color_to_u32(lv_obj_get_style_border_color(area, LV_PART_MAIN)) ==
              lv_color_to_u32(theme_manager_get_color("elevated_bg")));
    }
    lv_obj_t* mounted_area =
        lv_obj_find_by_name(named(h, "tool_switcher_seg_0"), "tool_switcher_seg_text");
    CHECK(lv_obj_get_style_bg_opa(mounted_area, LV_PART_MAIN) == LV_OPA_TRANSP);

    lv_subject_set_int(AmsState::instance().get_ams_action_subject(),
                       static_cast<int>(AmsAction::IDLE));
    process_lvgl(30);
    CHECK(state_of(1) == static_cast<int>(S::Idle));
}

// ============================================================================
// Physical heads only: lanes are chosen in the filament panel
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a hub-fed rig draws one segment per physical head, not per lane",
                 "[tool_switcher][physical-heads]") {
    ToolChangerRig rig(7, 18);
    ToolState::instance().set_ams_topology(helix::test::make_hub_topology());
    REQUIRE(ToolState::instance().tool_count() == 18);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 600, 60);
    process_lvgl(30);

    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_heads")) == 7);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    lv_obj_update_layout(h.root());
    CHECK(shown(named(h, "tool_switcher_seg_6")));
    CHECK(named(h, "tool_switcher_seg_7") == nullptr);
    // The sixth head is numbered as a head and named by the lane a tap sends.
    const std::string seg5 =
        std::string(
            lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_seg_5_label"))) +
        "|" + lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_seg_5_sub"));
    CAPTURE(seg5);
    CHECK(seg5.find("T8") != std::string::npos);
    CHECK(seg5.find('9') == std::string::npos);

    SECTION("a head is mounted by its own lowest lane") {
        tap(named(h, "tool_switcher_seg_5"));
        process_lvgl(30);
        CHECK(rig.backend->change_tool_calls == 1);
        CHECK(rig.backend->last_tool == 8);
    }
    SECTION("a parked head is mounted by the lane already seated in it") {
        // A seat moving changes that lane's loaded state too, so the same sync
        // that pushes the topology bumps the slot version.
        ToolState::instance().set_ams_topology(helix::test::make_hub_topology(0, {0, 10}));
        AmsState::instance().bump_slots_version();
        process_lvgl(30);
        const std::string label =
            std::string(
                lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_seg_5_label"))) +
            "|" + lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_seg_5_sub"));
        CAPTURE(label);
        CHECK(label.find("T10") != std::string::npos);
        tap(named(h, "tool_switcher_seg_5"));
        process_lvgl(30);
        CHECK(rig.backend->change_tool_calls == 1);
        CHECK(rig.backend->last_tool == 10);
    }
    SECTION("a rebuilt tool list names the seated lane from its first draw") {
        // Two shape changes, the second carrying a seat. No slot bump: the
        // tool-list version alone has to draw the head right.
        ToolState::instance().set_ams_topology(helix::test::make_head_topology(7));
        process_lvgl(30);
        ToolState::instance().set_ams_topology(helix::test::make_hub_topology(0, {0, 10}));
        process_lvgl(30);
        const std::string label =
            std::string(
                lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_seg_5_label"))) +
            "|" + lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_seg_5_sub"));
        CAPTURE(label);
        CHECK(label.find("T10") != std::string::npos);
        tap(named(h, "tool_switcher_seg_5"));
        process_lvgl(30);
        CHECK(rig.backend->last_tool == 10);
    }
    SECTION("the head a lane is mounted on reads mounted, and its tap opens the sheet") {
        ToolState::instance().set_ams_topology(helix::test::make_hub_topology(9));
        process_lvgl(30);
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_seg_5_state")) ==
              static_cast<int>(ToolSwitcherCellState::Mounted));
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_seg_0_state")) ==
              static_cast<int>(ToolSwitcherCellState::Idle));
        tap(named(h, "tool_switcher_seg_5"));
        process_lvgl(30);
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(ToolSwitcherTestAccess::picker_list(h.widget()) != nullptr);
        ToolSwitcherTestAccess::hide_picker(h.widget());
        process_lvgl(30);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the face's band names the mounted lane's material legibly",
                 "[tool_switcher][band]") {
    ToolChangerRig rig(4);
    map_heads_to_lanes(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 90, 90);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Single));

    auto text_of = [](const std::string& name) {
        return std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, name.c_str())));
    };
    auto color_of = [](const std::string& name) {
        return lv_color_to_u32(lv_subject_get_color(lv_xml_get_subject(nullptr, name.c_str())));
    };
    // Mounted in turn: a dark lane, a light one, then the dark one again, so
    // the face's text colour has to change both ways.
    for (const int head : {0, 1, 0}) {
        CAPTURE(head);
        ToolState::instance().set_ams_topology(helix::test::make_head_topology(4, head));
        process_lvgl(30);
        const std::string seg = "tool_switcher_seg_" + std::to_string(head);
        REQUIRE_FALSE(text_of(seg + "_material").empty());
        CHECK(text_of("tool_switcher_face_material") == text_of(seg + "_material"));
        CHECK(color_of("tool_switcher_face_color") == color_of(seg + "_color"));
        CHECK(color_of("tool_switcher_face_band_text") == color_of(seg + "_band_text"));
        CHECK(color_of("tool_switcher_face_band_text") ==
              lv_color_to_u32(theme_manager_get_readable_on(
                  lv_subject_get_color(lv_xml_get_subject(nullptr, "tool_switcher_face_color")))));
    }
    REQUIRE(color_of("tool_switcher_seg_0_band_text") != color_of("tool_switcher_seg_1_band_text"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a mount that renames a shared head re-decides the label form",
                 "[tool_switcher][physical-heads]") {
    ToolChangerRig rig(3);
    // Two named tools on one head, so the head reads as whichever is mounted.
    PrinterDiscovery hw;
    hw.parse_objects(
        nlohmann::json::array({"toolchanger", "heater_bed", "tool L", "extruder",
                               "tool VeryLongToolName", "extruder1", "tool C", "extruder2"}));
    ToolState::instance().init_tools(hw);
    ToolState::instance().update_from_status(
        nlohmann::json{{"tool VeryLongToolName", {{"extruder", "extruder"}}}});
    const auto& heads = ToolState::instance().physical_heads();
    REQUIRE(heads.size() == 2);
    const auto& tools = ToolState::instance().tools();
    const auto tool_named = [&tools](const char* name) {
        for (const auto& t : tools) {
            if (t.name == name)
                return t.index;
        }
        return -1;
    };
    const int short_tool = heads[0].tool_indices.front();
    const int long_tool = tool_named("VeryLongToolName");
    REQUIRE(heads[0].holds_tool(long_tool));
    REQUIRE(short_tool != long_tool);
    // The backend reports the mount, which changes no tool and so bumps no
    // tool-list version.
    auto mount = [](int tool) { ToolState::instance().set_backend_active_tool(tool); };

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    auto labels = []() {
        std::vector<std::string> out;
        for (const char* name : {"tool_switcher_seg_0_label", "tool_switcher_seg_0_sub",
                                 "tool_switcher_seg_1_label", "tool_switcher_seg_1_sub"}) {
            out.emplace_back(lv_subject_get_string(lv_xml_get_subject(nullptr, name)));
        }
        return out;
    };
    auto rail = []() {
        return mode() == static_cast<int>(ToolSwitcherMode::Rail) ||
               mode() == static_cast<int>(ToolSwitcherMode::RailWithActions);
    };

    int flips = 0;
    for (int w = 100; w <= 600; w += 4) {
        CAPTURE(w);
        mount(short_tool);
        resize_content(h, w, 60);
        process_lvgl(30);
        if (!rail())
            continue;
        const auto short_name = labels();
        mount(long_tool);
        process_lvgl(30);
        const auto after_mount = labels();
        // The same box laid out from scratch with the long name mounted.
        resize_content(h, w + 50, 60);
        process_lvgl(30);
        resize_content(h, w, 60);
        process_lvgl(30);
        REQUIRE(rail());
        CHECK(after_mount == labels());
        // Head 1 is never renamed: its label changes only with the form.
        if (short_name[2] != after_mount[2])
            ++flips;
    }
    mount(short_tool);
    process_lvgl(30);
    // Some box fits the short name and not the long one.
    REQUIRE(flips > 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "tool_switcher: the feeder state follows the backend",
                 "[tool_switcher][feeder]") {
    ToolChangerRig rig(2);
    rig.backend->has_feeder = true;
    lv_subject_t* open = AmsState::instance().get_feeder_open_subject();

    rig.backend->feeder_is_open = true;
    AmsState::instance().sync_from_backend();
    CHECK(lv_subject_get_int(open) == 1);

    rig.backend->feeder_is_open = false;
    AmsState::instance().sync_from_backend();
    CHECK(lv_subject_get_int(open) == 0);

    // A printer switch takes the backend away: its feeder is no one's.
    rig.backend->feeder_is_open = true;
    AmsState::instance().sync_from_backend();
    REQUIRE(lv_subject_get_int(open) == 1);
    rig.backend->wait_for_operation_thread();
    rig.backend->stop();
    rig.backend = nullptr;
    AmsState::instance().clear_backends();
    CHECK(lv_subject_get_int(open) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a multi-colour spool paints its split and finish through the "
                 "swatch painter",
                 "[tool_switcher][band][swatch]") {
    ToolChangerRig rig(3);
    map_heads_to_lanes(3);
    SlotInfo spool = rig.backend->get_slot_info(1);
    spool.color_rgb = 0xFF0000;
    spool.multi_color_hexes = "#FF0000,#00FF00,#0000FF";
    spool.material = "PLA Silk";
    REQUIRE(rig.backend->sync_external_identity(1, spool).success());
    process_lvgl(30);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 211);
    process_lvgl(30);
    REQUIRE(published_int("tool_switcher_detail") == 1);
    REQUIRE(published_int("tool_switcher_detail_rows") >= 2);

    // The rail segment's band: its colours split, its finish silk.
    lv_obj_t* band =
        lv_obj_find_by_name(named(h, "tool_switcher_seg_1"), "tool_switcher_band_fill");
    REQUIRE(band != nullptr);
    CHECK(ui::has_swatch_paint(band));
    CHECK(ui::swatch_paint_colors(band).size() == 3);
    CHECK(ui::swatch_paint_finish(band) == ui::SwatchFinish::Silk);
    // A single-colour head's band paints its one colour, plain.
    lv_obj_t* plain =
        lv_obj_find_by_name(named(h, "tool_switcher_seg_0"), "tool_switcher_band_fill");
    REQUIRE(plain != nullptr);
    CHECK(ui::has_swatch_paint(plain));
    CHECK(ui::swatch_paint_colors(plain).empty());

    // Head 0 is mounted, so head 1 is the second detail row: its dot splits too.
    lv_obj_t* dot = named(h, "tool_switcher_detail_dot_1");
    REQUIRE(dot != nullptr);
    lv_obj_t* disc = lv_obj_find_by_name(dot, "color_swatch_disc");
    REQUIRE(disc != nullptr);
    CHECK(ui::has_swatch_paint(disc));
    CHECK(ui::swatch_paint_colors(disc).size() == 3);

    // The face mounting head 1 carries the same split and finish.
    ToolState::instance().set_ams_topology(helix::test::make_head_topology(3, 1));
    h.resize(2, 2, 60, 60);
    process_lvgl(30);
    REQUIRE(published_int("tool_switcher_mode") == static_cast<int>(ToolSwitcherMode::Single));
    lv_obj_t* face_band =
        lv_obj_find_by_name(named(h, "tool_switcher_face"), "tool_switcher_band_fill");
    REQUIRE(face_band != nullptr);
    CHECK(ui::swatch_paint_colors(face_band).size() == 3);
    CHECK(ui::swatch_paint_finish(face_band) == ui::SwatchFinish::Silk);
}
