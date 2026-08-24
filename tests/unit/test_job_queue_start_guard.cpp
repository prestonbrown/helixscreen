// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_start_guard.cpp
 * @brief Starting a queued job must not delete the entry it then fails to start.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][print_state]"
 *
 * JobQueueModal::start_job() removes the entry from Moonraker's queue FIRST and
 * starts the print from the removal's success callback. That ordering is only
 * safe while the refusal in front of it is complete. It was not: the guard read
 * print_stats.state, which reports standby for the whole of a host-side
 * pre-print block, so a tap during that window deleted the queue entry and then
 * ran into PrintStartController's own refusal - the job was gone with nothing
 * printing and nothing queued.
 *
 * can_start_new_print() is the predicate that covers both axes: the printer's
 * reported state AND the app's committed-but-unconfirmed start.
 */

#include "ui_job_queue_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/job_queue_modal_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::JobQueueModal;
using helix::PrintJobState;
using helix::test::set_wire_state;

namespace {

/// Records every JSON-RPC method the modal sends, so "did it touch the queue?"
/// is answered by observing the wire rather than by a spy on our own code.
class RecordingClient : public MoonrakerClientMock {
  public:
    RecordingClient() : MoonrakerClientMock(MoonrakerClientMock::PrinterType::VORON_24) {}

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        methods.push_back(method);
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    [[nodiscard]] int count(const std::string& method) const {
        int n = 0;
        for (const auto& m : methods) {
            if (m == method) {
                ++n;
            }
        }
        return n;
    }

    std::vector<std::string> methods;
};

constexpr const char* DELETE_JOB = "server.job_queue.delete_job";

class JobQueueStartFixture : public LVGLTestFixture {
  public:
    JobQueueStartFixture() {
        // The global PrinterState is shared across the shard: reset and
        // re-init, or a prior case's subjects decide this one's answers.
        auto& ps0 = get_printer_state();
        PrinterStateTestAccess::reset(ps0);
        ps0.init_subjects(false);
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(client, get_printer_state());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        // A prior case's preparing job would refuse every start here.
        auto& ps = get_printer_state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        settle();
    }

    ~JobQueueStartFixture() override {
        auto& ps = get_printer_state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        settle();
        set_moonraker_api(previous_api_);
        api.reset();
        client.stop_temperature_simulation();
        client.disconnect();
    }

    /// One drain is not enough: a handler running during a drain queues more
    /// work that is still pending when drain() returns.
    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    RecordingClient client;
    std::unique_ptr<MoonrakerAPI> api;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(JobQueueStartFixture, "JobQueueModal starts a queued job when the printer is idle",
                 "[job_queue][print_state]") {
    // The baseline that stops every refusal assertion below from passing
    // vacuously: with nothing running, the queue entry IS removed.
    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "job-1", "benchy.gcode");
    settle();

    CHECK(client.count(DELETE_JOB) == 1);
}

TEST_CASE_METHOD(JobQueueStartFixture, "JobQueueModal refuses to start while a print is running",
                 "[job_queue][print_state]") {
    // Characterization of the behaviour that already worked - the wire is
    // enough here, and it must stay refused after the predicate changes.
    auto& ps = get_printer_state();

    SECTION("printing") {
        set_wire_state(ps, PrintJobState::PRINTING);
    }
    SECTION("paused") {
        set_wire_state(ps, PrintJobState::PAUSED);
    }
    settle();

    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "job-1", "benchy.gcode");
    settle();

    CHECK(client.count(DELETE_JOB) == 0);
}

TEST_CASE_METHOD(JobQueueStartFixture,
                 "JobQueueModal refuses to start during a host-side pre-print block",
                 "[job_queue][print_state]") {
    // THE BUG. The app has committed to a print and is running the user's
    // pre-start block itself, so print_stats.state still reads standby. Under
    // the old wire-only guard the entry was deleted here and the start then
    // failed, losing the job.
    auto& ps = get_printer_state();
    ps.begin_preparing(helix::PrintJobRef{"queued.gcode", "", ""});
    settle();
    REQUIRE(ps.get_print_job_state() == PrintJobState::STANDBY);
    REQUIRE(ps.is_print_in_progress());

    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "job-1", "benchy.gcode");
    settle();

    CHECK(client.count(DELETE_JOB) == 0);
}
