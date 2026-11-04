// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_controller_no_detail.cpp
 * @brief PrintStartController driving a start with no file view.
 *
 * Run with: ./build/bin/helix-tests "[print_start_controller]"
 *
 * With set_prep_manager() and set_file_facts() the controller is ready
 * without a detail view, its gates read the facts, and every way a start
 * ends without a print reaches the cancelled callback exactly once.
 */

#include "ui_print_preparation_manager.h"
#include "ui_print_start_controller.h"

#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_start_grace.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/running_op_backend.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "print_start_inputs.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::test::set_wire_state;

namespace {

class NoDetailFixture : private helix::PrintSelectGlobalStateReset, public LVGLUITestFixture {
  public:
    NoDetailFixture() : client(MoonrakerClientMock::PrinterType::VORON_24, 100.0) {
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
    }

    ~NoDetailFixture() override {
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        drain();
        controller.reset();
        prep.reset();
        api.reset();
        client.disconnect();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty(6);
    }

    /// printer.print.start answers with @p ok.
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

    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<helix::ui::PrintPreparationManager> prep;
    std::unique_ptr<helix::ui::PrintStartController> controller;
    int started = 0;
    int cancelled = 0;
};

helix::FileStartFacts facts_for(const std::string& filename) {
    helix::FileStartFacts facts;
    facts.filename = filename;
    facts.palette.colors = {"#FF0000"};
    facts.palette.materials = {"PLA"};
    facts.tools_used = {0};
    facts.tool_grams = {3.5};
    return facts;
}

} // namespace

TEST_CASE_METHOD(NoDetailFixture, "a controller with a prep manager and facts needs no detail view",
                 "[print_start_controller]") {
    CHECK_FALSE(controller->is_ready()); // no file yet
    controller->set_file_facts(facts_for("cube.gcode"));
    controller->set_file("cube.gcode", "", {"#FF0000"});
    CHECK(controller->is_ready());

    const auto ctx = PrintStartControllerTestAccess::gather(*controller);
    CHECK(ctx.has_file_facts);
    CHECK(ctx.tools_used == std::set<int>{0});
    CHECK(ctx.tool_grams == std::vector<double>{3.5});
    CHECK(ctx.filament_materials == std::vector<std::string>{"PLA"});

    helix::PrintStartContext expected;
    helix::fill_file_context(expected, facts_for("cube.gcode"));
    CHECK(ctx.mappings.size() == expected.mappings.size());
    CHECK(ctx.effective_remap == expected.effective_remap);

    SECTION("without facts the file half stays empty") {
        helix::ui::PrintStartController bare(get_printer_state(), api.get());
        bare.set_prep_manager(prep.get());
        bare.set_file("cube.gcode", "", {});
        CHECK(bare.is_ready());
        CHECK_FALSE(PrintStartControllerTestAccess::gather(bare).has_file_facts);
    }
}

TEST_CASE_METHOD(NoDetailFixture, "a busy printer ends the start through the cancelled callback",
                 "[print_start_controller]") {
    controller->set_file_facts(facts_for("cube.gcode"));
    controller->set_file("cube.gcode", "", {"#FF0000"});
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();

    controller->initiate();
    drain();
    CHECK(cancelled == 1);
    CHECK(started == 0);
}

TEST_CASE_METHOD(NoDetailFixture, "a start_print answer decides started or cancelled",
                 "[print_start_controller]") {
    controller->set_file_facts(facts_for("cube.gcode"));
    controller->set_file("cube.gcode", "", {"#FF0000"});

    SECTION("a failed start is cancelled once") {
        answer_print_start(false);
        PrintStartControllerTestAccess::execute(*controller);
        drain();
        CHECK(cancelled == 1);
        CHECK(started == 0);
    }
    SECTION("a confirmed start is started only") {
        answer_print_start(true);
        PrintStartControllerTestAccess::execute(*controller);
        drain();
        CHECK(started == 1);
        CHECK(cancelled == 0);
    }
}

TEST_CASE_METHOD(NoDetailFixture, "a missing prep manager ends the start through cancelled",
                 "[print_start_controller]") {
    helix::ui::PrintStartController bare(get_printer_state(), api.get());
    int bare_cancelled = 0;
    bare.set_on_print_cancelled([&bare_cancelled]() { ++bare_cancelled; });
    bare.set_file("cube.gcode", "", {});
    PrintStartControllerTestAccess::execute(bare);
    drain();
    CHECK(bare_cancelled == 1);
}

TEST_CASE_METHOD(NoDetailFixture, "a filament operation under way refuses the start",
                 "[print_start_controller]") {
    controller->set_file_facts(facts_for("cube.gcode"));
    controller->set_file("cube.gcode", "", {"#FF0000"});
    auto unloading = std::make_unique<helix::RunningOpBackend>();
    unloading->action = helix::AmsAction::UNLOADING;
    unloading->running = helix::StepOperationType::UNLOAD;
    helix::AmsState::instance().set_backend(std::move(unloading));
    struct Unset {
        ~Unset() {
            helix::AmsState::instance().set_backend(nullptr);
        }
    } unset;

    // The refusal is what says so; a start that went on would end for some
    // other reason with some other notice.
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& m) { warnings.push_back(m); });
    struct Unhook {
        ~Unhook() {
            helix::ui::set_test_notification_warning_hook(nullptr);
        }
    } unhook;

    controller->initiate();
    drain();
    CHECK(cancelled == 1);
    CHECK(started == 0);
    CHECK(std::count(warnings.begin(), warnings.end(),
                     "Wait for the current filament operation to finish") == 1);

    bool reprint_failed = false;
    controller->initiate_reprint("cube.gcode", "", {0}, nullptr,
                                 [&reprint_failed]() { reprint_failed = true; });
    drain();
    CHECK(reprint_failed);
}
