// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_static_subject_registry.cpp
 * @brief A subject source that is rebuilt must not leave its old deinit behind.
 *
 * Entries are keyed by name because the name identifies a subject source, not a
 * registration event. A source that is torn down and re-created re-registers
 * under the same name; if both entries survived, shutdown would run a callback
 * closed over the destroyed instance alongside the live one.
 *
 * Uses deinit_one() rather than deinit_all() so the case never touches the other
 * entries the process-wide registry is holding for the rest of the suite.
 */

#include "../helix_test_fixture.h"
#include "static_subject_registry.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(HelixTestFixture, "StaticSubjectRegistry keeps one entry per name",
                 "[core][subjects]") {
    auto& registry = StaticSubjectRegistry::instance();
    static constexpr const char* kName = "StaticSubjectRegistryTestProbe";

    std::vector<std::string> ran;
    registry.register_deinit(kName, [&ran]() { ran.emplace_back("first"); });
    registry.register_deinit(kName, [&ran]() { ran.emplace_back("second"); });

    REQUIRE(registry.deinit_one(kName));
    // The live registration wins, and nothing from the superseded one is left.
    CHECK(ran == std::vector<std::string>{"second"});
    CHECK_FALSE(registry.deinit_one(kName));
}
