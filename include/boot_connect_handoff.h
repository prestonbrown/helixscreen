// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

namespace helix {

/**
 * @brief Decides when the boot's first Moonraker connect may start (ESP32).
 *
 * The thread that brings WiFi up holds the largest internal-RAM block on the
 * boot path as its stack. The WebSocket task the connect starts needs an 8 KB
 * internal stack of its own; allocated while the boot thread's stack is still
 * held, it splits the largest free block for the rest of the session, which is
 * the margin the WebSocket client start fails on. So the connect waits for both
 * signals, then for one more poll: a thread's stack is freed by the idle task
 * after the thread returns, and the UI thread sleeps between polls.
 *
 * wifi_up() and thread_exited() may be called from any thread; poll() runs on
 * the UI thread and answers Connect exactly once.
 */
class BootConnectHandoff {
  public:
    enum class Step { Wait, Connect, Done };

    void wifi_up() {
        wifi_up_.store(true, std::memory_order_release);
    }
    void thread_exited() {
        exited_.store(true, std::memory_order_release);
    }

    Step poll() {
        if (done_) {
            return Step::Done;
        }
        if (!wifi_up_.load(std::memory_order_acquire) || !exited_.load(std::memory_order_acquire)) {
            return Step::Wait;
        }
        if (!exit_seen_) {
            exit_seen_ = true; // one poll for the idle task to free the stack
            return Step::Wait;
        }
        done_ = true;
        return Step::Connect;
    }

  private:
    std::atomic<bool> wifi_up_{false};
    std::atomic<bool> exited_{false};
    bool exit_seen_ = false;
    bool done_ = false;
};

} // namespace helix
