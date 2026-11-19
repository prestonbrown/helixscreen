// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::main_loop_wake {

/// Cuts a sleeping main loop's wait short so queued work is handled now rather than at
/// the next timer. Safe from any thread; costs one write when nothing waits.
void notify() noexcept;

/// The descriptor that turns readable after notify(), or -1 when none could be made.
int fd() noexcept;

/// Clears the pending notifications. Main thread, after a wait returns.
void drain() noexcept;

} // namespace helix::main_loop_wake
