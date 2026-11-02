// SPDX-License-Identifier: GPL-3.0-or-later
//
// A chamber heater that turns down the target it was sent: the latch on
// PrinterTemperatureState, the route around the gcode queue that
// TemperatureController takes when the backend has one, and the arbiter that
// decides when a print held by an unheated chamber is put to the user.
#include "ui_modal.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/abort_manager_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "../test_helpers/update_queue_test_access.h"
#include "abort_manager.h"
#include "app_globals.h"
#include "chamber_heater_backend.h"
#include "chamber_stall_prompt.h"
#include "lvgl.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "settings_manager.h"
#include "temperature_controller.h"
#include "temperature_service.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ChamberStallAction;
using helix::ChamberStallArbiter;
using helix::ChamberStallInputs;
using helix::PrinterTemperatureState;
using helix::chamber::CommandRejection;
using helix::chamber::RefusalCause;

namespace {

void drain() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

struct RejectionStateFixture : public LVGLTestFixture {
    PrinterTemperatureState ts;

    RejectionStateFixture() {
        ts.init_subjects(false);
        ts.set_chamber_heater_name("heater_generic dragonbreath");
        ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                          "output_pin dragonbreath_filter");
    }

    void frame(const nlohmann::json& status) {
        ts.update_from_status(status);
    }
    int rejected() {
        return lv_subject_get_int(ts.get_chamber_heater_command_rejected_subject());
    }
    int seq() {
        return lv_subject_get_int(ts.get_chamber_heater_rejection_seq_subject());
    }
    std::string text() {
        return lv_subject_get_string(ts.get_chamber_heater_rejection_text_subject());
    }
};

/// The frame the U1 rig sent when the box answered M141 S60 with a revision
/// conflict: the module zeroed the heater target and held the reason.
nlohmann::json conflict_frame(int seq) {
    return {{"heater_generic dragonbreath", {{"temperature", 28.0}, {"target", 0.0}}},
            {"dragonbreath",
             {{"protocol_error", "revision_conflict"},
              {"protocol_error_code", "revision_conflict"},
              {"protocol_error_seq", seq},
              {"protocol_error_target", 60.0}}}};
}

} // namespace

TEST_CASE_METHOD(RejectionStateFixture,
                 "a rejected chamber target latches until the heater takes one",
                 "[chamber][rejection][latch]") {
    REQUIRE(rejected() == 0);
    frame(conflict_frame(1));
    CHECK(rejected() == 1);
    CHECK(seq() == 1);
    CHECK(text() == lv_tr("The heater didn't accept the command."));
    CHECK(ts.chamber_command_rejection().kind == CommandRejection::Conflict);
    CHECK(ts.chamber_command_rejection().requested_target_c == Catch::Approx(60.0));

    SECTION("a frame that says nothing about it keeps the latch") {
        frame({{"dragonbreath", {{"ptc_temp", 31.0}}}});
        CHECK(rejected() == 1);
        CHECK(seq() == 1);
    }

    SECTION("each later rejection is a new event, the same reason or not") {
        frame({{"dragonbreath", {{"protocol_error_seq", 2}}}});
        CHECK(rejected() == 1);
        CHECK(seq() == 2);
        frame(
            {{"dragonbreath", {{"protocol_error_seq", 3}, {"protocol_error_code", "power_off"}}}});
        CHECK(seq() == 3);
        CHECK(ts.chamber_command_rejection().cause == RefusalCause::TurnedOff);
        CHECK(text() == lv_tr("The heater was turned off at the heater."));
    }

    SECTION("the heater taking the requested target clears it") {
        frame({{"heater_generic dragonbreath", {{"target", 59.0}}}});
        CHECK(rejected() == 1);
        frame({{"heater_generic dragonbreath", {{"target", 60.0}}}});
        CHECK(rejected() == 0);
        CHECK(text().empty());
        CHECK(ts.chamber_command_rejection().kind == CommandRejection::None);
    }

    SECTION("the device reporting nothing outstanding clears it") {
        frame({{"dragonbreath", {{"protocol_error", ""}, {"protocol_error_code", ""}}}});
        CHECK(rejected() == 0);
        CHECK(seq() == 1);
    }

    SECTION("clearing re-arms: the next rejection latches again") {
        frame({{"dragonbreath", {{"protocol_error_code", ""}}}});
        REQUIRE(rejected() == 0);
        frame(conflict_frame(2));
        CHECK(rejected() == 1);
        CHECK(seq() == 2);
    }
}

TEST_CASE_METHOD(RejectionStateFixture,
                 "a rejection without a requested target clears on a new one",
                 "[chamber][rejection][latch]") {
    frame({{"heater_generic dragonbreath", {{"temperature", 28.0}, {"target", 0.0}}},
           {"dragonbreath",
            {{"protocol_error_code", "heater_inhibited"}, {"protocol_error_seq", 1}}}});
    REQUIRE(rejected() == 1);
    CHECK(text() == lv_tr("The heater is inhibited."));
    frame({{"heater_generic dragonbreath", {{"target", 45.0}}}});
    CHECK(rejected() == 0);
}

TEST_CASE_METHOD(RejectionStateFixture, "a refusal for a fault names the fault",
                 "[chamber][rejection][latch]") {
    frame({{"dragonbreath",
            {{"fault", true},
             {"fault_reason", "ptc_overtemp"},
             {"protocol_error_code", "heater_fault"},
             {"protocol_error_seq", 1}}}});
    REQUIRE(rejected() == 1);
    CHECK(text() == fmt::format(fmt::runtime(lv_tr("The heater reports a fault: {}.")),
                                lv_tr("Heater over-temperature")));
}

TEST_CASE_METHOD(RejectionStateFixture, "what a glue reports at connect decides the latch",
                 "[chamber][rejection][latch]") {
    SECTION("an old seq with nothing outstanding is not a rejection") {
        frame({{"dragonbreath", {{"protocol_error_code", ""}, {"protocol_error_seq", 7}}}});
        CHECK(rejected() == 0);
        CHECK(seq() == 0);
        frame({{"dragonbreath",
                {{"protocol_error_code", "auth_failed"}, {"protocol_error_seq", 8}}}});
        CHECK(rejected() == 1);
        CHECK(text() == lv_tr("The heater did not accept the printer's credentials."));
    }

    SECTION("an older glue's free-text complaint never latches") {
        frame({{"dragonbreath", {{"protocol_error", "revision_conflict"}}}});
        CHECK(rejected() == 0);
    }

    SECTION("a heater whose backend has no diagnostics never latches") {
        ts.set_chamber_diagnostics_source("", "", "");
        frame(conflict_frame(1));
        CHECK(rejected() == 0);
    }
}

// ============================================================================
// TemperatureController: the route around the gcode queue
// ============================================================================

namespace {

/// Answers the appliance's set-target method the way the test asks: taken,
/// refused, or absent (an older glue Moonraker has no endpoint for).
class RouteMockClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    enum class Mode { Accept, Refuse, Missing };
    Mode mode = Mode::Accept;
    std::vector<double> route_targets;
    /// When set, the route answers this "result" instead of the mode's.
    nlohmann::json reply;

    helix::RequestId send_jsonrpc(
        const std::string& method, const nlohmann::json& params,
        helix::SharedCallback<void(const nlohmann::json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "printer.dragonbreath.set_target") {
            route_targets.push_back(params.value("target", -1.0));
            if (mode == Mode::Missing) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::JSON_RPC_ERROR;
                err.code = -32601;
                err.message = "Method not found";
                if (error_cb)
                    error_cb(err);
            } else if (success_cb && !reply.is_null()) {
                success_cb({{"result", reply}});
            } else if (success_cb) {
                success_cb({{"result",
                             {{"accepted", mode == Mode::Accept},
                              {"reason", mode == Mode::Accept ? "" : "power_off"},
                              {"seq", 1}}}});
            }
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    std::vector<std::string> heater_gcode() {
        std::vector<std::string> out;
        for (const auto& g : gcode_script_history()) {
            if (g.find("SET_HEATER_TEMPERATURE") != std::string::npos ||
                g.find("M104") != std::string::npos) {
                out.push_back(g);
            }
        }
        return out;
    }
};

struct RouteFixture : public LVGLTestFixture {
    RouteMockClient client;
    helix::PrinterState& state;
    MoonrakerAPI api;
    std::shared_ptr<helix::TemperatureController> controller;
    helix_test::ScopedSharedResource<helix::TemperatureController> scope;

    RouteFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), state(reset_global_state()),
          api(client, state),
          controller(std::make_shared<helix::TemperatureController>(state, &api)),
          scope(controller) {
        state.set_klippy_state_sync(helix::KlippyState::READY);
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
        helix::PrinterDiscovery hardware;
        hardware.parse_objects(nlohmann::json{"heater_generic dragonbreath", "dragonbreath",
                                              "extruder", "heater_bed"});
        state.set_hardware(hardware);
        drain();
        client.clear_gcode_script_history();
    }

    ~RouteFixture() override {
        drain();
    }

    static helix::PrinterState& reset_global_state() {
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        return get_printer_state();
    }
};

} // namespace

TEST_CASE_METHOD(RouteFixture, "chamber targets take the backend's route around the gcode queue",
                 "[chamber][rejection][route]") {
    REQUIRE(controller->chamber_has_direct_route());

    SECTION("a chamber target goes by the route, not gcode") {
        controller->set_target(helix::HeaterType::Chamber, 60.0);
        drain();
        REQUIRE(client.route_targets == std::vector<double>{60.0});
        CHECK(client.heater_gcode().empty());
        CHECK(controller->last_chamber_target_c() == Catch::Approx(60.0));
    }

    SECTION("other heaters never do") {
        controller->set_target(helix::HeaterType::Nozzle, 210.0);
        drain();
        CHECK(client.route_targets.empty());
        CHECK(client.heater_gcode().size() == 1);
    }

    SECTION("a glue without the route falls back to gcode, once and for good") {
        client.mode = RouteMockClient::Mode::Missing;
        controller->set_target(helix::HeaterType::Chamber, 60.0);
        drain();
        CHECK(client.route_targets.size() == 1);
        REQUIRE(client.heater_gcode().size() == 1);
        CHECK(client.heater_gcode()[0].find("HEATER=dragonbreath TARGET=60") != std::string::npos);
        CHECK_FALSE(controller->chamber_has_direct_route());

        controller->set_target(helix::HeaterType::Chamber, 50.0);
        drain();
        CHECK(client.route_targets.size() == 1);
        CHECK(client.heater_gcode().size() == 2);
    }

    SECTION("a refusal reaches the caller as an error") {
        client.mode = RouteMockClient::Mode::Refuse;
        std::string reason;
        helix::SendOptions opts;
        opts.toast = false;
        opts.on_error = [&reason](const MoonrakerError& e) { reason = e.message; };
        bool succeeded = false;
        opts.on_success = [&succeeded]() { succeeded = true; };
        controller->set_target(helix::HeaterType::Chamber, 60.0, opts);
        drain();
        // The vendor code never reaches the user.
        CHECK(reason == lv_tr("The heater didn't accept the command."));
        CHECK_FALSE(succeeded);
        CHECK(client.heater_gcode().empty());
    }
}

TEST_CASE_METHOD(RouteFixture,
                 "a chamber retry never queues behind the wait it is meant to release",
                 "[chamber][rejection][route]") {
    std::vector<helix::ChamberRouteOutcome> outcomes;
    auto done = [&outcomes](const helix::ChamberRouteReply& r) { outcomes.push_back(r.outcome); };

    SECTION("with the route, the retry goes by it") {
        controller->retry_chamber_target(60.0, done);
        drain();
        CHECK(client.route_targets == std::vector<double>{60.0});
        CHECK(outcomes == std::vector{helix::ChamberRouteOutcome::Accepted});
    }

    SECTION("a missing route fails the retry instead of sending gcode") {
        client.mode = RouteMockClient::Mode::Missing;
        controller->retry_chamber_target(60.0, done);
        drain();
        CHECK(outcomes == std::vector{helix::ChamberRouteOutcome::Unavailable});
        CHECK(client.heater_gcode().empty());
        CHECK_FALSE(controller->chamber_has_direct_route());
    }

    SECTION("a backend with no route sends nothing at all") {
        controller->set_chamber_actions("", "", 0.0, "");
        controller->retry_chamber_target(60.0, done);
        drain();
        CHECK(outcomes == std::vector{helix::ChamberRouteOutcome::Unavailable});
        CHECK(client.route_targets.empty());
        CHECK(client.heater_gcode().empty());
    }

    SECTION("a reply still pending is not a failure") {
        client.reply = {{"accepted", nullptr},
                        {"pending", true},
                        {"reason", nullptr},
                        {"seq", nullptr},
                        {"target", 60.0}};
        controller->retry_chamber_target(60.0, done);
        drain();
        CHECK(outcomes == std::vector{helix::ChamberRouteOutcome::Pending});
    }
}

TEST_CASE_METHOD(RouteFixture, "what the route answers reaches a plain chamber set",
                 "[chamber][rejection][route]") {
    std::string error;
    const char* error_tag = nullptr;
    bool succeeded = false;
    helix::SendOptions opts;
    opts.toast = false;
    opts.on_error = [&error, &error_tag](const MoonrakerError& e) {
        error = e.localized_message();
        error_tag = e.message_tag;
    };
    opts.on_success = [&succeeded]() { succeeded = true; };

    SECTION("superseded: the newer target governs, nothing failed") {
        client.reply = {{"accepted", false},
                        {"pending", false},
                        {"reason", "superseded"},
                        {"seq", 4},
                        {"target", 50.0}};
        controller->set_target(helix::HeaterType::Chamber, 50.0, opts);
        drain();
        CHECK(succeeded);
        CHECK(error.empty());
    }

    SECTION("a shut-down printer is said so") {
        client.reply = {{"accepted", false},
                        {"pending", false},
                        {"reason", "klipper_shutdown"},
                        {"seq", nullptr},
                        {"target", 50.0}};
        controller->set_target(helix::HeaterType::Chamber, 50.0, opts);
        drain();
        CHECK_FALSE(succeeded);
        CHECK(error == lv_tr("The printer is shut down."));
        // Carried untranslated off the WebSocket thread, translated where shown.
        CHECK(error_tag != nullptr);
    }

    SECTION("still pending after the route's wait counts as sent") {
        client.reply = {{"accepted", nullptr},
                        {"pending", true},
                        {"reason", nullptr},
                        {"seq", nullptr},
                        {"target", 50.0}};
        controller->set_target(helix::HeaterType::Chamber, 50.0, opts);
        drain();
        CHECK(succeeded);
        CHECK(error.empty());
    }
}

TEST_CASE("the route's reply parses with or without the result wrapper",
          "[chamber][rejection][route]") {
    using helix::ChamberRouteOutcome;
    using helix::parse_chamber_route_reply;
    const nlohmann::json accepted = {{"accepted", true}, {"pending", false}, {"reason", nullptr}};
    CHECK(parse_chamber_route_reply(accepted).outcome == ChamberRouteOutcome::Accepted);
    CHECK(parse_chamber_route_reply({{"result", accepted}}).outcome ==
          ChamberRouteOutcome::Accepted);
    const auto refused = parse_chamber_route_reply(
        {{"result", {{"accepted", false}, {"pending", false}, {"reason", "countermanded"}}}});
    CHECK(refused.outcome == ChamberRouteOutcome::Refused);
    CHECK(refused.reason == "countermanded");
    CHECK(parse_chamber_route_reply({{"accepted", nullptr}, {"pending", true}}).outcome ==
          ChamberRouteOutcome::Pending);
    CHECK(parse_chamber_route_reply({{"accepted", false}, {"reason", "superseded"}}).outcome ==
          ChamberRouteOutcome::Superseded);
    CHECK(parse_chamber_route_reply({{"accepted", nullptr}, {"reason", "klipper_shutdown"}})
              .outcome == ChamberRouteOutcome::PrinterShutdown);
    CHECK(parse_chamber_route_reply({{"result", "ok"}}).outcome == ChamberRouteOutcome::Accepted);
}

TEST_CASE_METHOD(RouteFixture, "Try again resends the target the heater turned down",
                 "[chamber][rejection][route][prompt]") {
    state.temperature_state().update_from_status(conflict_frame(1));
    REQUIRE(state.temperature_state().chamber_command_rejection().kind ==
            CommandRejection::Conflict);

    CHECK(helix::ui::ChamberStallPrompt::retry_target_c() == Catch::Approx(60.0));
    REQUIRE(helix::ui::ChamberStallPrompt::retry());
    drain();
    CHECK(client.route_targets == std::vector<double>{60.0});
    CHECK(client.heater_gcode().empty());

    SECTION("with no route it sends nothing") {
        controller->set_chamber_actions("", "", 0.0, "");
        CHECK_FALSE(helix::ui::ChamberStallPrompt::retry());
        drain();
        CHECK(client.route_targets.size() == 1);
        CHECK(client.heater_gcode().empty());
    }
}

TEST_CASE_METHOD(RouteFixture, "the banner's Retry never queues gcode behind a job",
                 "[chamber][rejection][route][banner]") {
    state.temperature_state().update_from_status(conflict_frame(1));
    controller->set_chamber_actions("", "", 0.0, ""); // a glue without the route

    SECTION("during a job it sends nothing") {
        state.update_from_status({{"print_stats", {{"state", "printing"}}}});
        drain();
        REQUIRE(lv_subject_get_int(state.print_state().get_job_holds_machine_subject()) == 1);
        TemperatureService::on_chamber_retry_target_clicked(nullptr);
        drain();
        CHECK(client.heater_gcode().empty());
    }

    SECTION("with no job, gcode is fine") {
        TemperatureService::on_chamber_retry_target_clicked(nullptr);
        drain();
        REQUIRE(client.heater_gcode().size() == 1);
        CHECK(client.heater_gcode()[0].find("TARGET=60") != std::string::npos);
    }
}

// ============================================================================
// When the user hears about it
// ============================================================================

TEST_CASE("the chamber prompt arms once per rejection", "[chamber][rejection][prompt]") {
    ChamberStallArbiter arbiter;
    ChamberStallInputs waiting;
    waiting.job_active = true;
    waiting.chamber_wait = true;

    SECTION("a refused target during a chamber wait prompts, once per rejection") {
        auto in = waiting;
        in.rejected = true;
        in.rejection_seq = 1;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.chamber_target_deci = 0;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.rejection_seq = 2;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
    }

    SECTION("a rejection outside a chamber wait mid-print stays on the banner") {
        auto in = waiting;
        in.chamber_wait = false;
        in.rejected = true;
        in.rejection_seq = 1;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        // The wait that follows still prompts for it.
        in.chamber_wait = true;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
    }

    SECTION("an unheated chamber with no rejection prompts once per job") {
        auto in = waiting;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.chamber_wait = false;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.chamber_wait = true;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.job_active = false;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.job_active = true;
        CHECK(arbiter.decide(in) == ChamberStallAction::Prompt);
    }

    SECTION("a heating chamber is just a wait") {
        auto in = waiting;
        in.chamber_target_deci = 600;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
    }

    SECTION("no job: a toast per rejection, never a prompt") {
        ChamberStallInputs in;
        in.rejected = true;
        in.rejection_seq = 1;
        CHECK(arbiter.decide(in) == ChamberStallAction::Toast);
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
        in.rejection_seq = 2;
        CHECK(arbiter.decide(in) == ChamberStallAction::Toast);
        in.rejected = false;
        in.rejection_seq = 2;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
    }

    SECTION("a rejection already prompted does not toast after the job") {
        auto in = waiting;
        in.rejected = true;
        in.rejection_seq = 1;
        REQUIRE(arbiter.decide(in) == ChamberStallAction::Prompt);
        in.job_active = false;
        CHECK(arbiter.decide(in) == ChamberStallAction::None);
    }
}

TEST_CASE("the prompt closes once there is nothing left to ask", "[chamber][rejection][prompt]") {
    ChamberStallInputs in;
    in.job_active = true;
    in.chamber_wait = true;
    in.rejected = true;
    CHECK_FALSE(ChamberStallArbiter::should_close(in));
    in.chamber_target_deci = 600; // the heater took the retry, but the latch has not cleared
    CHECK_FALSE(ChamberStallArbiter::should_close(in));
    in.rejected = false;
    CHECK(ChamberStallArbiter::should_close(in));

    in = {};
    in.job_active = true;
    in.chamber_wait = true;
    CHECK_FALSE(ChamberStallArbiter::should_close(in));
    in.chamber_wait = false; // the chamber reached the wait's temperature from any source
    CHECK(ChamberStallArbiter::should_close(in));
    in.chamber_wait = true;
    in.job_active = false;
    CHECK(ChamberStallArbiter::should_close(in));
}

TEST_CASE("each chamber wait gets the cancel that ends it", "[chamber][rejection][cancel]") {
    using helix::ChamberCancelPath;
    using helix::WaitKind;
    CHECK(helix::chamber_cancel_path(WaitKind::M191) == ChamberCancelPath::ClearTargetThenCancel);
    CHECK(helix::chamber_cancel_path(WaitKind::TemperatureWait) ==
          ChamberCancelPath::EmergencyStop);
    CHECK(helix::chamber_cancel_path(WaitKind::Other) == ChamberCancelPath::ClearTargetThenCancel);
}

TEST_CASE_METHOD(RouteFixture, "Cancel print ends the chamber wait the way it can end",
                 "[chamber][rejection][cancel]") {
    using helix::AbortManager;
    AbortManagerTestAccess::reset(AbortManager::instance());
    AbortManager::instance().init(nullptr, &state);
    // A finished print completes a cancel at once; this one is still running.
    state.update_from_status({{"print_stats", {{"state", "printing"}}}});
    drain();

    SECTION("M191: clear the target by the route, then cancel without probing") {
        helix::ui::ChamberStallPrompt::cancel_print(helix::WaitKind::M191);
        drain();
        CHECK(client.route_targets == std::vector<double>{0.0});
        CHECK(AbortManager::instance().get_state() == AbortManager::State::SENT_CANCEL);
    }

    SECTION("TEMPERATURE_WAIT: nothing happens before the user confirms the emergency stop") {
        helix::ui::ChamberStallPrompt::cancel_print(helix::WaitKind::TemperatureWait);
        drain();
        CHECK(client.route_targets.empty());
        CHECK(AbortManager::instance().get_state() == AbortManager::State::IDLE);
    }

    SECTION("a confirmed emergency stop goes straight to M112") {
        AbortManager::instance().start_abort(AbortManager::Entry::EmergencyStop);
        CHECK(AbortManager::instance().get_state() == AbortManager::State::SENT_ESTOP);
    }

    SECTION("an unknown wait clears the target and cancels without probing the queue") {
        helix::ui::ChamberStallPrompt::cancel_print(helix::WaitKind::Other);
        drain();
        CHECK(client.route_targets == std::vector<double>{0.0});
        CHECK(AbortManager::instance().get_state() == AbortManager::State::SENT_CANCEL);
    }

    SECTION("an M191 cancel completes on the print ending, with no emergency stop") {
        helix::ui::ChamberStallPrompt::cancel_print(helix::WaitKind::M191);
        drain();
        REQUIRE(AbortManager::instance().get_state() == AbortManager::State::SENT_CANCEL);
        // pause_resume:cancel ends the wait and the queued CANCEL_PRINT runs.
        state.update_from_status({{"print_stats", {{"state", "cancelled"}}}});
        drain();
        const auto st = AbortManager::instance().get_state();
        CHECK(st != AbortManager::State::SENT_ESTOP);
        CHECK(st != AbortManager::State::SENT_RESTART);
        CHECK((st == AbortManager::State::COMPLETE || st == AbortManager::State::IDLE));
    }

    SECTION("a wait the device reports as its own M191 takes the M191 cancel") {
        state.temperature_state().update_from_status(
            {{"dragonbreath", {{"wait_active", true}, {"wait_target", 60.0}}}});
        REQUIRE(state.temperature_state().chamber_wait_active());
        CHECK(state.temperature_state().chamber_wait_target_c() == Catch::Approx(60.0));
    }

    AbortManagerTestAccess::reset(AbortManager::instance());
}

// ============================================================================
// The prompt itself: its observers, show and hide, and Cancel from it
// ============================================================================

namespace {

struct PromptFixture : public LVGLUITestFixture {
    helix::ui::ChamberStallPrompt prompt;

    PromptFixture() {
        auto& temps = state().temperature_state();
        temps.set_chamber_heater_name("heater_generic dragonbreath");
        temps.set_chamber_diagnostics_source("dragonbreath", "dragonbreath", "");
        state().update_from_status({{"print_stats", {{"state", "printing"}}}});
        settle();
    }
    ~PromptFixture() override {
        helix::AbortManager::instance().deinit_subjects();
        AbortManagerTestAccess::reset(helix::AbortManager::instance());
    }

    void settle() {
        for (int i = 0; i < 3; ++i) {
            drain();
            process_lvgl(20);
        }
    }
    lv_obj_t* dialog() {
        return ModalStack::instance().dialog_for_component("chamber_stall_modal");
    }
    void refuse(int seq) {
        state().temperature_state().update_from_status(
            {{"heater_generic dragonbreath", {{"temperature", 28.0}, {"target", 0.0}}},
             {"dragonbreath",
              {{"protocol_error_code", "countermanded"},
               {"protocol_error_seq", seq},
               {"protocol_error_target", 60.0}}}});
        settle();
    }
    void chamber_wait(bool on) {
        state().print_state().set_print_start_chamber_wait(on);
        settle();
    }
};

} // namespace

TEST_CASE_METHOD(PromptFixture, "the chamber prompt opens on a refused wait and closes itself",
                 "[chamber][rejection][prompt][prompt_ui]") {
    REQUIRE(lv_subject_get_int(state().print_state().get_job_holds_machine_subject()) == 1);
    refuse(1);
    CHECK(dialog() == nullptr); // a refusal outside a chamber wait stays on the banner
    chamber_wait(true);
    REQUIRE(dialog() != nullptr);

    SECTION("the wait ends: the chamber got hot from any source") {
        chamber_wait(false);
        CHECK(dialog() == nullptr);
    }

    SECTION("the heater takes the target") {
        state().temperature_state().update_from_status(
            {{"heater_generic dragonbreath", {{"target", 60.0}}}});
        settle();
        CHECK(dialog() == nullptr);
    }

    SECTION("the job ends") {
        state().update_from_status({{"print_stats", {{"state", "complete"}}}});
        settle();
        CHECK(dialog() == nullptr);
    }

    SECTION("a later refusal under another modal does not stack a second prompt") {
        helix::ui::modal_alert("Other", "Something else", ModalSeverity::Info);
        settle();
        REQUIRE(ModalStack::instance().top_component_name() != "chamber_stall_modal");
        const std::string top = ModalStack::instance().top_component_name();
        refuse(2);
        CHECK(ModalStack::instance().top_component_name() == top);
        CHECK(dialog() != nullptr);
    }

    SECTION("Cancel print cancels without probing the blocked queue") {
        AbortManagerTestAccess::reset(helix::AbortManager::instance());
        helix::AbortManager::instance().init(nullptr, &state());
        lv_obj_t* cancel = lv_obj_find_by_name(dialog(), "btn_quaternary");
        REQUIRE(cancel != nullptr);
        lv_obj_send_event(cancel, LV_EVENT_CLICKED, nullptr);
        settle();
        CHECK(helix::AbortManager::instance().get_state() ==
              helix::AbortManager::State::SENT_CANCEL);
        CHECK(dialog() == nullptr);
    }
}

TEST_CASE_METHOD(PromptFixture, "a wait the device reports as its own M191 is cancelled as one",
                 "[chamber][rejection][prompt][prompt_ui]") {
    state().temperature_state().update_from_status(
        {{"dragonbreath", {{"wait_active", true}, {"wait_target", 45.0}}}});
    refuse(1);
    chamber_wait(true);
    REQUIRE(dialog() != nullptr);
    // No file read needed: the device named the command and its temperature.
    const std::string message =
        lv_subject_get_string(lv_xml_get_subject(nullptr, "chamber_stall_message"));
    CHECK(message.find("45") != std::string::npos);
    CHECK(std::string(
              lv_subject_get_string(lv_xml_get_subject(nullptr, "chamber_stall_cancel_hint"))) ==
          lv_tr("Canceling turns the chamber target off so the wait ends, then stops the print."));
}

namespace helix::ui {
class ChamberStallPromptTestAccess {
  public:
    /// Stand in for a read of the waiting command that is still in flight.
    static void begin_read(ChamberStallPrompt& p) {
        p.wait_line_probed_ = true;
        p.wait_line_pending_ = true;
        ++p.wait_line_gen_;
    }
    static void answer(ChamberStallPrompt& p, std::optional<WaitLine> line) {
        p.on_wait_line(p.wait_line_gen_, line);
    }
    static WaitKind kind(const ChamberStallPrompt& p) {
        return p.wait_line_.kind;
    }
    static bool probed(const ChamberStallPrompt& p) {
        return p.wait_line_probed_;
    }
    static void cancel(ChamberStallPrompt& p) {
        p.on_cancel();
    }
};
} // namespace helix::ui

using helix::ui::ChamberStallPromptTestAccess;

TEST_CASE_METHOD(PromptFixture, "the emergency-stop confirmation goes when its wait does",
                 "[chamber][rejection][prompt][prompt_ui][estop]") {
    AbortManagerTestAccess::reset(helix::AbortManager::instance());
    helix::AbortManager::instance().init(nullptr, &state());
    refuse(1);
    chamber_wait(true);
    REQUIRE(dialog() != nullptr);
    helix::ui::ChamberStallPrompt::cancel_print(helix::WaitKind::TemperatureWait);
    settle();
    REQUIRE(helix::ui::ChamberStallPrompt::emergency_stop_confirm_open());

    SECTION("the chamber reaches the wait's temperature") {
        chamber_wait(false);
        CHECK_FALSE(helix::ui::ChamberStallPrompt::emergency_stop_confirm_open());
    }
    SECTION("the job ends") {
        state().update_from_status({{"print_stats", {{"state", "complete"}}}});
        settle();
        CHECK_FALSE(helix::ui::ChamberStallPrompt::emergency_stop_confirm_open());
    }
    CHECK(helix::AbortManager::instance().get_state() == helix::AbortManager::State::IDLE);
}

TEST_CASE_METHOD(PromptFixture, "a confirmed emergency stop is checked against the wait again",
                 "[chamber][rejection][prompt][prompt_ui][estop]") {
    AbortManagerTestAccess::reset(helix::AbortManager::instance());
    helix::AbortManager::instance().init(nullptr, &state());

    SECTION("the wait already ended: nothing is sent") {
        chamber_wait(false);
        CHECK_FALSE(helix::ui::ChamberStallPrompt::confirm_emergency_stop());
        CHECK(helix::AbortManager::instance().get_state() == helix::AbortManager::State::IDLE);
    }
    SECTION("the job already ended: nothing is sent") {
        chamber_wait(true);
        state().update_from_status({{"print_stats", {{"state", "complete"}}}});
        settle();
        CHECK_FALSE(helix::ui::ChamberStallPrompt::confirm_emergency_stop());
        CHECK(helix::AbortManager::instance().get_state() == helix::AbortManager::State::IDLE);
    }
    SECTION("still waiting: the stop goes out") {
        chamber_wait(true);
        CHECK(helix::ui::ChamberStallPrompt::confirm_emergency_stop());
        CHECK(helix::AbortManager::instance().get_state() ==
              helix::AbortManager::State::SENT_ESTOP);
    }
}

TEST_CASE_METHOD(PromptFixture, "each chamber wait in a job has its command read again",
                 "[chamber][rejection][prompt][prompt_ui][wait_line]") {
    refuse(1);
    chamber_wait(true);
    REQUIRE(dialog() != nullptr);
    ChamberStallPromptTestAccess::begin_read(prompt);
    ChamberStallPromptTestAccess::answer(prompt, helix::classify_wait_line("M191 S60"));
    REQUIRE(ChamberStallPromptTestAccess::kind(prompt) == helix::WaitKind::M191);
    chamber_wait(false);
    CHECK(ChamberStallPromptTestAccess::kind(prompt) == helix::WaitKind::Other);
    CHECK_FALSE(ChamberStallPromptTestAccess::probed(prompt));
}

TEST_CASE_METHOD(PromptFixture, "Cancel tapped while the waiting command is read waits for it",
                 "[chamber][rejection][prompt][prompt_ui][wait_line]") {
    AbortManagerTestAccess::reset(helix::AbortManager::instance());
    helix::AbortManager::instance().init(nullptr, &state());
    refuse(1);
    chamber_wait(true);
    REQUIRE(dialog() != nullptr);
    ChamberStallPromptTestAccess::begin_read(prompt);
    ChamberStallPromptTestAccess::cancel(prompt);
    settle();
    REQUIRE(helix::AbortManager::instance().get_state() == helix::AbortManager::State::IDLE);

    SECTION("an M191 answer cancels") {
        ChamberStallPromptTestAccess::answer(prompt, helix::classify_wait_line("M191 S60"));
        settle();
        CHECK(helix::AbortManager::instance().get_state() ==
              helix::AbortManager::State::SENT_CANCEL);
    }
    SECTION("a TEMPERATURE_WAIT answer asks for the emergency stop") {
        ChamberStallPromptTestAccess::answer(
            prompt, helix::classify_wait_line("TEMPERATURE_WAIT SENSOR=chamber MINIMUM=35"));
        settle();
        CHECK(helix::ui::ChamberStallPrompt::emergency_stop_confirm_open());
        CHECK(helix::AbortManager::instance().get_state() == helix::AbortManager::State::IDLE);
    }
    SECTION("no answer in time: the cancel goes out anyway") {
        process_lvgl(3500);
        settle();
        CHECK(helix::AbortManager::instance().get_state() ==
              helix::AbortManager::State::SENT_CANCEL);
    }
}

TEST_CASE("a TEMPERATURE_WAIT names the chamber heater only by its own names",
          "[chamber][rejection][wait_line]") {
    using helix::classify_wait_line;
    using helix::wait_names_chamber_heater;
    const std::string heater = "heater_generic dragonbreath";
    const auto on_heater =
        classify_wait_line("TEMPERATURE_WAIT SENSOR=\"heater_generic dragonbreath\" MINIMUM=35");
    CHECK(wait_names_chamber_heater(on_heater, heater));
    CHECK(wait_names_chamber_heater(on_heater, "heater_generic DragonBreath"));
    // A separate chamber sensor is a passive soak's, even beside a heater.
    const auto on_sensor =
        classify_wait_line("TEMPERATURE_WAIT SENSOR=\"temperature_sensor chamber\" MINIMUM=35");
    CHECK_FALSE(wait_names_chamber_heater(on_sensor, heater));
    // A sensor-only printer has no chamber heater to name.
    CHECK_FALSE(wait_names_chamber_heater(on_sensor, ""));
    // A probe coil, and anything that is not a TEMPERATURE_WAIT.
    CHECK_FALSE(wait_names_chamber_heater(
        classify_wait_line("TEMPERATURE_WAIT SENSOR=\"temperature_probe beacon_coil\" MINIMUM=40"),
        heater));
    CHECK_FALSE(wait_names_chamber_heater(classify_wait_line("M191 S60"), heater));
}

TEST_CASE("the chamber wait's temperature is M191's target or TEMPERATURE_WAIT's MINIMUM",
          "[chamber][rejection][wait_line]") {
    using helix::chamber_wait_target_c;
    using helix::classify_wait_line;
    CHECK(chamber_wait_target_c(classify_wait_line("M191 S60")) == Catch::Approx(60));
    CHECK(chamber_wait_target_c(classify_wait_line("M191 R45")) == 0);
    CHECK(chamber_wait_target_c(classify_wait_line("TEMPERATURE_WAIT SENSOR=c MINIMUM=35")) ==
          Catch::Approx(35));
    CHECK(chamber_wait_target_c(classify_wait_line("TEMPERATURE_WAIT SENSOR=c MAXIMUM=80")) == 0);
    CHECK(chamber_wait_target_c(classify_wait_line("M190 S100")) == 0);
    CHECK(chamber_wait_target_c(classify_wait_line("G28")) == 0);
}

TEST_CASE_METHOD(PromptFixture, "an unheated chamber-heater wait with nothing refused prompts",
                 "[chamber][rejection][prompt][prompt_ui]") {
    chamber_wait(true);
    CHECK(dialog() != nullptr);
}
