// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_start_controller.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_mock.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// A file whose routing is known writes that routing in the pre-print send,
// not the previous job's crossover.
TEST_CASE("a reprint with an explicit routing sends that map", "[pa_tune][preprint]") {
    LVGLTestFixture fx;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;
    ps.init_subjects(false);
    ps.set_klippy_state_sync(KlippyState::READY);
    client.connect("ws://mock/websocket", []() {}, []() {});
    MoonrakerAPI api{client, ps};
    helix::ui::PrintStartController controller{ps, &api};
    test::RegisteredBackend<AmsBackendMock> backend(4);
    backend->set_snapmaker_mode(true);
    REQUIRE(backend->requires_preprint_send());

    controller.initiate_reprint_with_routing(
        "helix_pa_tuning.gcode", {2}, {{2, 2}}, []() {}, []() {});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.last_send_script().find(
              "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=2") != std::string::npos);
    client.stop_temperature_simulation();
    client.disconnect();
}
