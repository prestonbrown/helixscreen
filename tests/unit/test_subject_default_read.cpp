// SPDX-License-Identifier: GPL-3.0-or-later
//
// subject_get_bool_or(): the guard that keeps a default-true setting from
// reporting false while its subject is still uninitialized
// (prestonbrown/helixscreen#1288).

#include "ui_subject_registry.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("subject_get_bool_or reports the fallback until the subject is initialized",
          "[subject][settings][defaults]") {
    SECTION("a zero-initialized subject is INVALID, not int, so the fallback wins") {
        lv_subject_t subject{};
        REQUIRE(subject.type == LV_SUBJECT_TYPE_INVALID);

        // This is the whole point: a default-true setting must not read false
        // just because nobody has called init_subjects() yet.
        CHECK(subject_get_bool_or(subject, true));
        CHECK_FALSE(subject_get_bool_or(subject, false));
    }

    SECTION("a subject of the wrong type also falls back rather than reading 0") {
        // lv_subject_get_int() returns 0 for any non-int subject, which would
        // silently become "false" without the guard.
        lv_subject_t subject{};
        lv_subject_init_pointer(&subject, nullptr);
        REQUIRE(subject.type != LV_SUBJECT_TYPE_INT);

        CHECK(subject_get_bool_or(subject, true));
        CHECK_FALSE(subject_get_bool_or(subject, false));

        lv_subject_deinit(&subject);
    }
}

TEST_CASE("subject_get_bool_or reads the subject once it is initialized",
          "[subject][settings][defaults]") {
    SECTION("an initialized false subject overrides a true fallback") {
        lv_subject_t subject{};
        lv_subject_init_int(&subject, 0);

        // The fallback must NOT survive initialization - a user who turned a
        // default-true setting off has to see it stay off.
        CHECK_FALSE(subject_get_bool_or(subject, true));

        lv_subject_deinit(&subject);
    }

    SECTION("an initialized true subject overrides a false fallback") {
        lv_subject_t subject{};
        lv_subject_init_int(&subject, 1);

        CHECK(subject_get_bool_or(subject, false));

        lv_subject_deinit(&subject);
    }

    SECTION("any non-zero value reads true") {
        lv_subject_t subject{};
        lv_subject_init_int(&subject, 42);

        CHECK(subject_get_bool_or(subject, false));

        lv_subject_deinit(&subject);
    }

    SECTION("a value set after init is observed, not the fallback") {
        lv_subject_t subject{};
        lv_subject_init_int(&subject, 1);
        lv_subject_set_int(&subject, 0);

        CHECK_FALSE(subject_get_bool_or(subject, true));

        lv_subject_deinit(&subject);
    }
}
