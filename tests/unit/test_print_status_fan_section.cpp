// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "printer_fan_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Test access helpers — keep test-only mutators out of the production API.
namespace helix {
class PrinterFanStateTestAccess {
  public:
    static void set_fans(PrinterFanState& state, std::vector<FanInfo> fans) {
        state.fans_ = std::move(fans);
    }
    static void refresh_primary_fans(PrinterFanState& state) {
        state.refresh_primary_fans_selection();
    }
};
} // namespace helix

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans picks first of each type",
                 "[fan_state][drift]") {
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 50, true, std::nullopt});
    fans.push_back(
        {"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 80, false, std::nullopt});
    fans.push_back({"fan_generic chamber", "Chamber", FanType::GENERIC_FAN, 0, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend == "heater_fan hotend_fan");
    REQUIRE(picked.aux == "fan_generic chamber");
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans returns empty when no fans of type",
                 "[fan_state][drift]") {
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 50, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend.empty());
    REQUIRE(picked.aux.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans picks first not extras",
                 "[fan_state][drift]") {
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part 1", FanType::PART_COOLING, 0, true, std::nullopt});
    fans.push_back({"fan_generic part2", "Part 2", FanType::PART_COOLING, 0, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    REQUIRE(state.classify_primary_fans().part == "fan");
}

TEST_CASE_METHOD(
    HelixTestFixture,
    "classify_primary_fans: auto-controlled lone fan -> aux, commandable lone fan -> part",
    "[fan_state][drift]") {
    // controller_fan / temperature_fan are auto-controlled and can't be the part
    // fan, so a lone one lands in the aux slot.
    for (FanType aux_type : {FanType::CONTROLLER_FAN, FanType::TEMPERATURE_FAN}) {
        PrinterFanState state;
        std::vector<FanInfo> fans;
        fans.push_back({"fan_x", "X", aux_type, 0, false, std::nullopt});
        PrinterFanStateTestAccess::set_fans(state, fans);
        auto picked = state.classify_primary_fans();
        REQUIRE(picked.part.empty());
        REQUIRE(picked.aux == "fan_x");
    }
    // fan_generic / output_pin ARE commandable, so a lone one is the part cooling
    // fan (not duplicated into aux).
    for (FanType part_type : {FanType::GENERIC_FAN, FanType::OUTPUT_PIN_FAN}) {
        PrinterFanState state;
        std::vector<FanInfo> fans;
        fans.push_back({"fan_x", "X", part_type, 0, true, std::nullopt});
        PrinterFanStateTestAccess::set_fans(state, fans);
        auto picked = state.classify_primary_fans();
        REQUIRE(picked.part == "fan_x");
        REQUIRE(picked.aux.empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "classify_primary_fans: lone named part fan (no bare 'fan') becomes .part",
                 "[fan_state][drift]") {
    // User Discord report: the real part-cooling fan is a NAMED Klipper object
    // (output_pin fan0) with no bare "fan" and no configured part role. It classifies
    // as aux, so without the fallback .part is empty and the print-status view shows
    // 0% while the fan page (FanWidget -> fans_.front()) shows the live 80% fan.
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"output_pin fan0", "Fan 0", FanType::OUTPUT_PIN_FAN, 80, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    REQUIRE(state.classify_primary_fans().part == "output_pin fan0");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "classify_primary_fans: named part fan overrides stuck bare 'fan'",
                 "[fan_state][drift]") {
    // A stale/empty bare "fan" object (stuck at 0%) sits alongside the real named
    // part fan (80%), which the fan page shows because FanWidget auto-selects the
    // front-most fan. .part must follow the named fan, not the 0% stub. A distinct
    // controller_fan also proves the promoted part fan is not duplicated into aux.
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"output_pin fan0", "Fan 0", FanType::OUTPUT_PIN_FAN, 80, true, std::nullopt});
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 0, true, std::nullopt});
    fans.push_back(
        {"controller_fan board", "Board", FanType::CONTROLLER_FAN, 30, false, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "output_pin fan0");
    // The named fan is now the part fan; aux falls through to the distinct fan.
    REQUIRE(picked.aux == "controller_fan board");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "classify_primary_fans: working bare 'fan' behind a heater stays .part",
                 "[fan_state][drift]") {
    // Regression guard: a working bare "fan" discovered AFTER a heater_fan must
    // remain .part. The fallback skips HEATER_FAN so a leading hotend fan can never
    // be promoted into the part slot.
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back(
        {"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 100, false, std::nullopt});
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 60, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend == "heater_fan hotend_fan");
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans handles empty fan list",
                 "[fan_state][drift]") {
    PrinterFanState state;
    PrinterFanStateTestAccess::set_fans(state, {});

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part.empty());
    REQUIRE(picked.hotend.empty());
    REQUIRE(picked.aux.empty());
}

// -----------------------------------------------------------------------------
// #1124 — runtime-adaptive part-fan selection (Sovol SV08 regression)
// -----------------------------------------------------------------------------
// Promoting the front-most non-heater fan whenever .part is the bare "fan"
// sticks the SV08's real toolhead [fan] behind an idle controller_fan /
// temperature_fan that sorts ahead of it. controller_fan and
// temperature_fan are auto-controlled by Klipper and can never be the M106 part
// fan, so they must never be promoted into .part.

TEST_CASE_METHOD(HelixTestFixture,
                 "classify_primary_fans: bare 'fan' keeps .part over idle auto-controlled fans "
                 "(#1124)",
                 "[fan_state][drift]") {
    // SV08 at idle: a controller_fan (running from steppers) and a temperature_fan
    // chamber sort ahead of the bare [fan]. Neither is M106-commandable, so .part
    // must stay "fan" — not latch onto the board/chamber fan.
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"controller_fan controller_fan", "Board", FanType::CONTROLLER_FAN, 30, false,
                    std::nullopt});
    fans.push_back(
        {"temperature_fan chamber", "Chamber", FanType::TEMPERATURE_FAN, 0, false, std::nullopt});
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 0, true, std::nullopt});
    fans.push_back(
        {"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 0, false, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend == "heater_fan hotend_fan");
    REQUIRE(picked.aux == "controller_fan controller_fan");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "classify_primary_fans: running bare 'fan' is never displaced (#1124)",
                 "[fan_state][drift]") {
    // Mid-print: the bare [fan] runs at 100% while the controller_fan also spins.
    // The live part fan must win regardless of discovery order.
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"controller_fan controller_fan", "Board", FanType::CONTROLLER_FAN, 40, false,
                    std::nullopt});
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 100, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    REQUIRE(state.classify_primary_fans().part == "fan");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "classify_primary_fans: stale bare 'fan' yields to a RUNNING commandable fan "
                 "(#1124)",
                 "[fan_state][drift]") {
    // Stale-[fan] printer whose real part fan (output_pin) sorts AFTER the bare
    // "fan", which a front-most heuristic misses. While the
    // commandable fan is idle, keep the canonical "fan"; once it runs, it is the
    // real part fan.
    PrinterFanState state;
    std::vector<FanInfo> idle;
    idle.push_back({"fan", "Part", FanType::PART_COOLING, 0, true, std::nullopt});
    idle.push_back({"output_pin fan0", "Fan 0", FanType::OUTPUT_PIN_FAN, 0, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, idle);
    REQUIRE(state.classify_primary_fans().part == "fan");

    std::vector<FanInfo> running;
    running.push_back({"fan", "Part", FanType::PART_COOLING, 0, true, std::nullopt});
    running.push_back(
        {"output_pin fan0", "Fan 0", FanType::OUTPUT_PIN_FAN, 90, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, running);
    REQUIRE(state.classify_primary_fans().part == "output_pin fan0");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "primary_fans_version bumps when the part-fan selection changes (#1124)",
                 "[fan_state][drift]") {
    // The print-status compact row re-binds on primary_fans_version. It must tick
    // when a fan start/stop changes which fan owns the part slot.
    PrinterFanState state;
    state.init_subjects(false);
    auto* ver = state.get_primary_fans_version_subject();
    REQUIRE(ver != nullptr);

    std::vector<FanInfo> idle;
    idle.push_back({"fan", "Part", FanType::PART_COOLING, 0, true, std::nullopt});
    idle.push_back({"output_pin fan0", "Fan 0", FanType::OUTPUT_PIN_FAN, 0, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, idle);
    PrinterFanStateTestAccess::refresh_primary_fans(state);
    int v_idle = lv_subject_get_int(ver);
    REQUIRE(state.classify_primary_fans().part == "fan");

    // The commandable fan spins up → part slot must move to it and version tick.
    std::vector<FanInfo> running;
    running.push_back({"fan", "Part", FanType::PART_COOLING, 0, true, std::nullopt});
    running.push_back(
        {"output_pin fan0", "Fan 0", FanType::OUTPUT_PIN_FAN, 90, true, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, running);
    PrinterFanStateTestAccess::refresh_primary_fans(state);
    REQUIRE(lv_subject_get_int(ver) > v_idle);
    REQUIRE(state.classify_primary_fans().part == "output_pin fan0");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "classify_primary_fans: part fan that has run stays selected when idle (#1124)",
                 "[fan_state][drift]") {
    // Sticky selection: the bare [fan] has run (ever_ran) but is momentarily off
    // (first layer / bridge) while a running fan_generic (Nevermore) spins. The
    // part slot must stay on [fan], NOT flick to the running auxiliary fan.
    PrinterFanState state;
    std::vector<FanInfo> fans;
    // {name, display, type, speed, controllable, rpm, ever_ran}
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 0, true, std::nullopt, true});
    fans.push_back({"fan_generic nevermore", "Nevermore", FanType::GENERIC_FAN, 40, true,
                    std::nullopt, false});
    PrinterFanStateTestAccess::set_fans(state, fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");                  // sticky: stays on the proven part fan
    REQUIRE(picked.aux == "fan_generic nevermore"); // the running commandable fan goes to aux
}

TEST_CASE_METHOD(
    HelixTestFixture,
    "classify_primary_fans: aux prefers commandable fan over auto-controlled (SV08, #1124)",
    "[fan_state][drift]") {
    // Full Sovol SV08 fan set in real discovery order. The aux slot must surface
    // the user-commandable Nevermore, not the idle chamber temperature_fan that
    // sorts ahead of it.
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back(
        {"temperature_fan chamber", "Chamber", FanType::TEMPERATURE_FAN, 0, false, std::nullopt});
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 100, true, std::nullopt});
    fans.push_back(
        {"fan_generic nevermore", "Nevermore", FanType::GENERIC_FAN, 40, true, std::nullopt});
    fans.push_back(
        {"temperature_fan MCU_fan", "MCU", FanType::TEMPERATURE_FAN, 0, false, std::nullopt});
    fans.push_back(
        {"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 0, false, std::nullopt});
    PrinterFanStateTestAccess::set_fans(state, fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend == "heater_fan hotend_fan");
    REQUIRE(picked.aux == "fan_generic nevermore");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "possible_primary_fan_slots answers for the fan set, not today's assignment",
                 "[fan_state]") {
    PrinterFanState state;
    const auto fan = [](const char* name, FanType type) {
        const bool commandable = type == FanType::PART_COOLING || type == FanType::GENERIC_FAN ||
                                 type == FanType::OUTPUT_PIN_FAN;
        return FanInfo{name, name, type, 0, commandable, std::nullopt};
    };
    const auto slots = [&](std::vector<FanInfo> fans) {
        PrinterFanStateTestAccess::set_fans(state, std::move(fans));
        return state.possible_primary_fan_slots();
    };
    SECTION("a part fan, a hotend fan and a generic fan fill all three") {
        const auto s =
            slots({fan("fan", FanType::PART_COOLING), fan("heater_fan he", FanType::HEATER_FAN),
                   fan("fan_generic aux", FanType::GENERIC_FAN)});
        CHECK(s.part);
        CHECK(s.hotend);
        CHECK(s.aux);
    }
    SECTION("the only commandable fan always holds the part slot") {
        const auto s = slots({fan("fan_generic aux", FanType::GENERIC_FAN)});
        CHECK(s.part);
        CHECK_FALSE(s.hotend);
        CHECK_FALSE(s.aux);
    }
    SECTION("a second aux-type fan beside it can fill aux") {
        const auto s = slots({fan("fan_generic aux", FanType::GENERIC_FAN),
                              fan("controller_fan mcu", FanType::CONTROLLER_FAN)});
        CHECK(s.part);
        CHECK(s.aux);
    }
    SECTION("a hotend fan alone fills only its slot") {
        const auto s = slots({fan("heater_fan he", FanType::HEATER_FAN)});
        CHECK_FALSE(s.part);
        CHECK(s.hotend);
        CHECK_FALSE(s.aux);
    }
    SECTION("a promoted generic fan leaves the aux slot empty, which stays possible") {
        auto fans = std::vector<FanInfo>{fan("fan", FanType::PART_COOLING),
                                         fan("fan_generic aux", FanType::GENERIC_FAN)};
        fans[1].speed_percent = 60;
        PrinterFanStateTestAccess::set_fans(state, fans);
        REQUIRE(state.classify_primary_fans().part == "fan_generic aux");
        CHECK(state.classify_primary_fans().aux.empty());
        CHECK(state.possible_primary_fan_slots().aux);
    }
    PrinterFanStateTestAccess::set_fans(state, {});
}
