// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix {

/**
 * @brief Undo an IPv4 change the user never kept, once per app start
 *
 * A pending-revert record another run left means the app or the device
 * restarted mid-countdown (prestonbrown/helixscreen#1900). The snapshot is
 * restored on a worker, retried on helix::net::Ipv4RetrySchedule while the
 * network comes up, and a notice says so when it never succeeds. LVGL thread.
 */
void start_ipv4_startup_revert();

} // namespace helix
