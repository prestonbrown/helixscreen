// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_subjects.cpp
 * @brief WidgetSubjects: init once, register the deinit, re-init after it ran.
 *
 * Each case runs its registry entry with deinit_one() rather than deinit_all(),
 * so it never fires the entries the process-wide registry holds for the rest of
 * the suite, and leaves no entry of its own behind.
 */

#include "../lvgl_test_fixture.h"
#include "core/static_subject_registry.h"
#include "widget_subjects.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

bool registered(const char* name) {
    const std::vector<std::string> names = StaticSubjectRegistry::instance().names();
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "WidgetSubjects runs its init once until deinit",
                 "[core][subjects][1802]") {
    static constexpr const char* kName = "WidgetSubjectsTestOnce";
    helix::WidgetSubjects subjects{kName};
    lv_subject_t subject{};
    int init_runs = 0;
    auto init = [&] {
        ++init_runs;
        lv_subject_init_int(&subject, 7);
        subjects.publish("widget_subjects_test_once", &subject);
    };

    REQUIRE_FALSE(subjects.initialized());
    REQUIRE_FALSE(registered(kName));

    CHECK(subjects.ensure_init(init));
    CHECK(init_runs == 1);
    CHECK(subjects.initialized());
    CHECK(registered(kName));
    CHECK(lv_xml_get_subject(nullptr, "widget_subjects_test_once") == &subject);

    CHECK_FALSE(subjects.ensure_init(init));
    CHECK(init_runs == 1);

    REQUIRE(StaticSubjectRegistry::instance().deinit_one(kName));
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetSubjects deinit withdraws its subjects and re-inits",
                 "[core][subjects][1802]") {
    static constexpr const char* kName = "WidgetSubjectsTestReinit";
    helix::WidgetSubjects subjects{kName};
    lv_subject_t subject{};
    int init_runs = 0;
    auto init = [&] {
        ++init_runs;
        lv_subject_init_int(&subject, init_runs);
        subjects.publish("widget_subjects_test_reinit", &subject);
    };

    REQUIRE(subjects.ensure_init(init));
    const SubjectLifetime first_generation = subjects.get_subjects_lifetime();
    REQUIRE(first_generation);
    REQUIRE(*first_generation);

    REQUIRE(StaticSubjectRegistry::instance().deinit_one(kName));
    CHECK_FALSE(subjects.initialized());
    CHECK(lv_xml_get_subject(nullptr, "widget_subjects_test_reinit") == nullptr);
    CHECK_FALSE(*first_generation);

    // A printer switch: the registry is drained, then the widget initializes again.
    CHECK(subjects.ensure_init(init));
    CHECK(init_runs == 2);
    CHECK(subjects.initialized());
    CHECK(registered(kName));
    CHECK(lv_xml_get_subject(nullptr, "widget_subjects_test_reinit") == &subject);
    CHECK(lv_subject_get_int(&subject) == 2);

    REQUIRE(StaticSubjectRegistry::instance().deinit_one(kName));
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetSubjects teardown runs before its subjects are freed",
                 "[core][subjects][1802]") {
    static constexpr const char* kName = "WidgetSubjectsTestTeardown";
    helix::WidgetSubjects subjects{kName};
    lv_subject_t subject{};
    int teardown_runs = 0;
    bool subject_live_in_teardown = false;

    REQUIRE(subjects.ensure_init(
        [&] {
            lv_subject_init_int(&subject, 0);
            subjects.publish("widget_subjects_test_teardown", &subject);
        },
        [&] {
            ++teardown_runs;
            subject_live_in_teardown =
                lv_xml_get_subject(nullptr, "widget_subjects_test_teardown") == &subject;
        }));
    REQUIRE(teardown_runs == 0);

    REQUIRE(StaticSubjectRegistry::instance().deinit_one(kName));
    CHECK(teardown_runs == 1);
    CHECK(subject_live_in_teardown);
    CHECK_FALSE(subjects.initialized());
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetSubjects registers only when its init runs",
                 "[core][subjects][1802]") {
    static constexpr const char* kName = "WidgetSubjectsTestOneEntry";
    helix::WidgetSubjects subjects{kName};
    auto& registry = StaticSubjectRegistry::instance();
    const size_t before = registry.count();
    int teardown_runs = 0;
    auto teardown = [&] { ++teardown_runs; };

    REQUIRE(subjects.ensure_init([] {}, teardown));
    CHECK(registry.count() == before + 1);
    CHECK_FALSE(subjects.ensure_init([] {}, teardown));
    CHECK(registry.count() == before + 1);

    REQUIRE(registry.deinit_one(kName));
    CHECK(registry.count() == before);
    REQUIRE(subjects.ensure_init([] {}, teardown));
    CHECK(registry.count() == before + 1);

    REQUIRE(registry.deinit_one(kName));
    CHECK(teardown_runs == 2);
}
