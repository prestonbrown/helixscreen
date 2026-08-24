// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_observer_guard.h" // SubjectLifetime

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

struct FormatterScope {
    FormatterScope() {
        PrintStatusWidget::destroy_formatter_for_test();
        PrintStatusWidget::ensure_formatter_for_test();
    }
    ~FormatterScope() {
        PrintStatusWidget::release_formatter_for_test();
    }
};

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
    ps.init_extruders({"extruder", "extruder1"});

    lv_subject_set_int(ts.get_tool_count_subject(), 2);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    FormatterScope fs;

    // Seed per-tool and active-extruder subjects with distinct values
    SubjectLifetime lt;
    auto* e1_temp = ps.get_extruder_temp_subject("extruder1", lt);
    REQUIRE(e1_temp != nullptr);
    lv_subject_set_int(e1_temp, 25000);                               // 250 °C
    lv_subject_set_int(ps.get_active_extruder_temp_subject(), 19000); // 190 °C (distinct)
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    // Pin the formatter to extruder1
    PrintStatusWidget::set_nozzle_tool_override_for_test("extruder1");
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

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
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    FormatterScope fs;

    // Pin to a ghost extruder that doesn't exist — should fall back to auto
    PrintStatusWidget::set_nozzle_tool_override_for_test("extruder7_ghost");
    lv_subject_set_int(ps.get_active_extruder_temp_subject(), 12345); // 123 °C
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

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
// it stamped a "T0" badge and a tool-picker chevron onto single-hotend printers.
// ============================================================================

namespace {

helix::PrinterDiscovery discovery_with(const std::vector<std::string>& objects) {
    helix::PrinterDiscovery disc;
    disc.parse_objects(nlohmann::json(objects));
    return disc;
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
    topo.tool_name_prefix = "T";
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
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    FormatterScope fs;

    ts.set_ams_topology(lane_topology(12));
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    // The lane count really did land — this is the value the gate used to read.
    REQUIRE(ts.tool_count() == 12);
    REQUIRE(ts.extruder_count() == 1);

    auto* multi = lv_xml_get_subject(nullptr, "print_status_multi_tool");
    REQUIRE(multi != nullptr);
    REQUIRE(lv_subject_get_int(multi) == 0);

    auto* label = lv_xml_get_subject(nullptr, "print_status_nozzle_tool_label");
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_subject_get_string(label)).empty());

    ts.clear_ams_topology();
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
}

TEST_CASE_METHOD(HelixTestFixture, "Tool badge: a genuine second extruder still gets the badge",
                 "[print_status][tool_override]") {
    // Paired with the case above: a gate hardcoded to 0 would pass that one.
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);

    auto disc = discovery_with({"extruder", "extruder1", "heater_bed", "fan"});
    ts.init_tools(disc);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    FormatterScope fs;

    REQUIRE(ts.extruder_count() == 2);

    auto* multi = lv_xml_get_subject(nullptr, "print_status_multi_tool");
    REQUIRE(multi != nullptr);
    REQUIRE(lv_subject_get_int(multi) == 1);

    auto* label = lv_xml_get_subject(nullptr, "print_status_nozzle_tool_label");
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_subject_get_string(label)) == "T0");
}

// ============================================================================
// The nozzle picker lists extruders that exist
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "Nozzle picker: rows come from the discovered extruders",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);
    ps.init_extruders({"extruder", "extruder1"});

    auto disc = discovery_with({"extruder", "extruder1", "heater_bed"});
    ts.init_tools(disc);
    // A 6-lane AMS on top: the picker must not turn lanes into nozzles.
    ts.set_ams_topology(lane_topology(6));
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    REQUIRE(ts.tool_count() == 6);

    auto options = PrintStatusWidget::build_nozzle_tool_options(ps.temperature_state().extruders());
    REQUIRE(options.size() == 2);
    REQUIRE(options[0].extruder_name == "extruder");
    REQUIRE(options[1].extruder_name == "extruder1");
    REQUIRE_FALSE(options[0].label.empty());
    REQUIRE_FALSE(options[1].label.empty());
    REQUIRE(options[0].label != options[1].label);

    ts.clear_ams_topology();
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
}

TEST_CASE_METHOD(HelixTestFixture, "Nozzle picker: no discovered extruders means no rows",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);

    REQUIRE(
        PrintStatusWidget::build_nozzle_tool_options(ps.temperature_state().extruders()).empty());
}

// ============================================================================
// A rejected pin is not written to the widget's config
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "Nozzle picker: a rejected override is not persisted",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    PrinterState& ps = get_printer_state();
    reset_state_for_tool_gate(ts, ps);
    ps.init_extruders({"extruder", "extruder1"});
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

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
