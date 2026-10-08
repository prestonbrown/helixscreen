// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_session_wiring.cpp
 * @brief wire_discovery(): the discovery callbacks desktop and the ESP32 firmware share.
 *
 * The client's two callbacks are fired by hand and the UI queue drained, as the WebSocket
 * thread and the main loop would. A pass queued for a previous printer (the HTTP epoch
 * moved) or a closed session must not reach the API, the subjects or the steps.
 */

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "discovery_steps.h"
#include "filament_sensor_manager.h"
#include "http_request_epoch.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "moonraker_api.h"
#include "print_history_manager.h"
#include "printer_state.h"
#include "session_wiring.h"
#include "standard_macros.h"
#include "tool_state.h"
#include "width_sensor_manager.h"

#include "../catch_amalgamated.hpp"

using helix::PrinterDiscovery;

namespace {

struct CountingClient : helix::test::FakeMoonrakerClient {
    int dispatches = 0;
    void dispatch_status_update(const nlohmann::json&, bool) override {
        ++dispatches;
    }
};

PrinterDiscovery discovery_of(std::initializer_list<const char*> objects) {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json(std::vector<std::string>(objects.begin(), objects.end())));
    return hw;
}

struct SessionWiringFixture : public LVGLTestFixture {
    CountingClient client;
    MoonrakerAPI api{client, get_printer_state()};
    helix::HardwareChangeTracker changes;
    bool alive = true;
    int cycles = 0;
    std::vector<bool> passes; ///< hw_changed of each pass that reached after_core
    int dispatches_at_after_core = -1;
    int history_lists_at_after_core = -1;

    SessionWiringFixture() {
        auto& ts = helix::ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(false);
        helix::FilamentSensorManager::instance().init_subjects();
        helix::sensors::WidthSensorManager::instance().init_subjects();
        helix::wire_discovery(api, client,
                              {changes, [this] { return alive; }, [this] { ++cycles; },
                               [this](helix::DiscoveryContext& ctx) {
                                   passes.push_back(ctx.hw_changed);
                                   dispatches_at_after_core = client.dispatches;
                                   history_lists_at_after_core = history_lists();
                               }});
    }

    ~SessionWiringFixture() override {
        drain();
        helix::init_subsystems_from_hardware(PrinterDiscovery{}, nullptr, nullptr);
        drain();
        helix::led::LedAutoState::instance().deinit();
        helix::led::LedController::instance().deinit();
        StandardMacros::instance().reset();
    }

    int history_lists() const {
        int n = 0;
        for (const auto& call : client.rpc_calls) {
            n += call.method == "server.history.list" ? 1 : 0;
        }
        return n;
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// One discovery as the WebSocket thread delivers it.
    void fire(const PrinterDiscovery& hw) {
        REQUIRE(client.on_hardware_discovered);
        REQUIRE(client.on_discovery_complete);
        client.on_hardware_discovered(hw);
        client.on_discovery_complete(hw, nlohmann::json{{"webhooks", {{"state", "ready"}}}});
    }
};

} // namespace

TEST_CASE_METHOD(SessionWiringFixture,
                 "a discovery fills the API's hardware and dispatches the status once",
                 "[session_wiring]") {
    fire(discovery_of({"extruder", "heater_bed", "gcode_macro PAUSE"}));
    drain();

    CHECK(api.hardware().has_heater_bed());
    CHECK(api.hardware().has_macro("PAUSE"));
    CHECK(StandardMacros::instance().is_initialized());
    CHECK(client.dispatches == 1);
    CHECK(cycles == 1);
    CHECK(passes == std::vector<bool>{true});
}

TEST_CASE_METHOD(SessionWiringFixture,
                 "after_core runs after the core steps and before the print history is released",
                 "[session_wiring]") {
    PrintHistoryManager history(&api, &client);
    history.hold_until_discovery();
    history.ensure_loaded(helix::HistoryScope::RECENT);
    REQUIRE(history_lists() == 0);
    set_print_history_manager(&history);

    fire(discovery_of({"extruder", "heater_bed"}));
    drain();
    set_print_history_manager(nullptr);

    CHECK(dispatches_at_after_core == 1);
    CHECK(history_lists_at_after_core == 0);
    CHECK(history_lists() >= 1);
}

TEST_CASE_METHOD(SessionWiringFixture, "a reconnect with the same hardware is not a change",
                 "[session_wiring]") {
    const auto hw = discovery_of({"extruder", "heater_bed"});
    fire(hw);
    drain();
    fire(hw);
    drain();
    fire(discovery_of({"extruder", "heater_bed", "fan"}));
    drain();

    CHECK(passes == std::vector<bool>{true, false, true});
}

TEST_CASE_METHOD(SessionWiringFixture, "a pass queued for the previous printer is dropped",
                 "[session_wiring]") {
    fire(discovery_of({"extruder", "heater_bed"}));
    // The switch to the next printer moves the epoch before the queue drains.
    helix::http_epoch::advance();
    drain();

    CHECK_FALSE(api.hardware().has_heater_bed());
    CHECK(client.dispatches == 0);
    CHECK(cycles == 0);
    CHECK(passes.empty());
}

TEST_CASE_METHOD(SessionWiringFixture, "a pass that arrives after the session closed is dropped",
                 "[session_wiring]") {
    fire(discovery_of({"extruder", "heater_bed"}));
    alive = false;
    drain();

    CHECK_FALSE(api.hardware().has_heater_bed());
    CHECK(client.dispatches == 0);
    CHECK(cycles == 0);
    CHECK(passes.empty());
}
