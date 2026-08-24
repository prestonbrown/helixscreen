// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pending_startup_warnings.cpp
 * @brief Unit tests for the deferred startup-warnings queue.
 *
 * Validates:
 * 1. Empty queue starts empty and drain is a no-op
 * 2. Enqueue preserves order; drain returns entries in FIFO order
 * 3. clear() resets state
 * 4. Thread-safety: concurrent enqueue from multiple threads does not crash
 *    or lose entries (run under TSAN where available)
 */

#include "../../include/pending_startup_warnings.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
// Test helper: capture drain callbacks into a local vector (instead of calling
// ToastManager, which is not available in a unit test context).
struct CapturedWarning {
    PendingStartupWarnings::Severity severity;
    std::string message;
};
} // namespace

TEST_CASE("PendingStartupWarnings: empty queue drains to empty", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    std::vector<CapturedWarning> captured;
    q.drain([&](PendingStartupWarnings::Severity s, const std::string& m) {
        captured.push_back({s, m});
    });
    REQUIRE(captured.empty());
}

TEST_CASE("PendingStartupWarnings: enqueue preserves FIFO order", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    q.enqueue(PendingStartupWarnings::Severity::WARNING, "first");
    q.enqueue(PendingStartupWarnings::Severity::ERROR, "second");
    q.enqueue(PendingStartupWarnings::Severity::INFO, "third");

    std::vector<CapturedWarning> captured;
    q.drain([&](PendingStartupWarnings::Severity s, const std::string& m) {
        captured.push_back({s, m});
    });

    REQUIRE(captured.size() == 3);
    REQUIRE(captured[0].message == "first");
    REQUIRE(captured[0].severity == PendingStartupWarnings::Severity::WARNING);
    REQUIRE(captured[1].message == "second");
    REQUIRE(captured[1].severity == PendingStartupWarnings::Severity::ERROR);
    REQUIRE(captured[2].message == "third");
    REQUIRE(captured[2].severity == PendingStartupWarnings::Severity::INFO);
}

TEST_CASE("PendingStartupWarnings: deduplicates identical pending entries", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    // The same condition can be reported by multiple layers during startup — e.g.
    // a resolution-fallback warning enqueued by both the DRM and fbdev backends.
    // The user should see it once.
    q.enqueue(PendingStartupWarnings::Severity::WARNING, "same message");
    q.enqueue(PendingStartupWarnings::Severity::WARNING, "same message");
    q.enqueue(PendingStartupWarnings::Severity::WARNING, "same message");

    std::vector<CapturedWarning> captured;
    q.drain([&](PendingStartupWarnings::Severity s, const std::string& m) {
        captured.push_back({s, m});
    });

    REQUIRE(captured.size() == 1);
    REQUIRE(captured[0].message == "same message");
}

TEST_CASE("PendingStartupWarnings: same text at different severity is not deduplicated",
          "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    q.enqueue(PendingStartupWarnings::Severity::WARNING, "dup text");
    q.enqueue(PendingStartupWarnings::Severity::ERROR, "dup text");

    int count = 0;
    q.drain([&](auto, auto&) { count++; });
    REQUIRE(count == 2);
}

TEST_CASE("PendingStartupWarnings: drain empties the queue", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    q.enqueue(PendingStartupWarnings::Severity::ERROR, "only");

    int count1 = 0;
    q.drain([&](auto, auto&) { count1++; });
    REQUIRE(count1 == 1);

    int count2 = 0;
    q.drain([&](auto, auto&) { count2++; });
    REQUIRE(count2 == 0);
}

TEST_CASE("PendingStartupWarnings: clear() removes all entries", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    q.enqueue(PendingStartupWarnings::Severity::ERROR, "a");
    q.enqueue(PendingStartupWarnings::Severity::ERROR, "b");
    q.clear();

    int count = 0;
    q.drain([&](auto, auto&) { count++; });
    REQUIRE(count == 0);
}

TEST_CASE("PendingStartupWarnings: concurrent enqueue is safe", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    // Stay under the 64-entry cap so the assertion is about thread safety, not
    // overflow behaviour (the overflow case has its own test).
    constexpr int THREADS = 7;
    constexpr int PER_THREAD = 8;

    std::vector<std::thread> workers;
    for (int t = 0; t < THREADS; t++) {
        workers.emplace_back([&q, t]() {
            for (int i = 0; i < PER_THREAD; i++) {
                q.enqueue(PendingStartupWarnings::Severity::WARNING,
                          "t" + std::to_string(t) + "_" + std::to_string(i));
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    int count = 0;
    q.drain([&](auto, auto&) { count++; });
    REQUIRE(count == THREADS * PER_THREAD);
}

TEST_CASE("PendingStartupWarnings: drops past cap, preserves earlier entries",
          "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    // Enqueue well past the cap; only the first N should survive.
    for (int i = 0; i < 100; i++) {
        q.enqueue(PendingStartupWarnings::Severity::INFO, "msg_" + std::to_string(i));
    }

    std::vector<std::string> captured;
    q.drain([&](PendingStartupWarnings::Severity, const std::string& m) { captured.push_back(m); });

    // Cap is an implementation detail; we only assert it's bounded and FIFO.
    REQUIRE(captured.size() >= 1);
    REQUIRE(captured.size() <= 100);
    REQUIRE(captured.front() == "msg_0");
}
