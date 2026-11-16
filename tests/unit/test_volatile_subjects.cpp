// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "core/subject.h"
#include "state/volatile_subjects.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "VolatileSubjects restores each subject to its own default",
                 "[state][volatile]") {
    helix::Subject zero_default{};
    helix::Subject one_default{};
    zero_default.init_int(0);
    one_default.init_int(1);

    helix::subjects::VolatileSubjects v;
    v.register_subject(&zero_default, 0);
    v.register_subject(&one_default, 1);
    REQUIRE(v.size() == 2);

    // Drive both AWAY from their defaults, in opposite directions, so a
    // reset_all() that merely zeroes everything fails the second assertion.
    lv_subject_set_int(&zero_default, 7);
    lv_subject_set_int(&one_default, 0);

    v.reset_all();

    CHECK(lv_subject_get_int(&zero_default) == 0);
    CHECK(lv_subject_get_int(&one_default) == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "VolatileSubjects::clear drops registrations",
                 "[state][volatile]") {
    helix::Subject s{};
    s.init_int(0);

    helix::subjects::VolatileSubjects v;
    v.register_subject(&s, 0);
    v.clear();
    CHECK(v.size() == 0);

    lv_subject_set_int(&s, 5);
    v.reset_all();
    // Cleared registry must not touch the subject.
    CHECK(lv_subject_get_int(&s) == 5);
}
