// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_update_queue.h"

#include "http_executor.h"

#include <chrono>
#include <thread>

namespace helix::test {

/// Run @p drain until HttpExecutor::slow() has nothing in flight, so work
/// submitted there (install.sh runs) and the outcome it queues back have both
/// landed. An outcome drained here may submit the next run, which is waited
/// for too. Gives up after @p limit.
template <typename Drain>
void settle_slow_lane(Drain&& drain, std::chrono::seconds limit = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    drain();
    while (helix::http::HttpExecutor::slow().inflight() > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        drain();
    }
    drain();
}

/// settle_slow_lane() draining the update queue.
inline void settle_slow_lane() {
    settle_slow_lane([] { helix::ui::UpdateQueue::instance().drain(); });
}

} // namespace helix::test
