// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file usb_scan_wait.h
 * @brief Let an off-thread USB scan finish and deliver.
 *
 * Scans run on HttpExecutor::fast() and deliver through UpdateQueue. The wait
 * gates on the scanning object's own in-flight flag, which clears only once
 * its result is delivered on the UI thread. The executor's inflight() count
 * is not that condition: it counts every item on the lane, including other
 * tests' slow requests, and a walk can sit queued behind them.
 */

#include "ui_update_queue.h"

#include <chrono>
#include <functional>
#include <thread>

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// Drain the UI queue until @p scanning reports false. A delivered result can
/// start the next scan, so the flag is read after each drain. The bound only
/// turns a hang into a failure; it is not part of the condition.
inline void wait_for_usb_scan(const std::function<bool()>& scanning) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    helix::ui::UpdateQueue::instance().drain();
    while (scanning()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        helix::ui::UpdateQueue::instance().drain();
    }
    // The predicate can turn false between a drain and its check, with the
    // result already queued.
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace helix::test
