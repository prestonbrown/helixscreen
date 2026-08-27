// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_save_config.cpp
 * @brief The mock's pending-config model, and the per-tool offsets riding on it.
 *
 * Klipper splits a durable setting into three separate steps, and code that
 * conflates any two of them is broken on a real printer:
 *
 *   1. the runtime write (SET_TOOL_PARAMETER) - live now, gone on restart;
 *   2. the STAGE (SAVE_TOOL_PARAMETER -> configfile.set()) - changes nothing
 *      live, only marks the config dirty via configfile.save_config_pending;
 *   3. the COMMIT (SAVE_CONFIG) - writes printer.cfg and restarts Klipper.
 *
 * The mock previously modelled only step 1, so an offset that was set and never
 * saved survived a restart exactly like a saved one. That made the persist path
 * unfalsifiable: no test could tell a working save from a forgotten one, which
 * is the whole failure mode worth guarding.
 *
 * These tests drive raw gcode through the mock's synchronous
 * printer.gcode.script handler and assert on the mock's own state, so they
 * describe the SIMULATOR's fidelity to Klipper rather than any panel's use of
 * it.
 */

#include "../helix_test_fixture.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"

#include "../catch_amalgamated.hpp"

using nlohmann::json;

namespace {

/// A fast-restarting mock. The offset revert itself is synchronous inside
/// trigger_restart(); the speedup only keeps the restart thread the destructor
/// joins from costing two real seconds per case.
struct SaveConfigFixture : public HelixTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::MULTI_EXTRUDER, 100.0};

    /// Run one script through the mock's printer.gcode.script handler, which
    /// executes synchronously inside the call. Returns whether the rpc acked.
    ///
    /// The callback-taking overload is the only one that reaches the handler
    /// registry at all - send_jsonrpc(method, params) is a logging stub that
    /// returns success without running anything.
    bool gcode(const std::string& script) {
        bool acked = false;
        client.send_jsonrpc(
            "printer.gcode.script", json{{"script", script}},
            [&acked](const json&) { acked = true; }, [](const MoonrakerError&) {});
        return acked;
    }
};

} // namespace

TEST_CASE_METHOD(SaveConfigFixture, "mock: SET_TOOL_PARAMETER is runtime-only", "[mock][toolchanger]") {
    REQUIRE_FALSE(client.save_config_pending());

    gcode("SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.075");

    // Live immediately...
    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.075));
    // ...and nothing is owed to printer.cfg yet. A mock that marked the config
    // dirty here would hide a UI that never sends the persist half at all.
    CHECK_FALSE(client.save_config_pending());
}

TEST_CASE_METHOD(SaveConfigFixture, "mock: SAVE_TOOL_PARAMETER stages without changing anything live",
                 "[mock][toolchanger]") {
    gcode("SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.075");
    gcode("SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset");

    CHECK(client.save_config_pending());
    // Section keyed exactly as Klipper's config section, values stringified -
    // configfile.set() stores str(value).
    const json items = client.save_config_pending_items();
    REQUIRE(items.contains("tool T1"));
    CHECK(items["tool T1"]["gcode_z_offset"].get<std::string>() == "-0.075");

    // klipper-toolchanger's save_parameter() takes no VALUE=: it persists what
    // the tool already holds, so the runtime value is untouched.
    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.075));
}

TEST_CASE_METHOD(SaveConfigFixture, "mock: an unsaved offset does not survive a restart",
                 "[mock][toolchanger]") {
    // The case the old mock could not express. Without a durable store separate
    // from the runtime one, this reverting and NOT reverting look identical.
    gcode("SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.075");
    REQUIRE(client.tool_z_offset(1) == Catch::Approx(-0.075));

    gcode("RESTART");

    // Back to the distinct per-tool seed, i.e. whatever printer.cfg still says.
    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.025));
}

TEST_CASE_METHOD(SaveConfigFixture, "mock: SAVE_CONFIG commits the staged offset across the restart",
                 "[mock][toolchanger]") {
    gcode("SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.075");
    gcode("SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset");
    gcode("SAVE_CONFIG");

    // SAVE_CONFIG writes the file and restarts. Ordering is load-bearing:
    // committing after the restart would reload the pre-save value and throw
    // the save away.
    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.075));
    CHECK_FALSE(client.save_config_pending());
    CHECK(client.save_config_pending_items().empty());

    // And it is durable, not merely still-in-RAM.
    gcode("RESTART");
    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.075));
}

TEST_CASE_METHOD(SaveConfigFixture, "mock: staging one tool leaves the others alone",
                 "[mock][toolchanger]") {
    gcode("SET_TOOL_PARAMETER T=0 PARAMETER=gcode_z_offset VALUE=-0.100");
    gcode("SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.200");
    gcode("SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset");
    gcode("SAVE_CONFIG");

    // T1 was staged, T0 was not - a per-tool save that quietly persisted every
    // tool would look correct in any single-tool test.
    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.200));
    CHECK(client.tool_z_offset(0) == Catch::Approx(0.0));
}

TEST_CASE_METHOD(SaveConfigFixture, "mock: printer.restart reverts an unsaved offset too",
                 "[mock][toolchanger]") {
    // printer.restart used to fake only the klippy state, so a restart reached
    // through the RPC kept every unsaved runtime value alive - the same
    // unfalsifiable persist path, just through the other door. Both restart
    // entry points now share trigger_restart().
    //
    // This fixture never pumps LVGL, which is the second half of the fix: the
    // old handler scheduled its return to READY on an lv_timer that would never
    // fire here.
    gcode("SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.075");
    REQUIRE(client.tool_z_offset(1) == Catch::Approx(-0.075));

    bool acked = false;
    client.send_jsonrpc(
        "printer.restart", json::object(), [&acked](const json&) { acked = true; },
        [](const MoonrakerError&) {});

    // Moonraker's do_restart() catches "Klippy Disconnected" and returns "ok",
    // so unlike SAVE_CONFIG this one really does ack (klippy_apis.py).
    CHECK(acked);
    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.025));
}

TEST_CASE_METHOD(SaveConfigFixture, "mock: a committed offset survives printer.restart",
                 "[mock][toolchanger]") {
    gcode("SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.075");
    gcode("SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset");
    gcode("SAVE_CONFIG");

    client.send_jsonrpc(
        "printer.restart", json::object(), [](const json&) {}, [](const MoonrakerError&) {});

    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.075));
}

TEST_CASE_METHOD(SaveConfigFixture, "mock: a restart clears the print, as Klipper's does",
                 "[mock][toolchanger]") {
    // The other half of what the hand-rolled handler left out. A mock that
    // reports a print still running on a machine that just rebooted will let a
    // restart-during-print test pass against behaviour no printer has.
    client.send_jsonrpc(
        "printer.print.start", json{{"filename", "test.gcode"}}, [](const json&) {},
        [](const MoonrakerError&) {});
    REQUIRE(client.get_print_phase() != MoonrakerClientMock::MockPrintPhase::IDLE);

    client.send_jsonrpc(
        "printer.restart", json::object(), [](const json&) {}, [](const MoonrakerError&) {});

    CHECK(client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::IDLE);
}

TEST_CASE_METHOD(SaveConfigFixture, "mock: SAVE_CONFIG's own rpc still fails, as Klipper's does",
                 "[mock][toolchanger]") {
    // cmd_SAVE_CONFIG ends in request_restart(), so it never acks: Moonraker
    // fails the pending script with a disconnect. Committing the pending items
    // must not have turned that into a success - callers key their whole
    // save-and-wait flow off the failure.
    gcode("SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.075");
    gcode("SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset");

    CHECK_FALSE(gcode("SAVE_CONFIG"));
    CHECK(client.tool_z_offset(1) == Catch::Approx(-0.075));
}
