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
#include "../../include/pa_calibration_run.h"
#include "../../include/printer_discovery.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <regex>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

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

class PACalibrateTestFixture : public LVGLTestFixture {
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

// ============================================================================
// Stock FlashForge eBoard sweep: detection and the run driver
// ============================================================================

namespace {

/// A stock or Z-Mod Creator 5: [pa_adjust] in the config, the two latch pins
/// on the object list.
PrinterDiscovery stock_creator5(std::vector<std::string> extra = {}) {
    std::vector<std::string> objects = {"extruder", "output_pin enable_pin_tmc_x",
                                        "output_pin enable_pin_tmc_y"};
    objects.insert(objects.end(), extra.begin(), extra.end());
    PrinterDiscovery hw = with_objects(objects);
    hw.parse_config_keys(nlohmann::json{{"pa_adjust", nlohmann::json::object()}});
    return hw;
}

} // namespace

TEST_CASE("PA calibration recognises the stock FlashForge sweep by config and latch pins",
          "[pa_calibration]") {
    const PrinterDiscovery hw = stock_creator5();
    REQUIRE(pacal::is_supported(hw));
    REQUIRE(pacal::is_per_tool(hw));
    const auto t = pacal::traits(hw).value();
    CHECK(t.kept == pacal::Kept::ThisSession);
    CHECK_FALSE(t.firmware_runs_procedure);
    // No single command to send: the sweep is driven step by step.
    CHECK_FALSE(pacal::procedure_for(hw, 0, 220).has_value());

    // [pa_adjust] alone, without the latch the sweep needs, is not enough.
    PrinterDiscovery no_pins = with_objects({"extruder"});
    no_pins.parse_config_keys(nlohmann::json{{"pa_adjust", nlohmann::json::object()}});
    CHECK_FALSE(pacal::is_supported(no_pins));
    // Nor are the pins without [pa_adjust].
    CHECK_FALSE(pacal::is_supported(
        with_objects({"extruder", "output_pin enable_pin_tmc_x", "output_pin enable_pin_tmc_y"})));
}

TEST_CASE("PA calibration prefers the Klipper4FlashForge command over the stock sweep",
          "[pa_calibration]") {
    const PrinterDiscovery hw = stock_creator5({"ff_pa"});
    const auto t = pacal::traits(hw).value();
    CHECK(t.firmware_runs_procedure);
    CHECK(t.kept == pacal::Kept::Nothing);
    REQUIRE(pacal::procedure_for(hw, 1, 220).has_value());

    CHECK(pacal::traits(u1())->kept == pacal::Kept::Permanently);
    CHECK(pacal::traits(u1())->firmware_runs_procedure);
}

class PaSweepRunFixture : public LVGLTestFixture {
  public:
    PaSweepRunFixture() : mock_client_(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD) {
        state_.init_subjects(false);
        state_.set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
        hw_ = stock_creator5();
        mock_client_.set_pa_verdicts({0, 9, 0, 9, 0, 0, 0}); // 0.020 wins every sweep
    }
    ~PaSweepRunFixture() override {
        run_.reset();
        api_.reset();
    }

    void start() {
        pacal::RunCallbacks cb;
        cb.on_result = [this](float k) {
            k_ = k;
            done_.store(true);
        };
        cb.on_error = [this](const std::string& m) {
            error_ = m;
            done_.store(true);
        };
        cb.on_attempt = [this](const pacal::Attempt& a) {
            attempts_.push_back(a);
            if (at_attempt_ && static_cast<int>(attempts_.size()) == at_attempt_n_) {
                at_attempt_();
            }
        };
        run_ = pacal::start_run(*api_, mock_client_, hw_, {0, 220, "extruder"}, std::move(cb));
        REQUIRE(run_);
    }

    std::vector<std::string> scripts() const {
        return mock_client_.gcode_script_history();
    }

    bool sent(const std::string& needle) const {
        for (const auto& s : scripts()) {
            if (s.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    float pin(const char* object) const {
        return mock_client_.pa_latch_pin_status(object).value("value", -1.f);
    }

    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;
    PrinterDiscovery hw_;
    std::shared_ptr<pacal::Run> run_;
    std::atomic<bool> done_{false};
    /// Runs inside the Nth attempt callback, as a tap on Stop mid-run would.
    std::function<void()> at_attempt_;
    int at_attempt_n_ = 0;
    float k_ = 0.f;
    std::string error_;
    std::vector<pacal::Attempt> attempts_;
};

TEST_CASE_METHOD(PaSweepRunFixture, "PA sweep run measures, restores and applies the result",
                 "[pa_sweep]") {
    start();
    REQUIRE(wait_until([this]() { return done_.load(); }, 20000));
    INFO("error: " << error_);
    CHECK(error_.empty());
    CHECK(k_ == Catch::Approx(0.020f));
    // Three sweeps of seven lines, each judged.
    REQUIRE(attempts_.size() == 21);
    CHECK(attempts_[1].verdict == pacal::Verdict::Pass);
    CHECK(attempts_[0].verdict == pacal::Verdict::Fail);
    CHECK(attempts_[2].k_tried == Catch::Approx(0.015f));
    // The latch is released and the result installed for the session.
    CHECK(pin("output_pin enable_pin_tmc_x") == 0.f);
    CHECK(pin("output_pin enable_pin_tmc_y") == 0.f);
    CHECK(sent("SET_KINEMATIC_POSITION X="));
    CHECK(scripts().back() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.020000");
}

TEST_CASE_METHOD(PaSweepRunFixture, "PA sweep run stopped mid-sweep restores and reports nothing",
                 "[pa_sweep]") {
    at_attempt_n_ = 3;
    at_attempt_ = [this]() { run_->stop(); };
    start();
    REQUIRE(wait_until([this]() { return sent("RESTORE_GCODE_STATE NAME=helix_pa_sweep"); }));
    process_lvgl(200);
    CHECK_FALSE(done_.load());
    CHECK(attempts_.size() == 3);
    CHECK(pin("output_pin enable_pin_tmc_x") == 0.f);
    CHECK(pin("output_pin enable_pin_tmc_y") == 0.f);
    CHECK_FALSE(sent("SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.020000"));
}

TEST_CASE_METHOD(PaSweepRunFixture, "PA sweep run sends the restore at once on disconnect",
                 "[pa_sweep]") {
    float latched_at_disconnect = -1.f;
    at_attempt_n_ = 2;
    at_attempt_ = [this, &latched_at_disconnect]() {
        latched_at_disconnect = pin("output_pin enable_pin_tmc_x");
        run_->before_disconnect();
    };
    start();
    REQUIRE(wait_until(
        [this]() { return !attempts_.empty() && pin("output_pin enable_pin_tmc_y") == 0.f; }));
    CHECK(latched_at_disconnect == 1.f);
    CHECK(sent("SET_VELOCITY_LIMIT ACCEL=10000 SQUARE_CORNER_VELOCITY=5"));
    process_lvgl(200);
    CHECK_FALSE(done_.load());
    CHECK(attempts_.size() == 2);
}

TEST_CASE_METHOD(PaSweepRunFixture, "PA sweep run refuses a tool that is not mounted",
                 "[pa_sweep]") {
    mock_client_.set_zmod_active_tool(2); // T2 on the carriage, T0 asked for
    start();
    REQUIRE(wait_until([this]() { return done_.load(); }));
    INFO("error: " << error_);
    CHECK(error_.find("T0 is not mounted") != std::string::npos);
    CHECK_FALSE(sent("G28"));
    CHECK_FALSE(sent("SAVE_GCODE_STATE"));
}

TEST_CASE_METHOD(PaSweepRunFixture,
                 "PA sweep run that loses its link before starting sends nothing", "[pa_sweep]") {
    start();
    run_->before_disconnect(); // before the snapshot query has even gone out
    process_lvgl(300);
    CHECK_FALSE(done_.load());
    CHECK_FALSE(sent("G28"));
    CHECK_FALSE(sent("SAVE_GCODE_STATE"));
}

TEST_CASE_METHOD(PaSweepRunFixture, "PA sweep run ends when a line never gets a verdict",
                 "[pa_sweep]") {
    mock_client_.set_pa_verdicts({0, -1}); // the second line's PA_GET is never answered
    start();
    REQUIRE(wait_until([this]() { return done_.load(); }, 15000));
    INFO("error: " << error_);
    CHECK(error_.find("did not report a verdict") != std::string::npos);
    CHECK(attempts_.size() == 1);
    CHECK(pin("output_pin enable_pin_tmc_x") == 0.f);
    CHECK(sent("RESTORE_GCODE_STATE NAME=helix_pa_sweep"));
}

TEST_CASE_METHOD(PaSweepRunFixture, "PA sweep run refuses to start while the latch is already set",
                 "[pa_sweep]") {
    mock_client_.gcode_script("SET_PIN PIN=enable_pin_tmc_x VALUE=1.00");
    mock_client_.clear_gcode_script_history();
    start();
    REQUIRE(wait_until([this]() { return done_.load(); }));
    INFO("error: " << error_);
    CHECK(error_.find("still switched off") != std::string::npos);
    CHECK_FALSE(sent("G28"));
    CHECK_FALSE(sent("SAVE_GCODE_STATE"));
    CHECK(pin("output_pin enable_pin_tmc_x") == 1.f);
}

TEST_CASE("PA calibration is offered on the creator5_zmod persona through its query answers",
          "[pa_calibration][zmod]") {
    // The discovery sequence learns objects from printer.objects.list and
    // [pa_adjust] only from configfile.config; feed both the way it does.
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    std::vector<std::string> objects;
    nlohmann::json config;
    mock.send_jsonrpc("printer.objects.list", nlohmann::json::object(),
                      [&objects](const nlohmann::json& r) {
                          for (const auto& o : r["result"]["objects"]) {
                              objects.push_back(o.get<std::string>());
                          }
                      });
    mock.send_jsonrpc("printer.objects.query",
                      {{"objects", {{"configfile", nlohmann::json::array({"config"})}}}},
                      [&config](const nlohmann::json& r) {
                          config = r["result"]["status"]["configfile"]["config"];
                      });
    REQUIRE_FALSE(objects.empty());
    REQUIRE(config.is_object());

    PrinterDiscovery hw;
    hw.parse_objects(objects);
    hw.set_printer_objects(objects);
    hw.parse_config_keys(config);
    CHECK(hw.has_pa_adjust());
    CHECK(pacal::is_supported(hw));
    CHECK_FALSE(pacal::traits(hw)->firmware_runs_procedure);
}
