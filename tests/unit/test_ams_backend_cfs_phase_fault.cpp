// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The fault text a CFS action leaves standing, and what a language switch does to it.

#include "../helix_test_fixture.h"
#include "ams_backend_cfs.h"
#include "ams_types.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/cfs_box_fixtures.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/scoped_language.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;

TEST_CASE("CFS re-words the fault on a spool that ran out when the language switches",
          "[ams][cfs][i18n]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendCfs> backend;

    // The box's runout latch is what words the fault, so the box has spoken first.
    backend.fire_status(helix::test::make_cfs_status_json());
    REQUIRE(backend.get_system_info().filament_runout);

    // Raised in German: the shown text has to be one a switch can move. An
    // unload that leaves filament at the nozzle of a used-up spool is the one
    // verdict stored already composed from two catalog keys; the others are
    // stored as a single key.
    ScopedLanguage de("de");
    CfsTestAccess::force_phase_intent(backend, AmsAction::UNLOADING);
    CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/true);
    CfsTestAccess::complete_action(backend);
    REQUIRE(backend.get_system_info().action == AmsAction::ERROR);
    INFO(backend.get_system_info().operation_detail);
    REQUIRE(backend.get_system_info().operation_detail.rfind("Diese Spule ist leer", 0) == 0);

    // No new frame from the box: the backend re-words the fault it raised.
    ScopedLanguage fr("fr");
    backend.refresh_translated_texts();
    INFO(backend.get_system_info().operation_detail);
    CHECK(backend.get_system_info().operation_detail.rfind("Cette bobine est épuisée", 0) == 0);
}
