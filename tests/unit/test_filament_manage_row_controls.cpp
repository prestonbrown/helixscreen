// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_manage_row_controls.cpp
 * @brief The Filament panel's tool chooser and Manage button, and the head
 * count alone decides which one shows.
 *
 * A printer with several heads gets a row of heads in the Operations card,
 * the panel's only tool chooser; lanes a hub feeds into one head are a filament
 * system, not a tool choice, so a single-head printer gets no chooser at all
 * and keeps Manage, which opens the AMS panel (#1350).
 */

#include "ui_ams_mini_status.h"
#include "ui_filament_motion_glyph.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_filament.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/scoped_int_subject.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "helix/ui/text_metrics.h"
#include "printer_discovery.h"
#include "target_tool.h"
#include "theme_manager.h"
#include "tool_heads_model.h"
#include "tool_state.h"

#include <algorithm>
#include <cstdlib>
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
    }

    ~ManageRowHarness() {
        panel.reset();
        AmsState::instance().clear_backends();
        // The tools, mount report and ams_type set above are process-wide.
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }

    [[nodiscard]] bool hidden(const char* name) const {
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        return lv_obj_is_hidden(obj);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: several heads get the row of heads and a compact Manage",
                 "[filament][ui][tool]") {
    // The heads exist in ToolState whether or not a backend claimed them.
    ManageRowHarness h(*this, {"extruder", "extruder1"}, GENERATE(AmsType::NONE, AmsType::AFC));

    REQUIRE(ToolState::instance().has_multiple_toolheads());
    CHECK_FALSE(h.hidden("tool_card_row"));
    CHECK_FALSE(h.hidden("temp_status_card"));
    REQUIRE_FALSE(h.hidden("ams_manage_row"));
    // The card always has a way into the AMS panel; with the tool controls in
    // Operations its Manage is smaller than a one-head printer's.
    CHECK_FALSE(h.hidden("btn_manage_slots"));
    lv_obj_update_layout(h.root);
    lv_obj_t* manage = lv_obj_find_by_name(h.root, "btn_manage_slots");
    REQUIRE(manage != nullptr);
    CHECK(lv_obj_get_width(manage) >= 48);
    const char* full = lv_xml_get_const(nullptr, "button_height");
    REQUIRE(full != nullptr);
    CHECK(lv_obj_get_height(manage) < std::atoi(full));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: one head with AMS gets Manage and no chooser",
                 "[filament][ui][tool]") {
    ManageRowHarness h(*this, {"extruder"}, AmsType::AFC);

    REQUIRE_FALSE(ToolState::instance().is_multi_tool());
    CHECK(h.hidden("tool_card_row"));
    CHECK_FALSE(h.hidden("temp_status_card"));
    REQUIRE_FALSE(h.hidden("ams_manage_row"));
    CHECK_FALSE(h.hidden("btn_manage_slots"));
    CHECK(h.hidden("external_spool_row"));
    CHECK_FALSE(h.hidden("spool_card_header_row"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament card: one tool and no AMS shows the external spool and its text",
                 "[filament][ui][tool][external_spool]") {
    AmsState::instance().clear_external_spool_info();
    ManageRowHarness h(*this, {"extruder"}, AmsType::NONE);

    CHECK(h.hidden("ams_manage_row"));
    CHECK(h.hidden("spool_card_header_row"));
    REQUIRE_FALSE(h.hidden("external_spool_row"));
    // No spool: the translated placeholders stand in for the spool's lines.
    CHECK_FALSE(h.hidden("external_spool_none_label"));
    CHECK(h.hidden("external_spool_material_label"));

    SlotInfo spool;
    spool.brand = "Prusament";
    spool.material = "PETG";
    spool.color_name = "Galaxy Black";
    spool.color_rgb = 0x202020;
    spool.spoolman_id = 129;
    spool.remaining_weight_g = 640;
    spool.total_weight_g = 1000;
    // The record lives in the mock Moonraker's database; the write's answer
    // queues the read that notifies the panel.
    MockPrinter printer;
    AmsState::instance().set_moonraker_api(&printer.api);
    AmsState::instance().set_external_spool_info(spool);
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    h.fx.process_lvgl(30);

    CHECK(h.hidden("external_spool_none_label"));
    REQUIRE_FALSE(h.hidden("external_spool_material_label"));
    lv_obj_t* material = lv_obj_find_by_name(h.root, "external_spool_material_label");
    lv_obj_t* detail = lv_obj_find_by_name(h.root, "external_spool_color_label");
    CHECK(std::string(lv_label_get_text(material)) == "Prusament PETG #129");
    CHECK(std::string(lv_label_get_text(detail)) == "Galaxy Black · 640g");
    AmsState::instance().clear_external_spool_info();
    helix::ui::UpdateQueue::instance().drain();
    AmsState::instance().set_moonraker_api(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: a hub's lanes into one hotend are not a tool choice",
                 "[filament][ui][tool]") {
    // One hotend behind a 4-lane hub: ToolState lists a tool per lane, but
    // there is one head to aim at, so no T0..T3 chooser.
    ManageRowHarness h(*this, {"extruder"}, AmsType::AFC);
    ToolTopology topo;
    topo.tool_count = 4;
    topo.tool_to_slot = {0, 1, 2, 3};
    ToolState::instance().set_ams_topology(topo);
    h.fx.process_lvgl(30);
    REQUIRE(ToolState::instance().is_multi_tool());
    REQUIRE_FALSE(ToolState::instance().has_multiple_toolheads());

    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "target_tool_multi_head")) == 0);
    CHECK(h.hidden("tool_card_row"));
    CHECK_FALSE(h.hidden("btn_manage_slots"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "The panel glyph shows the loaded filament color",
                 "[filament][ui][filament-chevrons]") {
    ManageRowHarness h(*this, {"extruder"}, AmsType::NONE);
    lv_obj_t* glyph = lv_obj_find_by_name(h.root, "filament_motion_glyph");
    REQUIRE(glyph != nullptr);

    lv_subject_set_int(AmsState::instance().get_current_color_subject(), 0x12AB34);
    CHECK(helix::ui::filament_motion_glyph_color(glyph) == 0x12AB34);
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
    CHECK_FALSE(lv_obj_is_hidden(cooldown));
    CHECK(lv_obj_has_state(cooldown, LV_STATE_DISABLED));

    lv_subject_set_int(heating, 1);
    h.fx.process_lvgl(30);
    CHECK_FALSE(lv_obj_is_hidden(cooldown));
    CHECK_FALSE(lv_obj_has_state(cooldown, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Tool row names each head as the screen numbers it",
                 "[filament][ui][tool][1769]") {
    ManageRowHarness h(*this, {"extruder", "extruder1"}, AmsType::NONE);
    auto& model = helix::ui::ToolHeadsModel::instance();
    auto row_label = [](int head) {
        const std::string name = "tool_switcher_seg_" + std::to_string(head) + "_row";
        lv_subject_t* s = lv_xml_get_subject(nullptr, name.c_str());
        REQUIRE(s != nullptr);
        return std::string(lv_subject_get_string(s));
    };

    // Wide cells read the head's name, narrow ones its number: the same number
    // "Nozzle 2" carries beside them, never the G-code "T1".
    model.fit_row_labels("_row", 200);
    CHECK(row_label(1) == "Tool 2");
    model.fit_row_labels("_row", 20);
    CHECK(row_label(1) == "2");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Tool row shows a configured tool name as is",
                 "[filament][ui][tool][1769]") {
    ManageRowHarness h(*this, {"toolchanger", "tool Left", "tool Right", "extruder", "extruder1"},
                       AmsType::NONE);
    REQUIRE(ToolState::instance().tools()[0].name == "Left");
    auto& model = helix::ui::ToolHeadsModel::instance();
    lv_subject_t* first = lv_xml_get_subject(nullptr, "tool_switcher_seg_0_row");
    REQUIRE(first != nullptr);
    model.fit_row_labels("_row", 200);
    CHECK(std::string(lv_subject_get_string(first)) == "Left");
    model.fit_row_labels("_row", 20);
    CHECK(std::string(lv_subject_get_string(first)) == "Left");
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
    helix::test::ScopedIntSubject portrait("ui_is_portrait", 1);
    // The fixture registers fewer custom widgets than the app; the strip's
    // AMS mini status needs its registration or the element silently
    // creates nothing.
    ui_ams_mini_status_init();
    ManageRowHarness h(*this, {"extruder"}, AmsType::AFC);
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
    CHECK(lv_obj_is_hidden(container));
    CHECK_FALSE(lv_obj_is_hidden(card));
    CHECK(lv_obj_get_style_bg_opa(card, LV_PART_MAIN) == 0);
    CHECK_FALSE(lv_obj_is_clickable(card));

    // The strip button replaces the tappable card, left of the AMS mini
    // status widget.
    CHECK_FALSE(lv_obj_is_hidden(btn));
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
    // The overlay is a process-wide singleton: dropping it at the end withdraws
    // the temp_graph_mode subject this case makes it publish.
    struct DropGlobalOverlay {
        ~DropGlobalOverlay() {
            helix::detail::lazy_global_slot<TempGraphOverlay>().reset();
            helix::ui::UpdateQueue::instance().drain();
        }
    } drop_overlay;
    get_global_temp_graph_overlay().ensure_subjects();
    lv_subject_t* mode = lv_xml_get_subject(nullptr, "temp_graph_mode");
    REQUIRE(mode != nullptr);
    lv_subject_set_int(mode, 3);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    h.fx.process_lvgl(30);
    CHECK(lv_subject_get_int(mode) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Portrait 320x480 Filament panel fits with nothing clipped",
                 "[filament][ui][portrait][layout]") {
    const bool multi_head = GENERATE(false, true);
    CAPTURE(multi_head);
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution res(disp, 320, 480);
    theme_manager_refresh_layout_constants(disp);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "ui_is_portrait"), 1);
    ui_ams_mini_status_init();
    ManageRowHarness h(*this,
                       multi_head ? std::vector<std::string>{"extruder", "extruder1"}
                                  : std::vector<std::string>{"extruder"},
                       AmsType::AFC);
    // What a 320x480 screen leaves the panel under the navigation bar.
    lv_obj_set_size(h.root, 320, 402);
    lv_obj_update_layout(h.root);
    process_lvgl(30);
    lv_obj_update_layout(h.root);

    lv_obj_t* column = lv_obj_find_by_name(h.root, "portrait_column");
    REQUIRE(column != nullptr);
    lv_area_t col;
    lv_obj_get_content_coords(column, &col);
    // Every card the column shows ends inside it.
    for (uint32_t i = 0; i < lv_obj_get_child_count(column); ++i) {
        lv_obj_t* card = lv_obj_get_child(column, static_cast<int32_t>(i));
        if (lv_obj_is_hidden(card)) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(card, &a);
        CAPTURE(lv_obj_get_name(card), a.y2, col.y2);
        CHECK(a.y2 <= col.y2);
    }
    // The action rows give up at most a few pixels of a touch target; the
    // presets, the heads and Pick up keep a whole one.
    auto height = [&](const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(h.root, name);
        REQUIRE(obj != nullptr);
        return lv_obj_get_height(obj);
    };
    for (const char* name : {"btn_load", "btn_unload", "btn_purge", "btn_extrude", "btn_retract",
                             "extrude_length_0"}) {
        CAPTURE(name);
        CHECK(height(name) >= 40);
    }
    CHECK(height("preset_pla") >= 48);
    if (multi_head) {
        CHECK(height("tool_target_cell_0") >= 48);
        CHECK(h.hidden("spool_card"));
        // The lane bars and their chevron open the AMS panel from the
        // Operations header instead.
        CHECK_FALSE(h.hidden("operations_lanes_row"));
        CHECK(height("operations_lanes_row") >= 48);
    } else {
        CHECK_FALSE(h.hidden("spool_card"));
    }
    lv_subject_set_int(lv_xml_get_subject(nullptr, "ui_is_portrait"), 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Landscape 480x320 Filament panel fits with nothing clipped",
                 "[filament][ui][layout]") {
    const bool multi_head = GENERATE(false, true);
    CAPTURE(multi_head);
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution res(disp, 480, 320);
    theme_manager_refresh_layout_constants(disp);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "ui_is_portrait"), 0);
    ui_ams_mini_status_init();
    ManageRowHarness h(
        *this,
        multi_head ? std::vector<std::string>{"extruder", "extruder1", "extruder2", "extruder3"}
                   : std::vector<std::string>{"extruder"},
        AmsType::AFC);
    // What a 480x320 screen leaves the panel beside the navigation bar.
    lv_obj_set_size(h.root, 426, 320);
    lv_obj_update_layout(h.root);
    process_lvgl(30);
    lv_obj_update_layout(h.root);

    lv_obj_t* column = lv_obj_find_by_name(h.root, "right_column");
    REQUIRE(column != nullptr);
    lv_area_t col;
    lv_obj_get_content_coords(column, &col);
    lv_obj_t* ops = lv_obj_find_by_name(column, "filament_operations_group");
    REQUIRE(ops != nullptr);
    lv_area_t a;
    lv_obj_get_coords(ops, &a);
    CAPTURE(a.y2, col.y2);
    CHECK(a.y2 <= col.y2);
    // Heads, Park and the action rows stay touch targets.
    if (multi_head) {
        CHECK(h.hidden("operations_heading_row"));
        lv_obj_t* cell = lv_obj_find_by_name(h.root, "tool_target_cell_0");
        REQUIRE(cell != nullptr);
        CHECK(lv_obj_get_height(cell) >= 48);
    } else {
        CHECK_FALSE(h.hidden("operations_heading_row"));
    }
    lv_obj_t* load = lv_obj_find_by_name(h.root, "btn_load");
    REQUIRE(load != nullptr);
    CHECK(lv_obj_get_height(load) >= 40);
    // The cold-extrusion warning covers the row it disables rather than
    // adding one.
    CHECK_FALSE(h.hidden("safety_warning"));
    // Four presets share one row: whichever temperature line shows fits.
    lv_obj_t* pla = lv_obj_find_by_name(h.root, "preset_pla");
    REQUIRE(pla != nullptr);
    lv_obj_t* full = lv_obj_find_by_name(pla, "preset_temps");
    lv_obj_t* shown =
        lv_obj_is_hidden(full) ? lv_obj_find_by_name(pla, "preset_nozzle_temp") : full;
    CHECK(helix::ui::text_width(lv_label_get_text(shown),
                                lv_obj_get_style_text_font(shown, LV_PART_MAIN)) <=
          lv_obj_get_content_width(pla));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Strip graph button stays hidden in landscape",
                 "[filament][ui][portrait]") {
    ManageRowHarness h(*this, {"extruder"}, AmsType::AFC);

    lv_obj_t* btn = lv_obj_find_by_name(h.root, "btn_temp_graph");
    REQUIRE(btn != nullptr);
    h.fx.process_lvgl(30);
    CHECK(lv_obj_is_hidden(btn));
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
    ManageRowHarness h(*this, {"extruder"}, AmsType::AFC);

    lv_obj_t* landscape_card = TA::temp_graph_card(*h.panel);
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
        lv_obj_t* card_now = lv_obj_find_by_name(h.root, "temp_graph_card");
        REQUIRE(card_now != nullptr);
        CHECK(TA::temp_graph_card(*h.panel) == card_now);
    }
    // The pump lets the condemned branch's async deletion finish.
    h.fx.process_lvgl(30);

    lv_obj_t* column = lv_obj_find_by_name(h.root, "portrait_column");
    REQUIRE(column != nullptr);
    lv_obj_t* card = lv_obj_find_by_name(h.root, "temp_graph_card");
    lv_obj_t* btn = lv_obj_find_by_name(h.root, "btn_temp_graph");
    REQUIRE(card != nullptr);
    REQUIRE(btn != nullptr);
    // The members must point into the NEW tree — not just be non-null, or a
    // rebind that never ran would still pass on the condemned branch's
    // addresses while the panel draws from freed widgets.
    CHECK(TA::temp_graph_card(*h.panel) == card);
    CHECK(TA::btn_temp_graph(*h.panel) == btn);
    CHECK(TA::temp_graph_card(*h.panel) != landscape_card);

    // Flip back: same rewiring into the rebuilt landscape tree, and the
    // spacer subject is 0 again — a portrait spacer state must not survive
    // into a fresh tree.
    lv_subject_set_int(portrait, 0);
    h.fx.process_lvgl(30);

    lv_obj_t* card2 = lv_obj_find_by_name(h.root, "temp_graph_card");
    lv_obj_t* btn2 = lv_obj_find_by_name(h.root, "btn_temp_graph");
    REQUIRE(card2 != nullptr);
    REQUIRE(btn2 != nullptr);
    CHECK(TA::temp_graph_card(*h.panel) == card2);
    CHECK(TA::btn_temp_graph(*h.panel) == btn2);
    lv_subject_t* spacer = lv_xml_get_subject(nullptr, "filament_graph_spacer");
    REQUIRE(spacer != nullptr);
    CHECK(lv_subject_get_int(spacer) == 0);
    REQUIRE(lv_obj_find_by_name(h.root, "portrait_column") == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel keeps the aimed tool with the carriage empty",
                 "[filament][ui][tool]") {
    ManageRowHarness h(*this, {"extruder", "extruder1"}, AmsType::NONE);
    ToolState::instance().update_from_status({{"toolchanger", {{"tool_number", 1}}}});
    h.fx.process_lvgl(30);
    REQUIRE(TA::target_tool(*h.panel) == 1);

    // A park leaves no tool on the carriage; the panel still aims at the tool
    // it was on, so heating it needs no pick up.
    ToolState::instance().update_from_status({{"toolchanger", {{"tool_number", -1}}}});
    h.fx.process_lvgl(30);
    CHECK(TA::target_tool(*h.panel) == 1);
    ToolState::instance().update_from_status({{"toolchanger", {{"tool_number", 0}}}});
    h.fx.process_lvgl(30);
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

    const std::vector<std::string> names = {"preset_pla", "preset_petg", "preset_abs", "preset_tpu",
                                            "preset_spool"};
    std::vector<lv_obj_t*> controls;
    collect_named(h.root, names, controls);
    // Five presets, whichever layout this orientation built, and the nozzle row's reading.
    REQUIRE(controls.size() >= names.size());
    lv_obj_t* nozzle_row = lv_obj_find_by_name(h.root, "btn_nozzle_temp");
    REQUIRE(nozzle_row != nullptr);
    lv_obj_t* reading = lv_obj_find_by_name(nozzle_row, "heater_row_reading");
    REQUIRE(reading != nullptr);
    controls.push_back(reading);

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
