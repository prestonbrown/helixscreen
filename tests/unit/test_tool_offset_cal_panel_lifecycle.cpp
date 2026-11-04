// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offset_cal_panel_lifecycle.cpp
 * @brief A calibration run outlives the screen it was started from.
 *
 * The macro blocks Klipper for minutes and the only stop is M112, so pressing
 * Back must not end the run - and must not lose its completion either.
 * OverlayBase expires lifetime_ on every on_deactivate(); the run's callbacks
 * ride on a guard that Stop, cleanup() and destruction expire instead.
 */

#include "ui_guided_device_flow.h"
#include "ui_panel_calibration_tool_offset.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../toolchanger_panel_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_error.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "snapmaker_nozzle_offset.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"

using nlohmann::json;

namespace {

/// No widgets: these cases drive the run and its subjects directly.
using ToolCalPanelFixture = helix::test::ToolchangerPanelFixture<LVGLTestFixture>;

} // namespace

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: a run finishes after Back",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();

    panel.begin_run();
    REQUIRE(panel.is_calibration_active());
    REQUIRE(lv_subject_get_int(panel.get_active_subject()) == 1);

    // Back, mid-run. The base expires lifetime_ here.
    panel.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(panel.is_calibration_active());

    // The macro runs to completion on the printer while the panel is hidden,
    // and its completion still lands: nothing is left reading "active".
    REQUIRE(pump_until([&] { return !panel.is_calibration_active(); }));
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) ==
          "Calibration complete - save to keep the offsets");

    panel.on_activate(); // back on screen: the rows repaint from ToolState
    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: Stop drops the run's callbacks",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // Stop is M112 + FIRMWARE_RESTART; the rpc then fails with the shutdown,
    // and that failure must not be reported as the run's own.
    //
    // A second run is what makes run_lifetime_.invalidate() load-bearing: with
    // no run in flight, both completion handlers return on !run_active_ and the
    // guard is never consulted. Start one, and the stopped run's still-pending
    // rpc has a live run to land on.
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();

    panel.begin_run();
    REQUIRE(panel.is_calibration_active());
    // Let the stopped run get most of the way through the mock's sim (2 ticks
    // per tool plus a park, 600 ms each = 5.4 s for four tools) so its rpc
    // answers well before the second run's does.
    pump_until([] { return false; }, 40);
    REQUIRE(panel.is_calibration_active());

    REQUIRE(panel.abort_in_progress_calibration());
    CHECK_FALSE(panel.is_calibration_active());
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) == "Stopped");

    panel.begin_run();
    REQUIRE(panel.is_calibration_active());

    // Past the stopped run's completion (5.4 s), short of the second run's
    // (9.4 s): the callback that lands here belongs to the run Stop ended.
    pump_until([] { return false; }, 30);
    CHECK(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) == "Calibrating...");

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "tool offset panel: an rpc timeout under a busy printer waits for the idle edge",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // Moonraker never times out printer.gcode.script; our ceiling firing while
    // Klipper still reports idle_timeout "Printing" means the macro is still
    // running. Failing the run there re-enabled Save under a blocked queue.
    helix::PrinterState& ps = get_printer_state();
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    panel.begin_run();
    REQUIRE(panel.is_calibration_active());

    ps.update_from_status(json{{"idle_timeout", json{{"state", "Printing"}}}});
    helix::ui::UpdateQueue::instance().drain();
    panel.on_run_rpc_error(MoonrakerError::timeout("printer.gcode.script", 1));
    helix::ui::UpdateQueue::instance().drain();

    // Still a run: Save stays disabled, and the status says why.
    CHECK(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) ==
          "Calibration may still be running — response timed out");

    // The printer going idle is the completion.
    ps.update_from_status(json{{"idle_timeout", json{{"state", "Ready"}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) ==
          "Calibration complete - save to keep the offsets");

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "tool offset panel: a printer busy again by the idle edge keeps waiting",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // observe<int> defers through the UpdateQueue, so the handler runs with
    // the value the notification carried. A printer busy again by then is still
    // working through the macro, and completing the run would re-enable Save
    // under a queue it still blocks.
    helix::PrinterState& ps = get_printer_state();
    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    panel.begin_run();
    REQUIRE(panel.is_calibration_active());

    ps.update_from_status(json{{"idle_timeout", json{{"state", "Printing"}}}});
    helix::ui::UpdateQueue::instance().drain();
    panel.on_run_rpc_error(MoonrakerError::timeout("printer.gcode.script", 1));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(panel.is_calibration_active());

    // Idle then busy again, both before the deferred handler runs.
    ps.update_from_status(json{{"idle_timeout", json{{"state", "Ready"}}}});
    ps.update_from_status(json{{"idle_timeout", json{{"state", "Printing"}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 1);

    // The next real idle edge still finishes it.
    ps.update_from_status(json{{"idle_timeout", json{{"state", "Ready"}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(panel.is_calibration_active());

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "tool offset panel: an rpc timeout under an idle printer fails",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // No macro running behind the silence: nothing to wait for.
    helix::PrinterState& ps = get_printer_state();
    ps.update_from_status(json{{"idle_timeout", json{{"state", "Ready"}}}});
    helix::ui::UpdateQueue::instance().drain();

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    panel.begin_run();
    REQUIRE(panel.is_calibration_active());

    panel.on_run_rpc_error(MoonrakerError::timeout("printer.gcode.script", 1));
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) !=
          "Calibration complete - save to keep the offsets");

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: no tools is a refusal, not a run",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // ToolState is empty between an AMS topology clear and the next
    // init_tools(). PrinterDiscovery still reports the macro - the two are
    // filled from different places - so the panel is reachable with nothing to
    // calibrate and nothing to show a result on.
    helix::ToolState& ts = helix::ToolState::instance();
    helix::ToolTopology topo;
    topo.tool_count = 2;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1};
    ts.set_ams_topology(topo);
    REQUIRE(ts.ams_topology_active());
    ts.clear_ams_topology();
    REQUIRE(ts.tools().empty());
    REQUIRE(helix::ui::ToolOffsetCalibrationPanel::printer_supports_calibration());

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    panel.begin_run();
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(panel.is_calibration_active());
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);

    // And coming back to the panel never leaves the subject ahead of the run.
    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.on_activate();
    CHECK(lv_subject_get_int(panel.get_active_subject()) == 0);
    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: Save commits a pending babystep too",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // The SAVE_CONFIG a tool save ends in restarts Klipper, which resets
    // homing_origin: a babystep not applied in that same restart is lost, while
    // its pending delta went on being shown. Save must apply it, as the header
    // and Controls saves do, once the user has turned Z-offset saving on.
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_z_offset_save_mode(static_cast<int>(helix::zoffset::SaveMode::Show));
    helix::PrinterState& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        ps, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    ps.update_from_status(
        json{{"gcode_move", json{{"homing_origin", json::array({0.0, 0.0, 0.05, 0.0})}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(ps.motion_state().get_gcode_z_offset_subject()) == 50);

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    client->clear_gcode_script_history();
    panel.send_save();
    // The apply's success callback sends SAVE_CONFIG; pump until both are out.
    REQUIRE(pump_until([&] { return client->gcode_script_history().size() >= 2; }, 50));

    const auto& hist = client->gcode_script_history();
    CHECK(hist[hist.size() - 2] == "Z_OFFSET_APPLY_PROBE");
    CHECK(hist.back() == "SAVE_CONFIG");

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
    settings.set_z_offset_save_mode(static_cast<int>(helix::zoffset::SaveMode::Auto));
}

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "tool offset panel: Save leaves the babystep alone while Z-offset saving is off",
                 "[ui_integration][toolchanger][tool_offset_cal][save-toggle]") {
    // A toolchanger defaults to saving off: the tool offsets still save, the
    // machine-wide babystep is never folded into the config (#1766).
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_z_offset_save_mode(static_cast<int>(helix::zoffset::SaveMode::Auto));
    REQUIRE_FALSE(helix::zoffset::save_enabled());

    helix::PrinterState& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        ps, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    ps.update_from_status(
        json{{"gcode_move", json{{"homing_origin", json::array({0.0, 0.0, 0.05, 0.0})}}}});
    helix::ui::UpdateQueue::instance().drain();
    helix::ToolState::instance().set_tool_offset_local(1, helix::Axis::X, -120);
    REQUIRE_FALSE(helix::ToolState::instance().dirty_tool_indices().empty());

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    client->clear_gcode_script_history();
    panel.send_save();
    REQUIRE(pump_until([&] { return !client->gcode_script_history().empty(); }, 50));

    const auto& hist = client->gcode_script_history();
    CHECK(std::any_of(hist.begin(), hist.end(), [](const std::string& script) {
        return script.find("gcode_x_offset") != std::string::npos;
    }));
    for (const auto& script : hist) {
        CHECK(script.find("Z_OFFSET_APPLY") == std::string::npos);
    }

    // klipper-toolchanger commits the tool parameters with SAVE_CONFIG, whose
    // restart resets the live offset; the unsaved babystep comes back after it.
    auto saved = [&] {
        const auto& h = client->gcode_script_history();
        return std::find(h.begin(), h.end(), "SAVE_CONFIG") != h.end();
    };
    REQUIRE(pump_until(saved, 50));
    ps.set_klippy_state_sync(helix::KlippyState::STARTUP);
    ps.set_klippy_state_sync(helix::KlippyState::READY);
    auto restored = [&] {
        const auto& h = client->gcode_script_history();
        return std::find(h.begin(), h.end(), "SET_GCODE_OFFSET Z=0.050") != h.end();
    };
    REQUIRE(pump_until(restored, 50));
    const auto& after = client->gcode_script_history();
    const auto save_at = std::find(after.begin(), after.end(), "SAVE_CONFIG");
    REQUIRE(save_at != after.end());
    CHECK(std::find(save_at, after.end(), "SET_GCODE_OFFSET Z=0.050") != after.end());

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "tool offset panel: Save applies no babystep when none is pending",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    helix::PrinterState& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        ps, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    REQUIRE(lv_subject_get_int(ps.motion_state().get_gcode_z_offset_subject()) == 0);

    // A dirty tool, so the save has real work to do: without one it sends
    // nothing at all and "no babystep was applied" is true of an empty run.
    helix::ToolState::instance().set_tool_offset_local(1, helix::Axis::X, -120);
    REQUIRE_FALSE(helix::ToolState::instance().dirty_tool_indices().empty());

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    client->clear_gcode_script_history();
    panel.send_save();
    REQUIRE(pump_until([&] { return !client->gcode_script_history().empty(); }, 50));

    const auto& hist = client->gcode_script_history();
    CHECK(std::any_of(hist.begin(), hist.end(), [](const std::string& script) {
        return script.find("gcode_x_offset") != std::string::npos;
    }));
    for (const auto& script : hist) {
        CHECK(script.find("Z_OFFSET_APPLY") == std::string::npos);
    }

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture, "tool offset panel: Save with nothing dirty sends nothing",
                 "[ui_integration][toolchanger][tool_offset_cal]") {
    // A reconnect between the confirmation and the send re-seeds the baselines.
    // Trusting what save_offsets() saw would send no gcode and still report a
    // successful save.
    helix::PrinterState& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        ps, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    REQUIRE(lv_subject_get_int(ps.motion_state().get_gcode_z_offset_subject()) == 0);
    REQUIRE(helix::ToolState::instance().dirty_tool_indices().empty());

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.on_activate();
    client->clear_gcode_script_history();
    panel.send_save();
    pump_until([] { return false; }, 20);

    CHECK(client->gcode_script_history().empty());
    CHECK(std::string(lv_subject_get_string(panel.get_status_subject())) == "Ready to calibrate");

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.cleanup();
}

TEST_CASE_METHOD(ToolCalPanelFixture,
                 "the Calibration entry starts a printer's own guided flow instead of the panel",
                 "[tool_offset_cal][guided-flow]") {
    nlohmann::json objects = nlohmann::json::array({"gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART",
                                                    "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT"});
    for (int t = 0; t < 4; ++t) {
        for (const char* step :
             {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN", "WAIT_COOL", "PROBE_CALIBRATE"}) {
            objects.push_back(std::string("gcode_macro _EXTRUDER_OFFSET_ACTION_") + step + "_T" +
                              std::to_string(t));
        }
    }
    helix::PrinterDiscovery u1;
    u1.parse_objects(objects);
    get_printer_state().set_hardware(u1);

    int starts = 0;
    helix::ui::register_guided_device_flow(helix::snapmaker::nozzle_offset::kGuidedFlowId,
                                           {nullptr, [&](const std::string&) { ++starts; }});
    helix::ui::open_tool_offset_cal_panel(test_screen());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(starts == 1);
    auto& caps = get_printer_state().capabilities_state();
    CHECK(lv_subject_get_int(caps.subject(helix::Capability::HasToolOffsetCal)) == 1);

    helix::ui::unregister_guided_device_flow(helix::snapmaker::nozzle_offset::kGuidedFlowId);
    get_printer_state().set_hardware(helix::PrinterDiscovery{});
}
