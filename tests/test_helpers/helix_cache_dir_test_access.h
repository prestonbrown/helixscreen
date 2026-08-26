// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

// Grants tests reach into the reclaim half of the stale-cache sweep.
//
// sweep_stale_helix_cache_dirs() gates on a compile-time platform rung winning
// the cascade (HELIX_PLATFORM_MIPS and friends). No such rung is defined in a
// host build, so the public entry point is a guaranteed no-op here and the code
// that actually calls remove_all would never be exercised. reclaim_cache_paths
// is the half below that gate, and it takes plain paths, so a test can drive it
// against a temp tree. Follows the tests/test_helpers/ TestAccess pattern
// ([L088]) rather than adding a production _for_testing() entry point.
//
// The gate itself is covered from the other side: a host build must reclaim
// NOTHING through the public function, which is the property that keeps a
// developer's real ~/.cache/helix safe.
int reclaim_cache_paths(const std::vector<std::string>& paths, const char* subdir);
