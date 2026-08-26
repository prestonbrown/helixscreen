// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

// Grants tests reach into the reclaim half of the stale-cache sweep.
//
// sweep_stale_helix_cache_dirs() only proceeds on a build that HAS a
// compile-time platform rung (HELIX_PLATFORM_MIPS and friends). No such rung is
// defined in a host build, so the public entry point is a guaranteed no-op here
// and the code that actually calls remove_all would never be exercised.
// reclaim_cache_paths is the half below that gate, and it takes plain paths, so
// a test can drive it against a temp tree. Follows the tests/test_helpers/
// TestAccess pattern ([L088]) rather than a production _for_testing() entry.
//
// The gate's own decision is testable without this header: it lives in
// helix::cache_internal::select_stale_paths(), a pure function over a candidate
// list, so a test can build the embedded shapes a host build never produces.
//
// The desktop property is covered from the other side: a host build must
// reclaim NOTHING through the public function, which is what keeps a
// developer's real ~/.cache/helix safe.
int reclaim_cache_paths(const std::vector<std::string>& paths, const char* subdir);
