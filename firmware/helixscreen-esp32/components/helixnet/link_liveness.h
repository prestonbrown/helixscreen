// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix::net {

/// Whether a connection marked up has stopped carrying traffic: no PONG for our PINGs and
/// no other frame for longer than @p bound_us. Frames still arriving mean the link is
/// alive, only slow, whatever the PONGs are doing. @p last_pong_us is 0 before the first
/// connect.
constexpr bool link_dead(int64_t now_us, int64_t last_pong_us, int64_t last_rx_us,
                         int64_t bound_us) {
    return last_pong_us > 0 && now_us - last_pong_us > bound_us && now_us - last_rx_us > bound_us;
}

} // namespace helix::net
