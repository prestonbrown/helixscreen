// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_verbatim.cpp
 * @brief Regression gate: outgoing G-code is transmitted VERBATIM.
 *
 * HelixScreen must send Klipper exactly the bytes it was asked to send. It must
 * NOT append a provenance comment, a line-number prefix, a trailing space, or
 * anything else. Every test here compares the captured wire string against the
 * input with ==, so any decoration reintroduced at any of the three send paths
 * fails immediately.
 *
 * DO NOT RE-ADD " ; from helixscreen". It shipped once and broke three
 * different things before it was removed:
 *
 *   1. M117 / M118 — Klipper hands the ENTIRE remainder of the line to the
 *      command as a literal text payload without stripping a trailing ";"
 *      comment, so the printer's display read "Hello World ; from helixscreen".
 *   2. FlashForge AD5X — its firmware re-echoes each received line inside a
 *      quoted RESPOND MSG="...". Klipper truncates at the first ';' even inside
 *      quotes, leaving an unterminated quote -> "Malformed command".
 *   3. Kalico/Klipper macros that branch on `rawparams` — the case this file is
 *      named for; see the CC1/COSMOS test below.
 *
 * The three send paths under test (all of which must stay verbatim):
 *   - moonraker_client.cpp       — MoonrakerClient::gcode_script()
 *   - moonraker_api_controls.cpp — MoonrakerAPI::execute_gcode()
 *   - moonraker_motion_api.cpp   — MoonrakerMotionAPI::execute_gcode()
 *
 * Mutation check: append anything to the `script` param in any of those three
 * files and the corresponding test below fails.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

/// Minimal MoonrakerClient subclass that captures the `script` param of the
/// outgoing printer.gcode.script RPC instead of sending it over a real
/// WebSocket. Overriding only the 2-arg send_jsonrpc() (what gcode_script()
/// calls) leaves the real MoonrakerClient::gcode_script() path intact, unlike
/// MoonrakerClientMock which overrides gcode_script() itself.
class GcodeCaptureClient : public helix::MoonrakerClient {
  public:
    std::string last_script;
    int send_count = 0;

    int send_jsonrpc(const std::string& method, const json& params) override {
        if (method == "printer.gcode.script" && params.contains("script") &&
            params["script"].is_string()) {
            last_script = params["script"].get<std::string>();
            ++send_count;
        }
        return 0;
    }
};

} // namespace

// ============================================================================
// Site 1: moonraker_client.cpp — MoonrakerClient::gcode_script()
// ============================================================================

TEST_CASE("MoonrakerClient::gcode_script sends the command byte-for-byte",
          "[moonraker][gcode_verbatim][gate]") {
    GcodeCaptureClient client;

    // Each entry is sent and must come back out identical. Any appended
    // comment, prefix, or whitespace breaks the == comparison.
    const std::vector<std::string> commands = {
        "G28",
        "G28 X",
        "G1 X10 F3000",
        "M117 Hello World",
        "M118 status update",
        "m117 lower case",
        "M1170",
        "  G28",                      // leading whitespace preserved
        "G28\nG1 X10",                // multi-line: NO per-line decoration
        "M117 Status\nG28",           // mixed text-payload + ordinary
        "SET_LED LED=x RED=1 SYNC=0", // long parameterized macro
        "PURGE_FILAMENT",             // bare user macro
        "G28 ; user's own comment",   // a comment the USER wrote survives as-is
    };

    for (const auto& cmd : commands) {
        client.last_script.clear();
        client.gcode_script(cmd);
        INFO("command: " << cmd);
        REQUIRE(client.last_script == cmd);
    }
}

TEST_CASE("MoonrakerClient::gcode_script appends no trailing comment character",
          "[moonraker][gcode_verbatim][gate]") {
    GcodeCaptureClient client;
    client.gcode_script("G28");

    REQUIRE(client.send_count == 1);
    // Explicit belt-and-braces: no semicolon at all was introduced, and the
    // exact historical offender is absent.
    CHECK(client.last_script.find(';') == std::string::npos);
    CHECK(client.last_script.find("helixscreen") == std::string::npos);
    CHECK(client.last_script.size() == std::string("G28").size());
}

// ============================================================================
// Sites 2 & 3: MoonrakerAPI::execute_gcode() and the motion API
// ============================================================================

namespace {

class ExecuteGcodeFixture : public LVGLTestFixture {
  public:
    ExecuteGcodeFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~ExecuteGcodeFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    /// The exact `script` param of the last printer.gcode.script RPC — the wire
    /// string, before the mock splits a multi-line script for its own per-line
    /// simulation (that split is a mock artifact; gcode_script_history() would
    /// show the pieces, not what HelixScreen actually transmitted).
    const std::string& last_sent() const {
        return mock_client.last_send_script();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

} // namespace

TEST_CASE_METHOD(ExecuteGcodeFixture, "MoonrakerAPI::execute_gcode sends commands verbatim",
                 "[moonraker][gcode_verbatim][gate][mock]") {
    const std::vector<std::string> commands = {
        "G28", "G28 Z", "M117 Hello World", "M118 done homing", "BED_MESH_CALIBRATE", "G28\nG1 X10",
    };

    for (const auto& cmd : commands) {
        api->execute_gcode(cmd, nullptr, nullptr);
        INFO("command: " << cmd);
        REQUIRE(last_sent() == cmd);
    }
}

TEST_CASE_METHOD(ExecuteGcodeFixture,
                 "execute_gcode sends a discretionary command verbatim on the queued path",
                 "[moonraker][gcode_verbatim][gate][mock]") {
    // The discretionary (fan/temp/LED) branch of execute_gcode builds its own
    // params object and short-circuits before the normal send. It is a separate
    // code path and must be verbatim too. An externally-initiated blocking op
    // (idle_timeout "Printing" without a file print) is what routes into it.
    lv_subject_set_int(state.get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    helix::PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, true);

    api->execute_gcode("M106 S128", nullptr, nullptr);

    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    CHECK(last_sent() == "M106 S128");
}

TEST_CASE_METHOD(ExecuteGcodeFixture, "motion API sends a jog move verbatim",
                 "[moonraker][gcode_verbatim][gate][mock][motion]") {
    bool error_called = false;
    api->motion().move_axis('X', 10.0, 6000.0, nullptr,
                            [&](const MoonrakerError&) { error_called = true; });

    CHECK_FALSE(error_called);
    REQUIRE_FALSE(last_sent().empty());
    const std::string& sent = last_sent();
    INFO("motion sent: " << sent);
    // The motion API composes its own G-code, so this cannot compare against a
    // fixed input string — instead assert nothing was bolted onto the end.
    CHECK(sent.find(';') == std::string::npos);
    CHECK(sent.find("helixscreen") == std::string::npos);
    CHECK(sent.back() != ' ');
}

// ============================================================================
// The named regression: Elegoo CC1 / COSMOS 26.07.0 homing_override rawparams
// ============================================================================

TEST_CASE_METHOD(ExecuteGcodeFixture,
                 "CC1/COSMOS homing_override: bare G28 carries no rawparams payload",
                 "[moonraker][gcode_verbatim][gate][mock][rawparams]") {
    // WHY THIS TEST EXISTS.
    //
    // Klipper/Kalico's GCodeCommand::get_raw_command_parameters() (`rawparams`
    // in a macro) is derived from `origline`, which is captured BEFORE comment
    // stripping. It therefore does NOT strip a ';' comment. A macro that
    // branches on truthiness of rawparams sees ANY trailing comment as
    // "arguments were passed".
    //
    // The CC1's COSMOS 26.07.0 [homing_override] is exactly that shape:
    //
    //     G0 Z5 F1200
    //     {% if not rawparams %}         ... home all axes ...
    //     {% else %}
    //       {% if 'X' in rawparams %} G28 X {% endif %}   (etc. for Y, Z)
    //     {% endif %}
    //
    // Sending "G28 ; from helixscreen" made rawparams == "; from helixscreen",
    // which is truthy, so the else-branch ran; it then found no capital X/Y/Z
    // in that string and executed ZERO G28 calls — after the Z-hop had already
    // moved the head. The user saw "Z moves slightly then homing fails".
    //
    // The gate: a bare G28 must arrive as exactly "G28", so rawparams is empty
    // and the macro takes the home-all branch.
    api->execute_gcode("G28", nullptr, nullptr);

    const std::string& sent = last_sent();

    REQUIRE(sent == "G28");

    // Restate the failure mode directly in terms of what the macro computes:
    // everything after the command token is what Klipper hands to rawparams.
    const std::string rawparams = sent.substr(std::string("G28").size());
    CHECK(rawparams.empty()); // truthy rawparams => COSMOS skips homing entirely
}

TEST_CASE_METHOD(ExecuteGcodeFixture,
                 "CC1/COSMOS homing_override: axis-limited G28 keeps only the user's axes",
                 "[moonraker][gcode_verbatim][gate][mock][rawparams]") {
    // The else-branch of the same macro greps rawparams for 'X'/'Y'/'Z'. A
    // decoration that happened to contain one of those letters would silently
    // home an axis the caller never asked for, so the payload must be exactly
    // what was requested and nothing more.
    api->execute_gcode("G28 X", nullptr, nullptr);

    const std::string& sent = last_sent();

    REQUIRE(sent == "G28 X");

    const std::string rawparams = sent.substr(std::string("G28").size());
    CHECK(rawparams.find('Y') == std::string::npos);
    CHECK(rawparams.find('Z') == std::string::npos);
    CHECK(rawparams.find('X') != std::string::npos);
}
