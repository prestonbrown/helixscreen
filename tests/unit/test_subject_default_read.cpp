// SPDX-License-Identifier: GPL-3.0-or-later
//
// subject_get_bool_or(): the guard that keeps a default-true setting from
// reporting false while its subject is still uninitialized
// (prestonbrown/helixscreen#1288).

#include "../lvgl_test_fixture.h"
#include "core/subject.h"
#include "system/subject_registry.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture,
                 "subject_get_bool_or reports the fallback until the subject is initialized",
                 "[subject][settings][defaults]") {
    SECTION("a zero-initialized subject is INVALID, not int, so the fallback wins") {
        lv_subject_t subject{}; // SUBJECT_API_OK: the zeroed storage the guard must reject
        REQUIRE(subject.type == LV_SUBJECT_TYPE_INVALID);

        // This is the whole point: a default-true setting must not read false
        // just because nobody has called init_subjects() yet.
        CHECK(subject_get_bool_or(subject, true));
        CHECK_FALSE(subject_get_bool_or(subject, false));
    }

    SECTION("a subject of the wrong type also falls back rather than reading 0") {
        // lv_subject_get_int() returns 0 for any non-int subject, which would
        // silently become "false" without the guard.
        helix::Subject subject;
        subject.init_pointer(nullptr);
        REQUIRE(subject.get()->type != LV_SUBJECT_TYPE_INT);

        CHECK(subject_get_bool_or(*subject.get(), true));
        CHECK_FALSE(subject_get_bool_or(*subject.get(), false));
    }
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "subject_get_bool_or reports the fallback for a helix::Subject not yet initialized",
    "[subject][settings][defaults]") {
    helix::Subject subject; // its node is an INT placeholder from construction
    REQUIRE(subject.get() != nullptr);
    REQUIRE(subject.get()->type == LV_SUBJECT_TYPE_INT);

    CHECK(subject_get_bool_or(subject, true));
    CHECK_FALSE(subject_get_bool_or(subject, false));

    subject.init_int(0);
    CHECK_FALSE(subject_get_bool_or(subject, true));
    subject.reset();
    CHECK(subject_get_bool_or(subject, true));
}

TEST_CASE_METHOD(LVGLTestFixture, "subject_get_bool_or reads the subject once it is initialized",
                 "[subject][settings][defaults]") {
    SECTION("an initialized false subject overrides a true fallback") {
        helix::Subject subject;
        subject.init_int(0);

        // The fallback must NOT survive initialization - a user who turned a
        // default-true setting off has to see it stay off.
        CHECK_FALSE(subject_get_bool_or(*subject.get(), true));
    }

    SECTION("an initialized true subject overrides a false fallback") {
        helix::Subject subject;
        subject.init_int(1);

        CHECK(subject_get_bool_or(*subject.get(), false));
    }

    SECTION("any non-zero value reads true") {
        helix::Subject subject;
        subject.init_int(42);

        CHECK(subject_get_bool_or(*subject.get(), false));
    }

    SECTION("a value set after init is observed, not the fallback") {
        helix::Subject subject;
        subject.init_int(1);
        lv_subject_set_int(&subject, 0);

        CHECK_FALSE(subject_get_bool_or(*subject.get(), true));
    }
}
