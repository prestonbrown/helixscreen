// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: tests the test fixture itself (ApplicationTestFixture and
//                 MockPrinterState), which this gate exempts by design. No production
//                 logic is reimplemented here.

/**
 * @file test_infrastructure.cpp
 * @brief Tests to verify the application test infrastructure itself
 *
 * These tests ensure our test fixtures and helpers work correctly
 * before relying on them for module tests.
 */

#include "application_test_fixture.h"

#include "../../catch_amalgamated.hpp"

// ============================================================================
// Test Infrastructure Verification
// ============================================================================

TEST_CASE("Application test infrastructure is functional", "[application][infrastructure]") {
    SECTION("Basic test execution works") {
        REQUIRE(1 + 1 == 2);
    }

    SECTION("String operations work") {
        std::string test = "refactoring";
        REQUIRE(test == "refactoring");
    }
}

TEST_CASE_METHOD(ApplicationTestFixture, "ApplicationTestFixture initializes correctly",
                 "[application][infrastructure]") {
    SECTION("Fixture provides valid test screen") {
        REQUIRE(test_screen() != nullptr);
    }

    SECTION("Fixture provides test mode config") {
        REQUIRE(config().test_mode == true);
    }

    SECTION("Fixture provides mock state") {
        // Should be initialized to defaults
        REQUIRE(mock_state().extruder_temp == 25.0);
        REQUIRE(mock_state().bed_temp == 25.0);
    }
}

TEST_CASE_METHOD(ApplicationTestFixture, "Mock printer state is thread-safe",
                 "[application][infrastructure]") {
    SECTION("Atomic temperature updates") {
        mock_state().extruder_temp = 100.0;
        mock_state().extruder_target = 200.0;

        REQUIRE(mock_state().extruder_temp == 100.0);
        REQUIRE(mock_state().extruder_target == 200.0);
    }

    SECTION("Object exclusion with mutex") {
        mock_state().add_excluded_object("Part_1");
        mock_state().add_excluded_object("Part_2");

        auto excluded = mock_state().get_excluded_objects();
        REQUIRE(excluded.size() == 2);
        REQUIRE(excluded.count("Part_1") == 1);
        REQUIRE(excluded.count("Part_2") == 1);
    }

    SECTION("Available objects list") {
        std::vector<std::string> objects = {"Obj_A", "Obj_B", "Obj_C"};
        mock_state().set_available_objects(objects);

        auto retrieved = mock_state().get_available_objects();
        REQUIRE(retrieved.size() == 3);
        REQUIRE(retrieved[0] == "Obj_A");
    }
}
