// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The boot's first Moonraker connect waits for WiFi AND for the boot thread to
// have exited, plus one poll for its stack to be freed, and starts exactly once.

#include "boot_connect_handoff.h"

#include "../catch_amalgamated.hpp"

using helix::BootConnectHandoff;
using Step = BootConnectHandoff::Step;

TEST_CASE("BootConnectHandoff waits for the boot thread to exit after WiFi is up",
          "[boot_connect_handoff]") {
    BootConnectHandoff h;
    h.wifi_up();
    for (int i = 0; i < 5; ++i) {
        REQUIRE(h.poll() == Step::Wait);
    }
    h.thread_exited();
    REQUIRE(h.poll() == Step::Wait); // the stack is freed after the thread returns
    REQUIRE(h.poll() == Step::Connect);
}

TEST_CASE("BootConnectHandoff waits for WiFi after the boot thread exits",
          "[boot_connect_handoff]") {
    BootConnectHandoff h;
    h.thread_exited();
    for (int i = 0; i < 5; ++i) {
        REQUIRE(h.poll() == Step::Wait);
    }
    h.wifi_up();
    REQUIRE(h.poll() == Step::Wait);
    REQUIRE(h.poll() == Step::Connect);
}

TEST_CASE("BootConnectHandoff connects exactly once", "[boot_connect_handoff]") {
    BootConnectHandoff h;
    h.wifi_up();
    h.thread_exited();
    REQUIRE(h.poll() == Step::Wait);
    REQUIRE(h.poll() == Step::Connect);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(h.poll() == Step::Done);
    }
}
