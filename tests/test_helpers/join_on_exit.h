// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <thread>

namespace helix::test {

/**
 * @brief Joins a worker thread on scope exit.
 *
 * A test that launches a worker and asserts before joining it destroys a
 * joinable std::thread when that assertion fails. Destroying a joinable thread
 * calls std::terminate, which takes the whole shard process down and truncates
 * every result behind it, so one ordinary assertion failure reads as a crash
 * and hides the cases that never ran. Declaring one of these right after the
 * thread routes the unwind through a join instead.
 *
 * The explicit join a test needs for sequencing stays where it is: by then the
 * thread is no longer joinable and this destructor does nothing. Only worth it
 * for a worker whose call is bounded, which is every worker in a test with
 * injected timing knobs; a guard around an unbounded wait trades an abort for
 * a hang.
 */
class JoinOnExit {
  public:
    explicit JoinOnExit(std::thread& worker) : worker_(worker) {}

    ~JoinOnExit() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    JoinOnExit(const JoinOnExit&) = delete;
    JoinOnExit& operator=(const JoinOnExit&) = delete;
    JoinOnExit(JoinOnExit&&) = delete;
    JoinOnExit& operator=(JoinOnExit&&) = delete;

  private:
    std::thread& worker_;
};

} // namespace helix::test
