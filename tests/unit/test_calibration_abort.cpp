// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "calibration_abort.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <lvgl.h>
#include <thread>

#include "../catch_amalgamated.hpp"

// The mock routes the two halves of the sequence to different objects:
// printer.emergency_stop queues helix::ui::queue_update(SHUTDOWN) onto the
// process-global PrinterState (fires when the queue is drained), while
// printer.firmware_restart sets THIS client's klippy_state to STARTUP
// synchronously and returns it to READY on a background thread after 3 s
// divided by the speedup. So M112 is proven by the global state's subject,
// the restart by the client leaving and re-entering READY.
//
// Order is proven by the final print phase: emergency_stop_internal() sets
// the mock's phase to ERROR and trigger_restart() clears it to IDLE, so a
// restart that ran BEFORE the stop leaves the client in ERROR.
TEST_CASE_METHOD(LVGLTestFixture, "emergency_stop_and_restart sends M112 then a firmware restart",
                 "[calibration][abort]") {
    helix::PrinterState state;
    state.init_subjects(false);
    // Speedup 1000: the mock's 3 s firmware restart takes ~10 ms of real time.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24, 1000.0);
    MoonrakerAPIMock api(client, state);
    REQUIRE(client.get_klippy_state() == MoonrakerClientMock::KlippyState::READY);
    REQUIRE(client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::IDLE);
    // The mock's M112 lands on the global state; its subjects must exist, and
    // READY is 0, so an uninitialised subject would pass the check below.
    get_printer_state().init_subjects(false);
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    REQUIRE(static_cast<helix::KlippyState>(lv_subject_get_int(
                get_printer_state().network_state().get_klippy_state_subject())) ==
            helix::KlippyState::READY);

    helix::emergency_stop_and_restart(&api, "Test");

    // restart_firmware ran inside M112's success callback, synchronously, and
    // the return to READY needs a real ~10 ms — so the client is in STARTUP
    // the moment the helper returns.
    const bool saw_startup = client.get_klippy_state() == MoonrakerClientMock::KlippyState::STARTUP;
    REQUIRE(saw_startup);

    bool saw_shutdown = false;
    bool back_to_ready = false;
    for (int i = 0; i < 400 && !back_to_ready; ++i) {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        lv_tick_inc(5);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        saw_shutdown =
            saw_shutdown || static_cast<helix::KlippyState>(lv_subject_get_int(
                                get_printer_state().network_state().get_klippy_state_subject())) ==
                                helix::KlippyState::SHUTDOWN;
        back_to_ready =
            saw_shutdown && client.get_klippy_state() == MoonrakerClientMock::KlippyState::READY;
    }
    CHECK(saw_shutdown);
    CHECK(back_to_ready);
    CHECK(client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::IDLE);
}

TEST_CASE("emergency_stop_and_restart with no API does nothing", "[calibration][abort]") {
    helix::emergency_stop_and_restart(nullptr, "Test"); // must not crash
    SUCCEED();
}
