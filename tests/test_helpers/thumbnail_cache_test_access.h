// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "thumbnail_cache.h"

#include <mutex>

/// Friend access to ThumbnailCache internals a test cannot reach otherwise.
class ThumbnailCacheTestAccess {
  public:
    /// Hold the index lock the way eviction and a rescan do while they walk the
    /// directory, stat each file and unlink.
    static std::unique_lock<std::mutex> hold_index_lock(ThumbnailCache& cache) {
        return std::unique_lock<std::mutex>(cache.mutex_);
    }
};
