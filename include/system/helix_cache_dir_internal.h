// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file helix_cache_dir_internal.h
 * @brief Internals of the cache cascade, exposed so the sweep is testable.
 *
 * Which rungs exist is a compile-time question (HELIX_PLATFORM_*), so a host
 * build can only ever exercise the desktop case through the public entry point.
 * Exposing the decision as a pure function over a candidate list lets a test
 * build the embedded shapes directly instead of mirroring the logic.
 */

#include <functional>
#include <string>
#include <vector>

namespace helix::cache_internal {

/// One rung of the cache cascade.
struct CacheCandidate {
    std::string path;
    /// Labels the rung in the log; nullptr means resolve quietly. Non-null also
    /// marks the rung deliberate - chosen on purpose, never reclaimed.
    const char* tier = nullptr;
    bool ram_backed = false;
    /// True for the compile-time platform rung. Its presence marks an embedded
    /// build; it need not win, since every device hook exports HELIX_CACHE_DIR.
    bool platform = false;
};

/// Env override, config setting, or platform path. Never reclaimed.
inline bool is_deliberate(const CacheCandidate& c) {
    return c.tier != nullptr;
}

/**
 * @brief Paths safe to reclaim, given one subdir's candidate list.
 *
 * Empty unless the list contains a platform rung. Returns the non-deliberate
 * rungs below the first viable one: those above were rejected as unusable,
 * those below were never probed.
 *
 * @param candidates The cascade for one subdir, in priority order.
 * @param viable     Predicate deciding whether a candidate could be used.
 */
std::vector<std::string> select_stale_paths(const std::vector<CacheCandidate>& candidates,
                                            const std::function<bool(const std::string&)>& viable);

} // namespace helix::cache_internal
