// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

// Grants tests reach into the reclaim half of the stale-cache sweep.
//
// The sweep only proceeds on a build with a compile-time platform rung, and no
// such rung exists on x86, so the public entry point is a no-op here and the
// code that calls remove_all would never run. reclaim_cache_paths takes plain
// paths, so a test can drive it against a temp tree. TestAccess pattern ([L088])
// rather than a production _for_testing() entry.
//
// The gate's own decision is testable without this header: it lives in
// helix::cache_internal::select_stale_paths(), a pure function over a candidate
// list. And a host build must reclaim NOTHING through the public function,
// which is what keeps a developer's real ~/.cache/helix safe.
int reclaim_cache_paths(const std::vector<std::string>& paths, const char* subdir);
