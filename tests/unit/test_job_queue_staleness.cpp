// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_staleness.cpp
 * @brief The queue cache must not claim to be loaded across a dropped socket.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][staleness]"
 *
 * JobQueueState is the second instance of one bug shape, not a separate bug.
 * Its freshness rests entirely on notify_job_queue_changed
 * (job_queue_state.cpp), and an event notification only reaches a live socket.
 * So a queue edited from Mainsail while the WebSocket is down is never
 * announced, while is_loaded_ goes on reporting the cache as good.
 *
 * The rule itself -- and the half of it that must NOT fire, on a Klippy-ready
 * re-announce with no drop -- is covered once against PrintHistoryManager in
 * test_print_history_manager.cpp. Both owners share one implementation
 * (observe_connection_staleness in connection_staleness.h), so what is left to
 * prove here is only that this owner is wired to it.
 *
 * Subjects are deliberately never initialized: JobQueueState registers its
 * subjects into helix-xml's PROCESS-global scope and is process-lifetime in
 * production, so a local instance that inits them leaves dangling names behind
 * for the rest of the binary (see test_job_queue_count_subject.cpp). Nothing
 * here needs them -- set_jobs() and the staleness mark both bypass subjects.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "connection_state.h"
#include "job_queue_state.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

class JobQueueStalenessFixture : public LVGLTestFixture {
  public:
    JobQueueStalenessFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        printer_state_.init_subjects(false);
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(client_, printer_state_);
        state_ = std::make_unique<JobQueueState>(api_.get(), &client_);
    }

    ~JobQueueStalenessFixture() {
        state_.reset();
        api_.reset();
        client_.disconnect();
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    }

  protected:
    void pump(int rounds = 10) {
        for (int i = 0; i < rounds; ++i) {
            UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        }
    }

    MoonrakerClientMock client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<JobQueueState> state_;
};

} // namespace

TEST_CASE_METHOD(JobQueueStalenessFixture,
                 "JobQueueState marks the queue stale when the connection drops",
                 "[job_queue][staleness]") {
    lv_subject_t* conn = printer_state_.get_printer_connection_state_subject();
    REQUIRE(conn != nullptr);

    // Reach the connected steady state BEFORE seeding, and drain. observe_int_sync
    // defers its registration-time apply through the update queue, so the observer's
    // first call still carries the DISCONNECTED value the subject held when the
    // owner was constructed. set_jobs() writes the cache directly rather than
    // through a queued fetch completion, so seeding first would let that stale
    // apply land on top of it and pass this test for the wrong reason.
    lv_subject_set_int(conn, static_cast<int>(ConnectionState::CONNECTED));
    pump();

    JobQueueStateTestAccess::set_jobs(*state_, {});
    REQUIRE(state_->is_loaded());

    lv_subject_set_int(conn, static_cast<int>(ConnectionState::DISCONNECTED));
    pump();

    REQUIRE_FALSE(state_->is_loaded());
}
