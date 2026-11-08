// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cold_pull_runner.cpp
 * @brief ColdPullRunner against a mock printer: what reaches Moonraker, and when
 *        (prestonbrown/helixscreen#2011).
 *
 * Run with: ./build/bin/helix-tests "[cold_pull]"
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "cold_pull_runner.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "printer_state.h"
#include "temperature_controller.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ColdPullMaterial;
using helix::ColdPullStage;
using helix::ui::ColdPullRefusal;
using helix::ui::ColdPullRunner;

namespace {

/// Holds the extrusions the flow sends, keeping their callbacks for the test to answer.
class HeldExtrusionAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;

    void execute_gcode(const std::string& gcode, SuccessCallback on_success, ErrorCallback on_error,
                       uint32_t timeout_ms = 0, bool silent = false,
                       SuccessCallback on_queued = nullptr, bool caller_surfaces_errors = true,
                       bool bypass_busy_gate = false) override {
        if (gcode.find("G1 E") != std::string::npos) {
            queued.push_back(on_queued);
            held_queued = std::move(on_queued);
            held_surfaces_errors = caller_surfaces_errors;
            ++held;
            return;
        }
        MoonrakerAPI::execute_gcode(gcode, std::move(on_success), std::move(on_error), timeout_ms,
                                    silent, std::move(on_queued), caller_surfaces_errors,
                                    bypass_busy_gate);
    }

    int held = 0;
    std::vector<SuccessCallback> queued; ///< Every held extrusion's, oldest first
    SuccessCallback held_queued;
    bool held_surfaces_errors = true;
};

/// A printer whose objects query fails, or is held unanswered.
class QueryGateClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "printer.objects.query" && (fail_query || hold_query)) {
            ++queries;
            if (fail_query && error_cb) {
                error_cb(MoonrakerError::connection_lost(method));
            }
            return 1;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    bool fail_query = false;
    bool hold_query = false;
    int queries = 0;
};

/// A head whose unload the user does by hand.
class ManualUnloadBackend : public helix::AmsBackendMock {
  public:
    ManualUnloadBackend() : helix::AmsBackendMock(4) {}
    helix::HeadFilamentOps head_filament_ops(int) const override {
        helix::HeadFilamentOps ops;
        ops.unload = helix::FilamentOpSupport::Manual;
        ops.unload_reason = "Pull the filament out by hand.";
        return ops;
    }
};

class RunnerFixture : public LVGLTestFixture {
  public:
    RunnerFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.temperature_state().init_extruders({"extruder"});
        state.set_klippy_state_sync(helix::KlippyState::READY);
        state.set_printer_connection_state(2, "Connected");
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<HeldExtrusionAPI>(client, state);
        previous_api_ = get_moonraker_api();
        previous_client_ = get_moonraker_client();
        set_moonraker_api(api.get());
        set_moonraker_client(&client);
        controller =
            std::make_unique<helix_test::ScopedSharedResource<helix::TemperatureController>>(
                std::make_shared<helix::TemperatureController>(state, api.get()));
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().clear_backends();
        PostOpCooldownManager::instance().init();
        drain();
        runner = std::make_unique<ColdPullRunner>(state);
        client.clear_gcode_script_history();
    }

    ~RunnerFixture() override {
        runner.reset();
        controller.reset();
        PostOpCooldownManager::instance().cancel();
        helix::AmsState::instance().clear_backends();
        set_moonraker_api(previous_api_);
        set_moonraker_client(previous_client_);
        drain();
        client.disconnect();
        api.reset();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void feed(const char* heater, double actual_c, double target_c) {
        state.update_from_status({{heater, {{"temperature", actual_c}, {"target", target_c}}}});
        drain();
    }

    void connection(int value) {
        state.set_printer_connection_state(value, "");
        drain();
    }

    /// Start a PLA pull on the active head and take it to Heating at 230 °C.
    void begin_heating(const char* heater = "extruder") {
        REQUIRE(runner->start(ColdPullMaterial::Pla, {230, 90}, false) == ColdPullRefusal::Ok);
        runner->toolhead_clear();
        drain();
        feed(heater, 25, 230);
        REQUIRE(runner->flow()->stage() == ColdPullStage::Heating);
    }

    [[nodiscard]] bool sent(const std::string& needle) const {
        for (const auto& script : client.gcode_script_history()) {
            if (script.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    QueryGateClient client;
    helix::PrinterState state;
    std::unique_ptr<HeldExtrusionAPI> api;
    std::unique_ptr<helix_test::ScopedSharedResource<helix::TemperatureController>> controller;
    std::unique_ptr<ColdPullRunner> runner;

  private:
    helix::IMoonrakerAPI* previous_api_ = nullptr;
    helix::IMoonrakerClient* previous_client_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(RunnerFixture, "cold pull: teardown sends the cleanup before the disconnect only",
                 "[cold_pull]") {
    begin_heating();
    client.clear_gcode_script_history();

    runner->before_disconnect();
    drain();
    CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"));
    CHECK_FALSE(runner->running());

    // The link going down and a later connection owe nothing more.
    client.clear_gcode_script_history();
    connection(0);
    connection(2);
    runner.reset();
    drain();
    CHECK(client.gcode_script_history().empty());
}

TEST_CASE_METHOD(RunnerFixture,
                 "cold pull: a tool change mid-pull leaves the pull on its own heater",
                 "[cold_pull][multi_extruder]") {
    state.temperature_state().init_extruders({"extruder", "extruder1"});
    state.update_from_status({{"toolhead", {{"extruder", "extruder1"}}}});
    drain();
    REQUIRE(runner->start(ColdPullMaterial::Pla, {230, 90}, false) == ColdPullRefusal::Ok);

    // Another client sends T0.
    state.update_from_status({{"toolhead", {{"extruder", "extruder"}}}});
    drain();
    runner->toolhead_clear();
    drain();
    CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=230"));
    CHECK_FALSE(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET="));

    feed("extruder1", 228, 230);
    CHECK(runner->flow()->stage() == ColdPullStage::Feed);

    client.clear_gcode_script_history();
    runner->cancel();
    drain();
    CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0"));
}

TEST_CASE_METHOD(RunnerFixture, "cold pull: a feed queued behind another operation resolves",
                 "[cold_pull]") {
    begin_heating();
    feed("extruder", 228, 230);
    REQUIRE(runner->flow()->stage() == ColdPullStage::Feed);

    runner->extrude();
    drain();
    REQUIRE(api->held == 1);
    REQUIRE(runner->flow()->busy());
    // Its error callback only logs, so Klipper's own report of a refusal shows.
    CHECK_FALSE(api->held_surfaces_errors);

    REQUIRE(api->held_queued);
    api->held_queued();
    drain();
    CHECK_FALSE(runner->flow()->busy());
}

TEST_CASE_METHOD(RunnerFixture, "cold pull: a head that unloads by hand fails the unload step",
                 "[cold_pull]") {
    helix::AmsState::instance().set_backend(std::make_unique<ManualUnloadBackend>());
    REQUIRE(runner->start(ColdPullMaterial::Pla, {230, 90}, false) == ColdPullRefusal::Ok);
    REQUIRE(runner->flow()->stage() == ColdPullStage::AwaitClearToolhead);

    runner->unload_lane();
    drain();
    CHECK_FALSE(runner->flow()->busy());
    CHECK(runner->flow()->unload_failed());
}

TEST_CASE_METHOD(RunnerFixture, "cold pull: no extruder refuses the start", "[cold_pull]") {
    state.temperature_state().init_extruders({});
    CHECK(runner->can_start() == ColdPullRefusal::NoHeater);
}

TEST_CASE_METHOD(RunnerFixture, "cold pull: Setup shows the temperature start() will send",
                 "[cold_pull]") {
    SafetyLimits limits = api->get_safety_limits();
    limits.set_max_temp_for("extruder", 220);
    api->set_safety_limits(limits);
    runner->init_subjects();
    runner->select_material(ColdPullMaterial::Pla);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "cold_pull_soak_text"))) ==
          "220°C");
}

TEST_CASE_METHOD(RunnerFixture,
                 "cold pull: a reconnect cleans up only a target the printer still has",
                 "[cold_pull]") {
    begin_heating();
    connection(0);
    REQUIRE(runner->flow()->stage() == ColdPullStage::Abandoned);
    client.clear_gcode_script_history();

    SECTION("set elsewhere while the link was down: it stands") {
        // The heater's subject still shows 230 from before the drop.
        client.set_extruder_target(200);
        connection(2);
        CHECK_FALSE(sent("TARGET=0"));
    }
    SECTION("still ours: it goes off") {
        connection(2);
        CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"));
    }
}

TEST_CASE_METHOD(RunnerFixture,
                 "cold pull: an owed cleanup never waits on an answer that cannot come",
                 "[cold_pull]") {
    begin_heating();
    connection(0);
    client.clear_gcode_script_history();

    SECTION("the target query fails while still connected") {
        client.fail_query = true;
        connection(2);
        REQUIRE(client.queries == 1);
        CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"));
    }
    SECTION("teardown arrives while the query is unanswered") {
        client.hold_query = true;
        connection(2);
        REQUIRE(client.queries == 1);
        REQUIRE_FALSE(sent("TARGET=0"));
        runner->before_disconnect();
        drain();
        CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"));
    }
    SECTION("the overlay reopens Setup before the reconnect") {
        runner->prepare_setup();
        connection(2);
        CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"));
    }
}

TEST_CASE_METHOD(RunnerFixture,
                 "cold pull: an earlier pull's late reply leaves the next pull alone",
                 "[cold_pull]") {
    begin_heating();
    feed("extruder", 228, 230);
    runner->extrude();
    drain();
    runner->cancel();
    feed("extruder", 25, 0);

    begin_heating();
    feed("extruder", 228, 230);
    runner->extrude();
    drain();
    REQUIRE(api->queued.size() == 2);
    REQUIRE(runner->flow()->busy());

    api->queued.front()();
    drain();
    CHECK(runner->flow()->busy());
}
