// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_source_store.h"

namespace helix::ams {

/// Empties the process-wide lane store between tests. The store outlives any
/// one TEST_CASE, so a lane another test wrote would read back as a lane this
/// one never touched.
class LaneSourceStoreTestAccess {
  public:
    static void clear() {
        auto& store = LaneSourceStore::instance();
        std::lock_guard<std::mutex> lock(store.mutex_);
        store.lanes_.clear();
    }
};

} // namespace helix::ams
