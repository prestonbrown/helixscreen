// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: exercises the shipped ESP32 header unmodified --
//                 firmware/helixscreen-esp32/components/helixnet/link_liveness.h. That
//                 is production firmware code; this gate only scans include/ and src/, so
//                 a firmware/ include reads to it as no include at all.

/**
 * @file test_esp32_link_liveness.cpp
 * @brief When the K-Touch client treats its WebSocket link as dead.
 */

#include "firmware/helixscreen-esp32/components/helixnet/link_liveness.h"

#include "../catch_amalgamated.hpp"

using helix::net::link_dead;

namespace {
constexpr int64_t S = 1000 * 1000;
constexpr int64_t BOUND = 30 * S;
} // namespace

TEST_CASE("Link liveness: frames flowing keep a link alive while PONGs are late",
          "[esp32][transport]") {
    // A weak link delays PONGs while status frames keep arriving.
    CHECK_FALSE(link_dead(100 * S, 60 * S, 95 * S, BOUND));
    CHECK_FALSE(link_dead(100 * S, 60 * S, 92 * S, BOUND));
}

TEST_CASE("Link liveness: no PONG and no frame past the bound is dead", "[esp32][transport]") {
    CHECK(link_dead(100 * S, 60 * S, 65 * S, BOUND));
}

TEST_CASE("Link liveness: a recent PONG keeps a silent link alive", "[esp32][transport]") {
    CHECK_FALSE(link_dead(100 * S, 90 * S, 40 * S, BOUND));
}

TEST_CASE("Link liveness: nothing is dead before the first connect", "[esp32][transport]") {
    CHECK_FALSE(link_dead(100 * S, 0, 0, BOUND));
}
