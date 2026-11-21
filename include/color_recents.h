// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace helix {

/**
 * The colors a user committed recently, one list per kind, newest first,
 * de-duplicated and capped at kCap. A color joins its list when a host commits
 * it (the slot editor's Save, the LED picker's Done), never while browsing.
 * Stored in settings (global, not per printer). A list never written yet reads
 * as its seed, so a fresh install's strip is not empty.
 */
class ColorRecents {
  public:
    enum class Kind : uint8_t { Filament, Led };

    static constexpr size_t kCap = 12;

    static std::vector<uint32_t> list(Kind kind);
    /// Moves @p rgb to the front, adding it when absent; drops the oldest past kCap.
    static void remember(Kind kind, uint32_t rgb);
    /// remember() for several colors in one settings write: @p colors[0]
    /// ends up first, the rest after it in order.
    static void remember_all(Kind kind, const std::vector<uint32_t>& colors);
    static void remove(Kind kind, uint32_t rgb);
    /// The fresh-install list.
    static const std::vector<uint32_t>& seed(Kind kind);
};

} // namespace helix
