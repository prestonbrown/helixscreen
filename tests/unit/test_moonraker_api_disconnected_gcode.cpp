// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_disconnected_gcode.cpp
 * @brief The klippy-halted gate must not speak for a printer we never reached.
 *
 * Context (debug bundle XRK8KPTF, K2 Plus, v0.99.98): the WebSocket to
 * Moonraker never opened once for the whole session. klippy_state therefore
 * still held its startup default of SHUTDOWN (printer_network_state.cpp,
 * "default to SHUTDOWN until confirmed ready") — there is no UNKNOWN value to
 * distinguish "never heard from Klipper" from "Klipper e-stopped".
 *
 * MoonrakerAPI::execute_gcode's gate read only that subject, so every G-code
 * was refused with "Klipper is halted — restart firmware to continue". The PID
 * calibration screen printed that verbatim, sending the reporter to restart
 * firmware on a printer whose firmware was fine and simply unreachable.
 *
 * The gate may only claim Klipper is halted when we are actually connected to
 * it. Disconnected, the request falls through to client_.send_jsonrpc, whose
 * own ready_to_send() gate reports the truthful CONNECTION_LOST (#909).
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client.h"
#include "../../include/moonraker_error.h"
#include "../../include/printer_state.h"
#include "../lvgl_test_fixture.h"
#include "hv/EventLoopThread.h"

#include <atomic>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Real MoonrakerClient that owns an event loop but never connects, so
/// connection_state_ stays DISCONNECTED — the bundle's exact situation.
/// Mirrors UnconnectedClient in test_moonraker_client_send_gate.cpp.
struct UnconnectedApi {
    UnconnectedApi(PrinterState& state) : loop_(std::make_shared<hv::EventLoopThread>()) {
        loop_->start();
        client_ = std::make_unique<MoonrakerClient>(loop_->loop());
        api_ = std::make_unique<MoonrakerAPI>(*client_, state);
    }
    ~UnconnectedApi() {
        // JOIN the loop thread BEFORE destroying the client (#1146).
        loop_->stop();
        loop_->join();
        api_.reset();
        client_.reset();
    }

    std::shared_ptr<hv::EventLoopThread> loop_;
    std::unique_ptr<MoonrakerClient> client_;
    std::unique_ptr<MoonrakerAPI> api_;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "execute_gcode on a never-connected client reports connection loss, not a halt",
                 "[moonraker][api][regression][eventloop][slow]") {
    PrinterState state;
    state.init_subjects(false);

    // Deliberately NOT setting klippy_state: it sits at its startup default of
    // SHUTDOWN, which is precisely what made the old gate misfire. Asserting the
    // default here keeps the test honest if that default ever changes.
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::SHUTDOWN));

    UnconnectedApi u(state);
    REQUIRE(u.client_->get_connection_state() == ConnectionState::DISCONNECTED);

    auto error_fired = std::make_shared<std::atomic<bool>>(false);
    auto error_type = std::make_shared<MoonrakerErrorType>(MoonrakerErrorType::UNKNOWN);
    auto error_message = std::make_shared<std::string>();

    u.api_->execute_gcode("M117 hello", nullptr,
                          [error_fired, error_type, error_message](const MoonrakerError& err) {
                              error_fired->store(true);
                              *error_type = err.type;
                              *error_message = err.message;
                          });

    REQUIRE(error_fired->load());

    // The whole point: transport failure, not a firmware halt. Before the fix
    // this was NOT_READY / "Klipper is halted — restart firmware to continue".
    CHECK(*error_type == MoonrakerErrorType::CONNECTION_LOST);
    CHECK(error_message->find("halted") == std::string::npos);
    CHECK(error_message->find("restart firmware") == std::string::npos);
}
