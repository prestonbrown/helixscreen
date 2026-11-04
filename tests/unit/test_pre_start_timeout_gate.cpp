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
#include "ui_print_start_controller.h"
#include "ui_test_utils.h"
#include "ui_update_queue.h"

#include "lvgl_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "moonraker_job_api.h"
#include "moonraker_request_tracker.h"
#include "print_job_ref.h"
#include "printer_state.h"
#include "test_helpers/moonraker_client_mock_test_access.h"
#include "test_helpers/printer_state_test_access.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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
    int start_modified_print_calls = 0;
    std::string last_filename;
    /// Answer start_print and start_modified_print with this instead of success.
    std::optional<MoonrakerError> fail_with;

    void start_print(const std::string& filename, IMoonrakerAPI::SuccessCallback on_success,
                     IMoonrakerAPI::ErrorCallback on_error) override {
        ++start_print_calls;
        last_filename = filename;
        if (fail_with) {
            if (on_error)
                on_error(*fail_with);
            return;
        }
        if (on_success)
            on_success();
    }

    /// Send start_modified_print through MoonrakerJobAPI's own implementation,
    /// so the client's scripted answer and the plugin fallback both run.
    bool real_modified_print = false;

    void start_modified_print(const std::string& original_filename,
                              const std::string& temp_file_path,
                              const std::vector<std::string>& modifications,
                              ModifiedPrintCallback on_success,
                              IMoonrakerAPI::ErrorCallback on_error) override {
        ++start_modified_print_calls;
        if (real_modified_print) {
            MoonrakerJobAPI::start_modified_print(original_filename, temp_file_path, modifications,
                                                  std::move(on_success), std::move(on_error));
            return;
        }
        last_filename = original_filename;
        if (fail_with) {
            if (on_error)
                on_error(*fail_with);
            return;
        }
        if (on_success) {
            ModifiedPrintResult result;
            result.original_filename = original_filename;
            result.print_filename = temp_file_path;
            on_success(result);
        }
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

TEST_CASE_METHOD(PreStartGateFixture, "A start sent to the printer marks the preparing job",
                 "[print_preparation][preparing_cancel]") {
    // From the send on, withdrawing the job locally stops nothing, so Stop
    // must know to cancel on the printer instead (#1890).
    state.print_state().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    set_busy(false);

    manager.start_print("part.gcode", "", []() {}, completion());
    REQUIRE(api->captured_success);
    // The pre-start block is still running.
    CHECK(state.print_state().preparing_start() == helix::PreparingStart::None);
    api->captured_success();
    drain();

    REQUIRE(jobs.start_print_calls == 1);
    CHECK(state.print_state().has_preparing_job());
    CHECK(state.print_state().preparing_start() == helix::PreparingStart::Accepted);
}

TEST_CASE_METHOD(PreStartGateFixture, "A refused start fails; a lost one stays preparing",
                 "[print_preparation][preparing_cancel][1898]") {
    // A refusal never reached the printer, so the start fails and Stop must
    // not cancel there. A timeout says nothing about whether it did: a start
    // sequence run inside SDCARD_PRINT_FILE can outlast the RPC ceiling.
    const bool refused = GENERATE(true, false);
    CAPTURE(refused);
    jobs.fail_with = refused ? MoonrakerError::validation_error("printer.print.start", "Spools")
                             : MoonrakerError::timeout("printer.print.start", 600000);
    state.print_state().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    set_busy(false);

    manager.start_print("part.gcode", "", []() {}, completion());
    REQUIRE(api->captured_success);
    api->captured_success();
    drain();

    REQUIRE(jobs.start_print_calls == 1);
    CHECK(state.print_state().preparing_start() ==
          (refused ? helix::PreparingStart::None : helix::PreparingStart::Unanswered));
    CHECK(completed == refused);
    if (refused) {
        CHECK_FALSE(completion_success);
        CHECK_FALSE(completion_error.empty()); // what the controller retires on
    }
}

TEST_CASE_METHOD(PreStartGateFixture,
                 "A start that times out stays preparing until the printer reports it",
                 "[print_preparation][preparing_cancel][1898]") {
    const bool stop_waiting = GENERATE(true, false);
    CAPTURE(stop_waiting);
    jobs.fail_with = MoonrakerError::timeout("printer.print.start", 600000);
    state.print_state().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    set_busy(false);

    manager.start_print("part.gcode", "", []() {}, completion());
    REQUIRE(api->captured_success);
    if (stop_waiting) {
        state.print_state().note_preparing_cancel_requested(); // Stop while it runs
    }
    api->captured_success();
    drain();

    REQUIRE(jobs.start_print_calls == 1);
    CHECK(state.print_state().has_preparing_job());
    CHECK(state.print_state().preparing_start() == helix::PreparingStart::Unanswered);
    CHECK_FALSE(completed); // no failure reported for a start the printer may be running

    // The printer finishes its start sequence and reports the job.
    state.update_from_status(
        nlohmann::json{{"print_stats", {{"state", "printing"}, {"filename", "part.gcode"}}}});
    drain();
    CHECK_FALSE(state.print_state().has_preparing_job());
    CHECK(state.print_state().last_preparing_exit() == helix::PreparingExit::Confirmed);
}

TEST_CASE_METHOD(PreStartGateFixture,
                 "A start sent with the connection down fails unless Stop waits on it",
                 "[print_preparation][preparing_cancel][1898]") {
    // CONNECTION_LOST is also what a start refused for a link that is down
    // gets: nothing reached Moonraker, so the start fails and says so.
    const bool stop_waiting = GENERATE(true, false);
    CAPTURE(stop_waiting);
    jobs.fail_with = MoonrakerError::connection_lost("printer.print.start");
    std::vector<std::string> errors;
    helix::ui::set_test_notification_error_hook(
        [&errors](const std::string& msg) { errors.push_back(msg); });
    state.print_state().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    set_busy(false);

    manager.start_print("part.gcode", "", []() {}, completion());
    REQUIRE(api->captured_success);
    if (stop_waiting) {
        state.print_state().note_preparing_cancel_requested();
    }
    api->captured_success();
    drain();
    helix::ui::set_test_notification_error_hook(nullptr);

    REQUIRE(jobs.start_print_calls == 1);
    CHECK(completed == !stop_waiting);
    if (stop_waiting) {
        CHECK(state.print_state().preparing_start() == helix::PreparingStart::Unanswered);
        CHECK(errors.empty());
        return;
    }
    CHECK_FALSE(completion_success);
    CHECK_FALSE(completion_error.empty()); // what the controller retires Failed on
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].rfind("Failed to start print: ", 0) == 0);
}

namespace {

/// The four-colour U1 fixture the file-transfer mock resolves by basename.
constexpr const char* kRemapFixture = "u1_4color_ring.gcode";

} // namespace

TEST_CASE_METHOD(PreStartGateFixture, "A remap cancelled during the transfer never starts",
                 "[print_preparation][remap][preparing_cancel]") {
    state.print_state().begin_preparing(helix::PrintJobRef{kRemapFixture, "gcodes", ""});
    // T1 -> T2 rewrites and uploads a copy; T5 is not in the file, so the
    // original starts unchanged.
    const bool rewritten = GENERATE(true, false);
    CAPTURE(rewritten);
    const std::map<int, int> remap =
        rewritten ? std::map<int, int>{{1, 2}} : std::map<int, int>{{5, 6}};
    const bool withdraw = GENERATE(true, false);
    CAPTURE(withdraw);
    api->transfers_mock().mock_hold_path_uploads();

    manager.modify_and_print_with_remap(kRemapFixture, remap, nullptr);
    if (withdraw) {
        // Stop while the copy is still on its way.
        state.print_state().retire_preparing(helix::PreparingExit::Cancelled);
    }
    for (int i = 0; i < 4; ++i) {
        drain();
    }
    api->transfers_mock().release_held_path_uploads();
    for (int i = 0; i < 4; ++i) {
        drain();
    }

    CHECK(api->transfers_mock().path_uploads().size() == (rewritten ? 1u : 0u));
    const int sent = jobs.start_print_calls + jobs.start_modified_print_calls;
    CHECK(sent == (withdraw ? 0 : 1));
    if (!withdraw) {
        CHECK(state.print_state().preparing_start() == helix::PreparingStart::Accepted);
    }
}

TEST_CASE_METHOD(PreStartGateFixture, "A rewritten start that times out stays preparing",
                 "[print_preparation][remap][preparing_cancel][1898]") {
    // A timeout means Klipper is busy running the start: the job stays
    // preparing, Stop or no Stop.
    const bool stop_waiting = GENERATE(true, false);
    CAPTURE(stop_waiting);
    jobs.fail_with = MoonrakerError::timeout("server.helix.print", 600000);
    state.print_state().begin_preparing(helix::PrintJobRef{kRemapFixture, "gcodes", ""});
    if (stop_waiting) {
        state.print_state().note_preparing_cancel_requested();
    }

    manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
    for (int i = 0; i < 6; ++i) {
        drain();
    }

    REQUIRE(jobs.start_modified_print_calls == 1);
    CHECK(state.print_state().has_preparing_job());
    CHECK(state.print_state().preparing_start() == helix::PreparingStart::Unanswered);
}

TEST_CASE_METHOD(PreStartGateFixture, "A reprint whose start times out stays preparing",
                 "[print_preparation][preparing_cancel][1898]") {
    const bool stop_waiting = GENERATE(true, false);
    CAPTURE(stop_waiting);
    jobs.fail_with = MoonrakerError::timeout("printer.print.start", 600000);
    helix::ui::PrintStartController controller{state, api.get()};

    bool errored = false;
    controller.initiate_reprint(
        "part.gcode", "part.gcode", {}, []() {}, [&errored]() { errored = true; });
    if (stop_waiting) {
        state.print_state().note_preparing_cancel_requested();
    }
    drain();

    REQUIRE(jobs.start_print_calls == 1);
    CHECK(state.print_state().has_preparing_job());
    CHECK(state.print_state().preparing_start() == helix::PreparingStart::Unanswered);
    CHECK_FALSE(errored);
}

// ============================================================================
// printer.print.start's own RPC ceiling
// ============================================================================

TEST_CASE("start_print outlives a print-start macro that heats synchronously",
          "[moonraker][job][pre_start][1451]") {
    // Moonraker confirms printer.print.start only once Klipper acknowledges
    // SDCARD_PRINT_FILE. A print-start macro that blocks on M109/M190 holds
    // that ack for the whole heat-up, so on a cold machine the RPC answers
    // minutes after the print is already running. At the tracker's default
    // ceiling the app reports a timeout over a print that started fine.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    client.connect("ws://mock/websocket", []() {}, []() {});

    MoonrakerJobAPI job(client);

    bool started = false;
    job.start_print("part.gcode", [&started]() { started = true; }, [](const MoonrakerError&) {});

    REQUIRE(started);
    REQUIRE(client.last_send_method() == "printer.print.start");
    CHECK(client.last_send_timeout_ms() == MoonrakerJobAPI::PRINT_START_TIMEOUT_MS);
    CHECK(MoonrakerJobAPI::PRINT_START_TIMEOUT_MS == 600000u);
    // The ceiling only helps while it clears the default — pin the relation too.
    CHECK(MoonrakerJobAPI::PRINT_START_TIMEOUT_MS >
          helix::MoonrakerRequestTracker::DEFAULT_REQUEST_TIMEOUT_MS);
}

TEST_CASE_METHOD(PreStartGateFixture,
                 "A plugin start that times out while Stop waits is held, with no second start",
                 "[print_preparation][remap][preparing_cancel][1451]") {
    // The plugin starts the print before it answers print_modified, so a
    // timeout there is a lost answer, not a refusal: no printer.print.start
    // fallback goes out behind the running start sequence, and the job is held
    // for the cancel Stop asked for.
    jobs.real_modified_print = true;
    MoonrakerClientMockTestAccess::set_method_handler(
        mock_client, "server.helix.print_modified",
        [](MoonrakerClientMock*, const nlohmann::json&, std::function<void(const nlohmann::json&)>,
           std::function<void(const MoonrakerError&)> err) -> bool {
            err(MoonrakerError::timeout("server.helix.print_modified",
                                        MoonrakerJobAPI::PRINT_MODIFIED_TIMEOUT_MS));
            return true;
        });
    state.print_state().begin_preparing(helix::PrintJobRef{kRemapFixture, "gcodes", ""});
    state.print_state().note_preparing_cancel_requested();

    manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
    for (int i = 0; i < 6; ++i) {
        drain();
    }

    REQUIRE(jobs.start_modified_print_calls == 1);
    CHECK(jobs.start_print_calls == 0);
    CHECK(state.print_state().has_preparing_job());
    CHECK(state.print_state().preparing_start() == helix::PreparingStart::Unanswered);
}
