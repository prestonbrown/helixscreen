// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_current_loaded_log.cpp
 * @brief The "Currently Loaded" sync must be silent in a steady state.
 *
 * A backend status frame arrives several times a second for the whole duration
 * of a print, and each one re-runs AmsState::sync_current_loaded_from_backend().
 * Every subject write in there is guarded against a no-op; a log line that is
 * not costs the debug bundle its ring, which is the only record a user can send
 * of what every other subsystem was doing.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The debug line the loaded-slot branch emits.
constexpr const char* kSyncLine = "sync_current_loaded: slot=";

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState reports a loaded slot on change, not per status frame",
                 "[ams][ams_state][logging]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    // AmsState is a singleton, so it carries whatever the previous test left it
    // tracking. With no backends the sync resets the card, and the tracker with
    // it, which makes the load below a change from a known starting point.
    ams.clear_backends();
    ams.sync_current_loaded_from_backend();

    auto mock = std::make_unique<AmsBackendMock>(4);
    auto* backend = mock.get();
    ams.set_backend(std::move(mock));
    REQUIRE(backend->is_filament_loaded());
    REQUIRE(backend->get_current_slot() == 0);

    // Exclusive: the mock runs its operations on real threads, and pushing a
    // sink onto the shared logger races their sink_it_ iteration.
    ExclusiveLogCapture log(1024);

    ams.sync_current_loaded_from_backend();
    REQUIRE(log.count_containing(kSyncLine) == 1);

    for (int i = 0; i < 20; ++i) {
        ams.sync_current_loaded_from_backend();
    }
    CHECK(log.count_containing(kSyncLine) == 1);
}
