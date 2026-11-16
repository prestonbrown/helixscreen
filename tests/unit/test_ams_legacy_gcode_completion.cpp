// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_legacy_gcode_completion.cpp
 * @brief The callback-taking execute_gcode() forms run their callbacks on the
 *        main thread, behind earlier status frames (#1761).
 *
 * The real MoonrakerAPI answers on the libhv response thread, while the
 * backend applies status frames on the main thread through the UpdateQueue. A
 * completion run inline can judge state those queued frames are about to
 * replace. The mock client answers inside the send, so a callback that has
 * already run when execute_gcode() returns ran inline.
 */

#include "../test_helpers/mock_printer.h"
#include "ams_backend_toolchanger.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "test_helpers/moonraker_client_mock_test_access.h"
#include "test_helpers/update_queue_test_access.h"

#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// A real ToolChanger backend talking to the mock client through the real
/// MoonrakerAPI. Nothing overrides execute_gcode(), so every send takes the
/// production path.
class LiveToolChanger : public helix::AmsBackendToolChanger {
  public:
    explicit LiveToolChanger(MockPrinter& p) : helix::AmsBackendToolChanger(&p.api, &p.client) {
        set_discovered_tools({"T0", "T1", "T2", "T3", "T4"});
        running_ = true;
    }

    ~LiveToolChanger() override {
        if (drain_on_destroy) {
            drain();
        }
    }

    /// Off to destroy the backend with its deferred callbacks still queued.
    bool drain_on_destroy = true;

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Put tool @p n on the carriage with the toolchanger idle.
    void seat_tool(int n) {
        handle_status_update(
            json{{"method", "notify_status_update"},
                 {"params",
                  json::array(
                      {json{{"toolchanger", {{"status", "ready"}, {"tool_number", n}}}}, 0.0})}});
        drain();
    }

    [[nodiscard]] bool toolhead_homed() const override {
        return true;
    }
};

/// Records whether a callback ran and which UpdateQueue callback it ran from.
struct Witness {
    bool ran = false;
    std::string ran_from;

    helix::SharedCallback<void()> on_complete() {
        return [this]() { record(); };
    }
    helix::SharedCallback<void(const MoonrakerError&)> on_error() {
        return [this](const MoonrakerError&) { record(); };
    }

  private:
    void record() {
        ran = true;
        const char* tag = helix::ui::UpdateQueue::current_callback_tag();
        ran_from = tag ? tag : "";
    }
};

void refuse_every_script(MockPrinter& p) {
    helix::MoonrakerClientMockTestAccess::set_method_handler(
        p.client, "printer.gcode.script",
        [](MoonrakerClientMock*, const json&, helix::SharedCallback<void(const json&)>,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
            if (error_cb) {
                error_cb(MoonrakerError::unknown("Unknown command", "printer.gcode.script"));
            }
            return true;
        });
}

} // namespace

TEST_CASE("Legacy execute_gcode completion runs from the UpdateQueue, not the answering thread",
          "[ams][toolchanger][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    Witness w;

    REQUIRE(backend.execute_gcode("M400", w.on_complete()).success());
    // The mock answered inside that call; completion waits for the queue.
    CHECK_FALSE(w.ran);
    LiveToolChanger::drain();
    REQUIRE(w.ran);
    CHECK(w.ran_from == "AmsSubscriptionBackend::execute_gcode_complete");
}

TEST_CASE("Legacy execute_gcode with an error leg completes from the UpdateQueue",
          "[ams][toolchanger][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    Witness complete;
    Witness error;

    REQUIRE(backend.execute_gcode("M400", complete.on_complete(), error.on_error()).success());
    CHECK_FALSE(complete.ran);
    LiveToolChanger::drain();
    REQUIRE(complete.ran);
    CHECK(complete.ran_from == "AmsSubscriptionBackend::execute_gcode_complete");
    CHECK_FALSE(error.ran);
}

TEST_CASE("Legacy execute_gcode error runs from the UpdateQueue, not the answering thread",
          "[ams][toolchanger][threading][1761]") {
    MockPrinter printer;
    refuse_every_script(printer);
    LiveToolChanger backend(printer);
    Witness complete;
    Witness error;

    REQUIRE(backend.execute_gcode("M400", complete.on_complete(), error.on_error()).success());
    CHECK_FALSE(error.ran);
    LiveToolChanger::drain();
    REQUIRE(error.ran);
    CHECK(error.ran_from == "AmsSubscriptionBackend::execute_gcode_error");
    CHECK_FALSE(complete.ran);
}

TEST_CASE(
    "Legacy execute_gcode callbacks are dropped when the backend dies before the queue drains",
    "[ams][toolchanger][threading][1761]") {
    MockPrinter acking;
    MockPrinter refusing;
    refuse_every_script(refusing);
    Witness complete;
    Witness error;
    Witness refused_complete;
    Witness refused_error;

    {
        LiveToolChanger acked(acking);
        LiveToolChanger refused(refusing);
        acked.drain_on_destroy = false;
        refused.drain_on_destroy = false;
        REQUIRE(acked.execute_gcode("M400", complete.on_complete(), error.on_error()).success());
        REQUIRE(
            refused.execute_gcode("M400", refused_complete.on_complete(), refused_error.on_error())
                .success());
    }
    // Both answers are queued; their owner is gone.
    LiveToolChanger::drain();
    CHECK_FALSE(complete.ran);
    CHECK_FALSE(error.ran);
    CHECK_FALSE(refused_complete.ran);
    CHECK_FALSE(refused_error.ran);
}

TEST_CASE("Toolchanger mount of the mounted tool finalizes on main through the real API",
          "[ams][toolchanger][dispatch][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    backend.seat_tool(4);
    REQUIRE(backend.get_current_action() == helix::AmsAction::IDLE);

    std::vector<std::pair<helix::AmsAction, std::string>> trace;
    backend.set_event_callback([&](const std::string& event, const std::string&) {
        if (event == helix::AmsBackend::EVENT_STATE_CHANGED) {
            const char* tag = helix::ui::UpdateQueue::current_callback_tag();
            trace.emplace_back(backend.get_current_action(), tag ? tag : "");
        }
    });

    REQUIRE(backend.change_tool(4).success());
    // The mock already acked SELECT_TOOL; the finalize is still queued.
    CHECK(backend.get_current_action() == helix::AmsAction::SELECTING);

    LiveToolChanger::drain();
    CHECK(backend.get_current_action() == helix::AmsAction::IDLE);
    REQUIRE_FALSE(trace.empty());
    CHECK(trace.front().first == helix::AmsAction::SELECTING);
    CHECK(trace.back().first == helix::AmsAction::IDLE);
    CHECK(trace.back().second == "AmsSubscriptionBackend::execute_gcode_complete");
}

using helix::ui::UpdateQueueTestAccess;

TEST_CASE("Legacy execute_gcode completion runs in the queue pass after the answer",
          "[ams][toolchanger][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    Witness w;

    REQUIRE(backend.execute_gcode("M400", w.on_complete()).success());
    CHECK(UpdateQueueTestAccess::passes_until([&] { return w.ran; }) == 1);
}

TEST_CASE("ensure_homed_then with a state-only error leg completes in the answer's queue pass",
          "[ams][toolchanger][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    Witness complete;
    Witness error;

    // An on_error that only unwinds state, at the standard timeout: the shape
    // dispatch_payload sends through the 4-arg execute_gcode virtual.
    REQUIRE(backend
                .ensure_homed_then("M400", complete.on_complete(), error.on_error(),
                                   helix::IMoonrakerAPI::kAmsOperationTimeoutMs,
                                   /*skip_homing=*/false,
                                   /*silent=*/true, /*caller_surfaces_errors=*/false)
                .success());
    CHECK(UpdateQueueTestAccess::passes_until([&] { return complete.ran; }) == 1);
    CHECK(complete.ran_from == "AmsSubscriptionBackend::execute_gcode_complete");
    CHECK_FALSE(error.ran);
}

TEST_CASE("Toolchanger mount-to-feed sends the next stage in the queue pass after an ack",
          "[ams][toolchanger][mount_feed][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    backend.set_mounts_to_feed(true);
    backend.seat_tool(0);
    printer.client.clear_gcode_script_history();

    // An Unload: its first stage (the mount) moves on its ack alone, where a
    // Load's first stage waits on the operator's reply.
    REQUIRE(backend.unload_filament(2).success());
    const size_t first_stage = printer.client.gcode_script_history().size();
    REQUIRE(first_stage >= 1);
    CHECK(UpdateQueueTestAccess::passes_until(
              [&] { return printer.client.gcode_script_history().size() > first_stage; }) == 1);
}
