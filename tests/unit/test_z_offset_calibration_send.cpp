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

#include "ui_modal.h"
#include "ui_panel_calibration_zoffset.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "settings_manager.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/update_queue_test_access.h"
#include "test_helpers/zoffset_calibration_test_access.h"
#include "tool_state.h"

#include <algorithm>
#include <optional>
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
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Accept the calibrated -0.150 and return what went on the wire.
    std::string accept_sends() {
        MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
        MoonrakerAPI api(client, state());
        ZOffsetCalibrationPanel panel;
        panel.set_api(&api);
        ZOffsetCalibrationTestAccess::set_cumulative_delta(panel, -0.15f);
        ZOffsetCalibrationTestAccess::send_accept(panel);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
std::vector<std::string> probe_accept_scripts(
    helix::zoffset::SaveMode mode,
    ZOffsetCalibrationStrategy strategy = ZOffsetCalibrationStrategy::PROBE_CALIBRATE) {
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_z_offset_save_mode(static_cast<int>(mode));
    helix::PrinterStateTestAccess::pin_z_offset_strategy(get_printer_state(), strategy);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, get_printer_state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::send_accept(panel);
    for (int i = 0; i < 20; ++i) {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    std::vector<std::string> sent = client.gcode_script_history();
    settings.set_z_offset_save_mode(static_cast<int>(helix::zoffset::SaveMode::Auto));
    return sent;
}

bool sent_any(const std::vector<std::string>& scripts, const std::string& needle) {
    return std::any_of(scripts.begin(), scripts.end(),
                       [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

/// The scripts after ACCEPT: what the save wrote.
std::vector<std::string> after_accept(const std::vector<std::string>& scripts) {
    auto it = std::find(scripts.begin(), scripts.end(), "ACCEPT");
    return it == scripts.end() ? std::vector<std::string>{}
                               : std::vector<std::string>(std::next(it), scripts.end());
}

void discover_eddy() {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"probe_eddy_current btt_eddy"}));
    helix::sensors::ProbeSensorManager::instance().discover(hw);
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

// ACCEPT has staged the result; Z_OFFSET_APPLY_PROBE would replace it with the
// loaded z_offset minus the live gcode offset, so the save only commits (#1994).
TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: a live gcode offset does not reach the saved result",
                 "[ui_integration][zoffset][save-toggle]") {
    state().update_from_status(json{{"gcode_move", {{"homing_origin", {0.0, 0.0, -0.2, 0.0}}}}});
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(state().motion_state().get_gcode_z_offset_subject()) == -200);

    SECTION("probe") {
        const auto sent = probe_accept_scripts(helix::zoffset::SaveMode::Show);
        CHECK(after_accept(sent) == std::vector<std::string>{"SAVE_CONFIG"});
    }
    SECTION("endstop") {
        const auto sent = probe_accept_scripts(helix::zoffset::SaveMode::Show,
                                               ZOffsetCalibrationStrategy::ENDSTOP);
        CHECK(after_accept(sent) == std::vector<std::string>{"SAVE_CONFIG"});
    }
}

TEST_CASE_METHOD(CalibrationSendFixture, "Calibration accept: an eddy probe saves its sweep",
                 "[ui_integration][zoffset][save-toggle][eddy]") {
    discover_eddy();
    const auto sent = probe_accept_scripts(helix::zoffset::SaveMode::Show);
    CHECK(after_accept(sent) == std::vector<std::string>{"SAVE_CONFIG"});
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
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration screensaver: a bed warm-up keeps its heat and its phase",
                 "[ui_integration][zoffset][1516]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::mark_bed_warmed(panel);
    ZOffsetCalibrationTestAccess::force_state(panel, ZOffsetCalibrationPanel::State::WARMING);

    panel.on_deactivate(DeactivateReason::Suspended);
    panel.on_activate();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(client.last_send_script().empty()); // no M140 S0
    CHECK(ZOffsetCalibrationTestAccess::state(panel) == ZOffsetCalibrationPanel::State::WARMING);
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration screensaver: a finished result is still shown on wake",
                 "[ui_integration][zoffset][1516]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::force_state(panel, ZOffsetCalibrationPanel::State::COMPLETE);

    panel.on_deactivate(DeactivateReason::Suspended);
    panel.on_activate();

    CHECK(ZOffsetCalibrationTestAccess::state(panel) == ZOffsetCalibrationPanel::State::COMPLETE);
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

    ZOffsetCalibrationTestAccess::on_calibration_result(panel, false);

    CHECK(client.last_send_script() == "ABORT");
    // The user still has to read why the run failed.
    CHECK(ZOffsetCalibrationTestAccess::state(panel) == ZOffsetCalibrationPanel::State::ERROR);
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
    ZOffsetCalibrationTestAccess::on_calibration_result(panel, false);

    CHECK(client.last_send_script().empty());
    CHECK(ZOffsetCalibrationTestAccess::state(panel) == ZOffsetCalibrationPanel::State::ERROR);
}

TEST_CASE_METHOD(CalibrationSendFixture, "Calibration retry: an open session is aborted first",
                 "[ui_integration][zoffset]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ManualProbeActive session(true);
    ZOffsetCalibrationTestAccess::force_state(panel, ZOffsetCalibrationPanel::State::ERROR);

    ZOffsetCalibrationTestAccess::handle_retry_clicked(panel);

    CHECK(client.last_send_script() == "ABORT");
    CHECK(ZOffsetCalibrationTestAccess::state(panel) == ZOffsetCalibrationPanel::State::IDLE);
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
    ZOffsetCalibrationTestAccess::force_state(panel, ZOffsetCalibrationPanel::State::IDLE);

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
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, ps);
    api.hardware() = std::move(hw);
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::begin_probe_sequence(panel);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
                 "Calibration start: an eddy probe calibrates its discovered chip",
                 "[ui_integration][zoffset][zoffset_center][eddy]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"probe_eddy_current btt_eddy"}));
    helix::sensors::ProbeSensorManager::instance().discover(hw);
    // The paper test reads at the nozzle, so the probe offset moves nothing.
    hw.parse_probe_xy_offset(
        json{{"probe_eddy_current btt_eddy", {{"x_offset", -40.0}, {"y_offset", 10.0}}}});
    CHECK(start_script(ZOffsetCalibrationStrategy::PROBE_CALIBRATE, "PROBE_EDDY_CURRENT_CALIBRATE",
                       std::move(hw)) ==
          "G90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\nPROBE_EDDY_CURRENT_CALIBRATE CHIP=btt_eddy");
}

// A Helper-Script or Z-Mod SET_GCODE_OFFSET wrapper makes the printer
// FIRMWARE_MANAGED; an eddy there still needs its paper test and coil sweep (#1994).
TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: an eddy probe on a firmware-managed printer runs its sweep",
                 "[ui_integration][zoffset][zoffset_center][eddy][1994]") {
    discover_eddy();
    CHECK(start_script(ZOffsetCalibrationStrategy::FIRMWARE_MANAGED,
                       "PROBE_EDDY_CURRENT_CALIBRATE") ==
          "G90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\nPROBE_EDDY_CURRENT_CALIBRATE CHIP=btt_eddy");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: a contact probe on a firmware-managed printer keeps the "
                 "manual gcode offset",
                 "[ui_integration][zoffset][zoffset_center][1994]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"probe"}));
    helix::sensors::ProbeSensorManager::instance().discover(hw);
    REQUIRE_FALSE(helix::sensors::ProbeSensorManager::instance().get_sensors().empty());
    CHECK(start_script(ZOffsetCalibrationStrategy::FIRMWARE_MANAGED, "G1 Z0.1") ==
          "G90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\nG1 Z0.1 F300");
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration accept: an eddy probe on a firmware-managed printer saves its sweep",
                 "[ui_integration][zoffset][save-toggle][eddy][1994]") {
    discover_eddy();
    const auto sent = probe_accept_scripts(helix::zoffset::SaveMode::Show,
                                           ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK_FALSE(sent_any(sent, "SET_GCODE_OFFSET"));
    CHECK_FALSE(sent_any(sent, "Z_OFFSET_APPLY"));
    CHECK(after_accept(sent) == std::vector<std::string>{"SAVE_CONFIG"});
}

// The strategy is fixed when the run starts: a rediscovery that drops the eddy
// mid-session must not turn the open manual probe's jogs and Accept into
// G1 and SET_GCODE_OFFSET.
TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration session: losing the eddy mid-run keeps the manual probe commands",
                 "[ui_integration][zoffset][eddy][1994]") {
    auto& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        ps, ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    ps.update_from_status(json{{"toolhead",
                                {{"homed_axes", "xyz"},
                                 {"axis_minimum", json::array({0.0, 0.0, 0.0, 0.0})},
                                 {"axis_maximum", json::array({350.0, 350.0, 340.0, 0.0})}}}});
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    discover_eddy();

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, ps);
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::begin_probe_sequence(panel);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(client.last_send_script().find("PROBE_EDDY_CURRENT_CALIBRATE") != std::string::npos);

    helix::sensors::ProbeSensorManager::instance().discover(helix::PrinterDiscovery{});
    REQUIRE(helix::sensors::ProbeSensorManager::instance().get_sensors().empty());

    ZOffsetCalibrationTestAccess::adjust_z(panel, -0.05f);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(client.last_send_script() == "TESTZ Z=-0.050");

    SECTION("accept") {
        ZOffsetCalibrationTestAccess::send_accept(panel);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        const auto sent = client.gcode_script_history();
        CHECK(std::find(sent.begin(), sent.end(), "ACCEPT") != sent.end());
        CHECK(std::none_of(sent.begin(), sent.end(), [](const std::string& s) {
            return s.find("SET_GCODE_OFFSET") != std::string::npos;
        }));
    }
    SECTION("abort") {
        ZOffsetCalibrationTestAccess::force_state(panel, ZOffsetCalibrationPanel::State::ADJUSTING);
        panel.on_deactivate(DeactivateReason::NavigateAway);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        CHECK(client.last_send_script() == "ABORT");
    }
}

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration session: a finished run releases its strategy",
                 "[ui_integration][zoffset][eddy][1994]") {
    // The next run follows the probe list as it is then, not the last run's.
    auto& ps = get_printer_state();
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        ps, ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    discover_eddy();
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, ps);
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::begin_probe_sequence(panel);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    ZOffsetCalibrationTestAccess::on_calibration_result(panel, false);

    helix::sensors::ProbeSensorManager::instance().discover(helix::PrinterDiscovery{});
    ZOffsetCalibrationTestAccess::adjust_z(panel, 0.05f);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    const auto sent = client.gcode_script_history();
    CHECK(std::any_of(sent.begin(), sent.end(), [](const std::string& s) {
        return s.find("G1 Z0.050 F300") != std::string::npos;
    }));
    CHECK(std::none_of(sent.begin(), sent.end(),
                       [](const std::string& s) { return s.find("TESTZ") != std::string::npos; }));
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

// --- reference tool --------------------------------------------------------

namespace {

/// A klipper-toolchanger rig on the probe strategy, with the dialog layer up.
class ToolGateFixture : public LVGLUITestFixture {
  public:
    ToolGateFixture() {
        helix::ui::modal_init_subjects();
        auto& ps = get_printer_state();
        helix::PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        helix::PrinterStateTestAccess::pin_z_offset_strategy(
            ps, ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
        ps.set_klippy_state_sync(helix::KlippyState::READY);
        auto& probes = helix::sensors::ProbeSensorManager::instance();
        probes.discover(helix::PrinterDiscovery{});
        probes.set_probe_type_override(helix::sensors::ProbeSensorType::STANDARD);
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array(
            {"gcode_move", "toolhead", "extruder", "toolchanger", "tool T0", "tool T1"}));
        ps.set_hardware(hw);
        helix::ToolState::instance().init_tools(hw);
        panel.set_api(&api);
    }

    ~ToolGateFixture() override {
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
        }
        helix::ToolState::instance().init_tools(helix::PrinterDiscovery{});
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    void mount(int tool_number) {
        helix::ToolState::instance().update_from_status(
            json{{"toolchanger", json{{"tool_number", tool_number}}}});
        REQUIRE(helix::ToolState::instance().active_tool_index() == tool_number);
    }

    void start() {
        ZOffsetCalibrationTestAccess::start_calibration(panel);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        process_lvgl(50);
    }

    /// Press one of the open dialog's buttons by name.
    void press(const char* button) {
        lv_obj_t* dialog = Modal::get_top();
        REQUIRE(dialog != nullptr);
        lv_obj_t* btn = lv_obj_find_by_name(dialog, button);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        process_lvgl(50);
    }

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    ZOffsetCalibrationPanel panel;
};

} // namespace

TEST_CASE_METHOD(ToolGateFixture, "Calibration start: another tool mounted asks, then continues",
                 "[zoffset][calibration][reference_tool]") {
    panel.on_activate();
    mount(1);
    start();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::IDLE);
    CHECK(client.last_send_script().empty());

    // A second tap while the question is open asks nothing more.
    start();
    CHECK(Modal::get_top() == dialog);

    press("btn_primary");
    CHECK(panel.get_state() != ZOffsetCalibrationPanel::State::IDLE);
    CHECK(panel.get_state() != ZOffsetCalibrationPanel::State::ERROR);
    CHECK_FALSE(client.last_send_script().empty());
}

TEST_CASE_METHOD(ToolGateFixture, "Calibration start: declining the tool question starts nothing",
                 "[zoffset][calibration][reference_tool]") {
    panel.on_activate();
    mount(-1);
    start();
    REQUIRE(Modal::get_top() != nullptr);
    press("btn_secondary");
    CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::IDLE);
    CHECK(client.last_send_script().empty());

    // Declined, Start asks again rather than staying latched shut.
    start();
    CHECK(Modal::get_top() != nullptr);
}

TEST_CASE_METHOD(ToolGateFixture, "Calibration start: the reference tool mounted asks nothing",
                 "[zoffset][calibration][reference_tool]") {
    panel.on_activate();
    mount(0);
    start();
    CHECK(Modal::get_top() == nullptr);
    CHECK(panel.get_state() != ZOffsetCalibrationPanel::State::IDLE);
    CHECK_FALSE(client.last_send_script().empty());
}

TEST_CASE_METHOD(ToolGateFixture,
                 "Calibration start: Continue on a panel no longer on screen starts nothing",
                 "[zoffset][calibration][reference_tool]") {
    // Never activated reads exactly as deactivated: not visible.
    mount(1);
    start();
    REQUIRE(Modal::get_top() != nullptr);
    press("btn_primary");
    CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::IDLE);
    CHECK(client.last_send_script().empty());
}

TEST_CASE_METHOD(ToolGateFixture,
                 "Calibration start: Continue once a print has started starts nothing",
                 "[zoffset][calibration][reference_tool]") {
    panel.on_activate();
    mount(1);
    start();
    REQUIRE(Modal::get_top() != nullptr);
    get_printer_state().update_from_status(json{{"print_stats", json{{"state", "printing"}}}});
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(get_printer_state().print_state().get_print_active_subject()) != 0);

    press("btn_primary");
    CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::IDLE);
    CHECK(client.last_send_script().empty());

    get_printer_state().update_from_status(json{{"print_stats", json{{"state", "standby"}}}});
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(ToolGateFixture, "Calibration start: leaving the panel closes the tool question",
                 "[zoffset][calibration][reference_tool]") {
    panel.on_activate();
    mount(1);
    start();
    REQUIRE(Modal::get_top() != nullptr);

    panel.on_deactivate(DeactivateReason::NavigateAway);
    process_lvgl(400);
    CHECK(Modal::get_top() == nullptr);

    // Back on screen, Start asks again rather than staying latched shut.
    panel.on_activate();
    start();
    CHECK(Modal::get_top() != nullptr);
}

namespace {
/// An eddy probe for one case; the probe manager is process-wide.
struct EddyScope {
    EddyScope() {
        discover_eddy();
    }
    ~EddyScope() {
        helix::sensors::ProbeSensorManager::instance().discover(helix::PrinterDiscovery{});
    }
};
} // namespace

TEST_CASE_METHOD(ToolGateFixture,
                 "Calibration start: an eddy on a firmware-managed printer asks for the "
                 "reference tool",
                 "[zoffset][calibration][reference_tool][eddy][1994]") {
    // The eddy runs its own probe calibration here, so the reference tool matters.
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        get_printer_state(), ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    EddyScope eddy;
    panel.on_activate();
    mount(1);
    start();
    CHECK(Modal::get_top() != nullptr);
    CHECK(client.last_send_script().empty());
}

// A manual probe session the panel did not start keeps the strategy it was
// picked up with: a rediscovery that drops the eddy must not turn its jogs into G1.
TEST_CASE_METHOD(ToolGateFixture,
                 "Calibration session: a session picked up from Klipper keeps its strategy",
                 "[zoffset][eddy][1994]") {
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        get_printer_state(), ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    EddyScope eddy;
    std::optional<ManualProbeActive> session;
    SECTION("running when the panel opens") {
        session.emplace(true);
        panel.on_activate();
    }
    SECTION("starting while the panel is open") {
        panel.init_subjects();
        REQUIRE(panel.create(test_screen()) != nullptr);
        panel.on_activate();
        REQUIRE(panel.get_state() == ZOffsetCalibrationPanel::State::IDLE);
        session.emplace(true);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    REQUIRE(panel.get_state() == ZOffsetCalibrationPanel::State::ADJUSTING);

    helix::sensors::ProbeSensorManager::instance().discover(helix::PrinterDiscovery{});
    ZOffsetCalibrationTestAccess::adjust_z(panel, -0.05f);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(client.last_send_script() == "TESTZ Z=-0.050");
}

// --- per-head offsets -------------------------------------------------------

namespace {
/// Per-head storage on for one case, whatever the case does.
struct PerHeadScope {
    PerHeadScope() {
        get_printer_state().profile_state().set_z_offset_per_head(true);
    }
    ~PerHeadScope() {
        get_printer_state().profile_state().set_z_offset_per_head(false);
    }
};
} // namespace

TEST_CASE_METHOD(CalibrationSendFixture,
                 "Calibration start: a printer that applies head offsets at pickup refuses",
                 "[zoffset][calibration][per_head]") {
    PerHeadScope per_head;
    REQUIRE(state().profile_state().z_offset_reapplied_per_head());
    // A strategy that would otherwise start and send, so the refusal is what stops it.
    helix::PrinterStateTestAccess::pin_z_offset_strategy(
        state(), ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);

    ZOffsetCalibrationTestAccess::start_calibration(panel);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(panel.get_state() == ZOffsetCalibrationPanel::State::ERROR);
    CHECK(client.last_send_script().empty());
}

TEST_CASE_METHOD(
    CalibrationSendFixture,
    "Calibration accept: a printer that applies head offsets at pickup sends no offset",
    "[zoffset][calibration][per_head]") {
    PerHeadScope per_head;
    // The flow opened by a manual probe that was already running at connect,
    // with a jog total that would put -0.150 on a head running at +3.4.
    const std::string sent = accept_sends();
    CHECK(sent.find("SET_GCODE_OFFSET") == std::string::npos);
}
