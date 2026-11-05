// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "system/lvgl_thread_check.h"

#include <csignal>
#include <cstdio>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix::lvgl_thread_check;

namespace {

/// Sets a mode and an empty site set for the scope, then restores the mode.
struct ModeScope {
    explicit ModeScope(Mode m) : saved_(current_mode()) {
        set_mode(m);
        reset_reported_sites();
    }
    ~ModeScope() {
        set_mode(saved_);
        reset_reported_sites();
    }
    Mode saved_;
};

void set_int_off_thread(lv_subject_t* subject, int32_t value) {
    std::thread([subject, value] { lv_subject_set_int(subject, value); }).join();
}

} // namespace

TEST_CASE("lvgl_thread_check decides by thread, mode and whether the site was seen",
          "[lvgl_thread_check]") {
    for (Mode m : {Mode::Off, Mode::Report, Mode::Abort}) {
        CHECK(decide(m, true, false) == Action::None);
        CHECK(decide(m, true, true) == Action::None);
    }
    CHECK(decide(Mode::Off, false, false) == Action::None);
    CHECK(decide(Mode::Report, false, false) == Action::Report);
    CHECK(decide(Mode::Report, false, true) == Action::None);
    CHECK(decide(Mode::Abort, false, false) == Action::Abort);
    CHECK(decide(Mode::Abort, false, true) == Action::Abort);
}

TEST_CASE("lvgl_thread_check parses HELIX_LVGL_THREAD_CHECK values", "[lvgl_thread_check]") {
    CHECK(parse_mode("off", Mode::Report) == Mode::Off);
    CHECK(parse_mode("0", Mode::Report) == Mode::Off);
    CHECK(parse_mode("report", Mode::Off) == Mode::Report);
    CHECK(parse_mode("abort", Mode::Off) == Mode::Abort);
    CHECK(parse_mode(nullptr, Mode::Report) == Mode::Report);
    CHECK(parse_mode("", Mode::Abort) == Mode::Abort);
    CHECK(parse_mode("ABORT", Mode::Report) == Mode::Report);
}

TEST_CASE_METHOD(LVGLTestFixture, "lvgl_thread_check reports an off-thread subject write once",
                 "[lvgl_thread_check]") {
    ModeScope scope(Mode::Report);
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    lv_subject_set_int(&subject, 1);
    lv_subject_notify(&subject);
    lv_timer_t* timer = lv_timer_create([](lv_timer_t*) {}, 1000, nullptr);
    lv_timer_delete(timer);
    CHECK(reported_site_count() == 0);

    for (int i = 0; i < 3; ++i) {
        set_int_off_thread(&subject, 2 + i);
    }
    CHECK(reported_site_count() == 1);

    // A different entry point from another thread is a site of its own.
    std::thread([&subject] { lv_subject_notify(&subject); }).join();
    CHECK(reported_site_count() == 2);

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "lvgl_thread_check reports off-thread timer and delete calls",
                 "[lvgl_thread_check]") {
    ModeScope scope(Mode::Report);
    lv_obj_t* obj = lv_obj_create(test_screen());

    std::thread([] {
        lv_timer_t* timer = lv_timer_create([](lv_timer_t*) {}, 1000, nullptr);
        lv_timer_delete(timer);
    }).join();
    CHECK(reported_site_count() == 2);

    std::thread([obj] { lv_obj_delete(obj); }).join();
    CHECK(reported_site_count() == 3);
}

TEST_CASE_METHOD(LVGLTestFixture, "lvgl_thread_check is silent when off", "[lvgl_thread_check]") {
    ModeScope scope(Mode::Off);
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    set_int_off_thread(&subject, 5);
    CHECK(reported_site_count() == 0);
    lv_subject_deinit(&subject);
}

// TSan kills a forked child that starts a thread, before the check can abort.
#if !defined(__SANITIZE_THREAD__) && !(defined(__has_feature) && __has_feature(thread_sanitizer))
TEST_CASE_METHOD(LVGLTestFixture, "lvgl_thread_check aborts on an off-thread call in abort mode",
                 "[lvgl_thread_check][subprocess]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        std::signal(SIGABRT, SIG_DFL); // Catch2's handler would report instead of dying
        set_mode(Mode::Abort);
        lv_subject_set_int(&subject, 1); // main thread: must not abort
        set_int_off_thread(&subject, 2);
        _exit(0);
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);

    lv_subject_deinit(&subject);
}
#endif
