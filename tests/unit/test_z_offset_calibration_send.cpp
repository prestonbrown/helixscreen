// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_offset_calibration_send.cpp
 * @brief Wiring tests for the calibration panel's accept send under ZMOD.
 *
 * The FIRMWARE_MANAGED accept arm puts a bare SET_GCODE_OFFSET Z= on the wire.
 * ZMOD's override of that command persists `z - _TEST_POINT.temp_z_offset`
 * (ghzserg/zmod#699), and the variable survives END_PRINT/CANCEL_PRINT - so an
 * accept after a print stores the calibrated value minus that print's probe
 * delta. These tests pin that the stale-delta clear line rides in front of the
 * offset on ZMOD, and only there: never on a printer without the mechanism,
 * and never while a print is running (mid-print the subtraction excludes the
 * live per-print transient and is correct).
 *
 * Companion to test_print_tune_zoffset_adjust.cpp, which pins the same gate on
 * the tune overlay's baby-step path.
 */

#include "ui_panel_calibration_zoffset.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "settings_manager.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/zoffset_calibration_test_access.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using helix::ZOffsetCalibrationStrategy;
using nlohmann::json;

namespace {

class CalibrationSendFixture : public LVGLTestFixture {
  public:
    CalibrationSendFixture() {
        helix::PrinterStateTestAccess::reset(state());
        state().init_subjects(false);
        helix::PrinterStateTestAccess::pin_z_offset_strategy(
            state(), ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
        state().set_klippy_state_sync(helix::KlippyState::READY);
        // The discovery snapshot persists on the process-wide state across test
        // cases; start each one plain so only set_zmod() cases see the provider.
        state().set_hardware(helix::PrinterDiscovery{});
        // The calibrate command follows the discovered probe's type, and the probe
        // manager is process-wide: start each case with no probe, as a plain
        // printer has, so a scan probe another case discovered cannot pick it.
        auto& probes = helix::sensors::ProbeSensorManager::instance();
        probes.discover(helix::PrinterDiscovery{});
        probes.set_probe_type_override(helix::sensors::ProbeSensorType::STANDARD);
    }

    ~CalibrationSendFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    helix::PrinterState& state() {
        return get_printer_state();
    }

    /// ZMOD-shaped discovery snapshot, as Moonraker's macro listing produces.
    void set_zmod() {
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array({"gcode_macro SAVE_ZMOD_DATA"}));
        state().set_hardware(std::move(hw));
    }

    void set_printing(bool printing) {
        state().update_from_status(
            json{{"print_stats", json{{"state", printing ? "printing" : "standby"}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Accept the calibrated -0.150 and return what went on the wire.
    std::string accept_sends() {
        MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
        MoonrakerAPI api(client, state());
        ZOffsetCalibrationPanel panel;
        panel.set_api(&api);
        ZOffsetCalibrationTestAccess::set_cumulative_delta(panel, -0.15f);
        ZOffsetCalibrationTestAccess::send_accept(panel);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        return client.last_send_script();
    }
};

} // namespace

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: ZMOD clears the stale probe delta before the offset",
                 "[ui_integration][zoffset][zmod][regression]") {
    set_zmod();
    set_printing(false);

    CHECK(accept_sends() == "SET_GCODE_VARIABLE MACRO=_TEST_POINT VARIABLE=temp_z_offset VALUE=0\n"
                            "SET_GCODE_OFFSET Z=-0.150");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: no clear line without the firmware mechanism",
                 "[ui_integration][zoffset][regression]") {
    // Same FIRMWARE_MANAGED accept, but the printer is not ZMOD (Forge-X, or a
    // plain macro-persisted box): the offset goes out alone.
    set_printing(false);

    CHECK(accept_sends() == "SET_GCODE_OFFSET Z=-0.150");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: a running print keeps the probe delta intact",
                 "[ui_integration][zoffset][zmod][regression]") {
    // THE GATE. Mid-print temp_z_offset is the live transient the override is
    // supposed to subtract; zeroing it would corrupt the running job's stored
    // offset. Calibration is an idle activity, but the send must hold the same
    // gate the tune overlay does if it ever fires under one.
    set_zmod();
    set_printing(true);

    CHECK(accept_sends() == "SET_GCODE_OFFSET Z=-0.150");
}

// ============================================================================
// The per-printer Z-offset save setting (#1766)
// ============================================================================

namespace {

/// Accept a probe calibration with saving set to @p mode and return every
/// script that went on the wire.
std::vector<std::string> probe_accept_scripts(helix::zoffset::SaveMode mode) {
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_z_offset_save_mode(static_cast<int>(mode));
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        get_printer_state(), ZOffsetCalibrationStrategy::PROBE_CALIBRATE);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, get_printer_state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::send_accept(panel);
    for (int i = 0; i < 20; ++i) {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    std::vector<std::string> sent = client.gcode_script_history();
    settings.set_z_offset_save_mode(static_cast<int>(helix::zoffset::SaveMode::Auto));
    return sent;
}

bool sent_any(const std::vector<std::string>& scripts, const std::string& needle) {
    return std::any_of(scripts.begin(), scripts.end(),
                       [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: saving off accepts without writing the config",
                 "[ui_integration][zoffset][save-toggle]") {
    const auto sent = probe_accept_scripts(helix::zoffset::SaveMode::Hide);
    CHECK(sent_any(sent, "ACCEPT"));
    CHECK_FALSE(sent_any(sent, "Z_OFFSET_APPLY"));
    CHECK_FALSE(sent_any(sent, "SAVE_CONFIG"));
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: saving on applies and saves the result",
                 "[ui_integration][zoffset][save-toggle]") {
    const auto sent = probe_accept_scripts(helix::zoffset::SaveMode::Show);
    CHECK(sent_any(sent, "ACCEPT"));
    CHECK(sent_any(sent, "Z_OFFSET_APPLY_PROBE"));
}

// ============================================================================
// Deactivation: only walking away abandons a run
// ============================================================================

namespace {

/// Deactivate a panel mid-probe for `reason` and return what went on the wire.
/// FIRMWARE_MANAGED's abort is the G90/G1 retract, so an empty string means no
/// abort was sent at all.
std::string abort_sends_for(DeactivateReason reason) {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, get_printer_state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::force_state(panel, ZOffsetCalibrationPanel::State::PROBING);
    panel.on_deactivate(reason);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    return client.last_send_script();
}

} // namespace

TEST_CASE_METHOD(CalibrationSendFixture, "Calibration abort: navigating away cancels a live run",
                 "[ui_integration][zoffset][1516]") {
    CHECK(abort_sends_for(DeactivateReason::NavigateAway) == "G90\nG1 Z5 F1000");
}

TEST_CASE_METHOD(CalibrationSendFixture, "Calibration abort: app shutdown leaves the run alone",
                 "[ui_integration][zoffset][1516]") {
    // The printer keeps executing the probe after the UI is gone. Aborting here
    // would cancel a calibration nobody asked to stop.
    CHECK(abort_sends_for(DeactivateReason::Shutdown).empty());
}

TEST_CASE_METHOD(CalibrationSendFixture, "Calibration abort: a hot-reload rebuild leaves it alone",
                 "[ui_integration][zoffset][1516]") {
    // Same reasoning as shutdown: the widget tree is being swapped underneath a
    // run the printer is still executing.
    CHECK(abort_sends_for(DeactivateReason::Rebuild).empty());
}

// ============================================================================
// A failed run does not leave Klipper in a manual probe session
// ============================================================================

namespace {

/// Holds Klipper's manual_probe state for one case, on the process-wide subject.
class ManualProbeActive {
  public:
    explicit ManualProbeActive(bool active) {
        set(active ? 1 : 0);
    }
    ~ManualProbeActive() {
        set(0);
    }

  private:
    static void set(int value) {
        lv_subject_set_int(
            get_printer_state().calibration_state().get_manual_probe_active_subject(), value);
    }
};

} // namespace

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration failure: an open manual probe session is aborted, the error stays",
                 "[ui_integration][zoffset]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ManualProbeActive session(true);

    panel.on_calibration_result(false, "test failure");

    CHECK(client.last_send_script() == "ABORT");
    // The user still has to read why the run failed.
    CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::ERROR);
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration failure: no session open means no ABORT on the wire",
                 "[ui_integration][zoffset]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ManualProbeActive session(false);

    // Klipper registers ABORT only during a session, so a stray one logs Unknown command.
    panel.on_calibration_result(false, "test failure");

    CHECK(client.last_send_script().empty());
    CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::ERROR);
}

TEST_CASE_METHOD(CalibrationSendFixture, "Calibration retry: an open session is aborted first",
                 "[ui_integration][zoffset]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    auto& panel = get_global_zoffset_cal_panel();
    panel.init_subjects();
    panel.set_api(&api);
    ManualProbeActive session(true);

    lv_event_cb_t retry = lv_xml_get_event_cb(nullptr, "on_zoffset_retry_clicked");
    REQUIRE(retry != nullptr);
    retry(nullptr);

    CHECK(client.last_send_script() == "ABORT");
    CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::IDLE);
    panel.set_api(nullptr);
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration leave: an idle panel leaves another client's session alone",
                 "[ui_integration][zoffset]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    // A session Mainsail or Fluidd opened while this panel sat idle is not ours to end.
    ManualProbeActive session(true);
    REQUIRE(panel.get_state() == ZOffsetCalibrationPanel::State::IDLE);

    panel.on_deactivate(DeactivateReason::NavigateAway);

    CHECK(client.last_send_script().empty());
}

// ============================================================================
// Start: the head goes over the plate centre before the calibrate command
// ============================================================================

namespace {

/// A calibration start's outcome: every script sent, and the panel's state
/// once it settles.
struct StartOutcome {
    std::vector<std::string> scripts;
    ZOffsetCalibrationPanel::State state = ZOffsetCalibrationPanel::State::IDLE;
};

/// Start a calibration on a homed printer with @p hw discovered. @p
/// travel_known gives it 0-350 XY travel; without it neither the travel nor
/// the plate is known.
StartOutcome start_calibration(ZOffsetCalibrationStrategy strategy, helix::PrinterDiscovery hw = {},
                               bool travel_known = true) {
    auto& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(ps, strategy);
    json toolhead{{"homed_axes", "xyz"}};
    if (travel_known) {
        toolhead["axis_minimum"] = json::array({0.0, 0.0, 0.0, 0.0});
        toolhead["axis_maximum"] = json::array({350.0, 350.0, 340.0, 0.0});
    }
    ps.update_from_status(json{{"toolhead", toolhead}});
    helix::ui::UpdateQueue::instance().drain_until_empty();

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, ps);
    api.hardware() = std::move(hw);
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::begin_probe_sequence(panel);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    return {client.gcode_script_history(), panel.get_state()};
}

/// Every line sent on start, joined, up to and including the one carrying
/// @p command; empty when @p command was never sent.
std::string start_script(ZOffsetCalibrationStrategy strategy, const std::string& command,
                         helix::PrinterDiscovery hw = {}) {
    std::string sent;
    for (const auto& s : start_calibration(strategy, std::move(hw)).scripts) {
        sent += sent.empty() ? s : "\n" + s;
        if (s.find(command) != std::string::npos) {
            return sent;
        }
    }
    return "";
}

} // namespace

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: a homed printer moves to the centre before probing",
                 "[ui_integration][zoffset][zoffset_center]") {
    CHECK(start_script(ZOffsetCalibrationStrategy::PROBE_CALIBRATE, "PROBE_CALIBRATE") ==
          "G90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\nPROBE_CALIBRATE");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: the endstop calibration starts over the centre",
                 "[ui_integration][zoffset][zoffset_center]") {
    CHECK(start_script(ZOffsetCalibrationStrategy::ENDSTOP, "Z_ENDSTOP_CALIBRATE") ==
          "G90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\nZ_ENDSTOP_CALIBRATE");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: the manual gcode offset lowers over the plate centre",
                 "[ui_integration][zoffset][zoffset_center]") {
    CHECK(start_script(ZOffsetCalibrationStrategy::FIRMWARE_MANAGED, "G1 Z0.1") ==
          "G90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\nG1 Z0.1 F300");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: the discovered probe offset puts the probe on centre",
                 "[ui_integration][zoffset][zoffset_center]") {
    helix::PrinterDiscovery hw;
    hw.parse_probe_xy_offset(json{{"probe", {{"x_offset", -40.0}, {"y_offset", 10.0}}}});
    CHECK(start_script(ZOffsetCalibrationStrategy::PROBE_CALIBRATE, "PROBE_CALIBRATE",
                       std::move(hw)) ==
          "G90\nG1 Z5.0 F600\nG1 X215.0 Y165.0 F6000\nPROBE_CALIBRATE");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: a nozzle reading point ignores the probe offset",
                 "[ui_integration][zoffset][zoffset_center]") {
    // The manual calibration reads at the nozzle, so the head itself parks on
    // the plate centre; the probe offset shifts the target only where a probe
    // does the reading.
    helix::PrinterDiscovery hw;
    hw.parse_probe_xy_offset(json{{"probe", {{"x_offset", -40.0}, {"y_offset", 10.0}}}});
    CHECK(start_script(ZOffsetCalibrationStrategy::FIRMWARE_MANAGED, "G1 Z0.1", std::move(hw)) ==
          "G90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\nG1 Z0.1 F300");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: the manual gcode offset refuses an unknown plate",
                 "[ui_integration][zoffset][zoffset_center]") {
    // Lowering to Z0.1 at a guessed XY can put the nozzle off the plate.
    const auto start = start_calibration(ZOffsetCalibrationStrategy::FIRMWARE_MANAGED, {}, false);
    CHECK(start.scripts.empty());
    CHECK(start.state == ZOffsetCalibrationPanel::State::ERROR);
}
