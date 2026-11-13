// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_manage_row_controls.cpp
 * @brief The filament card's manage row carries two mutually exclusive
 * controls, and tool count alone decides which one.
 *
 * The tool selector belongs to every multi-tool printer: the options come from
 * ToolState, and handle_extruder_changed() issues a gcode Tn when no AMS
 * backend claims the tool. The Manage button navigates to the AMS panel, which
 * needs a backend, so it is the single-tool affordance (#1350).
 */

#include "ui_ams_mini_status.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_filament.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "printer_discovery.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <lvgl.h>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using TA = helix::ui::FilamentPanelTestAccess;

namespace {

/// Builds the real FilamentPanel over the real filament_panel.xml so the
/// visibility assertions read the same widgets production shows.
struct ManageRowHarness {
    LVGLUITestFixture& fx;
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;

    /// @param extruder_heaters Klipper extruder objects the printer reports.
    ///        Two or more with no [tool N] object is a plain multi-extruder
    ///        machine, which ToolState turns into one tool per heater.
    /// @param ams_type Value of the ams_type subject; 0 means no backend.
    ManageRowHarness(LVGLUITestFixture& f, const std::vector<std::string>& extruder_heaters,
                     AmsType ams_type)
        : fx(f) {
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();

        nlohmann::json objects = nlohmann::json::array();
        for (const auto& h : extruder_heaters) {
            objects.push_back(h);
        }
        objects.push_back("heater_bed");
        objects.push_back("fan");
        objects.push_back("gcode_move");

        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        ToolState::instance().init_tools(hw);
        // A changer's first status frame: T0 is on the carriage.
        ToolState::instance().update_from_status({{"toolchanger", {{"tool_number", 0}}}});

        lv_subject_set_int(AmsState::instance().get_ams_type_subject(), static_cast<int>(ams_type));

        panel = std::make_unique<FilamentPanel>(fx.state(), fx.api());
        panel->init_subjects();

        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());
        fx.process_lvgl(30);

        TA::populate_extruder_dropdown(*panel);
    }

    ~ManageRowHarness() {
        panel.reset();
        AmsState::instance().clear_backends();
    }

    [[nodiscard]] bool hidden(const char* name) const {
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Filament manage row: multi-tool gets the tool selector",
                 "[filament][ui][tool]") {
    // With no backend, Manage would navigate to an AMS panel that returns early;
    // the tools exist in ToolState whether or not a backend claimed them.
    ManageRowHarness h(*this, {"extruder", "extruder1"}, GENERATE(AmsType::NONE, AmsType::AFC));

    REQUIRE(ToolState::instance().is_multi_tool());
    // The row holds both controls, so it must be on screen for either check to mean anything.
    REQUIRE_FALSE(h.hidden("ams_manage_row"));
    CHECK(h.hidden("btn_manage_slots"));
    CHECK_FALSE(h.hidden("extruder_selector_group"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament manage row: single-tool with AMS gets Manage",
                 "[filament][ui][tool]") {
    ManageRowHarness h(*this, {"extruder"}, AmsType::AFC);

    REQUIRE_FALSE(ToolState::instance().is_multi_tool());
    REQUIRE_FALSE(h.hidden("ams_manage_row"));
    CHECK_FALSE(h.hidden("btn_manage_slots"));
    CHECK(h.hidden("extruder_selector_group"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Cool Down keeps its grid cell while nothing is heating",
                 "[filament][ui][cooldown]") {
    ManageRowHarness h(*this, {"extruder"}, AmsType::NONE);

    lv_obj_t* cooldown = lv_obj_find_by_name(h.root, "btn_cooldown");
    REQUIRE(cooldown != nullptr);

    lv_subject_t* heating = lv_xml_get_subject(nullptr, "filament_nozzle_heating");
    REQUIRE(heating != nullptr);

    lv_subject_set_int(heating, 0);
    h.fx.process_lvgl(30);
    // Disabled, never hidden: the material grid must not reflow when the
    // heaters are cold.
    CHECK_FALSE(lv_obj_has_flag(cooldown, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_state(cooldown, LV_STATE_DISABLED));

    lv_subject_set_int(heating, 1);
    h.fx.process_lvgl(30);
    CHECK_FALSE(lv_obj_has_flag(cooldown, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_state(cooldown, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Closed tool dropdown names the tool as the screen numbers it",
                 "[filament][ui][tool][1769]") {
    ManageRowHarness h(*this, {"extruder", "extruder1"}, AmsType::NONE);

    lv_obj_t* dd = TA::extruder_dropdown(*h.panel);
    REQUIRE(dd != nullptr);

    const char* closed = lv_dropdown_get_text(dd);
    REQUIRE(closed != nullptr);
    CHECK(std::string(closed) == "Tool 1");

    // The active tool moving to the second head moves the closed text with it.
    lv_subject_set_int(ToolState::instance().get_active_tool_subject(), 1);
    h.fx.process_lvgl(30);
    REQUIRE(lv_dropdown_get_selected(dd) == 1);
    closed = lv_dropdown_get_text(dd);
    REQUIRE(closed != nullptr);
    CHECK(std::string(closed) == "Tool 2");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Closed tool dropdown shows a configured tool name as is",
                 "[filament][ui][tool][1769]") {
    // A configured name is what the machine is labeled with; the open list
    // adds each tool's G-code name beside it.
    ManageRowHarness h(*this, {"toolchanger", "tool Left", "tool Right", "extruder", "extruder1"},
                       AmsType::NONE);

    lv_obj_t* dd = TA::extruder_dropdown(*h.panel);
    REQUIRE(dd != nullptr);
    REQUIRE(ToolState::instance().tool_count() == 2);
    REQUIRE(ToolState::instance().tools()[0].name == "Left");

    const char* closed = lv_dropdown_get_text(dd);
    REQUIRE(closed != nullptr);
    CHECK(std::string(closed) == "Left");
    const char* options = lv_dropdown_get_options(dd);
    REQUIRE(options != nullptr);
    CHECK(std::string(options) == "Left (T0)\nRight (T1)");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Open tool list names a head by tool and a hub's tool by lane",
                 "[filament][ui][tool][1769]") {
    // The open list sizes itself to its widest option, so it always has room
    // for the full form; the closed control stays the G-code name above.
    const bool hub = GENERATE(false, true);
    ManageRowHarness h(*this,
                       hub ? std::vector<std::string>{"extruder"}
                           : std::vector<std::string>{"extruder", "extruder1"},
                       hub ? AmsType::CFS : AmsType::NONE);
    if (hub) {
        // One head behind a 4-slot hub: each tool is a lane, remapped or unmapped.
        ToolTopology topo;
        topo.tool_count = 4;
        topo.tool_to_slot = {0, 1, 3, -1};
        ToolState::instance().set_ams_topology(topo);
        TA::populate_extruder_dropdown(*h.panel);
    }
    const char* options = lv_dropdown_get_options(TA::extruder_dropdown(*h.panel));
    REQUIRE(options != nullptr);
    CHECK(std::string(options) == (hub ? "Slot 1 (T0)\nSlot 2 (T1)\nSlot 4 (T2)\nTool 4 (T3)"
                                       : "Tool 1 (T0)\nTool 2 (T1)"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Open tool list names a CFS lane by its box and letter",
                 "[filament][ui][tool][cfs]") {
    // A CFS labels its bays 1A..1D, 2A..2D; T<n> stays the G-code name beside it.
    ManageRowHarness h(*this, {"extruder"}, AmsType::CFS);
    AmsState::instance().set_backend(
        std::make_unique<helix::printer::AmsBackendCfs>(nullptr, nullptr));
    ToolTopology topo;
    topo.tool_count = 4;
    topo.tool_to_slot = {0, 5, 3, -1};
    ToolState::instance().set_ams_topology(topo);
    REQUIRE_FALSE(ToolState::instance().has_multiple_toolheads());
    TA::populate_extruder_dropdown(*h.panel);

    const char* options = lv_dropdown_get_options(TA::extruder_dropdown(*h.panel));
    REQUIRE(options != nullptr);
    CHECK(std::string(options) == "1A (T0)\n2B (T1)\n1D (T2)\nTool 4 (T3)");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Open tool list names a lane through the backend that feeds it",
                 "[filament][ui][tool][cfs]") {
    // The CFS is the active backend, but these tools are fed from the second
    // one: their lanes take that backend's names, never a CFS bay.
    ManageRowHarness h(*this, {"extruder"}, AmsType::CFS);
    auto& ams = AmsState::instance();
    ams.set_backend(std::make_unique<helix::printer::AmsBackendCfs>(nullptr, nullptr));
    auto mock = std::make_unique<AmsBackendMock>(4);
    const AmsBackendMock* other = mock.get();
    REQUIRE(ams.add_backend(std::move(mock)) == 1);
    REQUIRE(ams.active_backend_index() == 0);
    ToolTopology topo;
    topo.tool_count = 2;
    topo.tool_to_slot = {0, 1};
    topo.backend_index = 1;
    ToolState::instance().set_ams_topology(topo);
    TA::populate_extruder_dropdown(*h.panel);

    const char* options = lv_dropdown_get_options(TA::extruder_dropdown(*h.panel));
    REQUIRE(options != nullptr);
    const std::string expected = helix::ui::backend_lane_label(*other, 0) + " (T0)\n" +
                                 helix::ui::backend_lane_label(*other, 1) + " (T1)";
    CHECK(std::string(options) == expected);
    CHECK(std::string(options).find("1A") == std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Portrait small screen turns the graph card into a spacer with a strip button",
                 "[filament][ui][portrait]") {
    // 320x480 is portrait: the refresh re-derives the breakpoint tokens from
    // the display, and the subject picks the XML branch (the fixture's
    // LayoutManager stays on its landscape default, so refresh_orientation
    // alone would not flip it).
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution res(disp, 320, 480);
    theme_manager_refresh_layout_constants(disp);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "ui_is_portrait"), 1);
    // The fixture registers fewer custom widgets than the app; the strip's
    // AMS mini status needs its registration or the element silently
    // creates nothing.
    ui_ams_mini_status_init();
    ManageRowHarness h(*this, {"extruder", "extruder1"}, AmsType::AFC);
    // The fixture's screen was created at the default geometry; size the
    // panel itself so the column's real remainder drives the fit decision.
    // 400 mirrors what a 320x480 device leaves the panel after its chrome —
    // at the full 480 the column has room and the graph legitimately shows.
    lv_obj_set_size(h.root, 320, 400);
    lv_obj_update_layout(h.root);

    lv_obj_t* card = lv_obj_find_by_name(h.root, "temp_graph_card");
    lv_obj_t* container = lv_obj_find_by_name(h.root, "temp_graph_container");
    lv_obj_t* btn = lv_obj_find_by_name(h.root, "btn_temp_graph");
    REQUIRE(card != nullptr);
    REQUIRE(container != nullptr);
    REQUIRE(btn != nullptr);

    // Too little room for the graph: the card stays in the flow as an
    // invisible spacer instead of vanishing and dropping the strip to
    // mid-column.
    lv_subject_t* spacer = lv_xml_get_subject(nullptr, "filament_graph_spacer");
    REQUIRE(spacer != nullptr);
    CHECK(lv_subject_get_int(spacer) == 1);
    CHECK(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_bg_opa(card, LV_PART_MAIN) == 0);
    CHECK_FALSE(lv_obj_has_flag(card, LV_OBJ_FLAG_CLICKABLE));

    // The strip button replaces the tappable card, left of the AMS mini
    // status widget.
    CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* mini = lv_obj_find_by_name(h.root, "ams_mini_status");
    REQUIRE(mini != nullptr);
    lv_area_t btn_area, mini_area;
    lv_obj_get_coords(btn, &btn_area);
    lv_obj_get_coords(mini, &mini_area);
    CHECK(btn_area.x1 < mini_area.x1);

    // The button opens the same overlay the graph card opens. The XML
    // callback routes through the global panel singleton (like every static
    // callback in this file), and the harness panel is a local instance — so
    // observe the call at the overlay: open() resyncs its mode subject to
    // GraphOnly, and nothing else in this test touches it.
    get_global_temp_graph_overlay().init_subjects();
    lv_subject_t* mode = lv_xml_get_subject(nullptr, "temp_graph_mode");
    REQUIRE(mode != nullptr);
    lv_subject_set_int(mode, 3);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    h.fx.process_lvgl(30);
    CHECK(lv_subject_get_int(mode) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Strip graph button stays hidden in landscape",
                 "[filament][ui][portrait]") {
    ManageRowHarness h(*this, {"extruder"}, AmsType::AFC);

    lv_obj_t* btn = lv_obj_find_by_name(h.root, "btn_temp_graph");
    REQUIRE(btn != nullptr);
    h.fx.process_lvgl(30);
    CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Orientation flip rewires the panel's widget pointers into the rebuilt tree",
                 "[filament][ui][portrait]") {
    // Landscape first. The subject is global and a prior case may have left
    // it at 1, so set it before the tree is built.
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    lv_subject_set_int(portrait, 0);
    ui_ams_mini_status_init();
    ManageRowHarness h(*this, {"extruder", "extruder1"}, AmsType::AFC);

    lv_obj_t* landscape_dropdown = TA::extruder_dropdown(*h.panel);
    lv_obj_t* landscape_card = TA::temp_graph_card(*h.panel);
    REQUIRE(landscape_dropdown != nullptr);
    REQUIRE(landscape_card != nullptr);
    REQUIRE(lv_obj_find_by_name(h.root, "portrait_column") == nullptr);

    // Flip to portrait: the <if> rebuilds synchronously inside set_int and the
    // immediate observer re-runs bind_widgets() before set_int returns — the
    // condemned branch is already detached here, so anything the panel queues
    // next dereferences live widgets. Asserting BEFORE the pump is what pins
    // the immediate dispatch; a deferred rebind would still be holding the
    // detached landscape pointers at this point.
    lv_subject_set_int(portrait, 1);
    {
        lv_obj_t* dd_now = lv_obj_find_by_name(h.root, "extruder_dropdown");
        lv_obj_t* card_now = lv_obj_find_by_name(h.root, "temp_graph_card");
        REQUIRE(dd_now != nullptr);
        REQUIRE(card_now != nullptr);
        CHECK(TA::extruder_dropdown(*h.panel) == dd_now);
        CHECK(TA::temp_graph_card(*h.panel) == card_now);
    }
    // The pump lets the condemned branch's async deletion finish.
    h.fx.process_lvgl(30);

    lv_obj_t* column = lv_obj_find_by_name(h.root, "portrait_column");
    REQUIRE(column != nullptr);
    lv_obj_t* dd = lv_obj_find_by_name(h.root, "extruder_dropdown");
    lv_obj_t* card = lv_obj_find_by_name(h.root, "temp_graph_card");
    lv_obj_t* btn = lv_obj_find_by_name(h.root, "btn_temp_graph");
    REQUIRE(dd != nullptr);
    REQUIRE(card != nullptr);
    REQUIRE(btn != nullptr);
    // The members must point into the NEW tree — not just be non-null, or a
    // rebind that never ran would still pass on the condemned branch's
    // addresses while the panel draws from freed widgets.
    CHECK(TA::extruder_dropdown(*h.panel) == dd);
    CHECK(TA::temp_graph_card(*h.panel) == card);
    CHECK(TA::btn_temp_graph(*h.panel) == btn);
    CHECK(TA::extruder_dropdown(*h.panel) != landscape_dropdown);
    CHECK(TA::temp_graph_card(*h.panel) != landscape_card);

    // Flip back: same rewiring into the rebuilt landscape tree, and the
    // spacer subject is 0 again — a portrait spacer state must not survive
    // into a fresh tree.
    lv_subject_set_int(portrait, 0);
    h.fx.process_lvgl(30);

    lv_obj_t* dd2 = lv_obj_find_by_name(h.root, "extruder_dropdown");
    lv_obj_t* card2 = lv_obj_find_by_name(h.root, "temp_graph_card");
    lv_obj_t* btn2 = lv_obj_find_by_name(h.root, "btn_temp_graph");
    REQUIRE(dd2 != nullptr);
    REQUIRE(card2 != nullptr);
    REQUIRE(btn2 != nullptr);
    CHECK(TA::extruder_dropdown(*h.panel) == dd2);
    CHECK(TA::temp_graph_card(*h.panel) == card2);
    CHECK(TA::btn_temp_graph(*h.panel) == btn2);
    lv_subject_t* spacer = lv_xml_get_subject(nullptr, "filament_graph_spacer");
    REQUIRE(spacer != nullptr);
    CHECK(lv_subject_get_int(spacer) == 0);
    REQUIRE(lv_obj_find_by_name(h.root, "portrait_column") == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Closed tool dropdown keeps the aimed tool with the carriage empty",
                 "[filament][ui][tool]") {
    ManageRowHarness h(*this, {"extruder", "extruder1"}, AmsType::NONE);
    lv_obj_t* dd = TA::extruder_dropdown(*h.panel);
    REQUIRE(dd != nullptr);
    lv_subject_set_int(ToolState::instance().get_active_tool_subject(), 1);
    h.fx.process_lvgl(30);
    REQUIRE(std::string(lv_dropdown_get_text(dd)) == "Tool 2");

    // A park leaves no tool on the carriage; the panel still aims at the tool
    // it was on, so heating it needs no pick up.
    lv_subject_set_int(ToolState::instance().get_active_tool_subject(), -1);
    h.fx.process_lvgl(30);
    CHECK(std::string(lv_dropdown_get_text(dd)) == "Tool 2");
}

namespace {

/// Every descendant of @p root whose name is in @p names, hidden or not.
void collect_named(lv_obj_t* root, const std::vector<std::string>& names,
                   std::vector<lv_obj_t*>& out) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
        const char* name = lv_obj_get_name(child);
        if (name && std::find(names.begin(), names.end(), name) != names.end()) {
            out.push_back(child);
        }
        collect_named(child, names, out);
    }
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel nozzle heat controls dim while heat is blocked",
                 "[filament][ui][indx][heat]") {
    const int portrait_mode = GENERATE(0, 1);
    CAPTURE(portrait_mode);
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    lv_subject_set_int(portrait, portrait_mode);
    ui_ams_mini_status_init();
    ManageRowHarness h(*this, {"extruder"}, AmsType::NONE);
    lv_subject_t* blocked = AmsState::instance().get_nozzle_heat_blocked_subject();

    const std::vector<std::string> names = {"preset_pla", "preset_petg",  "preset_abs",
                                            "preset_tpu", "preset_spool", "nozzle_target_temp"};
    std::vector<lv_obj_t*> controls;
    collect_named(h.root, names, controls);
    // Five presets and the nozzle target, whichever layout this orientation built.
    REQUIRE(controls.size() >= names.size());

    lv_subject_set_int(blocked, 1);
    h.fx.process_lvgl(30);
    for (lv_obj_t* obj : controls) {
        CAPTURE(lv_obj_get_name(obj));
        CHECK(lv_obj_get_style_opa(obj, LV_PART_MAIN) < LV_OPA_COVER);
    }
    lv_subject_set_int(blocked, 0);
    h.fx.process_lvgl(30);
    for (lv_obj_t* obj : controls) {
        CAPTURE(lv_obj_get_name(obj));
        CHECK(lv_obj_get_style_opa(obj, LV_PART_MAIN) == LV_OPA_COVER);
    }
    lv_subject_set_int(portrait, 0);
}
