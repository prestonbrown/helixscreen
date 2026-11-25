// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offset_cal_vrtx_run.cpp
 * @brief How a VRTX_CALIBRATE run ends on the Tool Offsets panel.
 *
 * VRTX answers a refusal ("... Nothing was done.") with a warning echo and a successful
 * rpc, and keeps the offsets it measured in its own save file. So the panel fails the run
 * on the echo, and a finished run leaves no unsaved offsets to restart Klipper for.
 */

#include "ui_panel_calibration_tool_offset.h"

#include "../lvgl_test_fixture.h"
#include "../toolchanger_panel_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_offset_calibration.h"
#include "tool_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using nlohmann::json;

namespace {

using VrtxRunFixture = helix::test::ToolchangerPanelFixture<LVGLTestFixture>;

/// Pins a VRTX printer's discovery for the case and restores the previous one.
struct PinnedVrtx {
    helix::PrinterDiscovery previous;
    PinnedVrtx() : previous(get_printer_state().get_discovery()) {
        json objects = json::array({"toolchanger", "extruder", "heater_bed", "toolhead",
                                    "gcode_macro VRTX_CALIBRATE", "gcode_macro _VRTX_T"});
        for (int i = 0; i < 4; ++i) {
            objects.push_back("tool T" + std::to_string(i));
        }
        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        get_printer_state().set_hardware(std::move(hw));
    }
    ~PinnedVrtx() {
        get_printer_state().set_hardware(std::move(previous));
    }
};

} // namespace

TEST_CASE_METHOD(VrtxRunFixture, "VRTX run: a printed refusal fails the run with its text",
                 "[ui_integration][toolchanger][tool_offset_cal][vrtx]") {
    PinnedVrtx pin;
    helix::ui::init_tool_offset_row_handler(); // installs the progress sink the app installs
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    panel.begin_run();
    REQUIRE(panel.is_calibration_active());

    // The console line arrives while the macro runs; the rpc then answers ok.
    helix::tool_offset_calibration::report_progress_line(
        "// VRTX WARNING: Home all axes first. Nothing was done.");
    REQUIRE(pump_until([&] { return !panel.is_calibration_active(); }));

    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) ==
          "Home all axes first. Nothing was done.");
    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(VrtxRunFixture, "VRTX run: a finished run leaves nothing to save",
                 "[ui_integration][toolchanger][tool_offset_cal][vrtx]") {
    auto& tools = helix::ToolState::instance();
    tools.set_tool_offset_local(1, helix::Axis::X, -120);
    REQUIRE_FALSE(tools.dirty_tool_indices().empty());

    SECTION("VRTX keeps its own offsets") {
        PinnedVrtx pin;
        helix::ui::ToolOffsetCalibrationPanel panel;
        panel.init_subjects();
        panel.on_activate();
        panel.begin_run();
        REQUIRE(pump_until([&] { return !panel.is_calibration_active(); }));

        CHECK(tools.dirty_tool_indices().empty());
        CHECK(lv_subject_get_int(tools.get_any_tool_offset_dirty_subject()) == 0);
        panel.on_deactivate(DeactivateReason::NavigateAway);
        panel.cleanup();
    }
    SECTION("klipper-toolchanger's macro leaves them for SAVE_CONFIG") {
        helix::ui::ToolOffsetCalibrationPanel panel;
        panel.init_subjects();
        panel.on_activate();
        panel.begin_run();
        REQUIRE(pump_until([&] { return !panel.is_calibration_active(); }));

        CHECK_FALSE(tools.dirty_tool_indices().empty());
        panel.on_deactivate(DeactivateReason::NavigateAway);
        panel.cleanup();
    }
}
