// SPDX-License-Identifier: GPL-3.0-or-later
#include "firmware_fault_codes.h"
#include "printer_discovery.h"
#include "qidi_fault_codes.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

/// A printer with a QIDI Box, as discovery sees it.
PrinterDiscovery qidi_box_printer() {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"box_extras", "box_stepper slot0", "box_stepper slot1",
                                            "box_stepper slot2", "box_stepper slot3",
                                            "save_variables", "extruder", "heater_bed"}));
    return hw;
}

PrinterDiscovery plain_klipper_printer() {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "save_variables"}));
    return hw;
}

} // namespace

TEST_CASE("every QIDI line form decodes to its code", "[qidi][fault-codes]") {
    struct Case {
        const char* line;
        int module;
        int index;
    };
    // Each is a form QIDI's extras raise (QIDI_SUPPORT.md "Fault codes").
    const std::vector<Case> cases = {
        {"!!Code:QDE_004_001; Message:Slot loading failure, please check the trigger, please "
         "reload 1A.",
         4, 1},
        {"// Code:QDE_004_006; Message:Extruder loading failure.", 4, 6},
        {"Code:QDE_002_002;Message:{\"fan_name\":\"chamber_fan\"} speed check error ,turn off "
         "{\"heater\":\"[chamber]\"}",
         2, 2},
        {"!! QDE_004_010 The current feeding status is incorrect. Please exit the filament from "
         "the extruder.",
         4, 10},
    };
    for (const auto& c : cases) {
        CAPTURE(c.line);
        const auto code = qidi::decode_fault_code(c.line);
        REQUIRE(code.has_value());
        CHECK(code->kind == 'E');
        CHECK(code->module == c.module);
        CHECK(code->index == c.index);
    }
    CHECK_FALSE(qidi::decode_fault_code("// Print already paused").has_value());
    CHECK(qidi::format_fault_code({'E', 4, 6}) == "QDE_004_006");
}

TEST_CASE("a QIDI code is worded with the slot its message names", "[qidi][fault-codes]") {
    ClassifyContext idle;
    const auto e = qidi::classify_fault_line(
        "Code:QDE_004_001; Message:Slot loading failure, please check the trigger, please "
        "reload 2C.",
        idle);
    REQUIRE(e.has_value());
    CHECK(e->code == "QDE_004_001");
    CHECK(e->source == ErrorSource::QIDI);
    // QIDI's 2C is the second Box's third lane: global slot 6, "Slot 7".
    CHECK(e->title.find("Slot 7") != std::string::npos);
    CHECK(e->detail.find("Slot 7") != std::string::npos);
    CHECK(e->severity == ErrorSeverity::WARNING);
}

TEST_CASE("a QIDI pause code blocks while printing, a warning code never does",
          "[qidi][fault-codes]") {
    ClassifyContext printing;
    printing.is_printing = true;
    const auto pause = qidi::classify_fault_line(
        "// Code:QDE_004_005; Message:Please load the filament to 1B first.", printing);
    REQUIRE(pause.has_value());
    CHECK(pause->severity == ErrorSeverity::CRITICAL);
    CHECK(pause->title.find("Slot 2") != std::string::npos);

    const auto dryer = qidi::classify_fault_line(
        "Code:QDE_005_001; Message:Heater heater_box1 not heating at expected rate", printing);
    REQUIRE(dryer.has_value());
    CHECK(dryer->severity == ErrorSeverity::WARNING);
    CHECK_FALSE(dryer->title.empty());
}

TEST_CASE("a QIDI runout leaves the paused print to the runout dialog", "[qidi][fault-codes]") {
    // The runout guidance (Load, Resume) owns a paused runout; this code says
    // why beside it, and stands down where that dialog explains its slot.
    ClassifyContext paused;
    paused.is_paused = true;
    const auto e = qidi::classify_fault_line(
        "// Code:QDE_004_016; Message:The filament has been exhausted, please load the filament "
        "to 1B.",
        paused);
    REQUIRE(e.has_value());
    CHECK(e->severity == ErrorSeverity::WARNING);
    CHECK(e->runout_lane == 1);
    CHECK(e->title.find("Slot 2") != std::string::npos);
}

TEST_CASE("a QIDI fan code names the fan", "[qidi][fault-codes]") {
    ClassifyContext idle;
    const auto e = qidi::classify_fault_line(
        "Code:QDE_002_001; Message:{\"fan_name\":\"hotend_fan\"} speed{\"0\"} may be error, "
        "please check!",
        idle);
    REQUIRE(e.has_value());
    CHECK(e->title.find("hotend_fan") != std::string::npos);
}

TEST_CASE("a QIDI code without a row keeps the firmware's message", "[qidi][fault-codes]") {
    ClassifyContext idle;
    const auto e =
        qidi::classify_fault_line("Code:QDE_004_008; Message:Extruder unloading failure.", idle);
    REQUIRE(e.has_value());
    CHECK(e->title.empty());
    CHECK(e->detail == "Extruder unloading failure.");
}

TEST_CASE("retry_step names a fault and its slot", "[qidi][fault-codes]") {
    int slot = -1;
    const auto c = qidi::decode_retry_step("QDE_004_006_3", &slot);
    REQUIRE(c.has_value());
    CHECK(c->index == 6);
    CHECK(slot == 3);
    CHECK(qidi::try_again_retries(*c));

    const auto global = qidi::decode_retry_step("QDE_004_016_00", &slot);
    REQUIRE(global.has_value());
    CHECK(slot == 0);
    CHECK_FALSE(qidi::try_again_retries(*global));

    CHECK_FALSE(qidi::decode_retry_step("None", &slot).has_value());
    CHECK_FALSE(qidi::decode_retry_step("", &slot).has_value());
}

TEST_CASE("a QIDI printer's comment lines classify only when they carry a code",
          "[qidi][fault-codes]") {
    const PrinterDiscovery hw = qidi_box_printer();
    REQUIRE(faultcodes::firmware_reports_fault_codes(hw));
    ClassifyContext printing;
    printing.is_printing = true;

    const auto coded = faultcodes::classify(
        hw, "// Code:QDE_004_005; Message:Please load the filament to 3A first.", printing);
    REQUIRE(coded.has_value());
    CHECK(coded->code == "QDE_004_005");
    CHECK(coded->severity == ErrorSeverity::CRITICAL);

    // The unprefixed channel the chamber fan raises on.
    const auto bare = faultcodes::classify(
        hw,
        "Code:QDE_002_002;Message:{\"fan_name\":\"chamber_fan\"} speed check error ,turn off "
        "{\"heater\":\"[chamber]\"}",
        printing);
    REQUIRE(bare.has_value());
    CHECK(bare->code == "QDE_002_002");

    CHECK_FALSE(faultcodes::classify(hw, "// Print already paused", printing).has_value());
    CHECK_FALSE(faultcodes::classify(hw, "ok", printing).has_value());
}

TEST_CASE("a printer without QIDI firmware never reads QIDI codes", "[qidi][fault-codes]") {
    const PrinterDiscovery hw = plain_klipper_printer();
    CHECK_FALSE(faultcodes::firmware_reports_fault_codes(hw));
    CHECK_FALSE(faultcodes::classify(hw, "// Code:QDE_004_005; Message:x", {}).has_value());
}
