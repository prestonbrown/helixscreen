// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_owner.cpp
 * @brief helix::Subject owns one lv_subject_create() node whose address never changes.
 *
 * Cached getters, the XML subject registry, SubjectDebugRegistry and ObserverGuard key on
 * a subject's address across a printer switch's teardown and re-init, so reset() keeps the
 * node and every init_*() reuses it. The cases that need LVGL down run in a fresh process.
 */

#include "../lvgl_test_fixture.h"
#include "core/subject.h"
#include "lvgl/lvgl_private.h"
#include "subject_managed_panel.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Subject;

namespace {

int g_calls = 0;
void count_cb(lv_observer_t* /*observer*/, lv_subject_t* /*subject*/) {
    ++g_calls;
}

uint32_t observer_count(lv_subject_t* subject) {
    return lv_ll_get_len(&subject->subs_ll);
}

uint32_t live_subject_nodes() {
    return lv_ll_get_len(&LV_GLOBAL_DEFAULT()->subject_ll);
}

/// Set only in the process run_case_in_fresh_process() starts; the child cases skip without
/// it, so the serial hidden-set run (make test-hidden) never takes LVGL down under its
/// neighbours.
constexpr const char* kChildEnv = "HELIX_SUBJECT_CHILD_CASE";

/// Runs the hidden case `name` in a fresh helix-tests process and returns its exit code
/// (0 when every assertion passed, 128 + signal number on a signal, -1 on a hang). A fresh
/// process is the only place LVGL can be down: this one never calls lv_deinit(), and a
/// forked child cannot either, because lv_deinit() joins draw threads the child lacks.
int run_case_in_fresh_process(const char* name) {
    // Built before fork: the child only redirects and execs.
    std::vector<std::string> env_strings;
    for (char** e = environ; *e != nullptr; ++e) {
        env_strings.emplace_back(*e);
    }
    env_strings.emplace_back(std::string(kChildEnv) + "=1");
    std::vector<char*> envp;
    for (auto& e : env_strings) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);
    char exe[] = "/proc/self/exe";
    char argv0[] = "helix-tests";
    std::string filter(name);
    char* argv[] = {argv0, filter.data(), nullptr};

    const pid_t pid = fork();
    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execve(exe, argv, envp.data());
        _exit(127);
    }
    if (pid < 0) {
        return -1;
    }
    int status = 0;
    for (int waited_ms = 0; waited_ms < 30000; waited_ms += 10) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Subject keeps its address across reset and re-init",
                 "[subject][raii]") {
    Subject s;
    s.init_int(1);
    lv_subject_t* const first = &s;
    REQUIRE(first != nullptr);
    const uint32_t nodes = live_subject_nodes();

    s.reset();
    CHECK(&s == first);
    CHECK(live_subject_nodes() == nodes);
    s.init_int(2);
    CHECK(&s == first);
    CHECK(lv_subject_get_int(&s) == 2);

    char buf[16];
    s.reset();
    s.init_string(buf, "text");
    CHECK(&s == first);
    CHECK(first->type == LV_SUBJECT_TYPE_STRING);
    CHECK(std::strcmp(lv_subject_get_string(&s), "text") == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Subject::reset removes every observer and refuses new ones until re-init",
                 "[subject][raii]") {
    Subject s;
    s.init_int(5);
    REQUIRE(lv_subject_add_observer(&s, count_cb, nullptr) != nullptr);
    REQUIRE(lv_subject_add_observer(&s, count_cb, nullptr) != nullptr);
    REQUIRE(observer_count(&s) == 2);

    s.reset();
    CHECK_FALSE(s.is_live());
    CHECK(observer_count(&s) == 0);
    CHECK(lv_subject_add_observer(&s, count_cb, nullptr) == nullptr);

    s.init_int(6);
    CHECK(lv_subject_add_observer(&s, count_cb, nullptr) != nullptr);
    CHECK(observer_count(&s) == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "Subject::reset unbinds a widget-bound observer from its widget",
                 "[subject][raii]") {
    Subject s;
    s.init_int(0);
    lv_obj_t* obj = lv_obj_create(lv_screen_active());
    const uint32_t before = lv_obj_get_event_count(obj);
    REQUIRE(lv_subject_add_observer_obj(&s, count_cb, obj, nullptr) != nullptr);
    REQUIRE(lv_obj_get_event_count(obj) > before);

    s.reset();
    CHECK(lv_obj_get_event_count(obj) == before);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "SubjectManager deinit_all resets a registered Subject through its owner pointer",
                 "[subject][raii]") {
    Subject s;
    SubjectManager manager;
    s.init_int(3);
    lv_subject_t* const handle = &s;
    manager.register_subject(&s);
    REQUIRE(lv_subject_add_observer(&s, count_cb, nullptr) != nullptr);

    manager.deinit_all();
    CHECK_FALSE(s.is_live());
    CHECK(&s == handle);
    CHECK(observer_count(handle) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Moving a Subject keeps its handle and re-points the owner",
                 "[subject][raii]") {
    Subject a;
    a.init_int(4);
    lv_subject_t* const handle = &a;

    Subject b(std::move(a));
    CHECK(&b == handle);
    CHECK(&a == nullptr);
    CHECK(Subject::owner_of(handle) == std::addressof(b));

    std::vector<Subject> lanes(3);
    std::vector<lv_subject_t*> handles;
    for (auto& lane : lanes) {
        lane.init_int(0);
        handles.push_back(&lane);
    }
    lanes.reserve(lanes.capacity() * 4 + 8);
    for (size_t i = 0; i < lanes.size(); ++i) {
        CHECK(&lanes[i] == handles[i]);
        CHECK(Subject::owner_of(handles[i]) == std::addressof(lanes[i]));
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "Subject destructor frees its LVGL node", "[subject][raii]") {
    const uint32_t before = live_subject_nodes();
    {
        Subject s;
        s.init_int(1);
        REQUIRE(lv_subject_add_observer(&s, count_cb, nullptr) != nullptr);
        REQUIRE(live_subject_nodes() == before + 1);
    }
    CHECK(live_subject_nodes() == before);
    {
        Subject never_initialized;
        REQUIRE(live_subject_nodes() == before + 1);
        Subject taker(std::move(never_initialized));
    }
    CHECK(live_subject_nodes() == before);
}

TEST_CASE_METHOD(LVGLTestFixture, "Subject init_string keeps a value that aliases its buffer",
                 "[subject][raii]") {
    char buf[16];
    std::strcpy(buf, "seed");
    Subject whole;
    whole.init_string(buf, nullptr, sizeof(buf), buf);
    CHECK(std::strcmp(lv_subject_get_string(&whole), "seed") == 0);

    char tail[16];
    std::strcpy(tail, "xseed");
    Subject inner;
    inner.init_string(tail, nullptr, sizeof(tail), tail + 1);
    CHECK(std::strcmp(lv_subject_get_string(&inner), "seed") == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Subject init_string with a previous buffer tracks the previous string",
                 "[subject][raii]") {
    char buf[16];
    char prev[16];
    Subject s;
    s.init_string(buf, prev, sizeof(buf), "first");
    lv_subject_set_string(&s, "second");
    CHECK(std::strcmp(lv_subject_get_string(&s), "second") == 0);
    CHECK(std::strcmp(lv_subject_get_previous_string(&s), "first") == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Subject re-init while live removes the old observers",
                 "[subject][raii]") {
    Subject s;
    s.init_int(1);
    REQUIRE(lv_subject_add_observer(&s, count_cb, nullptr) != nullptr);

    s.init_int(9);
    CHECK(observer_count(&s) == 0);
    CHECK(s.is_live());
    CHECK(lv_subject_get_int(&s) == 9);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Subject constructed after lv_init has a stable handle that init_* keeps",
                 "[subject][raii]") {
    Subject s;
    lv_subject_t* const early = &s;
    REQUIRE(early != nullptr);
    CHECK_FALSE(s.is_live());
    CHECK(lv_subject_add_observer(early, count_cb, nullptr) == nullptr);

    char buf[16];
    s.init_string(buf, "late");
    CHECK(&s == early);
    CHECK(std::strcmp(lv_subject_get_string(early), "late") == 0);
    CHECK(lv_subject_add_observer(early, count_cb, nullptr) != nullptr);
}

// The three cases below run their bodies in a fresh process: see run_case_in_fresh_process.
TEST_CASE("Subject constructed before lv_init yields null until init_*", "[subject][raii]") {
    CHECK(run_case_in_fresh_process("subject child: constructed before lv_init") == 0);
}

TEST_CASE("Subject init before lv_init allocates nothing", "[subject][raii]") {
    CHECK(run_case_in_fresh_process("subject child: init before lv_init") == 0);
}

TEST_CASE("Subject destructor after lv_deinit leaves the freed node alone", "[subject][raii]") {
    CHECK(run_case_in_fresh_process("subject child: destructor after lv_deinit") == 0);
}

// Every case starts with LVGL up (test_main.cpp's listener), so each child case takes it
// down first. Each runs alone in a fresh process started by its [subject][raii] case.
TEST_CASE("subject child: constructed before lv_init", "[.subject_child]") {
    if (std::getenv(kChildEnv) == nullptr) {
        SKIP("runs only in the process its [subject][raii] case starts");
    }
    lv_deinit();
    REQUIRE_FALSE(lv_is_initialized());
    Subject s;
    REQUIRE(&s == nullptr);

    lv_init();
    REQUIRE(&s == nullptr);
    s.init_int(3);
    REQUIRE(&s != nullptr);
    REQUIRE(s.is_live());
    REQUIRE(lv_subject_get_int(&s) == 3);
    // Every assertion held. Exit here: the test process's statics still hold objects of the
    // LVGL generation lv_deinit() freed, and their destructors free them a second time.
    std::_Exit(0);
}

TEST_CASE("subject child: init before lv_init", "[.subject_child]") {
    if (std::getenv(kChildEnv) == nullptr) {
        SKIP("runs only in the process its [subject][raii] case starts");
    }
    lv_deinit();
    REQUIRE_FALSE(lv_is_initialized());
    Subject s;
    s.init_int(4);
    CHECK(&s == nullptr);
    CHECK_FALSE(s.is_live());
}

TEST_CASE("subject child: destructor after lv_deinit", "[.subject_child]") {
    if (std::getenv(kChildEnv) == nullptr) {
        SKIP("runs only in the process its [subject][raii] case starts");
    }
    REQUIRE(lv_is_initialized());
    {
        Subject s;
        s.init_int(1);
        REQUIRE(&s != nullptr);
        lv_deinit();
    }
    CHECK_FALSE(lv_is_initialized());
}

TEST_CASE_METHOD(LVGLTestFixture, "Managed macros accept both lv_subject_t and helix::Subject",
                 "[subject][raii][macro]") {
    Subject owned;
    // SUBJECT_API_OK: the unconverted storage the transitional overloads still serve
    lv_subject_t plain{};
    SubjectManager manager;
    UI_MANAGED_SUBJECT_INT(owned, 5, "subject_owner_test_owned", manager);
    UI_MANAGED_SUBJECT_INT(plain, 6, "subject_owner_test_plain", manager);

    CHECK(owned.is_live());
    CHECK(Subject::owner_of(&owned) == std::addressof(owned));
    CHECK(lv_subject_get_int(&owned) == 5);
    CHECK(lv_subject_get_int(&plain) == 6);

    manager.deinit_all();
    CHECK_FALSE(owned.is_live());
    CHECK(lv_subject_add_observer(&plain, count_cb, nullptr) == nullptr);
}
