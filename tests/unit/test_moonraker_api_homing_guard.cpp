// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_homing_guard.cpp
 * @brief Tests for the "no app-initiated homing during an active print" guard.
 *
 * Layer 1 of the homing guard blocks a literal G28 at the gcode-send boundary in
 * BOTH MoonrakerAPI::execute_gcode (AMS auto-home, general controls) and
 * MoonrakerMotionAPI::execute_gcode (Home buttons, calibration auto-homes).
 *
 * Rationale: on loadcell-Z printers (AD5X) a G28 probes DOWN into the bed; issued
 * while a print is PRINTING or PAUSED it drives the nozzle into the part.
 *
 * Also unit-tests the is_homing_gcode() token matcher directly.
 */

#include "../../include/gcode_homing.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../src/api/moonraker_gcode_guards.h"
#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Case 1: is_homing_gcode() token matcher

TEST_CASE("is_homing_gcode matches G28 as first token, case-insensitive", "[homing_guard][gcode]") {
    // Positive: G28 is the first whitespace-delimited token on some line.
    CHECK(is_homing_gcode("G28"));
    CHECK(is_homing_gcode("g28"));
    CHECK(is_homing_gcode("G28 X"));
    CHECK(is_homing_gcode("G28 X Y"));
    CHECK(is_homing_gcode("G28 Z\nG1 X10"));
    CHECK(is_homing_gcode("  G28"));       // leading whitespace trimmed
    CHECK(is_homing_gcode("G1 X10\nG28")); // homing on a later line
    CHECK(is_homing_gcode("G28 ; comment"));

    // Negative: G28 only as an argument, a different command, or empty.
    CHECK_FALSE(is_homing_gcode("G1 X28"));
    CHECK_FALSE(is_homing_gcode("M28"));
    CHECK_FALSE(is_homing_gcode("G280"));
    CHECK_FALSE(is_homing_gcode("BED_MESH_CALIBRATE"));
    CHECK_FALSE(is_homing_gcode(""));
    CHECK_FALSE(is_homing_gcode("   \n  ")); // whitespace only
}

// Fixture: MoonrakerAPI + mock client, print-state settable per test

namespace {

class HomingGuardApiFixture : public LVGLTestFixture {
  public:
    HomingGuardApiFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        // execute_gcode()'s klippy-halted gate would otherwise reject everything:
        // subjects initialize to SHUTDOWN until a real state update arrives.
        state.set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~HomingGuardApiFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    void set_print_state(PrintJobState s) {
        lv_subject_set_int(state.print_state().get_print_state_enum_subject(), static_cast<int>(s));
    }

    void error_cb(const MoonrakerError& err) {
        error_called = true;
        captured_error = err;
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;

    bool error_called = false;
    MoonrakerError captured_error;
};

} // namespace

// Case 2: MoonrakerAPI::execute_gcode(G28) blocked while PRINTING / PAUSED

TEST_CASE_METHOD(HomingGuardApiFixture, "execute_gcode refuses G28 while a print is active",
                 "[homing_guard][mock]") {
    SECTION("PRINTING blocks G28 - no gcode sent, on_error fired") {
        set_print_state(PrintJobState::PRINTING);
        api->execute_gcode("G28", nullptr, [this](const MoonrakerError& err) { error_cb(err); });

        CHECK(error_called);
        CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
        CHECK(captured_error.message == "Homing is disabled while a print is in progress");
        CHECK(mock_client.gcode_script_history().empty());
    }

    SECTION("PAUSED blocks G28 - head parked over the print") {
        set_print_state(PrintJobState::PAUSED);
        api->execute_gcode("G28 X Y", nullptr,
                           [this](const MoonrakerError& err) { error_cb(err); });

        CHECK(error_called);
        CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
        CHECK(mock_client.gcode_script_history().empty());
    }
}

// Case 3: STANDBY / COMPLETE allow G28

TEST_CASE_METHOD(HomingGuardApiFixture, "execute_gcode allows G28 when idle",
                 "[homing_guard][mock]") {
    SECTION("STANDBY sends G28") {
        set_print_state(PrintJobState::STANDBY);
        api->execute_gcode("G28", nullptr, [this](const MoonrakerError& err) { error_cb(err); });

        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
        CHECK_FALSE(mock_client.gcode_script_history().empty());
    }

    SECTION("COMPLETE sends G28") {
        set_print_state(PrintJobState::COMPLETE);
        api->execute_gcode("G28", nullptr, [this](const MoonrakerError& err) { error_cb(err); });

        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }
}

// Case 4: non-homing gcode is never blocked

TEST_CASE_METHOD(HomingGuardApiFixture, "execute_gcode sends non-homing gcode even while printing",
                 "[homing_guard][mock]") {
    set_print_state(PrintJobState::PRINTING);
    api->execute_gcode("G1 X10 F3000", nullptr,
                       [this](const MoonrakerError& err) { error_cb(err); });

    CHECK_FALSE(error_called);
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    CHECK_FALSE(mock_client.gcode_script_history().empty());
}

// Case 5: MoonrakerMotionAPI::home_axes routes through the same guard

TEST_CASE_METHOD(HomingGuardApiFixture, "motion home_axes blocked while printing",
                 "[homing_guard][mock][motion]") {
    SECTION("PRINTING blocks home_axes - no gcode sent") {
        set_print_state(PrintJobState::PRINTING);
        api->motion().home_axes("", nullptr, [this](const MoonrakerError& err) { error_cb(err); });

        CHECK(error_called);
        CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
        CHECK(mock_client.gcode_script_history().empty());
    }

    SECTION("STANDBY allows home_axes - gcode sent") {
        set_print_state(PrintJobState::STANDBY);
        api->motion().home_axes("XY", nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });

        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }
}

// Case 6: reject_homing_during_active_print() shared helper, unit-tested directly

TEST_CASE_METHOD(HomingGuardApiFixture,
                 "reject_homing_during_active_print rejects only homing during an active print",
                 "[homing_guard][gcode]") {
    auto cb = [this](const MoonrakerError& err) { error_cb(err); };

    SECTION("G28 while PRINTING is rejected with the NOT_READY error") {
        set_print_state(PrintJobState::PRINTING);
        CHECK(helix::api::reject_homing_during_active_print("G28", state, false, cb, "[Test]"));
        CHECK(error_called);
        CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
        CHECK(captured_error.message == "Homing is disabled while a print is in progress");
    }

    SECTION("G28 while PAUSED is rejected") {
        set_print_state(PrintJobState::PAUSED);
        CHECK(helix::api::reject_homing_during_active_print("G28 X", state, false, cb, "[Test]"));
        CHECK(error_called);
    }

    SECTION("G28 while STANDBY passes through (returns false, no error)") {
        set_print_state(PrintJobState::STANDBY);
        CHECK_FALSE(
            helix::api::reject_homing_during_active_print("G28", state, false, cb, "[Test]"));
        CHECK_FALSE(error_called);
    }

    SECTION("non-homing gcode while PRINTING passes through") {
        set_print_state(PrintJobState::PRINTING);
        CHECK_FALSE(
            helix::api::reject_homing_during_active_print("G1 X10", state, false, cb, "[Test]"));
        CHECK_FALSE(error_called);
    }
}
