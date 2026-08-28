// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: implementation of ApplicationTestFixture, not a test case. It reaches
//                 production through application_test_fixture.h, which includes
//                 include/runtime_config.h -- RuntimeConfig is the shipped type that
//                 configure_test_mode() / configure_real_moonraker() populate.

#include "application_test_fixture.h"

ApplicationTestFixture::ApplicationTestFixture() : LVGLTestFixture() {
    configure_test_mode();
}

ApplicationTestFixture::~ApplicationTestFixture() = default;

void ApplicationTestFixture::configure_test_mode() {
    m_config = RuntimeConfig{};
    m_config.test_mode = true;
    m_config.skip_splash = true;
    m_config.sim_speedup = 10.0; // Speed up tests
}

void ApplicationTestFixture::configure_real_moonraker() {
    configure_test_mode();
    m_config.use_real_moonraker = true;
}

void ApplicationTestFixture::set_sim_speedup(double speedup) {
    m_config.sim_speedup = speedup;
}

void ApplicationTestFixture::reset_mocks() {
    m_mock_state.reset();
    configure_test_mode();
}
