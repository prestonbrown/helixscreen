// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_bypass_preflight_wiring.cpp
 * @brief AmsState::any_bypass_active() — the signal the pre-print check reads.
 *
 * PreflightValidator reasons purely over AmsState::collect_available_slots(), and
 * bypass is deliberately not a slot in that vector. So when the machine prints
 * from its bypass / external spool, no mapping can satisfy the gcode's T0 and the
 * validator would block every such print with "T0 has no filament loaded". The
 * validator's escape hatch is a bypass flag supplied by the caller, and this is
 * the accessor that supplies it.
 *
 * Pinned here separately from test_preflight_validator.cpp because the validator
 * tests are pure and never touch AmsState: a correct validator wired to a signal
 * that is always false would leave the bug fully intact and every one of those
 * tests green.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Install a started mock backend and hand back the raw pointer.
AmsBackendMock* install_mock(AmsState& ams, int slots) {
    auto mock = std::make_unique<AmsBackendMock>(slots);
    auto* raw = mock.get();
    ams.set_backend(std::move(mock));
    raw->start();
    return raw;
}

/// Clear AmsState's backends on scope exit. AmsState is a singleton, so a test
/// that installs a backend and walks away leaves it live for every test that
/// runs afterwards. Mirrors ScopedAd5xIfsBackend in
/// test_print_start_filament_gate.cpp.
struct ScopedBackends {
    ~ScopedBackends() {
        AmsState::instance().set_backend(nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState reports bypass to the pre-print check",
                 "[ams][ams_state][bypass][preflight_validator]") {
    ScopedBackends cleanup;
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto* backend = install_mock(ams, 4);
    REQUIRE(backend != nullptr);

    SECTION("idle machine is not in bypass") {
        REQUIRE_FALSE(backend->is_bypass_active());
        CHECK_FALSE(ams.any_bypass_active());
    }

    SECTION("engaging bypass is visible, and disengaging clears it") {
        REQUIRE(backend->enable_bypass().success());
        REQUIRE(backend->is_bypass_active());
        CHECK(ams.any_bypass_active());

        // Must not latch: a print started after the user pulls the bypass filament
        // has to get the slot checks back.
        REQUIRE(backend->disable_bypass().success());
        REQUIRE_FALSE(backend->is_bypass_active());
        CHECK_FALSE(ams.any_bypass_active());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "engaging bypass bumps slots_version so the check re-runs",
                 "[ams][ams_state][bypass][preflight_validator]") {
    ScopedBackends cleanup;
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    // Bypass is toggled on a SECOND backend on purpose. AmsBackendMock's
    // enable_bypass() moves its own current_slot/filament_loaded, which the
    // primary slot-delta scan in sync_from_backend() already notices — so a
    // single-backend version of this test passes with the bump deleted and proves
    // nothing. Toggling the secondary reproduces the real shape: bypass engages,
    // no slot anywhere changes, and only the explicit edge bump can fire.
    install_mock(ams, 4);
    auto secondary_owned = std::make_unique<AmsBackendMock>(4);
    auto* backend = secondary_owned.get();
    REQUIRE(ams.add_backend(std::move(secondary_owned)) == 1);
    backend->start();

    // slots_version is the ONLY thing that re-runs PrintSelectDetailView's cached
    // pre-flight result. Without an explicit bump, engaging bypass with a file
    // already open leaves the stale block in place.
    auto version = [&] { return lv_subject_get_int(ams.get_slots_version_subject()); };

    ams.sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    const int before = version();

    REQUIRE(backend->enable_bypass().success());
    ams.sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ams.any_bypass_active());
    CHECK(version() > before);

    // Idempotent: a steady bypass must not bump on every status frame, or the
    // detail view recomputes pre-flight continuously for the whole print.
    const int steady = version();
    ams.sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(version() == steady);

    // And the falling edge has to bump too, or the checks never come back.
    REQUIRE(backend->disable_bypass().success());
    ams.sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ams.any_bypass_active());
    CHECK(version() > steady);
}

TEST_CASE_METHOD(LVGLTestFixture, "AmsState bypass check covers every backend, not just the first",
                 "[ams][ams_state][bypass]") {
    ScopedBackends cleanup;
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    // set_backend() clears and installs backend 0; add_backend() appends backend 1.
    auto* primary = install_mock(ams, 4);
    auto secondary_owned = std::make_unique<AmsBackendMock>(4);
    auto* secondary = secondary_owned.get();
    REQUIRE(ams.add_backend(std::move(secondary_owned)) == 1);
    secondary->start();

    REQUIRE_FALSE(ams.any_bypass_active());

    // Bypass on the SECOND backend only. A get_backend()-based implementation
    // (backend 0 alone) reports false here and the false block returns.
    REQUIRE(secondary->enable_bypass().success());
    REQUIRE_FALSE(primary->is_bypass_active());
    REQUIRE(secondary->is_bypass_active());
    CHECK(ams.any_bypass_active());
}
