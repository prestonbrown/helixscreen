// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_restart.cpp
 * @brief The restart RPCs go through the mock's one restart model.
 *
 * printer.restart and printer.firmware_restart each used to hand-roll their own
 * restart that faked the klippy state and nothing else, so a restart reached
 * through the RPC left the mock reporting a print still running on a machine
 * that had just rebooted - and any test of restart-during-print behaviour was
 * asserting against something no printer does.
 *
 * Two subtler faults came with it, and these tests pin both:
 *
 *   - the handlers wrote to get_printer_state(), the PROCESS-GLOBAL state,
 *     rather than dispatching the webhooks update a real restart arrives as, so
 *     the client's own klippy_state_ never moved; and
 *   - the return to READY was scheduled on an lv_timer, which never fires in a
 *     fixture that does not pump LVGL - as this one does not - leaving klippy
 *     stuck SHUTDOWN forever.
 *
 * Asserting on the CLIENT's klippy state (not the global one) is therefore the
 * discriminator: the old path could not move it at all.
 */

#include "../helix_test_fixture.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"

#include "../catch_amalgamated.hpp"

using nlohmann::json;

namespace {

struct RestartFixture : public HelixTestFixture {
    // Real-time speedup: the assertions below run while the restart is still in
    // its STARTUP window (2s), and the destructor cancels the pending thread
    // rather than waiting it out, so this costs no test time.
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};

    /// Returns whether the rpc acked.
    bool rpc(const std::string& method) {
        bool acked = false;
        client.send_jsonrpc(
            method, json::object(), [&acked](const json&) { acked = true; },
            [](const MoonrakerError&) {});
        return acked;
    }
};

} // namespace

TEST_CASE_METHOD(RestartFixture, "mock: printer.restart moves this client's klippy state",
                 "[mock][restart]") {
    REQUIRE(client.get_klippy_state() == MoonrakerClientMock::KlippyState::READY);

    // Moonraker's do_restart() catches "Klippy Disconnected" and returns "ok",
    // so unlike SAVE_CONFIG this really does ack (klippy_apis.py).
    CHECK(rpc("printer.restart"));

    CHECK(client.get_klippy_state() == MoonrakerClientMock::KlippyState::STARTUP);
}

TEST_CASE_METHOD(RestartFixture, "mock: printer.firmware_restart does the same",
                 "[mock][restart]") {
    CHECK(rpc("printer.firmware_restart"));
    CHECK(client.get_klippy_state() == MoonrakerClientMock::KlippyState::STARTUP);
}

TEST_CASE_METHOD(RestartFixture, "mock: a restart clears the print, as Klipper's does",
                 "[mock][restart]") {
    // A mock that reports a print still running on a machine that just rebooted
    // will let a restart-during-print test pass against behaviour no printer has.
    client.send_jsonrpc(
        "printer.print.start", json{{"filename", "test.gcode"}}, [](const json&) {},
        [](const MoonrakerError&) {});
    REQUIRE(client.get_print_phase() != MoonrakerClientMock::MockPrintPhase::IDLE);

    rpc("printer.restart");

    CHECK(client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::IDLE);
}
