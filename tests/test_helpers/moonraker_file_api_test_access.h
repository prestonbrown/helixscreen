// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_file_api.h"

#include <string>

namespace helix {

// Grants tests visibility into MoonrakerFileAPI's metascan single-flight gate
// (in-flight set + completion cooldown). Declared a friend of MoonrakerFileAPI
// (see moonraker_file_api.h). Follows the existing TestAccess pattern
// (tests/test_helpers/) rather than adding production _for_testing() accessors.
class MoonrakerFileApiTestAccess {
  public:
    static bool metascan_in_flight(const MoonrakerFileAPI& api, const std::string& path) {
        return api.metascan_in_flight_.count(path) != 0;
    }

    static bool metascan_in_cooldown(const MoonrakerFileAPI& api, const std::string& path) {
        return api.metascan_cooldown_until_.find(path) != api.metascan_cooldown_until_.end();
    }

    /// Expire the cooldown for a path without waiting out the real TTL.
    static void expire_metascan_cooldown(MoonrakerFileAPI& api, const std::string& path) {
        std::lock_guard<std::mutex> lock(api.metascan_gate_mutex_);
        api.metascan_cooldown_until_.erase(path);
    }

    /// Pretend a scan for this path never completed, so the next call is
    /// judged against the in-flight guard alone.
    static void force_metascan_in_flight(MoonrakerFileAPI& api, const std::string& path) {
        std::lock_guard<std::mutex> lock(api.metascan_gate_mutex_);
        api.metascan_in_flight_.insert(path);
    }
};

} // namespace helix
