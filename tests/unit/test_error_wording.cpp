// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_classify.h"
#include "error_wording.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::error_wording;

namespace {

// Klipper's trailer on a host shutdown's state_message (extras/error_mcu.py).
const std::string kFirmwareRestartTrailer =
    "\nOnce the underlying issue is corrected, use the\n\"FIRMWARE_RESTART\" command to reset "
    "the firmware, reload the\nconfig, and restart the host software.\nPrinter is shutdown\n";

// Klipper's trailer on a config error at startup (klippy.py _connect).
const std::string kRestartTrailer =
    "\nOnce the underlying issue is corrected, use the \"RESTART\"\ncommand to reload the config "
    "and restart the host software.\nPrinter is halted\n";

struct Case {
    const char* line;
    const char* id;
    std::vector<std::string> captures; ///< from capture 1 on; empty checks none
};

} // namespace

TEST_CASE("each Klipper message classifies by its own row", "[error-wording]") {
    // Each line is the exact text Klipper or Kalico raises (errmap-klipper.md).
    const std::vector<Case> cases = {
        {"MCU 'mcu' shutdown: Timer too close", "MCU-1", {}},
        {"MCU 'EBBCan' shutdown: Missed scheduling of next digital out event", "MCU-2", {}},
        {"MCU 'mcu' shutdown: Rescheduled timer in the past", "MCU-3", {}},
        {"MCU 'mcu' shutdown: Stepper too far in past", "MCU-3", {}},
        {"Lost communication with MCU 'EBBCan'", "MCU-4", {"EBBCan"}},
        {"MCU 'EBBCan' spontaneous restart", "MCU-5", {"EBBCan"}},
        {"Shutdown due to M112 command", "MCU-10", {}},
        {"Shutdown due to webhooks request", "MCU-11", {}},
        {"mcu 'mcu': Unable to connect", "MCU-13", {"mcu"}},
        {"MCU Protocol error", "MCU-14", {}},
        {"Heater extruder not heating at expected rate", "HT-1", {"extruder"}},
        {"Heater heater_generic chamber not heating at expected rate",
         "HT-1",
         {"heater_generic chamber"}},
        {"MCU 'mcu' shutdown: ADC out of range", "HT-2", {}},
        {"Requested temperature (320.0) out of range (0.0:300.0)",
         "HT-4",
         {"320.0", "0.0", "300.0"}},
        {"TMC 'stepper_x' reports error: DRV_STATUS: 00000000 cs_actual=0(Reset?)",
         "TMC-1-power",
         {"stepper_x"}},
        {"Unable to read tmc uart 'stepper_y' register IFCNT", "TMC-2", {"stepper_y", "IFCNT"}},
        {"Unable to write tmc spi 'stepper_z' register GCONF", "TMC-2", {"stepper_z", "GCONF"}},
        {"Extrude below minimum temp", "EX-1", {}},
        {"Extrude only move too long (120.000mm vs 50.000mm)", "EX-2", {"120.000", "50.000"}},
        {"Move exceeds maximum extrusion (2.531mm^2 vs 0.640mm^2)", "EX-3", {"2.531", "0.640"}},
        {"Move out of range: 251.000 100.000 0.300 [12.400]",
         "MV-2",
         {"251.000", "100.000", "0.300"}},
        {"Must home axis first: 120.000 120.000 10.000 [0.000]", "MV-1", {}},
        {"Must home first: 0.000 0.000 200.000 [0.000]", "MV-1", {}},
        {"No trigger on probe after full movement", "MV-3", {"probe"}},
        {"No trigger on z1 after full movement", "MV-3", {"z1"}},
        {"Endstop x still triggered after retract", "MV-4", {"x"}},
        {"Probe triggered prior to movement", "PR-1", {}},
        {"Probe samples exceed samples_tolerance", "PR-2", {}},
        {"BLTouch failed to verify sensor state", "PR-4", {}},
        {"BLTouch failed to raise probe", "PR-4", {}},
        {"Aborting quad_gantry_level required adjustment 8.412000 is greater than max_adjust "
         "7.000000",
         "PR-10",
         {"8.412000", "7.000000"}},
        {"Retries aborting: Probed points range is increasing. Possibly Z motor numbering is "
         "wrong",
         "PR-11",
         {}},
        {"Invalid adxl345 id (got 0 vs e5). This is generally indicative of connection problems "
         "(e.g. faulty wiring) or a faulty adxl345 chip.",
         "IS-1",
         {"adxl345"}},
        {"Invalid lis2dw id (got 0 vs 44). This is generally indicative of connection problems "
         "(e.g. faulty wiring) or a faulty lis2dw chip.",
         "IS-1",
         {"lis2dw"}},
        {"Option 'max_temp' in section 'extruder' must be specified",
         "CF-1",
         {"max_temp", "extruder"}},
        {"Option 'sensor_typ' is not valid in section 'extruder'",
         "CF-2",
         {"sensor_typ", "extruder"}},
        {"Section 'fan_generic foo' is not a valid config section", "CF-3", {"fan_generic foo"}},
        {"Error evaluating 'gcode_macro PRINT_START:gcode': UndefinedError: 'dict object' has no "
         "attribute 'BED_TEMP'",
         "GC-5",
         {"gcode_macro PRINT_START:gcode", "UndefinedError",
          "'dict object' has no attribute 'BED_TEMP'"}},
        {"SAVE_CONFIG section 'probe' option 'z_offset' conflicts with included value",
         "SC-1",
         {"probe", "z_offset"}},
        {"bed_mesh: Unknown profile [default]", "BM-1", {"default"}},
    };
    for (const auto& c : cases) {
        CAPTURE(c.line);
        const auto got = classify(c.line);
        REQUIRE(got.has_value());
        CHECK(got->id == c.id);
        for (size_t i = 0; i < c.captures.size(); ++i) {
            REQUIRE(got->captures.size() > i + 1);
            CHECK(got->captures[i + 1] == c.captures[i]);
        }
    }
}

TEST_CASE("a message no row knows is left alone", "[error-wording]") {
    CHECK_FALSE(classify("Some macro said something").has_value());
    CHECK_FALSE(classify("").has_value());
    // The keys are anchored: the same words mid-sentence are not the message.
    CHECK_FALSE(classify("The macro reported: Timer too close").has_value());
    // And case-sensitive, as Klipper raises them.
    CHECK_FALSE(classify("timer too close").has_value());
}

TEST_CASE("a shutdown's state_message classifies by its reason", "[error-wording]") {
    const std::string msg =
        "MCU 'mcu' shutdown: Timer too close\nThis often indicates the host computer is "
        "overloaded. Check for other processes consuming excessive CPU time, high swap usage, "
        "disk errors, overheating, unstable voltage, or similar system problems on the host "
        "computer." +
        kFirmwareRestartTrailer;
    const auto got = classify(msg);
    REQUIRE(got.has_value());
    CHECK(got->id == "MCU-1");
    CHECK(got->captures.at(0) == "mcu");
    CHECK(got->recovery == Recovery::FirmwareRestart);
    CHECK(got->reason == "Timer too close");
}

TEST_CASE("a console line keeps classifying with its bang prefix", "[error-wording]") {
    const auto got = classify("!! MCU 'mcu' shutdown: Timer too close");
    REQUIRE(got.has_value());
    CHECK(got->id == "MCU-1");
}

TEST_CASE("an MCU reconnected after shutdown names the board", "[error-wording]") {
    const auto got = classify("Previous MCU 'EBBCan' shutdown: Timer too close");
    REQUIRE(got.has_value());
    CHECK(got->id == "MCU-1");
    CHECK(got->captures.at(0) == "EBBCan");
}

TEST_CASE("a config error asks for the file to be fixed", "[error-wording]") {
    const auto got =
        classify("Option 'max_temp' in section 'extruder' must be specified" + kRestartTrailer);
    REQUIRE(got.has_value());
    CHECK(got->category == Category::Config);
    CHECK(got->recovery == Recovery::RestartAfterFix);
}

TEST_CASE("an ADC shutdown names the sensor when Klipper clarifies it", "[error-wording]") {
    const std::string msg = "MCU 'mcu' shutdown: ADC out of range\n\nSensor 'extruder' temperature "
                            "-12.5 not in range 0.0:300.0\nThis generally occurs when a heater "
                            "temperature exceeds\nits configured min_temp or max_temp." +
                            kFirmwareRestartTrailer;
    const auto got = classify(msg);
    REQUIRE(got.has_value());
    CHECK(got->id == "HT-2");
    REQUIRE(got->captures.size() >= 5);
    CHECK(got->captures[1] == "extruder");
    CHECK(got->captures[2] == "-12.5");
    CHECK(got->captures[3] == "0.0");
    CHECK(got->captures[4] == "300.0");
    const auto w = word(*got);
    REQUIRE(w.has_value());
    CHECK(w->summary.find("extruder") != std::string::npos);
    CHECK(w->cause.find("-12.5") != std::string::npos);
}

TEST_CASE("a stepper driver fault is worded by its worst flag", "[error-wording]") {
    const auto ids = [](const char* line) {
        const auto got = classify(line);
        return got ? got->id : std::string();
    };
    // A short outranks the overtemperature and reset flags beside it.
    CHECK(ids("TMC 'stepper_x' reports error: DRV_STATUS: 18000000 ot=1(OvertempError!) "
              "s2ga=1(ShortToGND_A!)") == "TMC-1-short");
    CHECK(ids("TMC 'stepper_x' reports error: DRV_STATUS: 02000000 ot=1(OvertempError!)") ==
          "TMC-1-overtemp");
    CHECK(ids("TMC 'extruder' reports error: GSTAT: 00000004 uv_cp=1(Undervoltage!)") ==
          "TMC-1-power");
    CHECK(ids("TMC 'stepper_y' reports error: GSTAT: 00000002 drv_err=1(ErrorShutdown!)") ==
          "TMC-1");
}

TEST_CASE("the worded text carries the message's names and numbers", "[error-wording]") {
    const auto got = classify("Move out of range: 251.000 100.000 0.300 [12.400]");
    REQUIRE(got.has_value());
    const auto w = word(*got);
    REQUIRE(w.has_value());
    CHECK_FALSE(w->summary.empty());
    CHECK_FALSE(w->fix.empty());
    CHECK(w->cause.find("251.000") != std::string::npos);
    CHECK(w->detail().find(w->fix) != std::string::npos);

    const auto lost = classify("Lost communication with MCU 'EBBCan'");
    REQUIRE(lost.has_value());
    const auto lw = word(*lost);
    REQUIRE(lw.has_value());
    CHECK(lw->summary.find("EBBCan") != std::string::npos);
}

TEST_CASE("Kalico's calibration warning classifies as a warning with its own words",
          "[error-wording]") {
    const auto got = classify("Target calibration temperature of 30.0°C is <= 15°C below the "
                              "ambient temperature");
    REQUIRE(got.has_value());
    CHECK(got->severity == Severity::Warning);
    CHECK_FALSE(got->worded);
    CHECK_FALSE(word(*got).has_value());
}

// ---- The console classifier presents the wording ----

using helix::ClassifyContext;
using helix::ErrorSeverity;

TEST_CASE("a worded console error leads with its summary and keeps Klipper's text",
          "[error-wording][classify]") {
    ClassifyContext ctx;
    const std::string line = "!! Move out of range: 251.000 100.000 0.300 [12.400]";
    const auto e = helix::error_classify::classify(line, ctx);
    REQUIRE(e.has_value());
    const auto w = word(*classify(line));
    REQUIRE(w.has_value());
    CHECK(e->title == w->summary);
    CHECK(e->detail.find("251.000") != std::string::npos);
    CHECK(e->detail.find(w->fix) != std::string::npos);
    // The cross-channel dedup identity stays the firmware's own line.
    CHECK(e->raw_detail == "Move out of range: 251.000 100.000 0.300 [12.400]");
    CHECK(e->severity == ErrorSeverity::WARNING);
}

TEST_CASE("a worded console error mid-print still blocks", "[error-wording][classify]") {
    ClassifyContext ctx;
    ctx.is_printing = true;
    const auto e = helix::error_classify::classify("!! Extrude below minimum temp", ctx);
    REQUIRE(e.has_value());
    CHECK_FALSE(e->title.empty());
    CHECK(e->severity == ErrorSeverity::CRITICAL);
}

TEST_CASE("Kalico's calibration warning stays a toast mid-print", "[error-wording][classify]") {
    ClassifyContext ctx;
    ctx.is_printing = true;
    const auto e = helix::error_classify::classify(
        "!! Target calibration temperature of 30.0°C is <= 15°C below the ambient temperature",
        ctx);
    REQUIRE(e.has_value());
    CHECK(e->severity == ErrorSeverity::WARNING);
    CHECK(e->detail.find("Target calibration temperature") != std::string::npos);
}
