// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace helix {

/// Largest HELIX_IMAGE_CACHE_KB honoured: 64 MB of decoded images, the same bound the
/// launcher applies to helixscreen.env. A value exported by a shell or a unit file reaches
/// the app without that check.
constexpr long kImageCacheMaxKb = 65536;

/// LVGL image cache size in bytes for HELIX_IMAGE_CACHE_KB=kb: 0 for none, at most
/// kImageCacheMaxKb. Uncapped, kb * 1024 wraps a uint32_t above 4194303 KB.
inline uint32_t image_cache_bytes(long kb) {
    if (kb <= 0) {
        return 0;
    }
    const long capped = kb > kImageCacheMaxKb ? kImageCacheMaxKb : kb;
    return static_cast<uint32_t>(capped) * 1024u;
}

} // namespace helix
