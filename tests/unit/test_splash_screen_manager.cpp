// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "splash_screen_manager.h"

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix::application;

// ============================================================================
// SplashScreenManager tests
// ============================================================================

TEST_CASE("SplashScreenManager: no splash pid", "[splash][application]") {
    SplashScreenManager mgr;

    SECTION("starts as not exited") {
        REQUIRE_FALSE(mgr.has_exited());
    }

    SECTION("exits immediately with no pid") {
        mgr.start(0); // No splash
        mgr.check_and_signal();
        REQUIRE(mgr.has_exited());
    }

    SECTION("negative pid treated as no splash") {
        mgr.start(-1);
        mgr.check_and_signal();
        REQUIRE(mgr.has_exited());
    }
}

TEST_CASE("SplashScreenManager: discovery timing", "[splash][application]") {
    SplashScreenManager mgr;

    // Use a mock PID that won't exist - signal will fail but state transitions work
    mgr.start(999999);

    SECTION("waits for discovery before signaling") {
        // Discovery is not complete and DISCOVERY_TIMEOUT_MS has not elapsed, so
        // check_and_signal() must take the "keep splash showing" return and leave
        // the manager unsignaled. This SECTION previously made the call and
        // asserted nothing at all, so deleting that gate changed no test.
        REQUIRE_FALSE(mgr.is_discovery_complete());
        REQUIRE(mgr.elapsed_ms() < SplashScreenManager::DISCOVERY_TIMEOUT_MS);
        REQUIRE_FALSE(mgr.ready_to_signal());

        mgr.check_and_signal();

        REQUIRE_FALSE(mgr.has_exited());
        REQUIRE_FALSE(mgr.needs_post_splash_refresh());
    }

    SECTION("signals immediately when discovery complete") {
        mgr.on_discovery_complete();
        REQUIRE(mgr.is_discovery_complete());
    }

    SECTION("discovery_complete flag persists") {
        REQUIRE_FALSE(mgr.is_discovery_complete());
        mgr.on_discovery_complete();
        REQUIRE(mgr.is_discovery_complete());
        // Still true after check
        mgr.check_and_signal();
        REQUIRE(mgr.is_discovery_complete());
    }
}

TEST_CASE("SplashScreenManager: post-splash refresh", "[splash][application]") {
    SplashScreenManager mgr;

    SECTION("no refresh needed initially") {
        REQUIRE_FALSE(mgr.needs_post_splash_refresh());
    }

    SECTION("refresh needed after splash exits") {
        mgr.start(0); // No splash = immediate exit
        mgr.check_and_signal();
        REQUIRE(mgr.has_exited());
        REQUIRE(mgr.needs_post_splash_refresh());
    }

    SECTION("mark_refresh_done decrements counter") {
        mgr.start(0);
        mgr.check_and_signal();
        REQUIRE(mgr.needs_post_splash_refresh());

        mgr.mark_refresh_done();
        REQUIRE_FALSE(mgr.needs_post_splash_refresh());
    }

    SECTION("multiple refreshes if configured") {
        mgr.start(0);
        mgr.check_and_signal();

        // Default is 1 refresh
        REQUIRE(mgr.needs_post_splash_refresh());
        mgr.mark_refresh_done();
        REQUIRE_FALSE(mgr.needs_post_splash_refresh());

        // Extra mark_refresh_done is safe
        mgr.mark_refresh_done();
        REQUIRE_FALSE(mgr.needs_post_splash_refresh());
    }
}

TEST_CASE("SplashScreenManager: idempotent signaling", "[splash][application]") {
    SplashScreenManager mgr;
    mgr.start(0);

    SECTION("multiple check_and_signal calls are safe") {
        mgr.check_and_signal();
        REQUIRE(mgr.has_exited());

        // Second call should be no-op
        mgr.check_and_signal();
        REQUIRE(mgr.has_exited());
    }
}

TEST_CASE("SplashScreenManager: elapsed time tracking", "[splash][application]") {
    SplashScreenManager mgr;
    mgr.start(999999); // Non-existent PID

    SECTION("elapsed_ms starts near zero and then advances with the clock") {
        // `elapsed_ms() < 100` on its own also passes for a hardcoded
        // `return 0`, and elapsed_ms() is what gates the splash timeout — a
        // frozen clock means the splash never dismisses on a printer whose
        // discovery stalls. Sleeping a bounded interval and requiring the value
        // to have grown by roughly that much fails a stopped clock.
        const int64_t before = mgr.elapsed_ms();
        REQUIRE(before >= 0);
        REQUIRE(before < 100);

        constexpr int kSleepMs = 120;
        std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));

        const int64_t after = mgr.elapsed_ms();
        // Allow scheduler slop on the low side; the upper bound only has to be
        // loose enough not to flake under load and tight enough to stay well
        // clear of DISCOVERY_TIMEOUT_MS.
        REQUIRE(after - before >= kSleepMs - 20);
        REQUIRE(after < SplashScreenManager::DISCOVERY_TIMEOUT_MS);
    }

    SECTION("the timeout has not elapsed immediately after start") {
        REQUIRE(mgr.elapsed_ms() < SplashScreenManager::DISCOVERY_TIMEOUT_MS);
        REQUIRE_FALSE(mgr.ready_to_signal());
    }
}

// =============================================================================
// Signal escalation tests — use real forked processes
// =============================================================================

// Helper: fork a child that ignores SIGUSR1 but exits on SIGTERM.
// Resets all signal handlers and creates a new process group to avoid
// interfering with Catch2's signal handling in the parent.
static pid_t fork_sigusr1_ignoring_child() {
    pid_t pid = fork();
    if (pid == 0) {
        // New process group — signals to this PID won't propagate to parent
        setsid();
        // Reset inherited signal handlers (Catch2 installs its own)
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGUSR1, SIG_IGN);
        while (true) {
            pause();
        }
        _exit(0);
    }
    return pid;
}

// Helper: fork a child that exits cleanly on SIGUSR1
static pid_t fork_cooperative_child() {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGUSR1, [](int) { _exit(0); });
        while (true) {
            pause();
        }
        _exit(0);
    }
    return pid;
}

TEST_CASE("SplashScreenManager: signal escalation kills stubborn splash", "[splash][application]") {
    // Fork a child that ignores SIGUSR1 — simulates a stuck splash process
    pid_t child = fork_sigusr1_ignoring_child();
    REQUIRE(child > 0);

    // Small delay to ensure child is running
    usleep(50000); // 50ms

    SplashScreenManager mgr;
    mgr.start(child);
    mgr.on_discovery_complete();

    // This should: SIGUSR1 (ignored) → timeout → SIGTERM → child dies
    mgr.check_and_signal();

    REQUIRE(mgr.has_exited());

    // Verify child is actually dead
    usleep(100000); // 100ms grace
    int status = 0;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == 0) {
        // Still alive somehow — force kill to not leak processes, then fail
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        FAIL("Splash process survived signal escalation");
    }
    // Child was reaped — either by SplashManager or by us, either way it's dead
    REQUIRE(result == child);
}

TEST_CASE("SplashScreenManager: cooperative splash exits on SIGUSR1", "[splash][application]") {
    // Fork a child that exits cleanly on SIGUSR1
    pid_t child = fork_cooperative_child();
    REQUIRE(child > 0);

    usleep(50000); // 50ms for child to set up signal handler

    SplashScreenManager mgr;
    mgr.start(child);
    mgr.on_discovery_complete();

    mgr.check_and_signal();

    REQUIRE(mgr.has_exited());

    // Reap the child
    int status = 0;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == 0) {
        // Give a bit more time
        usleep(100000);
        result = waitpid(child, &status, WNOHANG);
    }
    if (result <= 0) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    // Process should have exited cleanly
    CHECK(result == child);
}
