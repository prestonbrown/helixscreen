// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_tool_switcher.cpp
 * @brief tool_switcher measures its granted box, decides a mode with
 * decide_tool_switcher_layout(), and publishes the verdict and every head's
 * text, colour and detail line as subjects the XML binds.
 *
 * The tests read the published subjects by name, as the XML does, rather than
 * the widget's cached decision. Observer paths (a new head count, a new
 * mounted head) fire long after the last resize, so several cases change the
 * tools with no further size call and check the republish used the cached box.
 *
 * Tool data is seeded directly on the ToolState singleton via
 * ToolTopology/set_ams_topology(): the mock `--test` printer never exposes
 * more than one tool, so multi-head scenarios are unreachable by driving the
 * real app.
 */

#include "ui_tile_rung.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "../test_helpers/tool_topologies.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "helix/ui/text_metrics.h"
#include "panel_widget_size.h"
#include "printer_discovery.h"
#include "src/ui/panel_widgets/tool_switcher_widget.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {

/// ToolState is a singleton outside LVGLUITestFixture's own init/deinit
/// chain, so a test that seeds it must clear it on the way out or later
/// test files in the same binary inherit stale tools (see
/// test_widget_size_nozzle_temps.cpp's NozzleTempsFixture, same trap).
struct ToolSwitcherFixture : public LVGLUITestFixture {
    ~ToolSwitcherFixture() override {
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

/// Seeds ToolState with `count` generated tools ("T0".."Tn-1") and the given
/// active index via the AMS-topology path — the simplest way to get an
/// arbitrary tool count without going through PrinterDiscovery/JSON.
void configure_tools(int count, int active_index = 0) {
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    ts.set_ams_topology(helix::test::make_head_topology(count, active_index));
}

/// Re-applies the topology with a new tool_count/active_tool so a later
/// call only flips the ONE subject the test cares about (tool_count_ or
/// active_tool_), isolating which observer fires.
void update_tools(int count, int active_index) {
    ToolState::instance().set_ams_topology(helix::test::make_head_topology(count, active_index));
}

int subject_int(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

std::string subject_string(const std::string& name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name.c_str());
    REQUIRE(s != nullptr);
    return lv_subject_get_string(s);
}

uint32_t subject_rgb(const std::string& name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name.c_str());
    REQUIRE(s != nullptr);
    return lv_color_to_u32(lv_subject_get_color(s)) & 0xFFFFFF;
}

std::string seg(int i, const char* field) {
    return "tool_switcher_seg_" + std::to_string(i) + "_" + field;
}

std::string detail(int i, const char* field) {
    return "tool_switcher_detail_" + std::to_string(i) + "_" + field;
}

int mode() {
    return subject_int("tool_switcher_mode");
}

/// What a generated head's segment reads in the published label form.
std::string expected_seg_label(int i) {
    return subject_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Full)
               ? "Tool " + std::to_string(i + 1)
               : "T" + std::to_string(i);
}

/// A tool-changer mock as the connected backend, for as long as it lives.
struct ParkingBackend {
    AmsBackendMock* backend = nullptr;
    explicit ParkingBackend(bool tool_changer) {
        auto owned = std::make_unique<AmsBackendMock>(3);
        owned->set_tool_changer_mode(tool_changer);
        owned->set_operation_delay(0);
        REQUIRE(owned->start());
        backend = owned.get();
        AmsState::instance().deinit_subjects();
        AmsState::instance().init_subjects(true);
        AmsState::instance().set_backend(std::move(owned));
    }
    ~ParkingBackend() {
        backend->wait_for_operation_thread();
        backend->stop();
        AmsState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

/// Tool names configured on the printer, as a klipper-toolchanger lists them.
void configure_named_tools(const std::vector<std::string>& names) {
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    helix::PrinterDiscovery disc;
    nlohmann::json objects = nlohmann::json::array({"toolchanger"});
    for (size_t i = 0; i < names.size(); ++i) {
        objects.push_back("tool " + names[i]);
        objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
    }
    disc.parse_objects(objects);
    ts.init_tools(disc);
    REQUIRE(ts.tool_count() == static_cast<int>(names.size()));
}

} // namespace

// ============================================================================
// The verdict (prestonbrown/helixscreen#1784)
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: publishes the mode its measured box earns, and re-decides on "
                 "every size change",
                 "[widget_size][tool_switcher][1784]") {
    // A backend that can dock a head, so the Park modes are open.
    ParkingBackend parking(/*tool_changer=*/true);
    // 214x93 is the 2x1 content box at 800x480: four 48 px segments fit, five
    // do not.
    configure_tools(5);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);
    CHECK(mode() == static_cast<int>(ToolSwitcherMode::Pair));

    configure_tools(4);
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);
    CHECK(mode() == static_cast<int>(ToolSwitcherMode::Rail));
    CHECK(subject_int("tool_switcher_cols") == 4);
    CHECK(subject_int("tool_switcher_rows") == 1);
    CHECK(subject_int("tool_switcher_cell_h") == 93);
    CHECK(subject_int("tool_switcher_cell_w") >= 48);

    // The 1x1 content box: the face alone.
    resize_content(h, 95, 93);
    process_lvgl(30);
    CHECK(mode() == static_cast<int>(ToolSwitcherMode::Single));

    CHECK(h.widget().fits_at(800, 480));
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a tile fits only where its content box holds one touch target",
                 "[widget_size][tool_switcher][1784]") {
    configure_tools(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    const int floor = resolve_px("touch_target_min", 48);
    const auto [w, hpx] = tile_for_content(floor, floor);
    CHECK(h.widget().fits_at(w, hpx));
    CHECK_FALSE(h.widget().fits_at(w - 1, hpx));
    CHECK_FALSE(h.widget().fits_at(w, hpx - 1));
    CHECK_FALSE(h.widget().fits_at(4, 4));
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a tall rail publishes its detail block and how many rows fit",
                 "[widget_size][tool_switcher][1784]") {
    configure_tools(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);

    // One touch row tall: no detail.
    resize_content(h, 600, 60);
    process_lvgl(30);
    CHECK(subject_int("tool_switcher_detail") == 0);
    CHECK(subject_int("tool_switcher_detail_rows") == 0);

    // Tall: the rail takes its rows and the rest holds the detail lines.
    resize_content(h, 600, 400);
    process_lvgl(30);
    const ToolSwitcherLayout layout = ToolSwitcherTestAccess::layout(h.widget());
    REQUIRE(layout.detail);
    CHECK(subject_int("tool_switcher_detail") == 1);
    CHECK(subject_int("tool_switcher_rail_h") == layout.rail_h);
    CHECK(subject_int("tool_switcher_rail_h") < 400);
    // 400 px holds every head's line; there are four.
    CHECK(subject_int("tool_switcher_detail_rows") == 4);
}

// ============================================================================
// Per-head pools
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: segments read \"Tool 1\" over \"T0\" where every head's lines "
                 "fit, else \"T0\"",
                 "[widget_size][tool_switcher][1784][1769]") {
    // A backend that can dock a head, so the Park modes are open.
    ParkingBackend parking(/*tool_changer=*/true);
    configure_tools(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const int pad = 2 * resolve_px("space_xs", 4);

    // One row of four wide segments.
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    REQUIRE(ui::text_width("Tool 4", body) <= subject_int("tool_switcher_cell_w") - pad);
    CHECK(subject_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Full));
    CHECK(subject_string(seg(0, "label")) == "Tool 1");
    CHECK(subject_string(seg(0, "sub")) == "T0");
    CHECK(subject_string(seg(3, "label")) == "Tool 4");
    CHECK(subject_string(seg(3, "sub")) == "T3");

    // Four 52 px segments: "Tool 1" does not fit, so every segment reads the
    // G-code name.
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));
    REQUIRE(ui::text_width("Tool 1", body) > subject_int("tool_switcher_cell_w") - pad);
    CHECK(subject_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Code));
    CHECK(subject_string(seg(0, "label")) == "T0");
    CHECK(subject_string(seg(3, "label")) == "T3");
    CHECK(subject_string(seg(0, "sub")) != "T0");
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: configured names read the G-code name where they do not fit",
                 "[widget_size][tool_switcher][1784][1769]") {
    // A backend that can dock a head, so the Park modes are open.
    ParkingBackend parking(/*tool_changer=*/true);
    configure_named_tools({"Left Extruder", "Right Extruder"});
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);

    // Two heads and Park in three cells on the 48 px floor.
    resize_content(h, 3 * 48 + 2 * 2, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    REQUIRE(subject_int("tool_switcher_cell_w") == 48);

    // "Left Extruder" does not fit, so every segment reads the G-code name its
    // head answers to, and that name fits the segment.
    const lv_font_t* body = theme_manager_get_font("font_body");
    const int pad = 2 * resolve_px("space_xs", 4);
    int room = subject_int("tool_switcher_cell_w") - pad;
    REQUIRE(ui::text_width("Left Extruder", body) > room);
    CHECK(subject_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Code));
    CHECK(subject_string(seg(0, "label")) == "T0");
    CHECK(subject_string(seg(1, "label")) == "T1");
    CHECK(ui::text_width(subject_string(seg(0, "label")).c_str(), body) <= room);
    CHECK(ui::text_width(subject_string(seg(1, "label")).c_str(), body) <= room);

    // Room for "Tool 1" is not room for the name the head is labeled with: the
    // widget measures the configured name, so the segments stay on the code.
    resize_content(h, 3 * 80 + 2 * 2, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    room = subject_int("tool_switcher_cell_w") - pad;
    REQUIRE(ui::text_width("Tool 1", body) <= room);
    REQUIRE(ui::text_width("Right Extruder", body) > room);
    CHECK(subject_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Code));
    CHECK(subject_string(seg(1, "label")) == "T1");

    // Where the name fits, the segment reads it over its G-code name.
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(ui::text_width("Right Extruder", body) <= subject_int("tool_switcher_cell_w") - pad);
    CHECK(subject_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Full));
    CHECK(subject_string(seg(0, "label")) == "Left Extruder");
    CHECK(subject_string(seg(0, "sub")) == "T0");
    CHECK(subject_string(seg(1, "label")) == "Right Extruder");
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a head's colour comes from its own slot, and an unknown one is "
                 "flagged rather than drawn",
                 "[widget_size][tool_switcher][1784]") {
    ParkingBackend parking(/*tool_changer=*/true);
    const SlotInfo slot0 = parking.backend->get_slot_info(0);
    REQUIRE(slot0.status != SlotStatus::EMPTY);
    REQUIRE(slot0.color_rgb != kAmsDefaultSlotColor);

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    ToolTopology topo = helix::test::make_head_topology(4);
    topo.backend_index = 0;
    // Head 3 has no slot: nothing knows what it holds.
    topo.tool_to_slot = {0, 1, 2, -1};
    ts.set_ams_topology(topo);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);

    CHECK(subject_int(seg(0, "has_color").c_str()) == 1);
    CHECK(subject_rgb(seg(0, "color")) == slot0.color_rgb);
    CHECK(subject_int(seg(3, "has_color").c_str()) == 0);

    // The mounted head's detail line names what it is loaded with.
    resize_content(h, 600, 400);
    process_lvgl(30);
    REQUIRE(subject_int("tool_switcher_detail") == 1);
    REQUIRE_FALSE(slot0.material.empty());
    CHECK(subject_string(detail(0, "text")) ==
          fmt::format(lv_tr("{} · {}"), "Tool 1 (T0)", slot0.material));
    CHECK(subject_int(detail(0, "has_color").c_str()) == 1);
    CHECK(subject_rgb(detail(0, "color")) == slot0.color_rgb);

    // A slot that runs empty keeps reporting its old colour; the head reads
    // as unknown, and the change reaches the published head on its own.
    REQUIRE(subject_int(seg(1, "has_color").c_str()) == 1);
    parking.backend->force_slot_status(1, SlotStatus::EMPTY);
    AmsState::instance().bump_slots_version();
    process_lvgl(30);
    REQUIRE(parking.backend->get_slot_info(1).color_rgb != kAmsDefaultSlotColor);
    CHECK(subject_int(seg(1, "has_color").c_str()) == 0);
}

TEST_CASE_METHOD(ToolSwitcherFixture, "tool_switcher: no filament system means no head colour",
                 "[widget_size][tool_switcher][1784]") {
    AmsState::instance().clear_backends();
    configure_tools(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(subject_int(seg(i, "has_color").c_str()) == 0);
    }
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a new head count republishes every pool at the cached size",
                 "[widget_size][tool_switcher][1784]") {
    // A backend that can dock a head, so the Park modes are open.
    ParkingBackend parking(/*tool_changer=*/true);
    configure_tools(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));
    REQUIRE(subject_string(seg(3, "label")) == "T3");
    REQUIRE(subject_string(seg(2, "label")) == "T2");

    // Fewer heads, with no size change: the verdict follows the count.
    update_tools(2, 0);
    process_lvgl(30);
    CHECK(lv_subject_get_int(ToolState::instance().get_tool_count_subject()) == 2);
    CHECK(mode() == static_cast<int>(ToolSwitcherMode::RailWithActions));
    CHECK(subject_string(seg(0, "label")) == expected_seg_label(0));
    CHECK(subject_string(seg(1, "label")) == expected_seg_label(1));

    // More heads: the new ones are filled.
    update_tools(6, 0);
    process_lvgl(30);
    CHECK(lv_subject_get_int(ToolState::instance().get_tool_count_subject()) == 6);
    CHECK(mode() == static_cast<int>(ToolSwitcherMode::Pair));
    CHECK(subject_string(seg(4, "label")) == expected_seg_label(4));
    CHECK(subject_string(seg(5, "label")) == expected_seg_label(5));

    // More heads than the rail draws: the pools grow to every head, the last
    // is filled, and the tile shows the face, whose picker lists them all.
    constexpr int kHeads = ToolSwitcherWidget::kMaxRailHeads + 4;
    update_tools(kHeads, 0);
    process_lvgl(30);
    resize_content(h, 2000, 400);
    process_lvgl(30);
    CHECK(mode() != static_cast<int>(ToolSwitcherMode::Rail));
    CHECK(mode() != static_cast<int>(ToolSwitcherMode::RailWithActions));
    CHECK(subject_int("tool_switcher_heads") == kHeads);
    CHECK(subject_int("tool_switcher_rail_heads") == 0);
    REQUIRE(lv_xml_get_subject(nullptr, seg(kHeads - 1, "label").c_str()) != nullptr);
    CHECK(subject_string(seg(kHeads - 1, "label")) == expected_seg_label(kHeads - 1));
}

// ============================================================================
// Detail rows
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: the detail block lists the mounted head first",
                 "[widget_size][tool_switcher][1784]") {
    configure_tools(4, /*active_index=*/0);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 600, 400);
    process_lvgl(30);
    REQUIRE(subject_int("tool_switcher_detail") == 1);

    // Mount head 2 with no size change.
    update_tools(4, 2);
    process_lvgl(30);
    CHECK(subject_string(detail(0, "text")).find("T2") != std::string::npos);
    CHECK(subject_int(detail(0, "main").c_str()) == 1);
    CHECK(subject_string(detail(1, "text")).rfind("T0", 0) == 0);
    CHECK(subject_int(detail(1, "main").c_str()) == 0);
    CHECK(subject_string(detail(2, "text")).rfind("T1", 0) == 0);
    CHECK(subject_string(detail(3, "text")).rfind("T3", 0) == 0);
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a head's temperature reaches its detail line and reads hot "
                 "from 150 C",
                 "[widget_size][tool_switcher][1784]") {
    configure_tools(2, /*active_index=*/0);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 600, 400);
    process_lvgl(30);
    REQUIRE(subject_int("tool_switcher_detail") == 1);
    REQUIRE(subject_string(detail(0, "text")).find("°C") == std::string::npos);

    // The extruder is discovered after the widget was built.
    state().temperature_state().init_extruders({"extruder"});
    process_lvgl(30);
    state().update_from_status(nlohmann::json{{"extruder", {{"temperature", 215.0}}}});
    process_lvgl(30);
    CHECK(subject_string(detail(0, "text")).find("215°C") != std::string::npos);
    CHECK(subject_int(detail(0, "hot").c_str()) == 1);

    state().update_from_status(nlohmann::json{{"extruder", {{"temperature", 149.0}}}});
    process_lvgl(30);
    CHECK(subject_string(detail(0, "text")).find("149°C") != std::string::npos);
    CHECK(subject_int(detail(0, "hot").c_str()) == 0);
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: an empty carriage leads the detail block with its own line",
                 "[widget_size][tool_switcher][1784]") {
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    ToolTopology topo = helix::test::make_head_topology(3, -1);
    topo.allows_empty_carriage = true;
    ts.set_ams_topology(topo);
    REQUIRE(ts.active_tool_index() == -1);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 600, 400);
    process_lvgl(30);

    CHECK(subject_string(detail(0, "text")) == lv_tr("Carriage empty · tap a head"));
    CHECK(subject_int(detail(0, "main").c_str()) == 1);
    CHECK(subject_int(detail(0, "has_color").c_str()) == 0);
    CHECK(subject_string(detail(1, "text")).rfind("T0", 0) == 0);
    CHECK(subject_int(detail(1, "main").c_str()) == 0);
    // The empty line and all three heads.
    CHECK(subject_int("tool_switcher_detail_rows") == 4);
}

// ============================================================================
// The face and the picker
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherFixture, "tool_switcher: the face marks an unknown mounted head",
                 "[widget_size][tool_switcher]") {
    // Klipper's toolchanger.tool_number is taken as reported, so a printer that
    // names a tool the discovery never listed leaves active_tool_index() past
    // the end of the tool list.
    configure_named_tools({"T0", "T1", "T2"});
    ToolState& ts = ToolState::instance();

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(2, 2, 60, 60);
    process_lvgl(30);
    REQUIRE(subject_string("tool_switcher_face_label") == "T0");

    ts.update_from_status(nlohmann::json{{"toolchanger", {{"tool_number", 7}}}});
    process_lvgl(30);
    REQUIRE(ts.active_tool_index() == 7);
    CHECK(subject_string("tool_switcher_face_label") == "?");
}

// ============================================================================
// #1769: the face and the picker name the tool the way G-code does
// ============================================================================

namespace {

/// The label a picker cell shows on its first line.
std::string picker_row_text(lv_obj_t* cell) {
    lv_obj_t* label = lv_obj_find_by_name(cell, "tool_switcher_seg_label");
    return label ? lv_label_get_text(label) : std::string();
}

/// The picker cell's second line.
std::string picker_row_sub(lv_obj_t* cell) {
    lv_obj_t* label = lv_obj_find_by_name(cell, "tool_switcher_seg_sub");
    return label ? lv_label_get_text(label) : std::string();
}

} // namespace

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a face too narrow for the full name reads the G-code name",
                 "[widget_size][tool_switcher][1769]") {
    // A face with no room for "Tool 2 (T1)" reads the T-number Klipper and
    // the slicer use; one with room reads both.
    configure_tools(3, /*active_index=*/1);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const int pad = 2 * resolve_px("space_xs", 4);
    lv_obj_t* label = h.child("tool_switcher_face_label");
    REQUIRE(label != nullptr);

    h.resize(2, 2, 60, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Single));
    REQUIRE(ui::text_width("Tool 2 (T1)", body) > subject_int("tool_switcher_cell_w") - pad);
    CHECK(subject_string("tool_switcher_face_label") == "T1");
    CHECK(std::string(lv_label_get_text(label)) == "T1");

    // Too short for a rail or a Pair, wide enough for the full name.
    resize_content(h, 280, 40);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Single));
    REQUIRE(ui::text_width("Tool 2 (T1)", body) <= subject_int("tool_switcher_cell_w") - pad);
    CHECK(std::string(lv_label_get_text(label)) == "Tool 2 (T1)");
}

TEST_CASE_METHOD(ToolSwitcherFixture, "tool_switcher: a configured tool name is the head's label",
                 "[widget_size][tool_switcher][1769]") {
    configure_named_tools({"Left", "Right"});
    REQUIRE(ToolState::instance().tools()[0].name == "Left");

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const int pad = 2 * resolve_px("space_xs", 4);

    // A wide rail: the name over the G-code name the head answers to.
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(subject_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Full));
    CHECK(subject_string(seg(0, "label")) == "Left");
    CHECK(subject_string(seg(0, "sub")) == "T0");
    CHECK(subject_string(seg(1, "label")) == "Right");
    CHECK(subject_string(seg(1, "sub")) == "T1");

    // A face with room reads both names.
    resize_content(h, 280, 40);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Single));
    REQUIRE(ui::text_width("Left (T0)", body) <= subject_int("tool_switcher_cell_w") - pad);
    CHECK(subject_string("tool_switcher_face_label") == "Left (T0)");

    // A face too narrow for "Left (T0)" reads the G-code name.
    h.resize(2, 2, 60, 60);
    process_lvgl(30);
    REQUIRE(ui::text_width("Left (T0)", body) > subject_int("tool_switcher_cell_w") - pad);
    CHECK(subject_string("tool_switcher_face_label") == "T0");
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: the picker cells read in the form every cell has room for",
                 "[widget_size][tool_switcher][1769]") {
    auto cells_after_opening = [&](PanelWidgetHarness<ToolSwitcherWidget>& h, size_t heads) {
        h.resize(1, 1, 60, 60);
        process_lvgl(30);
        ToolSwitcherTestAccess::show_picker(h.widget());
        lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
        REQUIRE(list != nullptr);
        REQUIRE(lv_obj_get_child_count(list) == heads);
        std::vector<std::pair<std::string, std::string>> lines;
        for (uint32_t i = 0; i < heads; ++i) {
            lv_obj_t* cell = lv_obj_get_child(list, static_cast<int32_t>(i));
            lines.emplace_back(picker_row_text(cell), picker_row_sub(cell));
        }
        ToolSwitcherTestAccess::hide_picker(h.widget());
        process_lvgl(30);
        return lines;
    };

    SECTION("generated names fit: the full form over two lines, whatever the tile") {
        configure_tools(3);
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        CHECK(cells_after_opening(h, 3) ==
              std::vector<std::pair<std::string, std::string>>{
                  {"Tool 1", "T0"}, {"Tool 2", "T1"}, {"Tool 3", "T2"}});
    }
    SECTION("one name too long for its cell: every cell reads the G-code name") {
        configure_named_tools({"Left Extruder", "Right Extruder"});
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        const auto lines = cells_after_opening(h, 2);
        CHECK(lines[0].first == "T0");
        CHECK(lines[1].first == "T1");
    }
}

// ============================================================================
// #1784: the picker offers Park only where the backend can dock a tool
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: the picker shows Park only where the backend can park",
                 "[widget_size][tool_switcher][1784]") {
    const bool tool_changer = GENERATE(true, false);
    ParkingBackend parking(tool_changer);
    configure_tools(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(2, 2, 60, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    lv_obj_t* park = lv_obj_find_by_name(lv_obj_get_parent(list), "tool_park_btn");
    REQUIRE(park != nullptr);
    CHECK(lv_obj_has_flag(park, LV_OBJ_FLAG_HIDDEN) == !tool_changer);
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a head's G-code name is the same on the rail and in the sheet",
                 "[widget_size][tool_switcher][1784]") {
    // A toolchanger whose tool numbers skip one: the second head answers to T2.
    configure_named_tools({"T0", "T2"});
    REQUIRE(ToolState::instance().tools()[1].name == "T2");
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);

    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(subject_int("tool_switcher_label_form") == static_cast<int>(ui::ToolLabelForm::Full));
    CHECK(subject_string(seg(1, "sub")) == "T2");

    // The detail block names the other heads by G-code name too.
    resize_content(h, 600, 400);
    process_lvgl(30);
    REQUIRE(subject_int("tool_switcher_detail_rows") >= 2);
    CHECK(subject_string(detail(1, "text")).rfind("T2", 0) == 0);

    // A one-cell tile: the segment-less face, whose picker names the head by
    // its G-code name too.
    h.resize(1, 1, 60, 60);
    process_lvgl(30);
    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    // The picker splits the full form like the rail: the name over "T2".
    CHECK(picker_row_sub(lv_obj_get_child(list, 1)) == "T2");
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}
