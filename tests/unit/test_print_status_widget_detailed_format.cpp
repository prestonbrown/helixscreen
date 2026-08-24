// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "print_history_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "tool_state.h"

#include <chrono>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

// RAII helper that creates the DetailedFormatter singleton without a real attach().
// Destroys any pre-existing formatter first so the new one's observers bind to
// the current PrinterState subjects (tests in this file reset PrinterState in
// their setup; a formatter from a prior test would hold dangling observer
// pointers to the freed subjects).
struct FormatterScope {
    FormatterScope() {
        PrintStatusWidget::destroy_formatter_for_test();
        PrintStatusWidget::ensure_formatter_for_test();
    }
    ~FormatterScope() {
        PrintStatusWidget::release_formatter_for_test();
    }
};

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter writes layer and time",
                 "[print_status][formatter]") {
    // Tear down any inherited formatter BEFORE resetting PrinterState — otherwise
    // its observers point to subjects that are about to be deinit'd/reinit'd, and
    // FormatterScope's later destroy walks a recycled lv_subject_t (macOS SIGSEGV).
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    // Real slicer layer fields — no "~" estimate marker.
    PrinterPrintStateTestAccess::set_has_real_layer_data(
        PrinterStateTestAccess::get_print_state(ps), true);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_layer_current_subject(), 42);
    lv_subject_set_int(ps.get_print_layer_total_subject(), 213);
    lv_subject_set_int(ps.get_print_elapsed_subject(), 42 * 60);              // 0h 42m
    lv_subject_set_int(ps.get_print_time_left_subject(), 2 * 3600 + 14 * 60); // 2h 14m

    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_layer_text"))) == "Layer 42 / 213");
    // elapsed=42m, total=42m+2h14m=2h56m. Sub-hour durations carry no "0h" —
    // helix::format::duration_padded drops the hours field below one hour.
    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_time_text"))) == "42m / 2h 56m");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter time text omits hours under an hour",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_elapsed_subject(), 45 * 60);
    lv_subject_set_int(ps.get_print_time_left_subject(), 0);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    // A hand-rolled "%dh %02dm" renders this as "0h 45m / 0h 45m".
    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_time_text"))) == "45m / 45m");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter filament text switches unit at 1000mm",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    // Below 1000mm the canonical formatter stays in millimetres. Dividing by
    // 1000 unconditionally renders this as "0.9m".
    lv_subject_set_int(ps.get_print_filament_used_subject(), 850);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_filament_text"))) == "Filament: 850mm");

    // Above 1000000mm it switches to kilometres rather than "1200.0m".
    lv_subject_set_int(ps.get_print_filament_used_subject(), 1200000);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_filament_text"))) == "Filament: 1.20km");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter seeds initial values on construction",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    PrinterPrintStateTestAccess::set_has_real_layer_data(
        PrinterStateTestAccess::get_print_state(ps), true);

    // Set subjects BEFORE creating formatter — seed calls in constructor pick them up
    lv_subject_set_int(ps.get_print_layer_current_subject(), 100);
    lv_subject_set_int(ps.get_print_layer_total_subject(), 200);

    FormatterScope fs;

    // No drain needed — seed calls are synchronous in the constructor
    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_layer_text"))) == "Layer 100 / 200");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter layer text omits total when zero",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    PrinterPrintStateTestAccess::set_has_real_layer_data(
        PrinterStateTestAccess::get_print_state(ps), true);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_layer_current_subject(), 7);
    lv_subject_set_int(ps.get_print_layer_total_subject(), 0);

    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_layer_text"))) == "Layer 7");
}

// The panel's richest layer path (on_print_layer_changed) marks progress-derived
// layers with "~" and appends the commanded Z height. The widget renders the same
// subjects and must produce the same string.
TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter layer text marks estimates and shows Z",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    // A freshly reset PrinterState has neither real layer data nor a Z-derived
    // layer, so layer_is_accurate() is false and the count is an estimate.
    REQUIRE(ps.layer_is_accurate() == false);

    lv_subject_set_int(ps.get_gcode_position_z_subject(), 2400); // 24.00mm
    lv_subject_set_int(ps.get_print_layer_current_subject(), 42);
    lv_subject_set_int(ps.get_print_layer_total_subject(), 213);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(lv_xml_get_subject(
                nullptr, "print_status_layer_text"))) == "Layer ~42 / 213 (24.0mm)");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter filament text empty when zero",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_filament_used_subject(), 0);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_filament_text"))) == "");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter filament text formatted in meters",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;

    lv_subject_set_int(ps.get_print_filament_used_subject(), 2500); // 2.5m
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_filament_text"))) == "Filament: 2.5m");
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter nozzle text (decidegree rounding)",
                 "[print_status][formatter][temps]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    FormatterScope fs;
    // Temp subjects store decidegrees (1 unit = 0.1°C; see L021 +
    // helix::units::to_decidegrees which multiplies by 10, not 100).
    lv_subject_set_int(ps.get_active_extruder_temp_subject(), 2157);   // 215.7°C → 216
    lv_subject_set_int(ps.get_active_extruder_target_subject(), 2200); // 220°C
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_nozzle_text"))) == "216 / 220°C");
    // bed_text / chamber_text are no longer formatted by the widget — the
    // XML's temp_display widgets bind directly to bed_temp / chamber_temp.
}

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter multi-extruder label and gate",
                 "[print_status][formatter][multi_tool]") {
    // Same hazard as the other tests, but for ToolState's subjects: tear down any
    // inherited formatter before re-initing the ones it observes.
    PrintStatusWidget::destroy_formatter_for_test();

    ToolState::instance().init_subjects(false);

    FormatterScope fs;
    auto& ts = ToolState::instance();

    // Driven through a real tool list, not a poked tool_count: the gate counts
    // hotends, and set_ams_topology() inflates the tool count to one entry per
    // filament lane on printers that have exactly one.
    auto discovery_with = [](const std::vector<std::string>& objects) {
        helix::PrinterDiscovery disc;
        disc.parse_objects(nlohmann::json(objects));
        return disc;
    };

    // Single extruder — no label, gate=0
    auto single = discovery_with({"extruder", "heater_bed", "fan"});
    ts.init_tools(single);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    REQUIRE(ts.extruder_count() == 1);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "print_status_multi_tool")) == 0);
    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_nozzle_tool_label"))) == "");

    // Two extruders — gate=1, label tracks active (default index = 0)
    auto dual = discovery_with({"extruder", "extruder1", "heater_bed", "fan"});
    ts.init_tools(dual);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    REQUIRE(ts.extruder_count() == 2);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "print_status_multi_tool")) == 1);
    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_nozzle_tool_label"))) == "T0");

    // Back to single — gate=0, label cleared
    ts.init_tools(single);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "print_status_multi_tool")) == 0);
    REQUIRE(std::string(lv_subject_get_string(
                lv_xml_get_subject(nullptr, "print_status_nozzle_tool_label"))) == "");
}

// =============================================================================
// Idle tile: which history job it describes
//
// Moonraker recomputes each job's `exists` flag per history request, so a job
// whose gcode has been deleted comes back with exists == false. The idle tile
// offers "Reprint Last" for whatever it names, so it must describe the newest
// job that can still be reprinted, not simply the newest job.
// =============================================================================

namespace {

PrintHistoryJob make_history_job(const char* filename, bool exists, double ended_secs_ago) {
    const double now =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    PrintHistoryJob job;
    job.filename = filename;
    job.exists = exists;
    job.status = PrintJobStatus::COMPLETED;
    job.start_time = now - ended_secs_ago - 3600.0;
    job.end_time = now - ended_secs_ago;
    job.duration_str = "1h 00m";
    job.filament_str = "12.5m";
    return job;
}

/// Installs a hand-built history for get_print_history_manager() and takes it
/// back down in the right order: the formatter deregisters its observer from
/// whatever this returns, so the global must outlive the FormatterScope.
struct ScopedHistory {
    explicit ScopedHistory(std::vector<PrintHistoryJob> jobs) : manager(nullptr, nullptr) {
        helix::PrintHistoryManagerTestAccess::set_loaded_jobs(manager, std::move(jobs));
        set_print_history_manager(&manager);
    }
    ~ScopedHistory() {
        set_print_history_manager(nullptr);
    }
    PrintHistoryManager manager;
};

std::string subject_text(const char* name) {
    return std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, name)));
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter idle tile skips a deleted newest print",
                 "[print_status][formatter][idle_exists]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    ScopedHistory history({make_history_job("deleted_yesterday.gcode", false, 60.0),
                           make_history_job("still_here.gcode", true, 2 * 3600.0)});

    {
        FormatterScope fs; // ctor populates the idle fields
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

        REQUIRE(subject_text("print_status_idle_filename") == "still_here.gcode");
        // Timing comes from the surviving job too, not the deleted head.
        REQUIRE(subject_text("print_status_idle_when") == "Completed 2h ago");
        REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "print_status_idle_has_last")) == 1);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "DetailedFormatter idle tile falls back to never-printed when nothing survives",
                 "[print_status][formatter][idle_exists]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    ScopedHistory history({make_history_job("deleted_a.gcode", false, 60.0),
                           make_history_job("deleted_b.gcode", false, 7200.0)});

    {
        FormatterScope fs;
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

        // Exactly the presentation used when there is no history at all.
        REQUIRE(subject_text("print_status_idle_filename").empty());
        REQUIRE(subject_text("print_status_idle_when") == "Never printed");
        REQUIRE(subject_text("print_status_idle_meta").empty());
        // print_status_detailed_idle.xml binds the Reprint Last button's
        // disabled state to this being 0.
        REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "print_status_idle_has_last")) == 0);
    }
}
