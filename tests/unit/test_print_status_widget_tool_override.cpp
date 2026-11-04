// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_observer_guard.h" // SubjectLifetime

#include "../helix_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "tool_state.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {
struct FormatterScope {
    FormatterScope() {
        PrintStatusWidget::destroy_formatter_for_test();
        PrintStatusWidget::ensure_formatter_for_test();
    }
    ~FormatterScope() {
        PrintStatusWidget::release_formatter_for_test();
    }
};
} // namespace

TEST_CASE_METHOD(HelixTestFixture, "Tool override: pinned reads per-tool subject",
                 "[print_status][tool_override]") {
    // Reset the singleton formatter first — any prior test's observers point
    // at PrinterState subjects we're about to destroy.
    PrintStatusWidget::destroy_formatter_for_test();
    auto& ts = ToolState::instance();
    ts.init_subjects(false);
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    // Register extruder + extruder1 so dynamic subjects exist
    ps.temperature_state().init_extruders({"extruder", "extruder1"});

    lv_subject_set_int(ts.get_tool_count_subject(), 2);
    UpdateQueue::instance().drain_until_empty();

    FormatterScope fs;

    // Seed per-tool and active-extruder subjects with distinct values
    SubjectLifetime lt;
    auto* e1_temp = ps.temperature_state().get_extruder_temp_subject("extruder1", lt);
    REQUIRE(e1_temp != nullptr);
    lv_subject_set_int(e1_temp, 25000); // 250 °C
    lv_subject_set_int(ps.temperature_state().get_active_extruder_temp_subject(),
                       19000); // 190 °C (distinct)
    UpdateQueue::instance().drain_until_empty();

    // Pin the formatter to extruder1
    PrintStatusWidget::set_nozzle_tool_override_for_test("extruder1");
    UpdateQueue::instance().drain_until_empty();

    auto* nozzle_sub = lv_xml_get_subject(nullptr, "print_status_nozzle_text");
    REQUIRE(nozzle_sub != nullptr);
    // Should read extruder1 (250°C), not active_extruder (190°C)
    REQUIRE(std::string(lv_subject_get_string(nozzle_sub)).find("250") != std::string::npos);
}

TEST_CASE_METHOD(HelixTestFixture, "Tool override: stale pin falls back to auto",
                 "[print_status][tool_override]") {
    PrintStatusWidget::destroy_formatter_for_test();
    auto& ts = ToolState::instance();
    ts.init_subjects(false);
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    lv_subject_set_int(ts.get_tool_count_subject(), 1);
    UpdateQueue::instance().drain_until_empty();

    FormatterScope fs;

    // Pin to a ghost extruder that doesn't exist — should fall back to auto
    PrintStatusWidget::set_nozzle_tool_override_for_test("extruder7_ghost");
    lv_subject_set_int(ps.temperature_state().get_active_extruder_temp_subject(), 12345); // 123 °C
    UpdateQueue::instance().drain_until_empty();

    auto* nozzle_sub = lv_xml_get_subject(nullptr, "print_status_nozzle_text");
    REQUIRE(nozzle_sub != nullptr);
    // Auto fallback reads active_extruder (123°C)
    REQUIRE(std::string(lv_subject_get_string(nozzle_sub)).find("123") != std::string::npos);
}

// ============================================================================
// The multi-tool gate counts HOTENDS, not lanes
//
// set_ams_topology() expands ToolState's tool list to one entry per filament
// slot, so tool_count() on a 4-lane AMS or a 16-wide AD5X tool map says nothing
// about how many nozzles the machine has. Driving print_status_multi_tool from
// it stamped a "Slot 1" badge and a tool-picker chevron onto single-hotend printers.
// ============================================================================

namespace {

helix::PrinterDiscovery discovery_with(const std::vector<std::string>& objects) {
    helix::PrinterDiscovery disc;
    disc.parse_objects(nlohmann::json(objects));
    return disc;
}

// The chevron is the click target's last child, after the numbered nozzle icon.
lv_obj_t* nozzle_chevron(lv_obj_t* target) {
    const uint32_t children = lv_obj_get_child_count(target);
    REQUIRE(children == 2);
    return lv_obj_get_child(target, 1);
}

/// Reset ToolState + PrinterState and destroy any formatter left bound to the
/// subjects we are about to replace.
void reset_state_for_tool_gate(helix::ToolState& ts, PrinterState& ps) {
    PrintStatusWidget::destroy_formatter_for_test();
    ts.init_subjects(false);
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
}

helix::ToolTopology lane_topology(int lanes) {
    helix::ToolTopology topo;
    topo.tool_count = lanes;
    topo.active_tool = 0;
    topo.tool_to_slot.resize(static_cast<size_t>(lanes));
    for (int i = 0; i < lanes; ++i)
        topo.tool_to_slot[static_cast<size_t>(i)] = i;
    return topo;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "Tool badge: many AMS lanes on one hotend are not multi-tool",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);

    auto disc = discovery_with({"extruder", "heater_bed", "fan"});
    ts.init_tools(disc);
    UpdateQueue::instance().drain_until_empty();

    FormatterScope fs;

    ts.set_ams_topology(lane_topology(12));
    UpdateQueue::instance().drain_until_empty();

    // The lane count really did land — this is the value the gate used to read.
    REQUIRE(ts.tool_count() == 12);
    REQUIRE(ts.extruder_count() == 1);

    auto* multi = lv_xml_get_subject(nullptr, "print_status_multi_tool");
    REQUIRE(multi != nullptr);
    REQUIRE(lv_subject_get_int(multi) == 0);

    auto* label = lv_xml_get_subject(nullptr, "print_status_nozzle_badge_text");
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_subject_get_string(label)).empty());

    ts.clear_ams_topology();
    UpdateQueue::instance().drain_until_empty();
}

TEST_CASE_METHOD(HelixTestFixture, "Tool badge: a genuine second extruder still gets the badge",
                 "[print_status][tool_override]") {
    // Paired with the case above: a gate hardcoded to 0 would pass that one.
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);

    auto disc = discovery_with({"extruder", "extruder1", "heater_bed", "fan"});
    ts.init_tools(disc);
    UpdateQueue::instance().drain_until_empty();

    FormatterScope fs;

    REQUIRE(ts.extruder_count() == 2);

    auto* multi = lv_xml_get_subject(nullptr, "print_status_multi_tool");
    REQUIRE(multi != nullptr);
    REQUIRE(lv_subject_get_int(multi) == 1);

    auto* label = lv_xml_get_subject(nullptr, "print_status_nozzle_badge_text");
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_subject_get_string(label)) == "1");

    // Two heaters: the readout can be pinned to either.
    auto* pickable = lv_xml_get_subject(nullptr, "print_status_nozzle_pickable");
    REQUIRE(pickable != nullptr);
    CHECK(lv_subject_get_int(pickable) == 1);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "Tool badge: nozzles on one shared heater name the mounted one, unpickable",
                 "[print_status][tool_override][indx]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);

    auto disc = discovery_with({"extruder", "heater_bed", "fan"});
    ts.init_tools(disc);
    UpdateQueue::instance().drain_until_empty();

    FormatterScope fs;

    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    topo.allows_empty_carriage = true;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_extruder = {"extruder", "extruder", "extruder", "extruder"};
    topo.heads_share_heater = true;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain_until_empty();

    auto* multi = lv_xml_get_subject(nullptr, "print_status_multi_tool");
    REQUIRE(multi != nullptr);
    CHECK(lv_subject_get_int(multi) == 1);
    auto* label = lv_xml_get_subject(nullptr, "print_status_nozzle_badge_text");
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_subject_get_string(label)) == "3");
    // One heater: nothing to pin the readout to.
    auto* pickable = lv_xml_get_subject(nullptr, "print_status_nozzle_pickable");
    REQUIRE(pickable != nullptr);
    CHECK(lv_subject_get_int(pickable) == 0);

    ts.clear_ams_topology();
    UpdateQueue::instance().drain_until_empty();
}

// ============================================================================
// The nozzle picker lists extruders that exist
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "Nozzle picker: rows come from the discovered extruders",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);
    ps.temperature_state().init_extruders({"extruder", "extruder1"});

    auto disc = discovery_with({"extruder", "extruder1", "heater_bed"});
    ts.init_tools(disc);
    // A 6-lane AMS on top: the picker must not turn lanes into nozzles.
    ts.set_ams_topology(lane_topology(6));
    UpdateQueue::instance().drain_until_empty();
    REQUIRE(ts.tool_count() == 6);

    auto options = PrintStatusWidget::build_nozzle_tool_options(ps.temperature_state().extruders());
    REQUIRE(options.size() == 2);
    REQUIRE(options[0].extruder_name == "extruder");
    REQUIRE(options[1].extruder_name == "extruder1");
    REQUIRE_FALSE(options[0].label.empty());
    REQUIRE_FALSE(options[1].label.empty());
    REQUIRE(options[0].label != options[1].label);

    ts.clear_ams_topology();
    UpdateQueue::instance().drain_until_empty();
}

TEST_CASE_METHOD(HelixTestFixture, "Nozzle picker: no discovered extruders means no rows",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);

    REQUIRE(
        PrintStatusWidget::build_nozzle_tool_options(ps.temperature_state().extruders()).empty());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "Nozzle picker: a lone extruder with no assigned display name reads Nozzle",
                 "[print_status][tool_override]") {
    // A bare "extruder" key by itself carries no signal about whether the printer
    // has one nozzle or several, so this exercises build_nozzle_tool_options()
    // directly rather than through PrinterState::init_extruders(), which would
    // have already populated display_name and skipped this fallback entirely.
    std::unordered_map<std::string, ExtruderInfo> extruders;
    extruders["extruder"];
    auto options = PrintStatusWidget::build_nozzle_tool_options(extruders);
    REQUIRE(options.size() == 1);
    CHECK(options[0].label == "Nozzle");
}

TEST_CASE_METHOD(
    HelixTestFixture,
    "Nozzle picker: a second extruder with no assigned display name still gets a number",
    "[print_status][tool_override]") {
    std::unordered_map<std::string, ExtruderInfo> extruders;
    extruders["extruder"];
    extruders["extruder1"];
    auto options = PrintStatusWidget::build_nozzle_tool_options(extruders);
    REQUIRE(options.size() == 2);
    CHECK(options[0].label == "Nozzle");
    CHECK(options[1].label == "Nozzle 2");
}

// ============================================================================
// A rejected pin is not written to the widget's config
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "Nozzle picker: a rejected override is not persisted",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);
    ps.temperature_state().init_extruders({"extruder", "extruder1"});
    UpdateQueue::instance().drain_until_empty();

    FormatterScope fs;
    PrintStatusWidget w;
    nlohmann::json cfg = {{"layout_style", "detailed"}};
    w.set_config(cfg);

    // A pin the formatter can bind is adopted and recorded.
    REQUIRE(w.apply_nozzle_tool_override("extruder1"));
    REQUIRE(w.nozzle_tool_override_for_test() == "extruder1");
    REQUIRE(w.config_for_test().value("nozzle_tool_override", std::string{}) == "extruder1");

    // A pin naming an extruder this printer does not have is refused, and the
    // widget records the fallback it actually applied rather than the ghost.
    REQUIRE_FALSE(w.apply_nozzle_tool_override("extruder9"));
    REQUIRE(w.nozzle_tool_override_for_test() == "auto");
    REQUIRE(w.config_for_test().value("nozzle_tool_override", std::string{}) == "auto");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Detailed card: a shared heater names the nozzle but offers no picker chevron",
                 "[print_status][tool_override][indx]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);
    ts.init_tools(discovery_with({"extruder", "heater_bed", "fan"}));
    UpdateQueue::instance().drain_until_empty();
    FormatterScope fs;

    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    topo.allows_empty_carriage = true;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_extruder = {"extruder", "extruder", "extruder", "extruder"};
    topo.heads_share_heater = true;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain_until_empty();

    REQUIRE(lv_xml_register_component_from_file(
                "A:ui_xml/components/print_status_detailed_active.xml") == LV_RESULT_OK);
    auto* card = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "print_status_detailed_active", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_t* target = lv_obj_find_by_name(card, "detailed_nozzle_click_target");
    REQUIRE(target != nullptr);
    lv_obj_t* number = lv_obj_find_by_name(target, "tool_badge");
    REQUIRE(number != nullptr);
    CHECK_FALSE(lv_obj_has_flag(number, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(number)) == "3");
    CHECK(lv_obj_has_flag(nozzle_chevron(target), LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(card);
    ts.clear_ams_topology();
    UpdateQueue::instance().drain_until_empty();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Detailed card: the picker goes where the row has no room",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);
    ts.init_tools(discovery_with({"extruder", "extruder1", "heater_bed", "fan"}));
    UpdateQueue::instance().drain_until_empty();
    FormatterScope fs;

    REQUIRE(lv_xml_register_component_from_file(
                "A:ui_xml/components/print_status_detailed_active.xml") == LV_RESULT_OK);
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_set_height(host, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(host, 0, LV_PART_MAIN);
    auto* card =
        static_cast<lv_obj_t*>(lv_xml_create(host, "print_status_detailed_active", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_t* target = lv_obj_find_by_name(card, "detailed_nozzle_click_target");
    REQUIRE(target != nullptr);
    lv_obj_t* chevron = nozzle_chevron(target);
    auto* fits = lv_xml_get_subject(nullptr, "print_status_nozzle_picker_fits");
    REQUIRE(fits != nullptr);

    PrintStatusWidget w;
    lv_obj_set_width(host, 600);
    PrintStatusWidgetTestAccess::fit_nozzle_picker(w, host);
    CHECK(lv_subject_get_int(fits) == 1);
    CHECK_FALSE(lv_obj_has_flag(chevron, LV_OBJ_FLAG_HIDDEN));
    CHECK(PrintStatusWidgetTestAccess::nozzle_picker_available(w));

    lv_obj_set_width(host, 120);
    PrintStatusWidgetTestAccess::fit_nozzle_picker(w, host);
    CHECK(lv_subject_get_int(fits) == 0);
    CHECK(lv_obj_has_flag(chevron, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(PrintStatusWidgetTestAccess::nozzle_picker_available(w));

    // Room for every reading but not the chevron: hiding the chevron must not
    // read as room for it.
    lv_obj_t* row = lv_obj_find_by_name(card, "detailed_temp_row");
    REQUIRE(row != nullptr);
    lv_obj_update_layout(host);
    int without_chevron = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        without_chevron += lv_obj_get_width(lv_obj_get_child(row, static_cast<int32_t>(i)));
    }
    without_chevron += lv_obj_get_style_pad_column(row, LV_PART_MAIN) *
                       (static_cast<int>(lv_obj_get_child_count(row)) - 1);
    const int frame = lv_obj_get_width(host) - lv_obj_get_content_width(row);
    lv_obj_set_width(host, without_chevron + frame + 1);
    lv_obj_update_layout(host);
    REQUIRE(lv_obj_get_content_width(row) > without_chevron);
    PrintStatusWidgetTestAccess::fit_nozzle_picker(w, host);
    CHECK(lv_subject_get_int(fits) == 0);

    lv_obj_delete(host);
}

namespace {

// Builds the detailed card with the given texts and checks that the named row
// drops its optional piece exactly when the row is one pixel short of it.
void check_row_drops_piece(lv_obj_t* screen, const char* row_name, const char* piece_name,
                           const char* fits_name) {
    REQUIRE(lv_xml_register_component_from_file(
                "A:ui_xml/components/print_status_detailed_active.xml") == LV_RESULT_OK);
    lv_obj_t* host = lv_obj_create(screen);
    lv_obj_set_size(host, 600, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(host, 0, LV_PART_MAIN);
    auto* card =
        static_cast<lv_obj_t*>(lv_xml_create(host, "print_status_detailed_active", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(card, row_name);
    REQUIRE(row != nullptr);
    lv_obj_t* piece = lv_obj_find_by_name(row, piece_name);
    REQUIRE(piece != nullptr);
    auto* fits = lv_xml_get_subject(nullptr, fits_name);
    REQUIRE(fits != nullptr);

    PrintStatusWidget w;
    lv_obj_set_width(row, 400);
    PrintStatusWidgetTestAccess::fit_row(w, host, row_name, fits);
    CHECK(lv_subject_get_int(fits) == 1);
    CHECK_FALSE(lv_obj_has_flag(piece, LV_OBJ_FLAG_HIDDEN));

    // The row as laid out with every piece shown.
    lv_obj_update_layout(row);
    int whole = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        whole += lv_obj_get_width(lv_obj_get_child(row, static_cast<int32_t>(i)));
    }
    whole += lv_obj_get_style_pad_column(row, LV_PART_MAIN) *
             (static_cast<int>(lv_obj_get_child_count(row)) - 1);

    lv_obj_set_width(row, whole - 1);
    PrintStatusWidgetTestAccess::fit_row(w, host, row_name, fits);
    CHECK(lv_subject_get_int(fits) == 0);
    CHECK(lv_obj_has_flag(piece, LV_OBJ_FLAG_HIDDEN));

    // Hiding the piece must not read as room for it.
    PrintStatusWidgetTestAccess::fit_row(w, host, row_name, fits);
    CHECK(lv_subject_get_int(fits) == 0);

    lv_obj_set_width(row, whole);
    PrintStatusWidgetTestAccess::fit_row(w, host, row_name, fits);
    CHECK(lv_subject_get_int(fits) == 1);

    lv_obj_delete(host);
}

struct FormatterTexts {
    FormatterTexts() {
        PrintStatusWidget::destroy_formatter_for_test();
        PrinterState& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        PrintStatusWidget::ensure_formatter_for_test();
    }
    ~FormatterTexts() {
        PrintStatusWidget::release_formatter_for_test();
    }
    static void set(const char* subject, const char* text) {
        lv_subject_copy_string(lv_xml_get_subject(nullptr, subject), text);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Detailed card: the layer Z goes where the row has no room",
                 "[print_status][formatter]") {
    FormatterTexts texts;
    FormatterTexts::set("print_status_layer_text", "120 / 240");
    FormatterTexts::set("print_status_layer_z_text", "· 24.0mm");
    check_row_drops_piece(test_screen(), "detailed_layer_row", "detailed_layer_z",
                          "print_status_layer_z_fits");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Detailed card: the elapsed time goes where the row has no room",
                 "[print_status][formatter]") {
    FormatterTexts texts;
    FormatterTexts::set("print_status_elapsed_text", "1h 14m");
    FormatterTexts::set("print_status_remaining_text", "35m");
    check_row_drops_piece(test_screen(), "detailed_time_row", "detailed_elapsed_group",
                          "print_status_elapsed_fits");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Detailed card: the flow goes where the filament row has no room",
                 "[print_status][formatter]") {
    FormatterTexts texts;
    FormatterTexts::set("print_status_filament_text", "1.20km");
    FormatterTexts::set("print_status_flow_text", "· 12.0 mm\xC2\xB3/s");
    REQUIRE(lv_xml_register_component_from_file(
                "A:ui_xml/components/print_status_detailed_active.xml") == LV_RESULT_OK);
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_set_size(host, 600, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(host, 0, LV_PART_MAIN);
    auto* card =
        static_cast<lv_obj_t*>(lv_xml_create(host, "print_status_detailed_active", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(card, "detailed_filament_row");
    lv_obj_t* flow = lv_obj_find_by_name(card, "detailed_flow_text");
    REQUIRE(row != nullptr);
    REQUIRE(flow != nullptr);
    auto* fits = lv_xml_get_subject(nullptr, "print_status_flow_fits");
    REQUIRE(fits != nullptr);

    // The filament label stretches to fill the row, so room is judged from the
    // texts, not from the laid-out widths.
    PrintStatusWidget w;
    lv_obj_set_width(row, 400);
    PrintStatusWidgetTestAccess::fit_row(w, host, "detailed_filament_row", fits);
    CHECK(lv_subject_get_int(fits) == 1);
    CHECK_FALSE(lv_obj_has_flag(flow, LV_OBJ_FLAG_HIDDEN));

    lv_obj_set_width(row, 60);
    PrintStatusWidgetTestAccess::fit_row(w, host, "detailed_filament_row", fits);
    CHECK(lv_subject_get_int(fits) == 0);
    CHECK(lv_obj_has_flag(flow, LV_OBJ_FLAG_HIDDEN));

    // Hiding the flow must not read as room for it.
    PrintStatusWidgetTestAccess::fit_row(w, host, "detailed_filament_row", fits);
    CHECK(lv_subject_get_int(fits) == 0);

    lv_obj_set_width(row, 400);
    PrintStatusWidgetTestAccess::fit_row(w, host, "detailed_filament_row", fits);
    CHECK(lv_subject_get_int(fits) == 1);

    lv_obj_delete(host);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Detailed card: the speed row goes where the column is short",
                 "[print_status][formatter]") {
    FormatterTexts texts;
    FormatterTexts::set("print_status_speed_text", "187 mm/s");
    REQUIRE(lv_xml_register_component_from_file(
                "A:ui_xml/components/print_status_detailed_active.xml") == LV_RESULT_OK);
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_set_size(host, 600, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(host, 0, LV_PART_MAIN);
    auto* card =
        static_cast<lv_obj_t*>(lv_xml_create(host, "print_status_detailed_active", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_t* col = lv_obj_find_by_name(card, "detailed_data_col");
    lv_obj_t* row = lv_obj_find_by_name(card, "detailed_speed_row");
    REQUIRE(col != nullptr);
    REQUIRE(row != nullptr);
    auto* fits = lv_xml_get_subject(nullptr, "print_status_speed_fits");
    REQUIRE(fits != nullptr);
    // A fresh card shows the row until a measurement says otherwise.
    CHECK(lv_subject_get_int(fits) == 1);

    // The card takes its height from a flex parent in the app; here it is set directly.
    PrintStatusWidget w;
    lv_obj_set_height(card, 300);
    PrintStatusWidgetTestAccess::fit_speed_row(w, host);
    CHECK(lv_subject_get_int(fits) == 1);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // Too short for the speed row beside the others.
    lv_obj_set_height(card, 90);
    PrintStatusWidgetTestAccess::fit_speed_row(w, host);
    CHECK(lv_subject_get_int(fits) == 0);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    lv_obj_set_height(card, 300);
    PrintStatusWidgetTestAccess::fit_speed_row(w, host);
    CHECK(lv_subject_get_int(fits) == 1);

    lv_obj_delete(host);
}
