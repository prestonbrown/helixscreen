// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "image_cache_env.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("HELIX_IMAGE_CACHE_KB sizes the image cache up to its 64 MB cap", "[image_cache]") {
    CHECK(helix::image_cache_bytes(0) == 0u);
    CHECK(helix::image_cache_bytes(-5) == 0u);
    CHECK(helix::image_cache_bytes(2048) == 2048u * 1024u);
    CHECK(helix::image_cache_bytes(helix::kImageCacheMaxKb) == 65536u * 1024u);
    CHECK(helix::image_cache_bytes(helix::kImageCacheMaxKb + 1) == 65536u * 1024u);
    // Past 4194303 KB the byte count wraps a uint32_t: 4194304 KB would read as an empty cache.
    CHECK(helix::image_cache_bytes(4194304) == 65536u * 1024u);
}
