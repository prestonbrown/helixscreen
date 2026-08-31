// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "thumbnail_processor.h"

#include <hv/hthreadpool.h>

#include <memory>

/// Grants tests the private constructor and the private pool handle.
/// Declared a friend in include/thumbnail_processor.h.
struct ThumbnailProcessorTestAccess {
    // Raw pointers, deliberately: both the ctor AND the dtor are private, so
    // unique_ptr's default deleter cannot be instantiated outside this friend.
    static helix::ThumbnailProcessor* make() {
        return new helix::ThumbnailProcessor();
    }
    static void destroy(helix::ThumbnailProcessor* p) {
        delete p;
    }

    /// The worker pool. The isolation listener pre-warms it to its
    /// steady-state max so on-demand growth is never misread as a per-test
    /// thread leak.
    static std::shared_ptr<HThreadPool> pool(helix::ThumbnailProcessor& p) {
        return p.thread_pool_;
    }
};
