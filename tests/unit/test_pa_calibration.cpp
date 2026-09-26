// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pa_calibration.cpp
 * @brief helix::pacal provider table + PACalibrateCollector
 *
 * Two things are worth pinning here.
 *
 * The capability gate: stock Klipper cannot measure pressure advance, so a
 * printer that does not advertise a measuring firmware's command must report
 * the capability ABSENT rather than offer a screen that can only fail. That is
 * the difference between a hidden button and a refusal the user cannot act on.
 *
 * The collector contract: the console result line - not the RPC reply - is the
 * authority for completion, exactly as for PID_CALIBRATE. A run that outlives
 * its RPC timeout is still running.
 */

#include "../../include/moonraker_advanced_api.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/pa_calibration.h"
#include "../../include/printer_discovery.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <regex>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct LVGLInitializerPACal {
    LVGLInitializerPACal() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};
static LVGLInitializerPACal lvgl_init;

/// A discovery populated the way Moonraker's object list would populate it.
PrinterDiscovery with_objects(const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    hw.set_printer_objects(objects);
    return hw;
}

/// A Snapmaker U1 as its object list reports it: the flow calibrator has no
/// status object, filament_parameters (which it depends on) does.
PrinterDiscovery u1() {
    return with_objects({"extruder", "filament_parameters"});
}

} // namespace

// ============================================================================
// Capability gate
// ============================================================================

TEST_CASE("PA calibration is unsupported on a printer with no measuring firmware",
          "[pa_calibration]") {
    // A perfectly ordinary Klipper machine: heaters, a probe, even the tool
    // offset macro. None of that can measure pressure advance.
    const PrinterDiscovery hw = with_objects(
        {"extruder", "heater_bed", "probe", "toolchanger", "gcode_macro CALIBRATE_TOOL_OFFSETS"});

    REQUIRE_FALSE(pacal::is_supported(hw));
    REQUIRE(pacal::provider_name(hw).empty());
    REQUIRE_FALSE(pacal::procedure_for(hw, 0, 245).has_value());
}

TEST_CASE("PA calibration recognises the U1 by its filament_parameters object",
          "[pa_calibration]") {
    REQUIRE_FALSE(pacal::is_supported(with_objects({"extruder", "toolchanger"})));

    const PrinterDiscovery hw = u1();
    REQUIRE(pacal::is_supported(hw));
    REQUIRE_FALSE(pacal::provider_name(hw).empty());
    REQUIRE(pacal::is_per_tool(hw));
}

TEST_CASE("U1 procedure runs FLOW_CALIBRATE at the chosen temperature", "[pa_calibration]") {
    // SM_PRINT_FLOW_CALIBRATE is the print-job wrapper and returns without a
    // word outside a print, so the screen must drive FLOW_CALIBRATE itself.
    const auto p = pacal::procedure_for(u1(), 2, 245).value();
    REQUIRE(p.start_gcode == "FLOW_CALIBRATE TEMP=245");
    REQUIRE(p.command_word == "FLOW_CALIBRATE");
    REQUIRE(p.applies_result);
}

TEST_CASE("U1 patterns match the flow calibrator's own lines", "[pa_calibration]") {
    const auto proc = pacal::procedure_for(u1(), 0, 245).value();
    const std::regex result_re(proc.result_pattern);
    const std::regex attempt_re(proc.attempt_pattern);
    const std::regex failure_re(proc.failure_pattern);
    std::smatch m;

    // Lines copied from flow_calibrator.py's format strings.
    const std::string candidate = "measure k: 0.02000";
    REQUIRE(std::regex_search(candidate, m, attempt_re));
    REQUIRE(m[1].str() == "0.02000");
    REQUIRE_FALSE(std::regex_search(candidate, m, result_re));

    const std::string result = "Got pressure advance: 0.0412";
    REQUIRE(std::regex_search(result, m, result_re));
    REQUIRE(m[1].str() == "0.0412");

    // A SET_PRESSURE_ADVANCE echo is not this firmware's result.
    const std::string echo = "pressure_advance: 0.041200";
    REQUIRE_FALSE(std::regex_search(echo, result_re));

    const std::string out_of_range = "flow k is out of range, use default value:0.02";
    const std::string aborted = "abort calibration: filament runout";
    REQUIRE(std::regex_search(out_of_range, failure_re));
    REQUIRE(std::regex_search(aborted, failure_re));
    REQUIRE_FALSE(std::regex_search(result, failure_re));
}

TEST_CASE("PA calibration detects a firmware advertised by printer object", "[pa_calibration]") {
    // FF_PA_CALIBRATE is registered by the [ff_pa] klippy extra, so it never
    // appears as a gcode_macro - only as the extra's config-section object.
    PrinterDiscovery hw = with_objects({"extruder", "toolchanger"});
    REQUIRE_FALSE(pacal::is_supported(hw));

    hw.set_printer_objects({"extruder", "toolchanger", "ff_pa"});
    REQUIRE(pacal::is_supported(hw));
    REQUIRE(pacal::is_per_tool(hw));

    auto p2 = pacal::procedure_for(hw, 2, 245);
    REQUIRE(p2.has_value());
    REQUIRE(p2->start_gcode == "FF_PA_CALIBRATE TOOL=2");
    REQUIRE(p2->command_word == "FF_PA_CALIBRATE");
    REQUIRE_FALSE(p2->applies_result);
}

TEST_CASE("FlashForge result pattern skips the sweep's candidate echoes", "[pa_calibration]") {
    PrinterDiscovery hw = with_objects({"extruder"});
    hw.set_printer_objects({"extruder", "ff_pa"});
    const auto proc = pacal::procedure_for(hw, 0, 245).value();

    const std::regex result_re(proc.result_pattern);
    const std::regex attempt_re(proc.attempt_pattern);

    // Every candidate the sweep tries is installed through Klipper's
    // SET_PRESSURE_ADVANCE, which echoes this ':' shape. Reading it as the
    // result would finish the run on the FIRST candidate - it must only ever
    // count as progress.
    const std::string echo = "// pressure_advance: 0.010000";
    std::smatch m;
    REQUIRE_FALSE(std::regex_search(echo, m, result_re));
    REQUIRE(std::regex_search(echo, m, attempt_re));
    REQUIRE(m[1].str() == "0.010000");

    // The real result is the '=' line FF_PA_CALIBRATE prints once, at the end.
    const std::string final_line =
        "// ff_pa: T0 pressure_advance = 0.021667   (mean of 3 sweep winners:"
        " 0.0200, 0.0250, 0.0200)";
    REQUIRE(std::regex_search(final_line, m, result_re));
    REQUIRE(m[1].str() == "0.021667");
}

TEST_CASE("PA plausibility band brackets a healthy direct-drive value", "[pa_calibration]") {
    const PrinterDiscovery hw = u1();

    const auto range = pacal::sane_range(hw);
    REQUIRE(range.low < range.high);
    REQUIRE(range.extruder_kind != nullptr);

    // A typical direct-drive result sits inside; a Bowden-scale number does
    // not. Flagging that is the one judgement the machine cannot make itself.
    REQUIRE(pacal::is_plausible(hw, 0.0412f));
    REQUIRE_FALSE(pacal::is_plausible(hw, 0.6f));
    REQUIRE_FALSE(pacal::is_plausible(hw, 0.0f));
}

// ============================================================================
// Collector
// ============================================================================

class PACalibrateTestFixture {
  public:
    PACalibrateTestFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        // execute_gcode()'s halted gate would otherwise reject every command.
        state_.set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);

        hw_ = u1();
        proc_ = pacal::procedure_for(hw_, 0, 245).value();
    }
    ~PACalibrateTestFixture() {
        api_.reset();
    }

    void start() {
        cancel_ = api_->advanced().start_pa_calibrate(
            proc_,
            [this](float k) {
                captured_k_ = k;
                result_received_.store(true);
            },
            [this](const MoonrakerError& err) {
                captured_error_ = err.message;
                error_received_.store(true);
            },
            [this](int attempt, int expected, float k_so_far) {
                attempts_.push_back(attempt);
                expected_ = expected;
                last_k_so_far_ = k_so_far;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void say(const std::string& line) {
        mock_client_.dispatch_gcode_response(line);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;
    PrinterDiscovery hw_;
    pacal::Procedure proc_;
    std::function<void()> cancel_;

    std::atomic<bool> result_received_{false};
    std::atomic<bool> error_received_{false};
    float captured_k_ = 0.0f;
    std::string captured_error_;
    std::vector<int> attempts_;
    int expected_ = 0;
    float last_k_so_far_ = 0.0f;
};

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector reads the measured value off the console",
                 "[pa_collector]") {
    start();
    say("// Got pressure advance: 0.0412");

    REQUIRE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_k_ == Catch::Approx(0.0412f).margin(0.00001f));
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector counts candidate probes as progress",
                 "[pa_collector]") {
    start();
    say("// measure k: 0.02000");
    say("// measure area: 0.01810");
    say("// measure k: 0.06000");
    say("// measure k: 0.04000");

    REQUIRE(attempts_.size() == 3);
    REQUIRE(attempts_.back() == 3);
    REQUIRE(last_k_so_far_ == Catch::Approx(0.04f));
    REQUIRE(expected_ > 0);
    // Progress is not completion: the run is still open until the value lands.
    REQUIRE_FALSE(result_received_.load());

    say("// Got pressure advance: 0.0412");
    REQUIRE(result_received_.load());
}

TEST_CASE_METHOD(PACalibrateTestFixture,
                 "PA collector reports a missing command as a capability problem",
                 "[pa_collector]") {
    start();
    say("!! Unknown command:\"FLOW_CALIBRATE\"");

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    // The message has to say the printer cannot do this, not that the run went
    // wrong: they call for completely different things from the user.
    REQUIRE(captured_error_.find("FLOW_CALIBRATE") != std::string::npos);
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector ends on the firmware's own failure line",
                 "[pa_collector]") {
    start();
    say("// flow k is out of range, use default value:0.02");

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    REQUIRE(captured_error_.find("out of range") != std::string::npos);
}

TEST_CASE_METHOD(PACalibrateTestFixture,
                 "PA collector does not end on a line that merely says error", "[pa_collector]") {
    start();
    // Only a line Klipper marks as an error ends the run.
    say("// measure area: fitting error 0.00120");

    REQUIRE_FALSE(error_received_.load());
    say("// Got pressure advance: 0.0412");
    REQUIRE(result_received_.load());
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector is silent after it is cancelled",
                 "[pa_collector]") {
    start();
    REQUIRE(cancel_);
    cancel_();

    // The firmware finishes the run it is in; its result is no longer news.
    say("// Got pressure advance: 0.0412");
    say("!! late failure");

    REQUIRE_FALSE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector surfaces a firmware refusal verbatim",
                 "[pa_collector]") {
    start();
    const std::string refusal =
        "!! Extruder reported no filament at the sensor after 40 mm of priming. The calibration"
        " was cancelled before any extrusion and the nozzle is cooling down.";
    say(refusal);

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    // The machine's own sentence is the most valuable text on the screen when
    // it appears, so nothing may trim it on the way through.
    REQUIRE(captured_error_.find("no filament at the sensor") != std::string::npos);
    REQUIRE(captured_error_.find("nozzle is cooling down") != std::string::npos);
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector completes exactly once", "[pa_collector]") {
    start();
    say("// Got pressure advance: 0.0412");
    REQUIRE(result_received_.load());

    // A second result line, or a late error, must not re-fire the callback and
    // overwrite a finished run.
    result_received_.store(false);
    say("// Got pressure advance: 0.0999");
    say("!! Something went wrong afterwards");

    REQUIRE_FALSE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_k_ == Catch::Approx(0.0412f).margin(0.00001f));
}
