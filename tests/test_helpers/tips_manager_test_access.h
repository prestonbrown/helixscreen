// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/tips_manager_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tips_manager.h"

#include <string>

namespace helix {
/// Keeps the tips singleton, between cases, as a fresh process has it: not
/// loaded.
///
/// TipsManager reads a database on the first get_instance(), so one case that
/// builds the tips tile, or loads a database of its own, leaves real titles
/// in front of every later case. A measurement that expects the tile's own
/// chrome then measures a title instead, a different one on every run.
struct TipsManagerTestAccess {
    /// Return the singleton, if one exists, to its never-loaded state: the next
    /// get_instance() reads the shipped database again. A case that pinned the
    /// cache to one title, or init()ed it from its own file, would otherwise
    /// leave every later widget on that data.
    static void reset_if_created() {
        TipsManager* mgr = TipsManager::instance;
        if (!mgr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mgr->tips_mutex);
        mgr->path.clear();
        mgr->data = decltype(mgr->data){};
        mgr->tips_cache.clear();
        mgr->viewed_tip_ids_.clear();
        mgr->categories_.clear();
        mgr->version_ = "unknown";
        mgr->load_attempted_ = false;
    }
};

} // namespace helix
