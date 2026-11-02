// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "memory_monitor.h"

#include <string>

namespace helix {

// Friend access to MemoryMonitor::fire_warning(). The pressure-response
// completion path — which hop the RSS sample takes relative to the responders'
// deferred widget deletes — is only observable by dispatching a real warning;
// no public API reaches it without a live monitor thread crossing real
// thresholds. Follows the tests/test_helpers/ TestAccess pattern.
class MemoryMonitorTestAccess {
  public:
    static void fire_warning(MemoryMonitor& monitor, MemoryPressureLevel level,
                             const std::string& reason, const MemoryStats& stats,
                             const MemoryInfo& sys_info, int64_t growth_kb) {
        monitor.fire_warning(level, reason, stats, sys_info, growth_kb, /*report=*/true,
                             /*dispatch=*/true);
    }
};

} // namespace helix
