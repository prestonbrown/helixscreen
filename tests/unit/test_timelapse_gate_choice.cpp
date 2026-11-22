// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_print_gate_choice_modal.h"
#include "ui_print_preparation_manager.h"
#include "ui_print_start_controller.h"

#include "../test_helpers/loopback_http_server.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_start_grace.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/scoped_config_write_counter.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "print_start_inputs.h"
#include "printer_pending_restore.h"
#include "printer_state.h"
#include "timelapse_state.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

void drain() {
    helix::ui::UpdateQueue::instance().drain_until_empty(6);
}

void click(lv_obj_t* dialog, const char* name) {
    lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    drain();
}

struct Answers {
    int first = 0, second = 0, proceed = 0, cancel = 0;
};

helix::ui::PrintGateChoiceModal::Config config_for(Answers& a) {
    helix::ui::PrintGateChoiceModal::Config c;
    c.title = "No Timelapse Frames";
    c.message = "message";
    c.first_choice = "Use Hyperlapse";
    c.second_choice = "Off for This Print";
    c.proceed = "Print Anyway";
    c.on_first = [&a] { ++a.first; };
    c.on_second = [&a] { ++a.second; };
    c.on_proceed = [&a] { ++a.proceed; };
    c.on_cancel = [&a] { ++a.cancel; };
    return c;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the choice dialog: each close answers once, a programmatic one never",
                 "[timelapse][gate-choice]") {
    Answers a;
    lv_obj_t* dialog = helix::ui::PrintGateChoiceModal::show_owned(config_for(a));
    REQUIRE(dialog != nullptr);
    SECTION("the owner closing it runs nothing") {
        Modal::hide(dialog);
        drain();
        CHECK(a.cancel == 0);
    }
    SECTION("a backdrop tap cancels") {
        Modal::hide(dialog, ModalCloseReason::BackdropTap);
        drain();
        CHECK(a.cancel == 1);
    }
    SECTION("each button runs its own answer") {
        click(dialog, "btn_secondary");
        CHECK(a.second == 1);
        CHECK(a.cancel == 0);
    }
    CHECK(a.first + a.proceed == 0);
}

namespace {

class GateChoiceFixture : private helix::PrintSelectGlobalStateReset, public LVGLUITestFixture {
  public:
    GateChoiceFixture() : client(MoonrakerClientMock::PrinterType::VORON_24, 100.0) {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(client, get_printer_state());
        prep = std::make_unique<helix::ui::PrintPreparationManager>();
        prep->set_dependencies(api.get(), &get_printer_state());
        controller =
            std::make_unique<helix::ui::PrintStartController>(get_printer_state(), api.get());
        controller->set_prep_manager(prep.get());
        controller->set_on_print_started([this]() { ++started; });
        controller->set_on_print_cancelled([this]() { ++cancelled; });
        helix::test::wait_out_print_start_grace();
        helix::TimelapseState::instance().forget_mode_restore();

        // No file facts: a start without a detail view and without a scan
        // answer stops before any gate.
        controller->set_file("cube.gcode", "", {"#FF0000"});
        PrintStartControllerTestAccess::set_gates(
            *controller, {{"timelapse_frames", [](const helix::PrintStartContext&) {
                               helix::PrintStartContext ctx;
                               ctx.timelapse_on = true;
                               ctx.timelapse_layer_mode = true;
                               ctx.timelapse_frames = helix::timelapse::FrameCalls::Missing;
                               return helix::gate_timelapse_frames(ctx);
                           }}});
    }

    ~GateChoiceFixture() override {
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        helix::test::set_wire_state(ps, helix::PrintJobState::STANDBY);
        drain();
        controller.reset();
        prep.reset();
        api.reset();
        client.disconnect();
        helix::TimelapseState::instance().forget_mode_restore();
    }

    void answer_print_start(bool ok) {
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            client, "printer.print.start",
            [ok](MoonrakerClientMock*, const json&,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
                if (ok) {
                    success_cb(json{{"result", "ok"}});
                } else {
                    error_cb(MoonrakerError::unknown("refused", "printer.print.start"));
                }
                return true;
            });
    }

    lv_obj_t* open_dialog() {
        PrintStartControllerTestAccess::run_gates(*controller);
        drain();
        lv_obj_t* dialog = PrintStartControllerTestAccess::print_gate_modal(*controller);
        REQUIRE(dialog != nullptr);
        return dialog;
    }

    static std::string owed() {
        return helix::pending_restore::read(helix::timelapse::kModeRestoreKey);
    }

    ScopedConfigWriteCounter store;
    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<helix::ui::PrintPreparationManager> prep;
    std::unique_ptr<helix::ui::PrintStartController> controller;
    int started = 0;
    int cancelled = 0;
};

} // namespace

TEST_CASE_METHOD(GateChoiceFixture, "Off for This Print owes timelapse back before the start",
                 "[timelapse][gate-choice]") {
    answer_print_start(true);
    click(open_dialog(), "btn_secondary");
    drain();
    CHECK(started == 1);
    REQUIRE_FALSE(owed().empty());
    CHECK(helix::timelapse::decode_owed(owed()) == helix::timelapse::timelapse_off_switch());
}

TEST_CASE_METHOD(GateChoiceFixture, "Cancel starts nothing and owes nothing",
                 "[timelapse][gate-choice]") {
    click(open_dialog(), "btn_quaternary");
    CHECK(cancelled == 1);
    CHECK(started == 0);
    CHECK(owed().empty());
}

TEST_CASE_METHOD(GateChoiceFixture, "Use Hyperlapse changes nothing when the start is refused",
                 "[timelapse][gate-choice]") {
    answer_print_start(false);
    click(open_dialog(), "btn_primary");
    drain();
    CHECK(cancelled == 1);
    CHECK(owed().empty());
}

namespace {

/// moonraker-timelapse's settings endpoint, recording in Layer mode with head
/// parking on; writes are accepted and change nothing.
class TimelapsePluginServer : public helix::test::LoopbackHttpServer {
  public:
    TimelapsePluginServer() {
        service().GET("/machine/timelapse/settings", [](HttpRequest*, HttpResponse* resp) {
            resp->content_type = APPLICATION_JSON;
            resp->body = R"({"result":{"enabled":true,"mode":"layermacro","parkhead":true}})";
            return 200;
        });
        service().POST("/machine/timelapse/settings", [](HttpRequest*, HttpResponse* resp) {
            resp->content_type = APPLICATION_JSON;
            resp->body = R"({"result":{}})";
            return 200;
        });
        start();
    }
};

} // namespace

TEST_CASE_METHOD(GateChoiceFixture, "Use Hyperlapse switches the plugin once the start is accepted",
                 "[timelapse][gate-choice]") {
    TimelapsePluginServer plugin;
    REQUIRE(plugin.started());
    api->set_http_base_url(plugin.url(""));
    answer_print_start(true);
    click(open_dialog(), "btn_primary");

    REQUIRE(wait_until([] {
        drain();
        return !owed().empty();
    }));
    CHECK(started == 1);
    const std::vector<helix::timelapse::OwedSetting> want = {{"mode", "layermacro", "hyperlapse"},
                                                             {"parkhead", "True", "False"}};
    CHECK(helix::timelapse::decode_owed(owed()) == want);
}

TEST_CASE_METHOD(GateChoiceFixture, "Off for This Print owes nothing once the start is refused",
                 "[timelapse][gate-choice]") {
    TimelapsePluginServer plugin;
    REQUIRE(plugin.started());
    api->set_http_base_url(plugin.url(""));
    answer_print_start(false);
    click(open_dialog(), "btn_secondary");
    REQUIRE(cancelled == 1);

    // The flag was owed before the start went out; the refusal settles it at
    // once rather than at a print end that never comes.
    CHECK(wait_until([] {
        drain();
        return owed().empty();
    }));
}

TEST_CASE_METHOD(GateChoiceFixture, "a timelapse answer from a cancelled start is not reused",
                 "[timelapse][gate-choice]") {
    // The timelapse dialog, then one the user cancels.
    PrintStartControllerTestAccess::set_gates(
        *controller, {{"timelapse_frames",
                       [](const helix::PrintStartContext&) {
                           helix::PrintStartContext ctx;
                           ctx.timelapse_on = true;
                           ctx.timelapse_layer_mode = true;
                           ctx.timelapse_frames = helix::timelapse::FrameCalls::Missing;
                           return helix::gate_timelapse_frames(ctx);
                       }},
                      {"second", [](const helix::PrintStartContext&) {
                           helix::PrintStartContext ctx;
                           ctx.timelapse_on = true;
                           ctx.timelapse_layer_mode = true;
                           ctx.timelapse_frames = helix::timelapse::FrameCalls::Missing;
                           return helix::gate_timelapse_frames(ctx);
                       }}});
    click(open_dialog(), "btn_secondary");
    drain();
    process_lvgl(20);
    drain();
    REQUIRE(PrintStartControllerTestAccess::gate_resume_index(*controller) == 1);
    PrintStartControllerTestAccess::gate_cancel(*controller);
    drain();
    REQUIRE(started == 0);
    REQUIRE(owed().empty());

    // The next start asks nothing and prints with timelapse as configured.
    PrintStartControllerTestAccess::set_gates(*controller, {});
    answer_print_start(true);
    controller->initiate();
    drain();
    REQUIRE(started == 1);
    CHECK(owed().empty());
}

TEST_CASE_METHOD(GateChoiceFixture, "the gates read the timelapse option and the plugin's mode",
                 "[timelapse][gate-choice]") {
    prep->set_option_state_provider(
        [](const std::string& id) { return id == "timelapse" ? 1 : -1; });
    helix::TimelapseState::instance().set_mode("layermacro");
    const auto ctx = PrintStartControllerTestAccess::gather(*controller);
    helix::TimelapseState::instance().set_mode("");
    CHECK(ctx.timelapse_on);
    CHECK(ctx.timelapse_layer_mode);

    prep->set_option_state_provider([](const std::string&) { return 0; });
    CHECK_FALSE(PrintStartControllerTestAccess::gather(*controller).timelapse_on);
}
