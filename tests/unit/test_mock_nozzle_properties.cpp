// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_nozzle_properties.cpp
 * @brief A head's fitted nozzle, changed through the mock's U1 firmware
 *
 * The firmware answers printer.control.nozzle_properties inside a successful
 * reply, refusals included, so a change counts only when that reply says
 * success. These run the backend's real send path against the mock's copy of
 * the firmware's checks.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_snapmaker.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "snapmaker_nozzle_properties.h"

#include <any>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::MoonrakerClientMockTestAccess;
using nlohmann::json;
namespace nozzle = helix::snapmaker::nozzle;

namespace {

/// The mock, plus every nozzle_properties call sent through it.
struct RecordingClient : public MoonrakerClientMock {
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;
    std::vector<json> sent;

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == nozzle::kMethod) {
            sent.push_back(params);
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }
};

/// Sends @p params straight to the mock and returns its reply's result.
json send_to_mock(MoonrakerClientMock& client, const json& params) {
    json result;
    bool answered = false;
    client.send_jsonrpc(
        nozzle::kMethod, params,
        [&](const json& r) {
            answered = true;
            result = r.value("result", json());
        },
        [&](const MoonrakerError&) { answered = true; });
    REQUIRE(answered);
    return result;
}

struct NozzleFixture : public LVGLTestFixture {
    RecordingClient client{MoonrakerClientMock::PrinterType::SNAPMAKER_U1, 100.0};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<helix::AmsBackendSnapmaker> backend;
    int state_changes = 0;
    helix::LogCapture logs;

    NozzleFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        backend = std::make_unique<helix::AmsBackendSnapmaker>(api.get(), &client);
        // The heads report their nozzles, as the objects query does.
        json frame = json::object();
        for (int h = 0; h < 4; ++h) {
            const auto [diameter, type] = client.nozzle_properties(h);
            frame[h == 0 ? std::string("extruder") : "extruder" + std::to_string(h)] = {
                {"nozzle_diameter", diameter}, {"nozzle_volume_type", type}};
        }
        helix::SnapmakerTestAccess::handle_status(*backend, frame);
        backend->set_event_callback([this](const std::string& ev, const std::string&) {
            if (ev == helix::AmsBackend::EVENT_STATE_CHANGED) {
                ++state_changes;
            }
        });
    }
    ~NozzleFixture() override {
        drain();
        backend.reset();
    }

    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    helix::AmsError pick(const std::string& id, const char* option) {
        auto err = backend->execute_device_action(id, std::any(std::string(option)));
        drain();
        return err;
    }

    [[nodiscard]] int user_errors() const {
        return logs.count_containing("[USER] Nozzle not changed");
    }
};

} // namespace

TEST_CASE("mock: nozzle_properties keeps the firmware's checks", "[mock][snapmaker][nozzle]") {
    RecordingClient client{MoonrakerClientMock::PrinterType::SNAPMAKER_U1, 100.0};
    REQUIRE(client.nozzle_properties(1) == std::make_pair(0.4, std::string("standard")));

    SECTION("a change is stored, reported and answered with success") {
        json pushed;
        client.register_notify_update([&pushed](const json& n) {
            if (n.contains("params") && n["params"].is_array() && !n["params"].empty()) {
                pushed = n["params"][0];
            }
        });
        CHECK(send_to_mock(client,
                           {{"extruder", 1}, {"diameter", 0.6}, {"volume_type", "high_flow"}}) ==
              json{{"state", "success"}});
        CHECK(client.nozzle_properties(1) == std::make_pair(0.6, std::string("high_flow")));
        CHECK(pushed ==
              json{{"extruder1", {{"nozzle_diameter", 0.6}, {"nozzle_volume_type", "high_flow"}}}});
        // The other heads are untouched.
        CHECK(client.nozzle_properties(0) == std::make_pair(0.4, std::string("standard")));
    }
    SECTION("refusals arrive inside a successful reply, with the firmware's words") {
        CHECK(send_to_mock(client, {{"diameter", 0.6}}) ==
              json{{"state", "error"}, {"message", "extruder must be specified!"}});
        CHECK(send_to_mock(client, {{"extruder", 4}, {"diameter", 0.6}}) ==
              json{{"state", "error"}, {"message", "extruder not found!"}});
        CHECK(send_to_mock(client, {{"extruder", 0}, {"diameter", 0.5}})["message"] ==
              "nozzle_diameter error: 0.5");
        CHECK(client.nozzle_properties(0).first == 0.4);
    }
    SECTION("a bad flow type is refused after a good diameter is already set") {
        CHECK(send_to_mock(
                  client, {{"extruder", 2}, {"diameter", 0.8}, {"volume_type", "fast"}})["state"] ==
              "error");
        CHECK(client.nozzle_properties(2) == std::make_pair(0.8, std::string("standard")));
    }
    SECTION("nothing changes while printing or paused") {
        for (int print_state : {1, 2}) {
            MoonrakerClientMockTestAccess::set_print_state(client, print_state);
            CHECK(send_to_mock(client, {{"extruder", 0}, {"diameter", 0.6}}) ==
                  json{{"state", "error"},
                       {"message", "Cannot change nozzle properties during printing!"}});
        }
        CHECK(client.nozzle_properties(0).first == 0.4);
        MoonrakerClientMockTestAccess::set_print_state(client, 3);
        CHECK(send_to_mock(client, {{"extruder", 0}, {"diameter", 0.6}})["state"] == "success");
    }
    SECTION("the objects query reports the stored nozzle") {
        send_to_mock(client, {{"extruder", 3}, {"volume_type", "high_flow"}});
        json status;
        client.send_jsonrpc(
            "printer.objects.query", json{{"objects", {{"extruder3", nullptr}}}},
            [&status](const json& r) { status = r["result"]["status"]; },
            [](const MoonrakerError&) {});
        REQUIRE(status.contains("extruder3"));
        CHECK(status["extruder3"]["nozzle_diameter"] == 0.4);
        CHECK(status["extruder3"]["nozzle_volume_type"] == "high_flow");
    }
    SECTION("the subscription snapshot reports the stored nozzle") {
        send_to_mock(client, {{"extruder", 2}, {"diameter", 0.6}});
        json status;
        client.send_jsonrpc(
            "printer.objects.subscribe",
            json{{"objects", {{"extruder", nullptr}, {"extruder2", nullptr}}}},
            [&status](const json& r) { status = r["result"]["status"]; },
            [](const MoonrakerError&) {});
        REQUIRE(status.contains("extruder2"));
        CHECK(status["extruder2"]["nozzle_diameter"] == 0.6);
        CHECK(status["extruder2"]["nozzle_volume_type"] == "standard");
        CHECK(status["extruder"]["nozzle_diameter"] == 0.4);
    }
}

TEST_CASE("mock: other printers have no nozzle_properties endpoint", "[mock][nozzle]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24, 100.0};
    bool failed = false;
    client.send_jsonrpc(
        nozzle::kMethod, json{{"extruder", 0}, {"diameter", 0.6}}, [](const json&) {},
        [&failed](const MoonrakerError&) { failed = true; });
    CHECK(failed);
}

TEST_CASE_METHOD(NozzleFixture, "a nozzle pick sends only the changed field and the head follows",
                 "[mock][snapmaker][nozzle]") {
    REQUIRE(pick("snapmaker_nozzle_flow_t2", "High Flow").success());
    REQUIRE(client.sent.size() == 1);
    CHECK(client.sent[0] == json{{"extruder", 2}, {"volume_type", "high_flow"}});
    CHECK(client.nozzle_properties(2) == std::make_pair(0.4, std::string("high_flow")));
    CHECK(user_errors() == 0);
    // The backend reports a failure itself, so the client's own error toast is off.
    CHECK(client.last_send_silent());
}

TEST_CASE_METHOD(NozzleFixture, "a pick of the value a head reports sends nothing",
                 "[mock][snapmaker][nozzle]") {
    CHECK(pick("snapmaker_nozzle_diameter_t0", "0.4 mm").success());
    CHECK(pick("snapmaker_nozzle_flow_t0", "Standard").success());
    CHECK(client.sent.empty());
    CHECK(user_errors() == 0);
}

TEST_CASE_METHOD(NozzleFixture,
                 "a refusal inside a successful reply tells the user and snaps the row back",
                 "[mock][snapmaker][nozzle]") {
    // The backend has not seen the print start; the firmware has.
    MoonrakerClientMockTestAccess::set_print_state(client, 1);
    const int before = state_changes;
    REQUIRE(pick("snapmaker_nozzle_diameter_t1", "0.6 mm").success());
    REQUIRE(client.sent.size() == 1);
    CHECK(client.nozzle_properties(1).first == 0.4);
    CHECK(user_errors() == 1);
    CHECK(logs.count_containing("Cannot change nozzle properties during printing!") >= 1);
    CHECK(state_changes > before);
    // The row still reads what the head reports.
    const auto actions = backend->get_device_actions();
    auto it = std::find_if(actions.begin(), actions.end(),
                           [](const auto& a) { return a.id == "snapmaker_nozzle_diameter_t1"; });
    REQUIRE(it != actions.end());
    CHECK(std::any_cast<std::string>(it->current_value) == "0.4 mm");
}

TEST_CASE_METHOD(NozzleFixture, "a reply that says neither success nor error is not success",
                 "[mock][snapmaker][nozzle]") {
    MoonrakerClientMockTestAccess::set_method_handler(
        client, nozzle::kMethod,
        [](MoonrakerClientMock*, const json&, helix::SharedCallback<void(const json&)> ok,
           helix::SharedCallback<void(const MoonrakerError&)>) -> bool {
            if (ok) {
                ok(json{{"result", "ok"}});
            }
            return true;
        });
    const int before = state_changes;
    REQUIRE(pick("snapmaker_nozzle_diameter_t3", "0.8 mm").success());
    CHECK(user_errors() == 1);
    CHECK(logs.count_containing("the printer did not confirm it") >= 1);
    CHECK(state_changes > before);
}

TEST_CASE_METHOD(NozzleFixture, "a failed send tells the user and snaps the row back",
                 "[mock][snapmaker][nozzle]") {
    client.fail_next(nozzle::kMethod, MoonrakerError::unknown("link dropped"));
    const int before = state_changes;
    REQUIRE(pick("snapmaker_nozzle_diameter_t0", "0.6 mm").success());
    CHECK(client.nozzle_properties(0).first == 0.4);
    CHECK(user_errors() == 1);
    CHECK(logs.count_containing("link dropped") >= 1);
    CHECK(state_changes > before);
}
