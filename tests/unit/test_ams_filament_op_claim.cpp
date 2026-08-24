// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_filament_op_claim.cpp
 * @brief The filament-op gate is a claim, not a test: exactly one op enters.
 *
 * AmsSubscriptionBackend's NVI wrappers gate load/unload/select_slot/change_tool
 * and dispatch to a protected do_* hook. The gate reads
 * system_info_.is_busy() — a field the WebSocket status path writes under
 * mutex_ — so a plain read is a data race, and a plain check is a check-then-act
 * whose "act" happens much later.
 *
 * How much later is the point. NO backend makes itself busy in the gate's
 * critical section. AFC and Tool Changer set the optimistic action inside
 * dispatch_operation()'s own, separate lock; ACE, CFS and AD5X set it inside the
 * hook; Happy Hare, Snapmaker and QIDI never set it at all and wait for firmware
 * to echo. So "hold mutex_ across the busy check", which is what the pre-NVI
 * backends did, never produced mutual exclusion — the write that would have
 * closed the window lands after that lock is dropped.
 *
 * The claim closes it. claim_filament_op() test-and-sets an in-flight flag in
 * the SAME critical section as the busy read, and run_filament_op() holds it
 * across the do_* dispatch via RAII. mutex_ itself is released before the hook —
 * the hooks send gcode, several call emit_event() (which takes mutex_), and
 * AD5X's unload re-enters eject_lane() (which takes mutex_), so a wrapper
 * holding it would deadlock outright.
 *
 * Every assertion below fails against the pre-claim gate:
 *   - "second op is refused" sees two hook entries and a success, not a BUSY.
 *   - "exactly one of N wins" sees all N win.
 * Nothing here is timing-tolerant in the failing direction: the blocked hook
 * only ever blocks its FIRST entrant, so a second entrant that should not exist
 * runs straight through and trips the count rather than hanging the suite.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_qidi.h"
#include "ams_error.h"
#include "ams_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

using namespace std::chrono_literals;

/// A backend whose do_* hooks are instrumented and controllable, and which
/// changes nothing else. Overriding the hooks (not the public entry points —
/// those are `final`) is the only way in, which is the NVI property under test.
///
/// Templated on the real backend so the same assertions run against both gate
/// modes: Happy Hare declares FilamentOpGate::Standard, QIDI declares
/// PrintActiveOnly.
template <typename Backend> class ClaimDouble : public Backend {
  public:
    ClaimDouble(MoonrakerAPI* api, helix::MoonrakerClient* client) : Backend(api, client) {
        this->running_.store(true);
    }

    /// Total do_* entries. The headline assertion: a refused op must not reach
    /// its hook at all, so this counts dispatches, not successes.
    std::atomic<int> hook_entries{0};

    /// When true the FIRST entrant parks until release() (or a 2s escape hatch).
    /// Later entrants never park — a wedged suite would hide the regression this
    /// file exists to catch, so an extra entrant has to run and be counted.
    std::atomic<bool> block_first_entrant{false};

    /// Busy-work inside the hook, before the optimistic action is published.
    /// This IS the window: the interval in which the backend has won the gate
    /// but system_info_.action still reads IDLE.
    std::chrono::milliseconds hook_work{0};

    /// Mimic AFC / ACE / CFS / AD5X, which publish an optimistic busy action
    /// from inside the hook. Happy Hare, Snapmaker and QIDI do not — for them
    /// the window stays open until firmware echoes, so the claim is the only
    /// thing that ever closes it.
    bool publish_busy_action{false};

    /// What the hook returns, and whether it throws instead. Both exercise claim
    /// release on a non-success exit.
    AmsError hook_result{AmsErrorHelper::success()};
    bool hook_throws{false};

    /// Drive the two things the gate consults, without a live Moonraker.
    void set_action_for_test(AmsAction action) {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->system_info_.action = action;
    }
    void stop_for_test() {
        this->running_.store(false);
    }

    /// Blocks until the first entrant is parked inside its hook.
    void wait_for_first_entrant() {
        std::unique_lock<std::mutex> lock(gate_m_);
        REQUIRE(gate_cv_.wait_for(lock, 5s, [this] { return entered_; }));
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(gate_m_);
            released_ = true;
        }
        gate_cv_.notify_all();
    }

  protected:
    AmsError do_load_filament(int) override {
        return run_hook();
    }
    AmsError do_unload_filament(int) override {
        return run_hook();
    }
    AmsError do_select_slot(int) override {
        return run_hook();
    }
    AmsError do_change_tool(int) override {
        return run_hook();
    }

  private:
    AmsError run_hook() {
        const int ordinal = hook_entries.fetch_add(1);

        if (ordinal == 0 && block_first_entrant.load()) {
            {
                std::lock_guard<std::mutex> lock(gate_m_);
                entered_ = true;
            }
            gate_cv_.notify_all();
            std::unique_lock<std::mutex> lock(gate_m_);
            // Bounded: an un-released park would turn a failing assertion into a
            // hung suite, and the pre-claim behaviour has to FAIL, not hang.
            gate_cv_.wait_for(lock, 2s, [this] { return released_; });
        }

        if (hook_work.count() > 0) {
            std::this_thread::sleep_for(hook_work);
        }

        if (publish_busy_action) {
            std::lock_guard<std::mutex> lock(this->mutex_);
            this->system_info_.action = AmsAction::LOADING;
        }

        if (hook_throws) {
            throw std::runtime_error("hook blew up");
        }
        return hook_result;
    }

    std::mutex gate_m_;
    std::condition_variable gate_cv_;
    bool entered_ = false;
    bool released_ = false;
};

struct ClaimFixture : public LVGLTestFixture {
    ClaimFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }

    /// Concurrency doubles get a NULL api on purpose: refuse_if_printing() reads
    /// the print state out of an lv_subject, and worker threads must not touch
    /// LVGL. A null api_ short-circuits it to success, leaving the claim as the
    /// only thing under test. Print-state assertions use with_api() instead,
    /// single-threaded.
    template <typename Backend> std::unique_ptr<ClaimDouble<Backend>> headless() {
        return std::make_unique<ClaimDouble<Backend>>(nullptr, nullptr);
    }

    template <typename Backend> std::unique_ptr<ClaimDouble<Backend>> with_api() {
        return std::make_unique<ClaimDouble<Backend>>(api.get(), &mock_client);
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
};

} // namespace

// ============================================================================
// The race itself
// ============================================================================

TEST_CASE_METHOD(ClaimFixture, "AMS filament op: a second op cannot enter while one is dispatching",
                 "[ams][threading][claim]") {
    auto backend = headless<AmsBackendHappyHare>();
    backend->block_first_entrant.store(true);

    AmsError first_result = AmsErrorHelper::busy("not yet run");
    std::thread first([&] { first_result = backend->load_filament(0); });

    // The first op now owns the claim and is parked inside its hook. Crucially
    // system_info_.action is still IDLE and running_ is still true, so the
    // pre-claim gate has nothing to refuse on and would wave the contender
    // straight through.
    backend->wait_for_first_entrant();
    CHECK(backend->get_current_action() == AmsAction::IDLE);

    const AmsError contender = backend->load_filament(1);

    backend->release();
    first.join();

    CHECK(first_result.success());
    CHECK_FALSE(contender.success());
    CHECK(contender.result == AmsResult::BUSY);
    // The assertion with teeth: refused means it never reached the backend, not
    // merely that it returned an error.
    CHECK(backend->hook_entries.load() == 1);
}

TEST_CASE_METHOD(ClaimFixture, "AMS filament op: the loser is refused, never blocked",
                 "[ams][threading][claim]") {
    // A claim that made the contender WAIT would serialize the UI thread behind
    // a gcode send. It must fail fast instead.
    auto backend = headless<AmsBackendHappyHare>();
    backend->block_first_entrant.store(true);

    std::thread first([&] { (void)backend->load_filament(0); });
    backend->wait_for_first_entrant();

    const auto t0 = std::chrono::steady_clock::now();
    const AmsError contender = backend->unload_filament(0);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    backend->release();
    first.join();

    CHECK(contender.result == AmsResult::BUSY);
    CHECK(elapsed < 500ms);
}

TEST_CASE_METHOD(ClaimFixture, "AMS filament op: exactly one of many concurrent ops wins",
                 "[ams][threading][claim]") {
    constexpr int THREADS = 8;

    // Model the real shape of the window on AFC / ACE / CFS / AD5X: the op wins
    // the gate, does work, and only THEN publishes the busy action. Without the
    // claim every thread reads IDLE during that work and all eight win.
    auto backend = headless<AmsBackendHappyHare>();
    backend->hook_work = 25ms;
    backend->publish_busy_action = true;

    std::atomic<bool> go{false};
    std::vector<AmsError> results(THREADS, AmsErrorHelper::busy("not yet run"));
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[static_cast<size_t>(i)] = backend->load_filament(i);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) {
        t.join();
    }

    int winners = 0;
    for (const auto& r : results) {
        if (r.success()) {
            ++winners;
        } else {
            CHECK(r.result == AmsResult::BUSY);
        }
    }
    CHECK(winners == 1);
    CHECK(backend->hook_entries.load() == 1);
}

TEST_CASE_METHOD(ClaimFixture, "AMS filament op: the claim covers a PrintActiveOnly backend too",
                 "[ams][threading][claim]") {
    // QIDI opts out of the running_/busy half of the gate
    // (FilamentOpGate::PrintActiveOnly). That is an opt-out of consulting an AMS
    // state it does not maintain — NOT an opt-out of the structural guarantee,
    // which a backend has no hook to decline.
    auto backend = headless<AmsBackendQidi>();
    backend->block_first_entrant.store(true);

    std::thread first([&] { (void)backend->load_filament(0); });
    backend->wait_for_first_entrant();

    const AmsError contender = backend->change_tool(1);

    backend->release();
    first.join();

    CHECK(contender.result == AmsResult::BUSY);
    CHECK(backend->hook_entries.load() == 1);
}

// ============================================================================
// Release on every exit path. A leaked claim is a permanently wedged backend
// with no action to explain it — strictly worse than the race being fixed.
// ============================================================================

TEST_CASE_METHOD(ClaimFixture, "AMS filament op: the claim is released on every exit path",
                 "[ams][claim]") {
    SECTION("hook returned success") {
        auto backend = headless<AmsBackendHappyHare>();
        CHECK(backend->load_filament(0).success());
        CHECK(backend->load_filament(0).success());
        CHECK(backend->hook_entries.load() == 2);
    }

    SECTION("hook returned a failure") {
        auto backend = headless<AmsBackendHappyHare>();
        backend->hook_result = AmsErrorHelper::invalid_slot(9, 3);
        CHECK(backend->load_filament(9).result == AmsResult::INVALID_SLOT);
        backend->hook_result = AmsErrorHelper::success();
        CHECK(backend->load_filament(0).success());
    }

    SECTION("hook threw") {
        auto backend = headless<AmsBackendHappyHare>();
        backend->hook_throws = true;
        CHECK_THROWS(backend->load_filament(0));
        backend->hook_throws = false;
        CHECK(backend->load_filament(0).success());
    }

    SECTION("the print-active gate refused, so the hook never ran") {
        auto backend = with_api<AmsBackendHappyHare>();
        set_print_state(helix::PrintJobState::PRINTING);
        const AmsError refused = backend->load_filament(0);
        CHECK_FALSE(refused.success());
        CHECK(backend->hook_entries.load() == 0);

        // The refusal claimed before it checked the print state. If that claim
        // leaked, the backend is dead for the rest of its life.
        set_print_state(helix::PrintJobState::STANDBY);
        CHECK(backend->load_filament(0).success());
        CHECK(backend->hook_entries.load() == 1);
    }

    SECTION("a backend that reports busy is refused without claiming") {
        auto backend = headless<AmsBackendHappyHare>();
        backend->set_action_for_test(AmsAction::UNLOADING);
        CHECK(backend->load_filament(0).result == AmsResult::BUSY);
        backend->set_action_for_test(AmsAction::IDLE);
        CHECK(backend->load_filament(0).success());
    }
}

// ============================================================================
// The claim must not reorder or reword what the gate already refused.
// ============================================================================

TEST_CASE_METHOD(ClaimFixture, "AMS filament op: refusal precedence is unchanged by the claim",
                 "[ams][claim]") {
    SECTION("not started outranks everything") {
        auto backend = with_api<AmsBackendHappyHare>();
        backend->stop_for_test();
        set_print_state(helix::PrintJobState::PRINTING);
        const AmsError err = backend->load_filament(0);
        CHECK(err.result == AmsResult::NOT_CONNECTED);
        CHECK(backend->hook_entries.load() == 0);
    }

    SECTION("busy outranks print-active") {
        auto backend = with_api<AmsBackendHappyHare>();
        backend->set_action_for_test(AmsAction::LOADING);
        set_print_state(helix::PrintJobState::PRINTING);
        const AmsError err = backend->load_filament(0);
        CHECK(err.result == AmsResult::BUSY);
        CHECK(backend->hook_entries.load() == 0);
    }

    SECTION("an in-flight claim reports busy the same way an AMS-reported one does") {
        auto backend = headless<AmsBackendHappyHare>();
        backend->set_action_for_test(AmsAction::LOADING);
        const AmsError from_state = backend->load_filament(0);
        backend->set_action_for_test(AmsAction::IDLE);

        backend->block_first_entrant.store(true);
        std::thread first([&] { (void)backend->load_filament(0); });
        backend->wait_for_first_entrant();
        const AmsError from_claim = backend->load_filament(0);
        backend->release();
        first.join();

        CHECK(from_claim.result == from_state.result);
        CHECK(from_claim.user_msg == from_state.user_msg);
        CHECK(from_claim.technical_msg == from_state.technical_msg);
    }
}
