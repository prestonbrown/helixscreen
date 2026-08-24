// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_subject_registry.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

// A caller that computes a default into the buffer and then registers the subject
// passes the buffer as its own initial value. snprintf() with overlapping source and
// destination is undefined behaviour and empties the buffer on glibc, so the seeded
// default is lost and the widget renders blank. The wizard's Moonraker host/port
// fields did exactly that: the localhost default was resolved correctly, then
// discarded, which also suppressed the localhost auto-probe that keys off it.
TEST_CASE_METHOD(LVGLTestFixture, "string subject keeps a pre-seeded buffer",
                 "[subjects][registry]") {
    lv_subject_t subject;
    char buffer[32];

    snprintf(buffer, sizeof(buffer), "127.0.0.1");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(subject, buffer, buffer, "seed_self_ref");

    CHECK(std::string(buffer) == "127.0.0.1");
    CHECK(std::string(lv_subject_get_string(&subject)) == "127.0.0.1");

    lv_subject_deinit(&subject);
}

// The ordinary case must keep working: a distinct initial value overwrites whatever
// the buffer happened to hold.
TEST_CASE_METHOD(LVGLTestFixture, "string subject adopts a distinct initial value",
                 "[subjects][registry]") {
    lv_subject_t subject;
    char buffer[32];

    snprintf(buffer, sizeof(buffer), "stale");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(subject, buffer, "fresh", "seed_distinct");

    CHECK(std::string(buffer) == "fresh");
    CHECK(std::string(lv_subject_get_string(&subject)) == "fresh");

    lv_subject_deinit(&subject);
}

// Same two cases for the explicit-size variant used with std::array-backed buffers.
TEST_CASE_METHOD(LVGLTestFixture, "sized string subject keeps a pre-seeded buffer",
                 "[subjects][registry]") {
    lv_subject_t subject;
    std::array<char, 32> buffer{};

    snprintf(buffer.data(), buffer.size(), "7125");
    UI_SUBJECT_INIT_AND_REGISTER_STRING_N(subject, buffer.data(), buffer.size(), buffer.data(),
                                          "seed_self_ref_n");

    CHECK(std::string(buffer.data()) == "7125");
    CHECK(std::string(lv_subject_get_string(&subject)) == "7125");

    lv_subject_deinit(&subject);
}
