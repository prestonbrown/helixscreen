// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_debug_registry.cpp
 * @brief Unit tests for SubjectDebugRegistry
 *
 * SubjectDebugRegistry provides debug information for LVGL subjects,
 * mapping subject pointers to metadata (name, type, file, line).
 */

#include "ui_subject_registry.h"

#include "../lvgl_test_fixture.h"
#include "core/subject.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"

#include "../catch_amalgamated.hpp"

// The subject macros feed desktop debug tooling (LVGL subject warnings, the
// name lookups below); only the ESP32 firmware may compile the registry out.
static_assert(HELIX_SUBJECT_DEBUG_REGISTRY_ENABLED,
              "the subject debug registry is compiled out of a non-ESP32 build");

// ============================================================================
// Register and Lookup Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "SubjectDebugRegistry: register subject and look it up by pointer",
                 "[subject_debug][registry][basic]") {
    SubjectDebugRegistry& registry = SubjectDebugRegistry::instance();
    registry.clear();

    // Create a dummy subject pointer (we just need an address, not a real subject)
    helix::Subject dummy_subject = {};

    registry.register_subject(&dummy_subject, "test_subject", LV_SUBJECT_TYPE_INT, __FILE__,
                              __LINE__);

    const SubjectDebugInfo* info = registry.lookup(&dummy_subject);
    REQUIRE(info != nullptr);
    REQUIRE(info->name == "test_subject");
    REQUIRE(info->type == LV_SUBJECT_TYPE_INT);
    REQUIRE(info->file == __FILE__);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "SubjectDebugRegistry: lookup non-existent subject returns nullptr",
                 "[subject_debug][registry][basic]") {
    SubjectDebugRegistry& registry = SubjectDebugRegistry::instance();
    registry.clear();

    // Create a dummy subject pointer that was never registered
    helix::Subject unregistered_subject = {};

    const SubjectDebugInfo* info = registry.lookup(&unregistered_subject);
    REQUIRE(info == nullptr);
}

// ============================================================================
// type_name() Tests
// ============================================================================

TEST_CASE("SubjectDebugRegistry: type_name returns correct strings for each type",
          "[subject_debug][registry][type_name]") {
    SECTION("LV_SUBJECT_TYPE_INVALID") {
        REQUIRE(SubjectDebugRegistry::type_name(LV_SUBJECT_TYPE_INVALID) == std::string("INVALID"));
    }

    SECTION("LV_SUBJECT_TYPE_NONE") {
        REQUIRE(SubjectDebugRegistry::type_name(LV_SUBJECT_TYPE_NONE) == std::string("NONE"));
    }

    SECTION("LV_SUBJECT_TYPE_INT") {
        REQUIRE(SubjectDebugRegistry::type_name(LV_SUBJECT_TYPE_INT) == std::string("INT"));
    }

    SECTION("LV_SUBJECT_TYPE_FLOAT") {
        REQUIRE(SubjectDebugRegistry::type_name(LV_SUBJECT_TYPE_FLOAT) == std::string("FLOAT"));
    }

    SECTION("LV_SUBJECT_TYPE_POINTER") {
        REQUIRE(SubjectDebugRegistry::type_name(LV_SUBJECT_TYPE_POINTER) == std::string("POINTER"));
    }

    SECTION("LV_SUBJECT_TYPE_COLOR") {
        REQUIRE(SubjectDebugRegistry::type_name(LV_SUBJECT_TYPE_COLOR) == std::string("COLOR"));
    }

    SECTION("LV_SUBJECT_TYPE_GROUP") {
        REQUIRE(SubjectDebugRegistry::type_name(LV_SUBJECT_TYPE_GROUP) == std::string("GROUP"));
    }

    SECTION("LV_SUBJECT_TYPE_STRING") {
        REQUIRE(SubjectDebugRegistry::type_name(LV_SUBJECT_TYPE_STRING) == std::string("STRING"));
    }

    SECTION("Unknown type returns UNKNOWN") {
        // Use a type value that doesn't exist
        REQUIRE(SubjectDebugRegistry::type_name(static_cast<lv_subject_type_t>(99)) ==
                std::string("UNKNOWN"));
    }
}

// ============================================================================
// Multiple Subject Registration Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "SubjectDebugRegistry: multiple subjects can be registered",
                 "[subject_debug][registry][multiple]") {
    SubjectDebugRegistry& registry = SubjectDebugRegistry::instance();
    registry.clear();

    // Create multiple dummy subjects
    helix::Subject subject1 = {};
    helix::Subject subject2 = {};
    helix::Subject subject3 = {};

    registry.register_subject(&subject1, "subject_int", LV_SUBJECT_TYPE_INT, "file1.cpp", 10);
    registry.register_subject(&subject2, "subject_string", LV_SUBJECT_TYPE_STRING, "file2.cpp", 20);
    registry.register_subject(&subject3, "subject_pointer", LV_SUBJECT_TYPE_POINTER, "file3.cpp",
                              30);

    // Verify each can be looked up
    const SubjectDebugInfo* info1 = registry.lookup(&subject1);
    REQUIRE(info1 != nullptr);
    REQUIRE(info1->name == "subject_int");
    REQUIRE(info1->type == LV_SUBJECT_TYPE_INT);
    REQUIRE(info1->file == "file1.cpp");
    REQUIRE(info1->line == 10);

    const SubjectDebugInfo* info2 = registry.lookup(&subject2);
    REQUIRE(info2 != nullptr);
    REQUIRE(info2->name == "subject_string");
    REQUIRE(info2->type == LV_SUBJECT_TYPE_STRING);
    REQUIRE(info2->file == "file2.cpp");
    REQUIRE(info2->line == 20);

    const SubjectDebugInfo* info3 = registry.lookup(&subject3);
    REQUIRE(info3 != nullptr);
    REQUIRE(info3->name == "subject_pointer");
    REQUIRE(info3->type == LV_SUBJECT_TYPE_POINTER);
    REQUIRE(info3->file == "file3.cpp");
    REQUIRE(info3->line == 30);
}

// ============================================================================
// dump_all_subjects() Smoke Test
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "SubjectDebugRegistry: dump_all_subjects does not crash",
                 "[subject_debug][registry][dump]") {
    SubjectDebugRegistry& registry = SubjectDebugRegistry::instance();
    registry.clear();

    // Empty registry - should not crash
    REQUIRE_NOTHROW(registry.dump_all_subjects());

    // Add some subjects
    helix::Subject subject1 = {};
    helix::Subject subject2 = {};

    registry.register_subject(&subject1, "dump_test_1", LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);
    registry.register_subject(&subject2, "dump_test_2", LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    // With subjects - should not crash
    REQUIRE_NOTHROW(registry.dump_all_subjects());
}

// ============================================================================
// Singleton Tests
// ============================================================================

TEST_CASE("SubjectDebugRegistry: singleton returns same instance",
          "[subject_debug][registry][singleton]") {
    SubjectDebugRegistry& instance1 = SubjectDebugRegistry::instance();
    SubjectDebugRegistry& instance2 = SubjectDebugRegistry::instance();

    REQUIRE(&instance1 == &instance2);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "SubjectDebugRegistry: re-registering same pointer updates info",
                 "[subject_debug][registry][edge]") {
    SubjectDebugRegistry& registry = SubjectDebugRegistry::instance();
    registry.clear();

    helix::Subject subject = {};

    // First registration
    registry.register_subject(&subject, "original_name", LV_SUBJECT_TYPE_INT, "original.cpp", 100);

    const SubjectDebugInfo* info1 = registry.lookup(&subject);
    REQUIRE(info1 != nullptr);
    REQUIRE(info1->name == "original_name");

    // Re-register with different info
    registry.register_subject(&subject, "updated_name", LV_SUBJECT_TYPE_STRING, "updated.cpp", 200);

    const SubjectDebugInfo* info2 = registry.lookup(&subject);
    REQUIRE(info2 != nullptr);
    REQUIRE(info2->name == "updated_name");
    REQUIRE(info2->type == LV_SUBJECT_TYPE_STRING);
    REQUIRE(info2->file == "updated.cpp");
    REQUIRE(info2->line == 200);
}

TEST_CASE("SubjectDebugRegistry: lookup with nullptr returns nullptr",
          "[subject_debug][registry][edge]") {
    SubjectDebugRegistry& registry = SubjectDebugRegistry::instance();

    const SubjectDebugInfo* info = registry.lookup(nullptr);
    REQUIRE(info == nullptr);
}

// ============================================================================
// Registration Macros
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "SubjectDebugRegistry: the subject macros record their subjects",
                 "[subject_debug][registry][macros]") {
    SubjectDebugRegistry& registry = SubjectDebugRegistry::instance();
    registry.clear();

    SECTION("UI_MANAGED_SUBJECT_INT") {
        helix::Subject subject{};
        SubjectManager manager;
        UI_MANAGED_SUBJECT_INT(subject, 7, "registry_probe_managed_int", manager);

        REQUIRE(registry.lookup_by_name("registry_probe_managed_int") == &subject);
        const SubjectDebugInfo* info = registry.lookup(&subject);
        REQUIRE(info != nullptr);
        REQUIRE(info->type == LV_SUBJECT_TYPE_INT);
        REQUIRE(info->file == __FILE__);
        manager.deinit_all();
    }

    SECTION("UI_SUBJECT_INIT_AND_REGISTER_STRING") {
        helix::Subject subject{};
        char buf[16];
        SubjectManager manager;
        UI_SUBJECT_INIT_AND_REGISTER_STRING(subject, buf, "x", "registry_probe_string");
        manager.register_subject(&subject, "registry_probe_string");

        REQUIRE(registry.lookup_by_name("registry_probe_string") == &subject);
        const SubjectDebugInfo* info = registry.lookup(&subject);
        REQUIRE(info != nullptr);
        REQUIRE(info->type == LV_SUBJECT_TYPE_STRING);
        manager.deinit_all();
    }

    registry.clear();
}
