// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pre_start_timeout_gate.cpp
 * @brief The pre-start RPC timeout must not fail a print the printer is still executing.
 *
 * execute_gcode blocks until the macro finishes. A long pre-start macro
 * (Creality's BED_MESH_CALIBRATE_START_PRINT chain: home, wipe, soak, mesh —
 * ~10-19 min measured on a K2 Plus) can outlive the RPC ceiling even at
 * PRE_START_MACRO_TIMEOUT_MS. When that happens and Klipper still reports
 * idle_timeout "Printing", the app must wait for the busy->idle edge and start
 * the print then, not abort with "Pre-print command failed" while the printer
 * is mid-mesh.
 */

#include "ui_print_preparation_manager.h"
#include "ui_update_queue.h"

#include "lvgl_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "moonraker_job_api.h"
#include "print_job_ref.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include "../catch_amalgamated.hpp"

// Minimal local twin of the accessor in test_print_preparation_manager.cpp —
// the manager declares `friend class ::PrintPreparationManagerTestAccess`, so
// any class with this exact name reaches its privates.
class PrintPreparationManagerTestAccess {
  public:
    static lv_timer_t* pending_wait_timer(const helix::ui::PrintPreparationManager& m);
};

namespace {

/// Records start_print calls and succeeds synchronously — the wait gate's
/// continuation must reach job().start_print exactly once, with the job file.
struct RecordingJobAPI : public MoonrakerJobAPI {
    explicit RecordingJobAPI(helix::IMoonrakerClient& client) : MoonrakerJobAPI(client) {}

    int start_print_calls = 0;
    std::string last_filename;

    void start_print(const std::string& filename, IMoonrakerAPI::SuccessCallback on_success,
                     IMoonrakerAPI::ErrorCallback /*on_error*/) override {
        ++start_print_calls;
        last_filename = filename;
        if (on_success)
            on_success();
    }
};

/// Intercepts the pre-start block so the test controls when the RPC answers
/// (or times out). Everything else behaves like the stock mock API.
struct PreStartGateAPI : public MoonrakerAPIMock {
    PreStartGateAPI(helix::MoonrakerClient& client, helix::PrinterState& state,
                    RecordingJobAPI& jobs)
        : MoonrakerAPIMock(client, state), jobs_(jobs) {}

    void execute_gcode(const std::string& gcode, IMoonrakerAPI::SuccessCallback on_success,
                       IMoonrakerAPI::ErrorCallback on_error, uint32_t timeout_ms = 0,
                       bool /*silent*/ = false, IMoonrakerAPI::SuccessCallback = nullptr,
                       bool /*caller_surfaces_errors*/ = true) override {
        captured_gcode = gcode;
        captured_success = std::move(on_success);
        captured_error = std::move(on_error);
        captured_timeout = timeout_ms;
    }

    MoonrakerJobAPI& job() override {
        return jobs_;
    }

    RecordingJobAPI& jobs_;
    std::string captured_gcode;
    IMoonrakerAPI::SuccessCallback captured_success;
    IMoonrakerAPI::ErrorCallback captured_error;
    uint32_t captured_timeout = 0;
};

} // namespace

lv_timer_t*
PrintPreparationManagerTestAccess::pending_wait_timer(const helix::ui::PrintPreparationManager& m) {
    return m.pre_start_wait_guard_.pending_timer();
}

class PreStartGateFixture : public LVGLTestFixture {
  public:
    PreStartGateFixture()
        : mock_client(MoonrakerClientMock::PrinterType::VORON_24), jobs(mock_client) {
        state.init_subjects(false);
        state.set_printer_type_sync("Creality K2 Plus");
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<PreStartGateAPI>(mock_client, state, jobs);

        manager.set_dependencies(api.get(), &state);
        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 1 : -1; });
    }

    ~PreStartGateFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(60);
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_busy(bool busy) {
        lv_subject_set_int(state.get_idle_timeout_printing_subject(), busy ? 1 : 0);
    }

    MoonrakerClientMock mock_client;
    RecordingJobAPI jobs;
    helix::PrinterState state;
    std::unique_ptr<PreStartGateAPI> api;
    helix::ui::PrintPreparationManager manager;

    bool completed = false;
    bool completion_success = false;
    std::string completion_error;

    helix::ui::PrintCompletionCallback completion() {
        return [this](bool ok, const std::string& err) {
            completed = true;
            completion_success = ok;
            completion_error = err;
        };
    }
};

TEST_CASE_METHOD(PreStartGateFixture,
                 "Pre-start RPC timeout with printer still busy waits for the busy->idle edge",
                 "[print_preparation][pre_start][timeout]") {
    set_busy(true);

    manager.start_print("part.gcode", "", []() {}, completion());
    drain();

    // The pre-start block went through execute_gcode with the long ceiling.
    REQUIRE(api->captured_error);
    REQUIRE(api->captured_gcode.find("BED_MESH_CALIBRATE_START_PRINT") != std::string::npos);
    CHECK(api->captured_timeout == IMoonrakerAPI::PRE_START_MACRO_TIMEOUT_MS);

    // RPC ceiling hit — but Klipper is still executing the macro.
    api->captured_error(
        MoonrakerError::timeout("printer.gcode.script", IMoonrakerAPI::PRE_START_MACRO_TIMEOUT_MS));
    drain();

    CHECK_FALSE(completed);
    CHECK(jobs.start_print_calls == 0);

    // Macro finishes: busy -> idle. The print starts now, and only now.
    set_busy(false);
    drain();

    CHECK(completed);
    CHECK(completion_success);
    CHECK(jobs.start_print_calls == 1);
    CHECK(jobs.last_filename == "part.gcode");
}

TEST_CASE_METHOD(PreStartGateFixture,
                 "Pre-start RPC timeout with an idle printer fails the print start",
                 "[print_preparation][pre_start][timeout]") {
    set_busy(false);

    manager.start_print("part.gcode", "", []() {}, completion());
    drain();
    REQUIRE(api->captured_error);

    api->captured_error(
        MoonrakerError::timeout("printer.gcode.script", IMoonrakerAPI::PRE_START_MACRO_TIMEOUT_MS));
    drain();

    CHECK(completed);
    CHECK_FALSE(completion_success);
    CHECK(jobs.start_print_calls == 0);
}

TEST_CASE_METHOD(PreStartGateFixture,
                 "A non-timeout pre-start error fails immediately even while busy",
                 "[print_preparation][pre_start][timeout]") {
    set_busy(true);

    manager.start_print("part.gcode", "", []() {}, completion());
    drain();
    REQUIRE(api->captured_error);

    MoonrakerError rejected =
        MoonrakerError::json_rpc_error("printer.gcode.script", "Macro failed");
    api->captured_error(rejected);
    drain();

    CHECK(completed);
    CHECK_FALSE(completion_success);
    CHECK(jobs.start_print_calls == 0);
}

TEST_CASE_METHOD(PreStartGateFixture,
                 "The wait backstop fails the print if the printer never goes idle",
                 "[print_preparation][pre_start][timeout]") {
    set_busy(true);

    manager.start_print("part.gcode", "", []() {}, completion());
    drain();
    REQUIRE(api->captured_error);

    api->captured_error(
        MoonrakerError::timeout("printer.gcode.script", IMoonrakerAPI::PRE_START_MACRO_TIMEOUT_MS));
    drain();
    REQUIRE_FALSE(completed);

    // Force the backstop timer to fire while still busy.
    lv_timer_t* backstop = PrintPreparationManagerTestAccess::pending_wait_timer(manager);
    REQUIRE(backstop);
    lv_timer_set_period(backstop, 1);
    lv_timer_ready(backstop);
    drain();

    CHECK(completed);
    CHECK_FALSE(completion_success);
    CHECK(jobs.start_print_calls == 0);
}

TEST_CASE_METHOD(PreStartGateFixture,
                 "Cancelling during a blocking pre-start block abandons the queued start",
                 "[print_preparation][pre_start][preparing]") {
    // A host-side pre-start block can run for ten minutes. If the user cancels
    // during it, the queued start must not fire when the macro finally returns —
    // starting a print the user already cancelled is the worst outcome here.
    // Cancel is expressed by retiring the preparing job, which is the same token
    // the start was armed with.
    state.begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    set_busy(true);

    manager.start_print("part.gcode", "", []() {}, completion());
    drain();
    REQUIRE(api->captured_error);

    // User cancels while the macro is still running.
    state.retire_preparing(helix::PreparingExit::Cancelled);

    // Macro finishes and the printer goes idle.
    api->captured_error(
        MoonrakerError::timeout("printer.gcode.script", IMoonrakerAPI::PRE_START_MACRO_TIMEOUT_MS));
    drain();
    set_busy(false);
    drain();

    CHECK(jobs.start_print_calls == 0);
    CHECK_FALSE(completion_success);
}

TEST_CASE_METHOD(PreStartGateFixture, "A start with no preparing job still runs",
                 "[print_preparation][pre_start][preparing]") {
    // Guard must not punish a caller that never armed a job — otherwise every
    // path is forced to arm before it can print.
    set_busy(false);

    manager.start_print("part.gcode", "", []() {}, completion());
    drain();
    REQUIRE(api->captured_success);
    api->captured_success();
    drain();

    CHECK(jobs.start_print_calls == 1);
}
