// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "watchdog_isolated_dialog.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix::watchdog;

namespace {
volatile sig_atomic_t g_test_quit = 0;
void set_test_quit(int) {
    g_test_quit = 1;
}
} // namespace

TEST_CASE("isolated dialog returns the child's answer", "[watchdog][isolated-dialog]") {
    g_test_quit = 0;
    const auto result = run_isolated_dialog([] { return 3; }, g_test_quit);
    CHECK(result.outcome == IsolatedDialogOutcome::COMPLETED);
    CHECK(result.exit_code == 3);
}

TEST_CASE("isolated dialog survives a child that crashes", "[watchdog][isolated-dialog]") {
    g_test_quit = 0;
    const auto result = run_isolated_dialog(
        []() -> int {
            signal(SIGSEGV, SIG_DFL);
            raise(SIGSEGV);
            return 0;
        },
        g_test_quit);
    CHECK(result.outcome == IsolatedDialogOutcome::CHILD_DIED);
    CHECK(result.term_signal == SIGSEGV);
}

TEST_CASE("isolated dialog does not touch the caller's memory", "[watchdog][isolated-dialog]") {
    g_test_quit = 0;
    int caller_state = 7;
    const auto result = run_isolated_dialog(
        [&caller_state] {
            caller_state = 99;
            return 0;
        },
        g_test_quit);
    CHECK(result.outcome == IsolatedDialogOutcome::COMPLETED);
    CHECK(caller_state == 7);
}

TEST_CASE("isolated dialog kills a hung child when the supervisor quits",
          "[watchdog][isolated-dialog]") {
    g_test_quit = 0;
    struct sigaction action {};
    struct sigaction previous {};
    action.sa_handler = set_test_quit;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    REQUIRE(sigaction(SIGUSR2, &action, &previous) == 0);

    const pid_t waiter_pid = getpid();
    std::thread signaller([waiter_pid] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        kill(waiter_pid, SIGUSR2);
    });

    const auto start = std::chrono::steady_clock::now();
    const auto result = run_isolated_dialog(
        []() -> int {
            for (;;) {
                pause();
            }
        },
        g_test_quit);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    signaller.join();
    sigaction(SIGUSR2, &previous, nullptr);

    CHECK(result.outcome == IsolatedDialogOutcome::INTERRUPTED);
    CHECK(elapsed < std::chrono::seconds(5));
}
