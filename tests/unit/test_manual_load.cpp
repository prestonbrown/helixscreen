// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_manual_load.cpp
 * @brief The manual load (prestonbrown/helixscreen#2000): heat, push, prime, against
 *        a mock printer. What reaches Moonraker, in what order, and the heater left
 *        off on every way out.
 *
 * Run with: ./build/bin/helix-tests "[manual_load]"
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/backend_harness.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "app_globals.h"
#include "cleanup_hold.h"
#include "core/update_queue.h"
#include "filament_op_execute.h"
#include "filament_op_heater.h"
#include "guided_load_session.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"
#include "temperature_controller.h"
#include "tool_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::GuidedLoadStage;
using helix::ui::FilamentOpHeater;
using helix::ui::GuidedLoadSession;
using helix::ui::ManualLoadRefusal;

namespace {

constexpr const char* kHeatOff = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0";
constexpr const char* kHeatLoad = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=220";

/// Holds every extrusion unanswered, for the test to answer.
class HeldExtrusionAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;

    void execute_gcode(const std::string& gcode, SuccessCallback on_success, ErrorCallback on_error,
                       uint32_t timeout_ms = 0, bool silent = false,
                       SuccessCallback on_queued = nullptr, bool caller_surfaces_errors = true,
                       bool bypass_busy_gate = false) override {
        if (gcode.find("G1 E") != std::string::npos) {
            extrusions.push_back(gcode);
            ok = std::move(on_success);
            fail = std::move(on_error);
            return;
        }
        MoonrakerAPI::execute_gcode(gcode, std::move(on_success), std::move(on_error), timeout_ms,
                                    silent, std::move(on_queued), caller_surfaces_errors,
                                    bypass_busy_gate);
    }

    std::vector<std::string> extrusions;
    SuccessCallback ok;
    ErrorCallback fail;
};

class ManualLoadFixture : public LVGLTestFixture {
  public:
    ManualLoadFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.temperature_state().init_extruders({"extruder", "extruder1"});
        state.set_klippy_state_sync(helix::KlippyState::READY);
        state.set_printer_connection_state(2, "Connected");
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<HeldExtrusionAPI>(client, state);
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        controller =
            std::make_unique<helix_test::ScopedSharedResource<helix::TemperatureController>>(
                std::make_shared<helix::TemperatureController>(state, api.get()));
        heater = std::make_unique<helix_test::ScopedSharedResource<FilamentOpHeater>>(
            std::make_shared<FilamentOpHeater>(state));
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().clear_backends();
        cold_extrude_was_ = helix::SafetySettingsManager::instance().get_allow_cold_extrude();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
        PostOpCooldownManager::instance().init();
        drain();
        client.clear_gcode_script_history();
    }

    ~ManualLoadFixture() override {
        session.reset();
        heater.reset();
        controller.reset();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(cold_extrude_was_);
        PostOpCooldownManager::instance().cancel();
        StandardMacros::instance().reset();
        helix::AmsState::instance().clear_backends();
        set_moonraker_api(previous_api_);
        drain();
        client.disconnect();
        api.reset();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void feed_nozzle(double actual_c, double target_c) {
        state.update_from_status({{"extruder", {{"temperature", actual_c}, {"target", target_c}}}});
        drain();
    }

    void set_print_state(const char* wire) {
        state.update_from_status({{"print_stats", {{"state", wire}}}});
        drain();
    }

    /// Open a manual load on a cold nozzle and take it to the hand push.
    void begin_at_push() {
        feed_nozzle(25, 0);
        session = std::make_unique<GuidedLoadSession>(state, -1);
        session->begin();
        drain();
        REQUIRE(session->stage() == GuidedLoadStage::Heating);
        feed_nozzle(150, 220);
        REQUIRE(session->stage() == GuidedLoadStage::Heating);
        feed_nozzle(218, 220);
        REQUIRE(session->stage() == GuidedLoadStage::AwaitPush);
    }

    void answer_extrusion(bool succeeded) {
        if (succeeded) {
            auto cb = std::move(api->ok);
            cb();
        } else {
            auto cb = std::move(api->fail);
            cb(MoonrakerError::connection_lost("gcode"));
        }
        drain();
    }

    [[nodiscard]] int count(const std::string& needle) const {
        const auto& hist = client.gcode_script_history();
        return static_cast<int>(std::count_if(hist.begin(), hist.end(), [&](const std::string& s) {
            return s.find(needle) != std::string::npos;
        }));
    }

    MoonrakerClientMock client;
    helix::PrinterState state;
    std::unique_ptr<HeldExtrusionAPI> api;
    std::unique_ptr<helix_test::ScopedSharedResource<helix::TemperatureController>> controller;
    std::unique_ptr<helix_test::ScopedSharedResource<FilamentOpHeater>> heater;
    std::unique_ptr<GuidedLoadSession> session;

  private:
    helix::IMoonrakerAPI* previous_api_ = nullptr;
    bool cold_extrude_was_ = false;
};

} // namespace

TEST_CASE("manual load refusal: checked in order", "[manual_load]") {
    const auto refusal = [](PrintState lifecycle, bool reachable, bool pending, bool cold_pull,
                            const char* slot_extruder) {
        return helix::ui::manual_load_refusal(lifecycle, reachable, pending, cold_pull,
                                              slot_extruder, "extruder");
    };
    CHECK(refusal(PrintState::Idle, true, false, false, "extruder") == ManualLoadRefusal::None);
    CHECK(refusal(PrintState::Printing, false, true, true, "") ==
          ManualLoadRefusal::JobHoldsMachine);
    CHECK(refusal(PrintState::Paused, true, false, false, "extruder") ==
          ManualLoadRefusal::JobHoldsMachine);
    CHECK(refusal(PrintState::Idle, false, true, true, "") == ManualLoadRefusal::NotReady);
    CHECK(refusal(PrintState::Idle, true, true, true, "") == ManualLoadRefusal::PreheatPending);
    CHECK(refusal(PrintState::Idle, true, false, true, "") == ManualLoadRefusal::ColdPullRunning);
    // An unknown tool refuses: it never falls back to the active nozzle.
    CHECK(refusal(PrintState::Complete, true, false, false, "") == ManualLoadRefusal::ToolUnknown);
    CHECK(refusal(PrintState::Complete, true, false, false, "extruder1") ==
          ManualLoadRefusal::ToolNotActive);
    CHECK(helix::ui::manual_load_refusal_text(ManualLoadRefusal::None).empty());
    CHECK_FALSE(helix::ui::manual_load_refusal_text(ManualLoadRefusal::ToolUnknown).empty());
}

TEST_CASE_METHOD(ManualLoadFixture, "manual load refusal: read off the live printer",
                 "[manual_load]") {
    CHECK(helix::ui::manual_load_refusal_now(state, -1) == ManualLoadRefusal::None);
    set_print_state("printing");
    CHECK(helix::ui::manual_load_refusal_now(state, -1) == ManualLoadRefusal::JobHoldsMachine);
    set_print_state("standby");
    state.set_printer_connection_state(0, "Disconnected");
    drain();
    CHECK(helix::ui::manual_load_refusal_now(state, -1) == ManualLoadRefusal::NotReady);
}

TEST_CASE_METHOD(ManualLoadFixture, "manual load: heat, push, feed, more, Done, in that order",
                 "[manual_load]") {
    begin_at_push();
    // Heat went through TemperatureController; nothing moved while it heated.
    CHECK(count(kHeatLoad) == 1);
    CHECK(api->extrusions.empty());
    CHECK(heater->get().holding());
    CHECK(helix::CleanupHold::any_held_signal_safe());

    session->extrude();
    REQUIRE(api->extrusions.size() == 1);
    CHECK(api->extrusions[0] == "M83\nG1 E60 F300");
    CHECK(session->stage() == GuidedLoadStage::Feeding);
    session->extrude(); // a second tap while one runs sends nothing
    CHECK(api->extrusions.size() == 1);
    answer_extrusion(true);
    CHECK(session->stage() == GuidedLoadStage::ConfirmOutput);

    session->extrude();
    REQUIRE(api->extrusions.size() == 2);
    CHECK(api->extrusions[1] == "M83\nG1 E20 F300");
    answer_extrusion(true);

    session->done();
    drain();
    CHECK(session->stage() == GuidedLoadStage::Finished);
    // Done leaves the cooldown to the post-op timer, which owns the cool down after.
    CHECK(count(kHeatOff) == 0);
    const auto& owed = PostOpCooldownManager::instance().pending_heaters();
    CHECK(std::find(owed.begin(), owed.end(), "extruder") != owed.end());
    // The cooldown timer holds the SIGTERM path now, not the load.
    CHECK_FALSE(heater->get().holding());
}

TEST_CASE_METHOD(ManualLoadFixture, "manual load: every way out turns the heater off",
                 "[manual_load]") {
    SECTION("Cancel while heating") {
        feed_nozzle(25, 0);
        session = std::make_unique<GuidedLoadSession>(state, -1);
        session->begin();
        drain();
        feed_nozzle(90, 220);
        session->cancel();
        drain();
        CHECK(count(kHeatOff) == 1);
    }
    SECTION("Cancel at the hand push") {
        begin_at_push();
        session->cancel();
        drain();
        CHECK(count(kHeatOff) == 1);
        CHECK(PostOpCooldownManager::instance().pending_heaters().empty());
    }
    SECTION("a failed extrusion") {
        begin_at_push();
        session->extrude();
        answer_extrusion(false);
        CHECK(session->stage() == GuidedLoadStage::Cancelled);
        CHECK(count(kHeatOff) == 1);
    }
    SECTION("the dialog going away (app exit, a dismissal)") {
        begin_at_push();
        session.reset();
        drain();
        CHECK(count(kHeatOff) == 1);
    }
    SECTION("teardown before the disconnect, then nothing more is sent") {
        begin_at_push();
        heater->get().before_disconnect();
        drain();
        CHECK(count(kHeatOff) == 1);
        CHECK(session->ended());
        CHECK_FALSE(helix::CleanupHold::any_held_signal_safe());
        session->extrude();
        session.reset();
        drain();
        CHECK(api->extrusions.empty());
        CHECK(count(kHeatOff) == 1);
    }
    SECTION("the printer going away: the cooldown is owed") {
        begin_at_push();
        state.set_printer_connection_state(0, "Disconnected");
        drain();
        CHECK(session->ended());
        session->extrude();
        CHECK(api->extrusions.empty());
        const auto& owed = PostOpCooldownManager::instance().pending_heaters();
        CHECK(std::find(owed.begin(), owed.end(), "extruder") != owed.end());
    }
}

TEST_CASE_METHOD(ManualLoadFixture, "manual load: a tool change mid-load ends it before any push",
                 "[manual_load]") {
    begin_at_push();
    REQUIRE(state.temperature_state().active_extruder_name() == "extruder");
    state.update_from_status({{"toolhead", {{"extruder", "extruder1"}}}});
    drain();
    CHECK(session->ended());
    session->extrude();
    drain();
    // No G1 E reaches extruder1, and the nozzle this load heated goes off.
    CHECK(api->extrusions.empty());
    CHECK(count(kHeatOff) == 1);
    CHECK_FALSE(heater->get().holding());
}

namespace {

/// Lane 1 feeds tool 1; lane 2 names a tool the printer does not have.
class MappedLanes : public helix::AmsBackendMock {
  public:
    MappedLanes() : helix::AmsBackendMock(4) {}
    helix::SlotInfo get_slot_info(int slot_index) const override {
        helix::SlotInfo info = helix::AmsBackendMock::get_slot_info(slot_index);
        info.mapped_tool = slot_index == 1 ? 1 : (slot_index == 2 ? 5 : -1);
        return info;
    }
};

} // namespace

TEST_CASE_METHOD(ManualLoadFixture, "manual load: a slot's tool must resolve to an extruder",
                 "[manual_load]") {
    helix::PrinterDiscovery two;
    two.parse_objects(nlohmann::json{"extruder", "extruder1"});
    helix::ToolState::instance().init_tools(two);
    MappedLanes lanes;
    CHECK(helix::ui::manual_load_extruder(nullptr, -1, state) == "extruder");
    CHECK(helix::ui::manual_load_extruder(&lanes, 1, state) == "extruder1");
    CHECK(helix::ui::manual_load_extruder(&lanes, 2, state).empty());
    CHECK(helix::ui::manual_load_extruder(&lanes, 3, state).empty());

    helix::PrinterDiscovery one;
    one.parse_objects(nlohmann::json{"extruder"});
    helix::ToolState::instance().init_tools(one);
}

TEST_CASE_METHOD(ManualLoadFixture, "manual load: a print taking the machine ends it, heat left",
                 "[manual_load]") {
    begin_at_push();
    set_print_state("printing");
    CHECK(session->ended());
    session->extrude();
    session.reset();
    drain();
    CHECK(api->extrusions.empty());
    CHECK(count(kHeatOff) == 0);
}

TEST_CASE_METHOD(ManualLoadFixture, "load_is_by_hand: no filament system, no load macro, no job",
                 "[manual_load]") {
    StandardMacros::instance().reset();
    helix::PrinterDiscovery bare;
    bare.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().init(bare);
    CHECK(helix::ui::load_is_by_hand(nullptr, -1, state));
    set_print_state("paused");
    CHECK_FALSE(helix::ui::load_is_by_hand(nullptr, -1, state));
    set_print_state("standby");

    helix::PrinterDiscovery with_macro;
    with_macro.parse_objects(nlohmann::json{"extruder", "gcode_macro LOAD_FILAMENT"});
    StandardMacros::instance().reset();
    StandardMacros::instance().init(with_macro);
    CHECK_FALSE(helix::ui::load_is_by_hand(nullptr, -1, state));
}

namespace {

/// Drains what the backend defers while it and AmsState are still alive.
struct DrainingFixture : public LVGLTestFixture {
    ~DrainingFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

} // namespace

// A head with no automatic feeder loads by hand: the screen offers the guided
// manual load (prestonbrown/helixscreen#2000) instead of a feed the firmware refuses.
TEST_CASE_METHOD(DrainingFixture, "Snapmaker: a head with no automatic feeder loads by hand",
                 "[ams][snapmaker][manual_load]") {
    helix::test::RegisteredWired<helix::AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    // Before any feeder frame nothing is known: the load stays automatic.
    CHECK(backend.head_filament_ops(1).load == helix::FilamentOpSupport::Automatic);
    CHECK_FALSE(backend.head_filament_ops(1).guided_hand_load);

    const bool no_module = GENERATE(true, false);
    backend.fire_status(nlohmann::json{{"filament_feed left",
                                        {{"extruder1",
                                          {{"filament_detected", false},
                                           {"channel_state", "wait_insert"},
                                           {"module_exist", !no_module},
                                           {"disable_auto", !no_module}}}}}});
    const helix::HeadFilamentOps ops = backend.head_filament_ops(1);
    CHECK(ops.load == helix::FilamentOpSupport::Manual);
    CHECK(ops.guided_hand_load);
    CHECK_FALSE(ops.load_reason.empty());
    // Only that head: its neighbour's feeder said nothing.
    CHECK(backend.head_filament_ops(0).load == helix::FilamentOpSupport::Automatic);

    backend.fire_status(
        nlohmann::json{{"filament_feed left",
                        {{"extruder1", {{"module_exist", true}, {"disable_auto", false}}}}}});
    CHECK(backend.head_filament_ops(1).load == helix::FilamentOpSupport::Automatic);
}
