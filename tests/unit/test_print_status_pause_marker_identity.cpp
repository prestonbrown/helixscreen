// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_pause_marker_identity.cpp
 * @brief A print-status gcode load only takes effect for the print
 *        PrinterState currently reports as effective (prestonbrown/helixscreen#1509).
 *
 * A gcode fetch crosses a metadata lookup, a download and the viewer's own
 * background build, and the print can change at any point along that chain.
 * Every stage compares the print it is fetching or loading against
 * PrinterState::get_effective_print_filename() before acting on it, and drops
 * the result instead of applying it when the two no longer match: the
 * currently-displayed print's geometry, gcode_displayed_file_ and its pause
 * markers are left exactly as they were, and ensure_preview_current()
 * reconciles against whichever print is effective by then.
 *
 * A load that DOES apply also leaves the running print's own state alone when
 * it turns out to be scoped wrong: the runout badge is not scoped to a file
 * the viewer no longer displays, and a stale file's layer count does not
 * become the print's.
 *
 * Every download is held by the transfer mock until the test releases it by
 * name, so two fetches can be made to land in either order.
 */

#include "ui_gcode_viewer.h"
#include "ui_panel_print_status.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/print_status_preview_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using print_status_preview_test::PrintStatusPreviewFixture;

namespace {

/// Carries scheduled pauses, so its load publishes a non-empty list.
constexpr const char* kPrintA = "pause_markers_demo.gcode";
/// Carries none.
constexpr const char* kPrintB = "xyz-10mm-calibration-cube.gcode";

} // namespace

TEST_CASE_METHOD(PrintStatusPreviewFixture,
                 "Print status: a gcode load for the current print shows its pause markers",
                 "[print_status][pause_markers][1509][slow]") {
    report_print(kPrintA);
    start_fetch(kPrintA);
    land(kPrintA);

    REQUIRE_FALSE(state_.print_state().get_scheduled_pauses().empty());
    CHECK(state_.print_state().pause_markers_match_current_file());
    CHECK(gcode_displayed_file() == kPrintA);
}

TEST_CASE_METHOD(PrintStatusPreviewFixture,
                 "Print status: a superseded print's gcode load cannot mark the new print",
                 "[print_status][pause_markers][1509][slow]") {
    report_print(kPrintA);
    start_fetch(kPrintA);

    // A is cancelled and B started while A's download is still running.
    report_print(kPrintB);
    start_fetch(kPrintB);

    SECTION("A lands while B is still downloading") {
        land_dropped(kPrintA);

        // A's load never reached the viewer: neither print has anything
        // displayed yet, its scan was never published, and the widget itself
        // holds no geometry - not just the panel's own bookkeeping.
        CHECK(state_.print_state().get_scheduled_pauses().empty());
        CHECK(gcode_displayed_file().empty());
        CHECK_FALSE(helix::ui::gcode_viewer_has_content(viewer_));

        land(kPrintB);

        // B's own load is the only one that ever applied: kPrintB carries no
        // pauses, and its displayed-file name is its own, not A's.
        CHECK(state_.print_state().get_scheduled_pauses().empty());
        CHECK(gcode_displayed_file() == kPrintB);
    }

    SECTION("A lands after B has loaded") {
        land(kPrintB);
        CHECK(gcode_displayed_file() == kPrintB);
        const int version_after_b = pause_markers_version();
        const char* widget_file_raw = helix::ui::gcode_viewer_get_filename(viewer_);
        const std::string widget_file_after_b = widget_file_raw ? widget_file_raw : "";

        land_dropped(kPrintA);

        // A's late load is dropped before it reaches the viewer: B's own
        // geometry and pause markers, published above, are untouched by it -
        // including the widget's own record of which file it holds, not just
        // the panel's copy of that name.
        CHECK(pause_markers_version() == version_after_b);
        CHECK(state_.print_state().get_scheduled_pauses().empty());
        CHECK(gcode_displayed_file() == kPrintB);
        widget_file_raw = helix::ui::gcode_viewer_get_filename(viewer_);
        CHECK((widget_file_raw ? std::string(widget_file_raw) : std::string()) ==
              widget_file_after_b);
    }
}

TEST_CASE_METHOD(PrintStatusPreviewFixture,
                 "Print status: a load that goes stale mid-flight is dropped when it is delivered",
                 "[print_status][pause_markers][1509][slow]") {
    report_print(kPrintA);
    start_fetch(kPrintA);

    // A is still the effective print when its download lands, so the entry
    // check passes and the viewer's own background build actually starts.
    REQUIRE(transfers_.release(kPrintA));
    drain();

    // A's build finished and its result is sitting in the UpdateQueue,
    // undelivered, when the print moves on. Nothing has fetched B's own gcode
    // yet, so the viewer's widget-level load generation never advances: only
    // the panel's own identity check, made when the result is delivered, can
    // catch this.
    REQUIRE(wait_for_queued_result(std::chrono::seconds(30)));

    nlohmann::json status = {{"print_stats", {{"filename", kPrintB}}}};
    state_.update_from_status(status);
    REQUIRE(state_.print_state().get_effective_print_filename() == kPrintB);

    drain();

    CHECK(gcode_displayed_file().empty());
    CHECK(state_.print_state().get_scheduled_pauses().empty());
}

namespace {

constexpr const char* kRunoutSensor = "filament_switch_sensor runout";
/// Uses tools 0-3 across nine layers, so a load of it has a tool scope and a
/// layer count to impose.
constexpr const char* kStalePrint = "u1_4color_ring.gcode";

/// The panel as a running print sees it: subjects up, the print reported as
/// printing, and one runout sensor with no filament system, so the print-scoped
/// runout badge takes its value from the tools of the file in the viewer.
class StaleLoadFixture : public PrintStatusPreviewFixture {
  public:
    StaleLoadFixture() {
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();

        auto& fsm = FilamentSensorManager::instance();
        fsm.init_subjects();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        discover_filament_sensors(fsm, {kRunoutSensor});
        fsm.set_role(kRunoutSensor, static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
        fsm.update_from_status(
            nlohmann::json{{kRunoutSensor, {{"filament_detected", true}, {"enabled", true}}}});

        panel_->init_subjects();
        drain();
    }

    ~StaleLoadFixture() override {
        PostUnloadGraceTestAccess::reset(FilamentSensorManager::instance());
    }

    void report_printing(const std::string& filename) {
        nlohmann::json status = {{"print_stats", {{"filename", filename}, {"state", "printing"}}}};
        state_.update_from_status(status);
        drain();
    }

    int scoped_runout() {
        return lv_subject_get_int(FilamentSensorManagerTestAccess::scoped_runout_subject(
            FilamentSensorManager::instance()));
    }

    int layer_total() {
        return lv_subject_get_int(state_.print_state().get_print_layer_total_subject());
    }
};

} // namespace

TEST_CASE_METHOD(StaleLoadFixture,
                 "Print status: a superseded print's gcode load leaves the new print's state alone",
                 "[print_status][pause_markers][1509][slow]") {
    report_printing(kStalePrint);
    start_fetch(kStalePrint);

    // The stale print is cancelled and B started while its download is running.
    report_printing(kPrintB);
    start_fetch(kPrintB);

    // B has no file in the viewer yet: no tools to scope the badge to, and no
    // layer count from metadata.
    REQUIRE(scoped_runout() == -1);
    REQUIRE(layer_total() == 0);

    land_dropped(kStalePrint);

    // The stale load is dropped before it reaches the viewer: nothing has
    // been displayed yet, for either print, and the widget itself holds no
    // geometry - not just the panel's own bookkeeping.
    REQUIRE(gcode_displayed_file().empty());
    CHECK_FALSE(helix::ui::gcode_viewer_has_content(viewer_));
    CHECK(scoped_runout() == -1);
    CHECK(layer_total() == 0);

    // B's own load does apply both, so the two checks above are the drop
    // holding the stale load back rather than effects that never run.
    land(kPrintB);
    CHECK(scoped_runout() == 1);
    CHECK(layer_total() > 0);
}

TEST_CASE_METHOD(
    StaleLoadFixture,
    "Print status: a runout-sensor edge does not scope the badge to a stale load's tools",
    "[print_status][pause_markers][1509][slow]") {
    // The stale print's own load lands while it is still the effective print,
    // so it applies normally: the viewer really does hold its geometry.
    report_printing(kStalePrint);
    start_fetch(kStalePrint);
    land(kStalePrint);
    REQUIRE(gcode_displayed_file() == kStalePrint);
    REQUIRE(scoped_runout() == 1);

    // The print moves on, but nothing has fetched B's own gcode yet: the
    // viewer's widget still physically holds the stale print's geometry
    // (tools 0-3), even though set_filename() cleared the displayed-file
    // marker to force a reload.
    report_printing(kPrintB);
    REQUIRE(gcode_displayed_file().empty());

    // A runout-sensor edge or an AMS slots_version bump recomputes the badge
    // independently of any load completing (the observers at
    // scoped_runout_observer_ / scoped_runout_slots_observer_ call
    // recompute_scoped_runout() directly). Without the guard this would read
    // the stale print's tools straight off the viewer.
    PrintStatusPanelTestAccess::recompute_scoped_runout(*panel_);
    CHECK(scoped_runout() == -1);

    // B's own load does apply the real value once it lands, so the check
    // above is the guard holding the badge back rather than a badge that
    // never updates.
    start_fetch(kPrintB);
    land(kPrintB);
    CHECK(scoped_runout() == 1);
}
