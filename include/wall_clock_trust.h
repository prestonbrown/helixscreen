// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix {

/// A clock reading older than this names no real moment: a device without an RTC reads
/// 1970 (or its build date) until NTP reaches it. 2020-01-01 UTC; HelixScreen first
/// shipped in 2025.
inline constexpr int64_t kEarliestTrustedClockS = 1577836800;

/// Whether @p s, seconds since the epoch, can be a real wall-clock time.
[[nodiscard]] constexpr bool wall_clock_trusted(int64_t s) {
    return s >= kEarliestTrustedClockS;
}

} // namespace helix
