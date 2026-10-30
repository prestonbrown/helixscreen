// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WifiBackendMock: the radio switches off and on without stopping the backend.

#include "wifi_backend.h"

#include "../catch_amalgamated.hpp"

#ifdef HELIX_ENABLE_MOCKS
#include "wifi_backend_mock.h"

TEST_CASE("Mock backend tracks radio state independently of running state", "[wifi][radio]") {
    WifiBackendMock backend;
    REQUIRE(backend.start().success());

    CHECK(backend.is_radio_enabled());

    REQUIRE(backend.set_radio_enabled(false).success());
    CHECK_FALSE(backend.is_radio_enabled());
    // Radio off must NOT tear the backend down — the control path stays up so
    // state can be reported and the radio switched back on.
    CHECK(backend.is_running());

    REQUIRE(backend.set_radio_enabled(true).success());
    CHECK(backend.is_radio_enabled());
}
#endif
