// SPDX-License-Identifier: GPL-3.0-or-later

// StandardMacros::execute callbacks run on the main thread. A caller like
// PrintControlButtons redraws its buttons from the error callback, and the
// printer's answer arrives on the WebSocket thread.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "standard_macros.h"

#include <atomic>
#include <thread>

#include "../catch_amalgamated.hpp"

namespace {

struct MacroThreadFixture : public LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api{client, state};

    MacroThreadFixture() {
        state.init_subjects(false);
        StandardMacros::instance().reset();
        StandardMacros::instance().set_macro(StandardMacroSlot::Pause, "PAUSE");
    }

    ~MacroThreadFixture() override {
        StandardMacros::instance().reset();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Runs execute() on a worker thread, the way the WebSocket thread reaches
    /// a callback, and joins it before returning.
    void execute_from_worker(std::atomic<int>& calls, std::atomic<bool>& on_main) {
        auto record = [&calls, &on_main]() {
            on_main = helix::ui::is_main_thread();
            ++calls;
        };
        std::thread worker([&]() {
            StandardMacros::instance().execute(
                StandardMacroSlot::Pause, &api, [record]() { record(); },
                [record](const MoonrakerError&) { record(); }, 0, true);
        });
        worker.join();
    }
};

} // namespace

TEST_CASE_METHOD(MacroThreadFixture,
                 "StandardMacros::execute answers its callbacks on the main thread",
                 "[standard_macros][threading]") {
    REQUIRE(helix::ui::is_main_thread());
    std::atomic<int> calls{0};
    std::atomic<bool> on_main{false};

    SECTION("the error callback") {
        client.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                      "MCU 'mcu' shutdown: Timer too close", "PAUSE");
        execute_from_worker(calls, on_main);
    }
    SECTION("the success callback") {
        execute_from_worker(calls, on_main);
    }

    // The mock answers on the thread that sent, so nothing has run yet unless
    // the callback skipped the queue.
    CHECK(calls == 0);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(calls == 1);
    CHECK(on_main);
}
