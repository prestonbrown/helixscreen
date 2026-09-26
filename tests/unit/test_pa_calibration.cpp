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
 * The collector contract: the console result line — not the RPC reply — is the
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
    return hw;
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
    REQUIRE_FALSE(pacal::procedure_for(hw, 0).has_value());
}

TEST_CASE("PA calibration matches the firmware that advertises its command", "[pa_calibration]") {
    const PrinterDiscovery hw = with_objects({"extruder", "gcode_macro SM_PRINT_FLOW_CALIBRATE"});

    REQUIRE(pacal::is_supported(hw));
    REQUIRE_FALSE(pacal::provider_name(hw).empty());
    REQUIRE(pacal::is_per_tool(hw));
}

TEST_CASE("PA procedure addresses the requested tool", "[pa_calibration]") {
    const PrinterDiscovery hw = with_objects({"extruder", "gcode_macro SM_PRINT_FLOW_CALIBRATE"});

    auto p0 = pacal::procedure_for(hw, 0);
    auto p2 = pacal::procedure_for(hw, 2);
    REQUIRE(p0.has_value());
    REQUIRE(p2.has_value());

    // Pressure advance belongs to an extruder, so a tool changer must be able
    // to say WHICH one; a command that ignored the index would silently
    // calibrate the wrong head.
    REQUIRE(p0->start_gcode != p2->start_gcode);
    REQUIRE(p2->start_gcode.find("2") != std::string::npos);

    // The command word is what Klipper quotes back in "Unknown command:", so it
    // must be the bare first token, with no arguments attached.
    REQUIRE(p0->command_word.find(' ') == std::string::npos);
    REQUIRE(p0->start_gcode.rfind(p0->command_word, 0) == 0);
    REQUIRE_FALSE(p0->result_pattern.empty());
}

TEST_CASE("PA plausibility band brackets a healthy direct-drive value", "[pa_calibration]") {
    const PrinterDiscovery hw = with_objects({"extruder", "gcode_macro SM_PRINT_FLOW_CALIBRATE"});

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

        hw_ = with_objects({"extruder", "gcode_macro SM_PRINT_FLOW_CALIBRATE"});
        proc_ = pacal::procedure_for(hw_, 0).value();
    }
    ~PACalibrateTestFixture() {
        api_.reset();
    }

    void start() {
        api_->advanced().start_pa_calibrate(
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
    // Every provider so far applies its measurement through Klipper's own
    // SET_PRESSURE_ADVANCE, which echoes the applied value in this shape.
    say("// pressure_advance: 0.041200");

    REQUIRE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_k_ == Catch::Approx(0.0412f).margin(0.00001f));
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector counts candidate probes as progress",
                 "[pa_collector]") {
    start();
    say("// flow calibrate: k=0.0200 area=+0.01810");
    say("// flow calibrate: k=0.0600 area=-0.01420");
    say("// flow calibrate: k=0.0400 area=+0.00110");

    REQUIRE(attempts_.size() == 3);
    REQUIRE(attempts_.back() == 3);
    REQUIRE(expected_ > 0);
    // Progress is not completion: the run is still open until the value lands.
    REQUIRE_FALSE(result_received_.load());

    say("// pressure_advance: 0.041200");
    REQUIRE(result_received_.load());
}

TEST_CASE_METHOD(PACalibrateTestFixture,
                 "PA collector reports a missing command as a capability problem",
                 "[pa_collector]") {
    start();
    say("!! Unknown command:\"SM_PRINT_FLOW_CALIBRATE\"");

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    // The message has to say the printer cannot do this, not that the run went
    // wrong — they call for completely different things from the user.
    REQUIRE(captured_error_.find("SM_PRINT_FLOW_CALIBRATE") != std::string::npos);
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
    say("// pressure_advance: 0.041200");
    REQUIRE(result_received_.load());

    // A second result line, or a late error, must not re-fire the callback and
    // overwrite a finished run.
    result_received_.store(false);
    say("// pressure_advance: 0.099900");
    say("!! Something went wrong afterwards");

    REQUIRE_FALSE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_k_ == Catch::Approx(0.0412f).margin(0.00001f));
}
