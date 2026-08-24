// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_async_lifetime_guard.cpp
 * @brief Unit tests for AsyncLifetimeGuard — generation-counter-based async callback safety
 */

#include "../lvgl_test_fixture.h"
#include "async_lifetime_guard.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Drop whatever a previous test left in the skip-counter store. take_snapshot()
/// releases every slot unconditionally, so discarding its result *is* the reset —
/// there is no separate entry point that could drift from the drain it mirrors.
void drain_skip_counters() {
    (void)helix::async_lifetime::take_snapshot();
}

} // namespace

// ============================================================================
// Pure token tests (no LVGL needed)
// ============================================================================

TEST_CASE("Token valid when no invalidation", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    REQUIRE_FALSE(tok.expired());
    REQUIRE(static_cast<bool>(tok));
}

TEST_CASE("Token expired after invalidate", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    guard.invalidate();

    REQUIRE(tok.expired());
    REQUIRE_FALSE(static_cast<bool>(tok));
}

TEST_CASE("Multiple tokens all expire", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto t1 = guard.token();
    auto t2 = guard.token();
    auto t3 = guard.token();

    REQUIRE_FALSE(t1.expired());
    REQUIRE_FALSE(t2.expired());
    REQUIRE_FALSE(t3.expired());

    guard.invalidate();

    REQUIRE(t1.expired());
    REQUIRE(t2.expired());
    REQUIRE(t3.expired());
}

TEST_CASE("Generation cycling — old token expired, new token valid", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto old_tok = guard.token();

    guard.invalidate();

    auto new_tok = guard.token();

    REQUIRE(old_tok.expired());
    REQUIRE_FALSE(new_tok.expired());
}

TEST_CASE("Double invalidate still works correctly", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto t1 = guard.token();

    guard.invalidate();
    REQUIRE(t1.expired());

    // Second invalidate should not break anything
    guard.invalidate();
    REQUIRE(t1.expired());

    // New token after double-invalidate should be valid
    auto t2 = guard.token();
    REQUIRE_FALSE(t2.expired());
}

// ============================================================================
// Defer tests (need LVGL for UpdateQueue)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Defer runs when valid", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    bool ran = false;

    guard.defer([&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Defer skips when invalidated", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    bool ran = false;

    guard.defer([&ran]() { ran = true; });
    guard.invalidate();

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Defer with tag skips when invalidated", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    bool ran = false;

    guard.defer("test::tagged_callback", [&ran]() { ran = true; });
    guard.invalidate();

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Defer safe after guard destroyed", "[lifetime_guard]") {
    bool ran = false;

    {
        AsyncLifetimeGuard guard;
        guard.defer([&ran]() { ran = true; });
        // guard destroyed here — invalidate() called in destructor
    }

    // Draining should not crash, and callback should NOT run
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Defer after invalidate uses new generation",
                 "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    bool first_ran = false;
    bool second_ran = false;

    guard.defer([&first_ran]() { first_ran = true; });
    guard.invalidate();
    guard.defer([&second_ran]() { second_ran = true; });

    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(first_ran);
    REQUIRE(second_ran);
}

// ============================================================================
// LifetimeToken::defer() tests (need LVGL for UpdateQueue)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Token defer runs when valid", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    bool ran = false;

    tok.defer([&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer skips when guard invalidated", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    bool ran = false;

    guard.invalidate();
    tok.defer([&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer skips when guard destroyed", "[lifetime_guard]") {
    bool ran = false;

    // Use optional to control guard lifetime independently of token
    auto guard_ptr = std::make_unique<AsyncLifetimeGuard>();
    auto tok = guard_ptr->token();

    // Destroy guard — invalidate() in destructor expires tok
    guard_ptr.reset();

    // Token outlives guard — defer must not crash and must skip callback
    tok.defer([&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer with tag skips when invalidated",
                 "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    bool ran = false;

    guard.invalidate();
    tok.defer("test::tagged_token_callback", [&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer after invalidate — new token works",
                 "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto old_tok = guard.token();
    bool old_ran = false;
    bool new_ran = false;

    guard.invalidate();
    auto new_tok = guard.token();

    old_tok.defer([&old_ran]() { old_ran = true; });
    new_tok.defer([&new_ran]() { new_ran = true; });

    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(old_ran);
    REQUIRE(new_ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer queued before invalidate is skipped",
                 "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    bool ran = false;

    // Queue via token, then invalidate before drain
    tok.defer([&ran]() { ran = true; });
    guard.invalidate();

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

// ============================================================================
// Bg-thread expired() detector (3XNZQB2R / cluster:pstat-async-delete)
// ============================================================================
//
// These tests verify expired()'s correctness invariant under bg-thread use.
// The *anomaly reporting* side-effect (helix_lvgl_anomaly call + spdlog
// warn) is exercised here too — the binary links the real telemetry
// implementation, so a misfire would surface as a [Warn] in the test log.
//
// What these don't test: that telemetry actually emitted the bundle field
// the field-resolver reads. That's verified manually after the AD5M deploy
// smoke + via the next field bundle in this signature.

TEST_CASE("expired() correct on main thread (alive)", "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    REQUIRE_FALSE(tok.expired());
}

TEST_CASE("expired() correct on main thread (after invalidate)", "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    guard.invalidate();
    REQUIRE(tok.expired());
}

// Fixture for tests that DELIBERATELY exercise the bg-thread `expired()`
// path the detector aborts on under strict mode. Disables strict for the
// scope of the test and re-enables on teardown so other tests retain their
// strict guarantees. Used by the [bg_detector] tag tests below — they prove
// the *boolean* result of expired() is correct under bg use even though the
// detector classifies the callsite as suspicious.
struct BgDetectorFixture {
    BgDetectorFixture() {
        helix::internal::set_strict_bg_check(false);
    }
    ~BgDetectorFixture() {
        helix::internal::set_strict_bg_check(true);
    }
};

TEST_CASE_METHOD(BgDetectorFixture, "expired() correct on bg thread (alive)",
                 "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    // Bg thread + alive token: should return false (the dangerous case the
    // detector flags via anomaly, but the boolean must still be correct).
    std::atomic<bool> bg_saw_alive{false};
    std::thread bg([&]() { bg_saw_alive.store(!tok.expired()); });
    bg.join();
    REQUIRE(bg_saw_alive.load());
}

TEST_CASE("expired() correct on bg thread (after invalidate)", "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    guard.invalidate();

    std::atomic<bool> bg_saw_expired{false};
    std::thread bg([&]() { bg_saw_expired.store(tok.expired()); });
    bg.join();
    REQUIRE(bg_saw_expired.load());
}

TEST_CASE_METHOD(BgDetectorFixture, "expired() under tight bg-thread loop doesn't crash",
                 "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    // First-fire-only seen-set must tolerate the same callsite firing many
    // times without spamming, crashing, or breaking the boolean result.
    std::thread bg([&]() {
        for (int i = 0; i < 1000; ++i) {
            REQUIRE_FALSE(tok.expired());
        }
    });
    bg.join();
}

// ---------------------------------------------------------------------------
// expired_no_lvgl() — the audited-callsite spelling
// ---------------------------------------------------------------------------
//
// These deliberately run WITHOUT BgDetectorFixture. Strict mode is on for the
// suite, so any call that still routes through report_bg_expired_check()
// aborts the process — which is exactly the regression to catch. A test that
// merely asserts the boolean would pass just as happily if someone "simplified"
// expired_no_lvgl() into a call to expired().

TEST_CASE("expired_no_lvgl() on a bg thread does not trip the detector",
          "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    helix::internal::set_strict_bg_check(true);
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    // Alive token + bg thread is precisely the combination expired() reports
    // (and aborts on under strict mode). Reaching the assert proves we did not.
    std::atomic<bool> saw_alive{false};
    std::thread bg([&]() { saw_alive.store(!tok.expired_no_lvgl()); });
    bg.join();
    REQUIRE(saw_alive.load());
}

TEST_CASE("expired_no_lvgl() still reports expiry correctly on a bg thread",
          "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    helix::internal::set_strict_bg_check(true);
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    guard.invalidate();

    std::atomic<bool> saw_expired{false};
    std::thread bg([&]() { saw_expired.store(tok.expired_no_lvgl()); });
    bg.join();
    REQUIRE(saw_expired.load());
}

TEST_CASE("expired_no_lvgl() agrees with expired() on the main thread",
          "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    REQUIRE(tok.expired_no_lvgl() == tok.expired());
    guard.invalidate();
    REQUIRE(tok.expired_no_lvgl() == tok.expired());
    REQUIRE(tok.expired_no_lvgl());
}

TEST_CASE("expired_no_lvgl() tracks generation cycling", "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto old_tok = guard.token();
    guard.invalidate();
    auto new_tok = guard.token();

    REQUIRE(old_tok.expired_no_lvgl());
    REQUIRE_FALSE(new_tok.expired_no_lvgl());
}

TEST_CASE("on_main_thread() reflects current thread", "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    REQUIRE(helix::internal::on_main_thread());

    std::atomic<bool> bg_saw_main{true};
    std::thread bg([&]() { bg_saw_main.store(helix::internal::on_main_thread()); });
    bg.join();
    REQUIRE_FALSE(bg_saw_main.load());
}

TEST_CASE_METHOD(BgDetectorFixture, "Thread safety — concurrent token and invalidate",
                 "[lifetime_guard][slow]") {
    AsyncLifetimeGuard guard;
    std::atomic<bool> stop{false};
    std::atomic<int> token_count{0};
    std::atomic<int> invalidate_count{0};

    // Thread 1: repeatedly create tokens and check them
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            auto tok = guard.token();
            // Just exercise expired() — result may vary due to races, but must not crash
            (void)tok.expired();
            token_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Thread 2: repeatedly invalidate
    std::thread writer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            guard.invalidate();
            invalidate_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Let them race for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_relaxed);

    reader.join();
    writer.join();

    // If we got here without crashing, the test passed
    REQUIRE(token_count.load() > 0);
    REQUIRE(invalidate_count.load() > 0);
}

// ============================================================================
// Skip-rate telemetry tests — helix::async_lifetime counter module
// ============================================================================

TEST_CASE("async_lifetime counters start empty after reset", "[lifetime_guard][telemetry]") {
    drain_skip_counters();
    auto snap = helix::async_lifetime::take_snapshot();
    REQUIRE(snap.entries.empty());
    REQUIRE(snap.total == 0);
    REQUIRE(snap.other_count == 0);
}

TEST_CASE("async_lifetime note_skipped increments per tag", "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    helix::async_lifetime::note_skipped("TagA");
    helix::async_lifetime::note_skipped("TagA");
    helix::async_lifetime::note_skipped("TagA");
    helix::async_lifetime::note_skipped("TagB");

    auto snap = helix::async_lifetime::take_snapshot();
    REQUIRE(snap.total == 4);
    REQUIRE(snap.entries.size() == 2);
    // Sorted descending by count
    REQUIRE(snap.entries[0].tag == "TagA");
    REQUIRE(snap.entries[0].count == 3);
    REQUIRE(snap.entries[1].tag == "TagB");
    REQUIRE(snap.entries[1].count == 1);
    REQUIRE(snap.other_count == 0);
}

TEST_CASE("async_lifetime take_snapshot resets counters", "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    helix::async_lifetime::note_skipped("TagA");
    helix::async_lifetime::note_skipped("TagA");

    auto first = helix::async_lifetime::take_snapshot();
    REQUIRE(first.total == 2);

    // Second snapshot should be empty — counters were drained
    auto second = helix::async_lifetime::take_snapshot();
    REQUIRE(second.total == 0);
    REQUIRE(second.entries.empty());
}

TEST_CASE("async_lifetime nullptr tag normalised to (null)", "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    helix::async_lifetime::note_skipped(nullptr);
    helix::async_lifetime::note_skipped(nullptr);

    auto snap = helix::async_lifetime::take_snapshot();
    REQUIRE(snap.total == 2);
    REQUIRE(snap.entries.size() == 1);
    REQUIRE(snap.entries[0].tag == "(null)");
    REQUIRE(snap.entries[0].count == 2);
}

TEST_CASE("async_lifetime overflow rolls into (other)", "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    // MAX_TRACKED_TAGS is 64. Generate 128 distinct stable tag pointers so the
    // first 64 claim tracked slots and the next 64 roll into "(other)". The
    // counter interns by pointer equality, so each call hits the cold path
    // and claims (or overflows) a slot.
    std::vector<std::string> owners(128);
    std::vector<const char*> tags(128);
    for (int i = 0; i < 128; ++i) {
        owners[i] = std::string("overflow_tag_") + std::to_string(i);
        tags[i] = owners[i].c_str();
    }

    for (int i = 0; i < 128; ++i) {
        helix::async_lifetime::note_skipped(tags[i]);
    }

    auto snap = helix::async_lifetime::take_snapshot();
    REQUIRE(snap.total == 128);
    REQUIRE(snap.other_count == 64);
    // 64 tracked + 1 "(other)" entry
    REQUIRE(snap.entries.size() == 65);
    // "(other)" should be the largest entry (64 vs 1 each for the tracked tags)
    REQUIRE(snap.entries.front().tag == "(other)");
    REQUIRE(snap.entries.front().count == 64);
}

TEST_CASE("async_lifetime quiet tags release their slots", "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    // Saturate every tracked slot, then drain. All of them go quiet, so the
    // drain must hand their slots back.
    const size_t SLOTS = helix::async_lifetime::MAX_TRACKED_TAGS;
    std::vector<std::string> owners(SLOTS);
    for (size_t i = 0; i < SLOTS; ++i) {
        owners[i] = "saturating_tag_" + std::to_string(i);
        helix::async_lifetime::note_skipped(owners[i].c_str());
    }

    auto first = helix::async_lifetime::take_snapshot();
    REQUIRE(first.total == SLOTS);
    REQUIRE(first.other_count == 0);

    // A producer that only turns hot in a later window must still be named.
    // Without slot release it lands in "(other)" and the tag — the one field
    // that identifies which owner is dying with pending work — is lost.
    helix::async_lifetime::note_skipped("late_producer");
    auto second = helix::async_lifetime::take_snapshot();

    REQUIRE(second.other_count == 0);
    REQUIRE(second.entries.size() == 1);
    REQUIRE(second.entries[0].tag == "late_producer");
    REQUIRE(second.entries[0].count == 1);
}

TEST_CASE("async_lifetime a producer hot in consecutive windows stays named",
          "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    // Fill every slot but keep ONE producer hot across both windows. Slots are
    // released on every drain, so the hot tag re-claims one — what matters is
    // that it is still reported by name, never rolled into "(other)".
    const size_t SLOTS = helix::async_lifetime::MAX_TRACKED_TAGS;
    std::vector<std::string> owners(SLOTS - 1);
    for (size_t i = 0; i < SLOTS - 1; ++i) {
        owners[i] = "filler_tag_" + std::to_string(i);
        helix::async_lifetime::note_skipped(owners[i].c_str());
    }
    helix::async_lifetime::note_skipped("persistent_producer");

    auto first = helix::async_lifetime::take_snapshot();
    REQUIRE(first.total == SLOTS);
    REQUIRE(first.other_count == 0);

    helix::async_lifetime::note_skipped("persistent_producer");
    auto second = helix::async_lifetime::take_snapshot();

    REQUIRE(second.total == 1);
    REQUIRE(second.other_count == 0);
    REQUIRE(second.entries.size() == 1);
    REQUIRE(second.entries[0].tag == "persistent_producer");
    REQUIRE(second.entries[0].count == 1);
}

TEST_CASE("async_lifetime repeated tags after snapshot count fresh",
          "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    helix::async_lifetime::note_skipped("TagA");
    helix::async_lifetime::note_skipped("TagA");
    auto first = helix::async_lifetime::take_snapshot();
    REQUIRE(first.total == 2);

    // Same tag again after the snapshot — should count as 1, not 3
    helix::async_lifetime::note_skipped("TagA");
    auto second = helix::async_lifetime::take_snapshot();
    REQUIRE(second.total == 1);
    REQUIRE(second.entries.size() == 1);
    REQUIRE(second.entries[0].tag == "TagA");
    REQUIRE(second.entries[0].count == 1);
}

// ============================================================================
// Integration: skip paths actually invoke note_skipped
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "AsyncLifetimeGuard::defer skip path increments counter",
                 "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    {
        AsyncLifetimeGuard guard;
        guard.defer("TestProducer::guarded_callback", []() { /* never runs */ });
        // guard destroyed here — invalidate() called in destructor
    }

    helix::ui::UpdateQueue::instance().drain();

    auto snap = helix::async_lifetime::take_snapshot();
    REQUIRE(snap.total == 1);
    REQUIRE(snap.entries.size() == 1);
    REQUIRE(snap.entries[0].tag == "TestProducer::guarded_callback");
    REQUIRE(snap.entries[0].count == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "LifetimeToken::defer skip path increments counter",
                 "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    guard.invalidate();
    tok.defer("TestProducer::token_defer", []() { /* never runs */ });

    helix::ui::UpdateQueue::instance().drain();

    auto snap = helix::async_lifetime::take_snapshot();
    REQUIRE(snap.total == 1);
    REQUIRE(snap.entries[0].tag == "TestProducer::token_defer");
}

TEST_CASE_METHOD(LVGLTestFixture, "AsyncLifetimeGuard::bg_cb skip path increments counter",
                 "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    {
        AsyncLifetimeGuard guard;
        auto cb = guard.bg_cb("TestProducer::bg_cb", [](int /*v*/) { /* never runs */ });
        guard.invalidate();
        cb(42); // queues via queue_update; will skip on drain
    }

    helix::ui::UpdateQueue::instance().drain();

    auto snap = helix::async_lifetime::take_snapshot();
    REQUIRE(snap.total == 1);
    REQUIRE(snap.entries[0].tag == "TestProducer::bg_cb");
}

TEST_CASE_METHOD(LVGLTestFixture, "Non-skipped callbacks do NOT increment counter",
                 "[lifetime_guard][telemetry]") {
    drain_skip_counters();

    bool ran = false;
    {
        AsyncLifetimeGuard guard;
        guard.defer("TestProducer::runs", [&ran]() { ran = true; });
        helix::ui::UpdateQueue::instance().drain(); // runs while guard still alive
        REQUIRE(ran);
        // guard destroyed here
    }

    helix::ui::UpdateQueue::instance().drain(); // nothing queued

    auto snap = helix::async_lifetime::take_snapshot();
    REQUIRE(snap.total == 0);
    REQUIRE(snap.entries.empty());
}
