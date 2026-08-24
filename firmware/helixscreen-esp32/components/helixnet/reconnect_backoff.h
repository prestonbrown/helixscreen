// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure, platform-independent predicates used by EspMoonrakerClient's reconnect
// logic (Task 9: F5/F8/R3/R4). Deliberately free of ESP-IDF/FreeRTOS includes
// so it can be compiled by both the ESP32 IDF build (esp_moonraker_client.cpp)
// and the desktop host test suite (tests/unit/test_esp32_reconnect_backoff.cpp)
// via the repo-root include path both builds already have.

#pragma once

#include <algorithm>
#include <cstdint>

namespace helix {

// Exponential backoff step: doubles the current delay, capped at max_delay_ms.
// Used by on_ws_disconnected() to compute the delay for the NEXT attempt after
// arming the current one.
inline int next_backoff_delay_ms(int current_delay_ms, int max_delay_ms) {
    return std::min(current_delay_ms * 2, max_delay_ms);
}

// R3 staleness predicate: true when a chain (discovery step, or a scheduled
// reconnect intent) captured at chain_generation no longer matches the
// client's current connection_generation_ — i.e. a manual connect(),
// force_reconnect(), or the auto-reconnect executor has since moved on to a
// new connection attempt and this chain's continuation must abandon in place.
inline bool is_stale_generation(uint64_t chain_generation, uint64_t current_generation) {
    return chain_generation != current_generation;
}

} // namespace helix
