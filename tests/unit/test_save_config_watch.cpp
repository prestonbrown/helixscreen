// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// SaveConfigWatch against the mock printer: which saves restart Klipper, and
// which replies still speak for the save in flight.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "save_config_restart.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

constexpr const char* kQuery = "printer.objects.query";

class WatchFixture : public LVGLTestFixture {
  public:
    WatchFixture() : client(MoonrakerClientMock::PrinterType::VORON_24, 100.0) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        // SaveConfigWatch observes the process-wide PrinterState's klippy subject.
        get_printer_state().init_subjects(false);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(client, state);
    }
    ~WatchFixture() override {
        watch.end();
        client.stop_temperature_simulation();
        client.disconnect();
        api.reset();
        helix::ui::UpdateQueue::instance().drain();
    }

    void kalico() {
        api->hardware().apply_printer_info(
            nlohmann::json{{"app", "Kalico"}, {"software_version", "v2025.1"}});
    }

    std::optional<bool> ask() {
        std::optional<bool> answer;
        watch.query_restarts(api.get(), [&answer](bool restarts) { answer = restarts; });
        wait_until([&] { return answer.has_value(); }, 2000);
        return answer;
    }

    MoonrakerClientMock client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    helix::ui::SaveConfigWatch watch;
};

} // namespace

TEST_CASE_METHOD(WatchFixture, "query_restarts: mainline Klipper always restarts, without asking",
                 "[save_config][save_watch]") {
    const int before = client.call_count(kQuery);
    CHECK(ask() == std::optional<bool>{true});
    CHECK(client.call_count(kQuery) == before);
}

TEST_CASE_METHOD(WatchFixture, "query_restarts: Kalico with nothing pending needs no restart",
                 "[save_config][save_watch]") {
    helix::ScopedEnv env{"HELIX_MOCK_KALICO", "1"};
    kalico();
    CHECK(ask() == std::optional<bool>{false});
}

TEST_CASE_METHOD(WatchFixture, "query_restarts: an unreadable pending list means a restart",
                 "[save_config][save_watch]") {
    helix::ScopedEnv env{"HELIX_MOCK_KALICO", "1"};
    kalico();
    client.fail_next(kQuery, MoonrakerError::unknown("Klippy not ready"));
    CHECK(ask() == std::optional<bool>{true});
}

TEST_CASE_METHOD(WatchFixture,
                 "a pending-items reply for a save that already ended starts no SAVE_CONFIG",
                 "[save_config][save_watch]") {
    helix::ScopedEnv env{"HELIX_MOCK_KALICO", "1"};
    kalico();
    client.clear_gcode_script_history();
    client.defer_next(kQuery);
    client.defer_next(kQuery);

    int first_saved = 0;
    int second_saved = 0;
    watch.begin(api.get(), "", [&] { ++first_saved; }, [](const std::string&) {});
    watch.end();
    watch.begin(api.get(), "", [&] { ++second_saved; }, [](const std::string&) {});

    // The first save's reply arrives while the second is waiting on its own.
    client.fire_deferred(kQuery);
    process_lvgl(50);
    CHECK(client.gcode_script_history().empty());
    CHECK(watch.in_flight());

    client.fire_deferred(kQuery);
    REQUIRE(wait_until([&] { return second_saved == 1; }, 2000));
    CHECK(client.gcode_script_history() == std::vector<std::string>{"SAVE_CONFIG RESTART=0"});
    CHECK(first_saved == 0);
}
