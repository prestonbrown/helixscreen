// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file helix_cache_dir_internal.h
 * @brief Internals of the cache cascade, exposed so the sweep is testable.
 *
 * The stale-cache sweep decides what to delete from the shape of the candidate
 * list, but which rungs exist is a compile-time question (HELIX_PLATFORM_*).
 * A host build has no platform rung, so driving sweep_stale_helix_cache_dirs()
 * there can only ever prove the desktop case. Exposing the decision as a pure
 * function over a candidate list lets a test build the embedded shapes directly
 * instead of mirroring the logic ([L088] TestAccess pattern, seam in production).
 */

#include <functional>
#include <string>
#include <vector>

namespace helix::cache_internal {

/// One rung of the cache cascade.
struct CacheCandidate {
    std::string path;
    /// Labels the rung in the log; nullptr means resolve quietly. Non-null also
    /// marks the rung as *deliberate* - somebody chose this path on purpose.
    const char* tier = nullptr;
    bool ram_backed = false;
    /// True for the compile-time platform rung. Its presence in the list marks
    /// an embedded build; it does not need to win (every device ships a hook
    /// exporting HELIX_CACHE_DIR, so rung 1 wins in practice on all of them).
    bool platform = false;
};

/// A rung somebody chose on purpose: env override, config setting, or the
/// compile-time platform path. Never reclaimed.
inline bool is_deliberate(const CacheCandidate& c) {
    return c.tier != nullptr;
}

/**
 * @brief Paths safe to reclaim, given one subdir's candidate list.
 *
 * Empty unless the list contains a platform rung (i.e. this is an embedded
 * build). Returns the non-deliberate rungs *below* the first viable one:
 * rungs above it were rejected as unusable, rungs below were never probed.
 *
 * @param candidates The cascade for one subdir, in priority order.
 * @param viable     Predicate deciding whether a candidate could be used.
 */
std::vector<std::string> select_stale_paths(const std::vector<CacheCandidate>& candidates,
                                            const std::function<bool(const std::string&)>& viable);

} // namespace helix::cache_internal
