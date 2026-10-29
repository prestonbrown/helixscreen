// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cancel_sent_start.cpp
 * @brief Stop during preparing, after the start was sent to the printer (#1890).
 *
 * A firmware that runs its print-start sequence inside SDCARD_PRINT_FILE keeps
 * print_stats idle while it homes, heats and probes. The job is already on the
 * printer, so withdrawing it locally stops nothing: the cancel has to go to the
 * printer, only once the start has answered OK, and the job is not cancelled
 * until the printer says so. The state table is in
 * docs/devel/PRINT_CONTROL_BUTTONS.md.
 */

#include "ui_test_utils.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_control_buttons_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "print_control_buttons.h"
#include "print_job_ref.h"
#include "printer_state.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::PrintControlButtons;
using helix::ui::PrintControlButtonsTestAccess;
using json = nlohmann::json;

namespace {

struct SentStartFixture : LVGLTestFixture {
    SentStartFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        PrintControlButtonsTestAccess::reset();
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        PrintControlButtons::instance().init_subjects();

        client.connect("ws://mock/websocket", []() {}, []() {});
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            client, "printer.print.cancel",
            [this](MoonrakerClientMock*, const json&, helix::SharedCallback<void(const json&)> ok,
                   helix::SharedCallback<void(const MoonrakerError&)> err) -> bool {
                ++cancels;
                if (refuse_unsent) {
                    // What a client that is not connected does: refuse before
                    // sending, calling the error back at once.
                    err(MoonrakerError::connection_lost("printer.print.cancel"));
                    return true;
                }
                cancel_ok = std::move(ok);
                cancel_err = std::move(err);
                return true;
            });
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
        PrintControlButtons::instance().set_api(api.get());
    }

    ~SentStartFixture() override {
        helix::ui::set_test_notification_warning_hook(nullptr);
        PrintControlButtons::instance().set_api(nullptr);
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    static helix::PrinterPrintState& ps() {
        return get_printer_state().print_state();
    }

    /// Drive print_stats through the real parse, as Moonraker reports it.
    static void report(const std::string& job_state, const std::string& filename) {
        get_printer_state().update_from_status(
            json{{"print_stats", {{"state", job_state}, {"filename", filename}}}});
        drain();
    }

    /// Arm a job whose start is in flight, and return its epoch.
    static int arm_in_flight(const char* file = "part.gcode") {
        ps().begin_preparing(helix::PrintJobRef{file, "", ""});
        ps().note_preparing_start_sent();
        drain();
        return lv_subject_get_int(ps().get_preparing_epoch_subject());
    }

    /// The start answered for @p epoch.
    static void start_answers(int epoch, bool accepted) {
        ps().note_preparing_start_answered(epoch, accepted);
        drain();
    }

    static void stop() {
        PrintControlButtonsTestAccess::confirm_stop();
        drain();
    }

    static int stop_enabled() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "print_control_stop_enabled"));
    }

    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPIMock> api;
    int cancels = 0;
    bool refuse_unsent = false;
    std::function<void(const json&)> cancel_ok;
    std::function<void(const MoonrakerError&)> cancel_err;
};

} // namespace

TEST_CASE_METHOD(SentStartFixture, "Stop before the start is sent withdraws it locally",
                 "[print_control][preparing_cancel]") {
    ps().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    drain();

    stop();

    CHECK(cancels == 0);
    CHECK_FALSE(ps().has_preparing_job());
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::Cancelled);
}

TEST_CASE_METHOD(SentStartFixture,
                 "Stop while the start is in flight cancels only once the start answers",
                 "[print_control][preparing_cancel]") {
    const int epoch = arm_in_flight();
    stop();

    // A cancel sent now could reach an idle printer before the start does.
    CHECK(cancels == 0);
    CHECK(ps().has_preparing_job());
    CHECK(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(stop_enabled() == 0);

    start_answers(epoch, true);
    REQUIRE(cancels == 1);
    CHECK(ps().has_preparing_job()); // not cancelled until the printer says so

    // The cancel's answer alone is not the printer's report.
    REQUIRE(cancel_ok);
    report("printing", "part.gcode");
    cancel_ok(json::object());
    drain();
    CHECK(PrintControlButtonsTestAccess::cancel_requested());

    report("cancelled", "part.gcode");
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::Confirmed);
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_confirm_armed());
}

TEST_CASE_METHOD(SentStartFixture, "Stop after the start answered cancels at once",
                 "[print_control][preparing_cancel]") {
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    CHECK(cancels == 1);
    CHECK(stop_enabled() == 0);

    // The printer ends our job before ever reporting it printing: the
    // completion observer reports it, the preparing exit says nothing.
    report("cancelled", "part.gcode");
    CHECK_FALSE(ps().has_preparing_job());
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::EndedByPrinter);
    CHECK(ps().get_print_lifecycle() == PrintState::Cancelled);
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture, "A refused start withdraws a job whose cancel was waiting",
                 "[print_control][preparing_cancel]") {
    const int epoch = arm_in_flight();
    stop();
    start_answers(epoch + 1, false); // another job's answer
    CHECK(PrintControlButtonsTestAccess::cancel_requested());

    start_answers(epoch, false);
    CHECK(cancels == 0);
    CHECK_FALSE(ps().has_preparing_job());
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::Cancelled);
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture,
                 "A start that loses its answer while a cancel waits is held and cancelled",
                 "[print_control][preparing_cancel]") {
    // A timeout means Klipper is busy running the start, so the cancel queues
    // behind it. Failing the job instead would drop the cancel and print.
    const int epoch = arm_in_flight();
    stop();
    // What every start site does with a lost answer.
    REQUIRE(ps().hold_unanswered_start(epoch, MoonrakerError::timeout("printer.print.start", 1)));
    drain();

    CHECK(cancels == 1);
    CHECK(ps().has_preparing_job());
    CHECK(ps().preparing_start() == helix::PreparingStart::Unanswered);
    CHECK(PrintControlButtonsTestAccess::cancel_requested());

    report("cancelled", "part.gcode");
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::EndedByPrinter);
}

TEST_CASE_METHOD(SentStartFixture, "Only a lost answer holds the preparing job's start",
                 "[print_control][preparing_cancel][1898]") {
    // A refusal is the printer's answer: nothing runs, the caller fails it.
    const int epoch = arm_in_flight();
    CHECK_FALSE(ps().hold_unanswered_start(
        epoch, MoonrakerError::validation_error("printer.print.start", "Spools")));
    CHECK(ps().preparing_start() == helix::PreparingStart::InFlight);
    CHECK_FALSE(ps().hold_unanswered_start(0, MoonrakerError::timeout("printer.print.start", 1)));
    CHECK_FALSE(
        ps().hold_unanswered_start(epoch + 1, MoonrakerError::timeout("printer.print.start", 1)));
    CHECK(ps().preparing_start() == helix::PreparingStart::InFlight);
}

TEST_CASE_METHOD(SentStartFixture,
                 "A start that outlasts its RPC stays preparing with Stop able to cancel it",
                 "[print_control][preparing_cancel][1898]") {
    // The start sequence runs inside SDCARD_PRINT_FILE and outlives the RPC
    // ceiling: the printer is still working on the job.
    const int epoch = arm_in_flight();
    REQUIRE(
        ps().hold_unanswered_start(epoch, MoonrakerError::connection_lost("printer.print.start")));
    drain();
    CHECK(ps().has_preparing_job());
    CHECK(ps().preparing_start() == helix::PreparingStart::Unanswered);
    CHECK(cancels == 0);

    const bool stop_first = GENERATE(true, false);
    CAPTURE(stop_first);
    if (stop_first) {
        // The cancel queues behind the start the printer is running.
        stop();
        CHECK(cancels == 1);
        report("cancelled", "part.gcode");
        CHECK(ps().last_preparing_exit() == helix::PreparingExit::EndedByPrinter);
        return;
    }
    report("printing", "part.gcode");
    CHECK_FALSE(ps().has_preparing_job());
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::Confirmed);
}

TEST_CASE_METHOD(SentStartFixture, "A cancel requested on one job does not carry to the next one",
                 "[print_control][preparing_cancel]") {
    ps().begin_preparing(helix::PrintJobRef{"a.gcode", "", ""});
    ps().note_preparing_cancel_requested();
    REQUIRE(ps().preparing_cancel_requested());

    // Another job replaces it without a retirement in between.
    ps().begin_preparing(helix::PrintJobRef{"b.gcode", "", ""});
    ps().note_preparing_start_sent();
    const int epoch = lv_subject_get_int(ps().get_preparing_epoch_subject());
    CHECK_FALSE(ps().preparing_cancel_requested());
    CHECK(epoch != 0);
    CHECK(ps().preparing_start() == helix::PreparingStart::InFlight);
}

TEST_CASE_METHOD(SentStartFixture, "A start that fails outright while a cancel waits sends none",
                 "[print_control][preparing_cancel]") {
    arm_in_flight();
    stop();
    ps().retire_preparing(helix::PreparingExit::Failed);
    drain();

    CHECK(cancels == 0);
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture, "A cancel the client refused before sending gives Stop back",
                 "[print_control][preparing_cancel]") {
    std::vector<std::string> errors;
    helix::ui::set_test_notification_error_hook(
        [&errors](const std::string& m) { errors.push_back(m); });
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    refuse_unsent = true;
    stop();
    helix::ui::set_test_notification_error_hook(nullptr);

    CHECK(cancels == 1);
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(stop_enabled() == 1);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Not connected") != std::string::npos);
}

TEST_CASE_METHOD(SentStartFixture,
                 "A cancel whose answer was lost gives Stop back once the printer keeps printing",
                 "[print_control][preparing_cancel]") {
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    // Still waiting on the answer: printing is the cancel queued behind a line.
    report("printing", "part.gcode");
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_confirm_armed());

    REQUIRE(cancel_err);
    cancel_err(MoonrakerError::timeout("printer.print.cancel", 300000));
    drain();
    REQUIRE(PrintControlButtonsTestAccess::cancel_confirm_armed());
    PrintControlButtonsTestAccess::fire_cancel_confirm();
    drain();
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture, "A cancel never outlives its job into the next one",
                 "[print_control][preparing_cancel]") {
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    REQUIRE(PrintControlButtonsTestAccess::cancel_requested());

    // Our job ended without the printer ever reporting a transition.
    ps().retire_preparing(helix::PreparingExit::Confirmed);
    drain();
    arm_in_flight("next.gcode");
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture,
                 "The printer reporting the job sends a waiting cancel before the start answers",
                 "[print_control][preparing_cancel]") {
    arm_in_flight();
    stop();
    report("printing", "part.gcode"); // retired Confirmed: the printer has the start
    CHECK(cancels == 1);
    CHECK(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture, "A refused cancel leaves the job and Stop live",
                 "[print_control][preparing_cancel]") {
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    REQUIRE(cancel_err);
    cancel_err(MoonrakerError::validation_error("printer.print.cancel", "Klippy not ready"));
    drain();

    CHECK(ps().has_preparing_job());
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(stop_enabled() == 1);
}

TEST_CASE_METHOD(SentStartFixture,
                 "An unanswered cancel waits on the printer through printing, without a warning",
                 "[print_control][preparing_cancel]") {
    // A print-start macro can outlast any RPC ceiling, so a timeout or a lost
    // connection says nothing about whether the cancel is still queued.
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& m) { warnings.push_back(m); });
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    REQUIRE(cancel_err);
    const bool timed_out = GENERATE(true, false);
    CAPTURE(timed_out);
    cancel_err(timed_out ? MoonrakerError::timeout("printer.print.cancel", 300000)
                         : MoonrakerError::connection_lost("printer.print.cancel"));
    drain();
    CHECK(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(stop_enabled() == 0);

    // The file loading, then the cancel macro running, on the way to the end.
    report("standby", "part.gcode");
    report("printing", "part.gcode");
    CHECK(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(PrintControlButtonsTestAccess::cancel_confirm_armed());

    report("cancelled", "part.gcode");
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_confirm_armed());
    CHECK(warnings.empty());
}

TEST_CASE_METHOD(SentStartFixture,
                 "A cancel answered while the printer already reads cancelled settles at once",
                 "[print_control][preparing_cancel]") {
    // The previous print ended cancelled, and ours went cancelled again inside
    // one status tick: no transition will ever report it.
    report("cancelled", "old.gcode");
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    REQUIRE(cancel_ok);
    cancel_ok(json::object());
    drain();

    CHECK_FALSE(ps().has_preparing_job());
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::EndedByPrinter);
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture,
                 "A printer still printing after the cancel answered is judged after a grace",
                 "[print_control][preparing_cancel]") {
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& m) { warnings.push_back(m); });
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    report("printing", "part.gcode");
    REQUIRE(cancel_ok);
    cancel_ok(json::object());
    drain();

    // A frame from before the cancel can land after its answer.
    CHECK(PrintControlButtonsTestAccess::cancel_requested());
    REQUIRE(PrintControlButtonsTestAccess::cancel_confirm_armed());
    CHECK(warnings.empty());

    PrintControlButtonsTestAccess::fire_cancel_confirm();
    drain();
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(warnings.size() == 1);
}

TEST_CASE_METHOD(SentStartFixture, "A cancel settled after a newer job began leaves that job alone",
                 "[print_control][preparing_cancel]") {
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    REQUIRE(cancel_ok);

    ps().retire_preparing(helix::PreparingExit::Confirmed);
    ps().begin_preparing(helix::PrintJobRef{"next.gcode", "", ""});
    drain();
    report("cancelled", "part.gcode");

    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(ps().has_preparing_job());
    CHECK(ps().preparing_job().filename == "next.gcode");
}

TEST_CASE_METHOD(SentStartFixture, "Swapping the API or tearing down drops a pending cancel",
                 "[print_control][preparing_cancel]") {
    const int epoch = arm_in_flight();
    start_answers(epoch, true);
    stop();
    REQUIRE(PrintControlButtonsTestAccess::cancel_requested());

    const bool swap = GENERATE(true, false);
    CAPTURE(swap);
    if (swap) {
        MoonrakerClientMock other(MoonrakerClientMock::PrinterType::VORON_24);
        MoonrakerAPIMock other_api(other, get_printer_state());
        PrintControlButtons::instance().set_api(&other_api);
        CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
        PrintControlButtons::instance().set_api(api.get());
    } else {
        PrintControlButtonsTestAccess::teardown_subjects();
        CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
        PrintControlButtons::instance().init_subjects();
    }
    // The same object again is not a swap.
    stop();
    PrintControlButtons::instance().set_api(api.get());
    CHECK(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture, "The start hand-over belongs to one preparing job",
                 "[print_control][preparing_cancel]") {
    using helix::PreparingStart;
    // Nothing to mark without a job.
    ps().note_preparing_start_sent();
    CHECK(ps().preparing_start() == PreparingStart::None);

    const int epoch = arm_in_flight();
    CHECK(ps().preparing_start() == PreparingStart::InFlight);
    ps().note_preparing_start_answered(epoch + 1, true);
    CHECK(ps().preparing_start() == PreparingStart::InFlight);
    ps().note_preparing_start_answered(epoch, true);
    CHECK(ps().preparing_start() == PreparingStart::Accepted);
    CHECK(lv_subject_get_int(ps().get_preparing_start_subject()) ==
          static_cast<int>(PreparingStart::Accepted));

    ps().retire_preparing(helix::PreparingExit::Cancelled);
    CHECK(ps().preparing_start() == PreparingStart::None);

    ps().begin_preparing(helix::PrintJobRef{"next.gcode", "", ""});
    CHECK(ps().preparing_start() == PreparingStart::None);
}
