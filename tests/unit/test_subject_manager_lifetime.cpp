// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_manager_lifetime.cpp
 * @brief SubjectManager owns the death signal: deinit_all() flips it before
 *        the subjects are freed, and a reused manager hands out a fresh LIVE
 *        token — never an empty one.
 *
 * An empty token is not neutral: ObserverGuard::reset() reads a null/empty
 * alive token as "subject already dead" and skips lv_observer_remove(),
 * orphaning a live observer node whose context is about to be freed. An owner
 * torn down and re-initialized mid-process (test fixtures, printer switches)
 * must therefore be able to fetch a live token between the two.
 */

#include "../lvgl_test_fixture.h"
#include "core/observer_guard.h"
#include "subject_managed_panel.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {

void noop_observer_cb(lv_observer_t*, lv_subject_t*) {}

struct ManagedOwner {
    SubjectManager subjects_;
    lv_subject_t value_{};

    void init_subjects() {
        lv_subject_init_int(&value_, 7);
        subjects_.register_subject(&value_, "manager_lifetime_probe");
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "SubjectManager flips its death signal before freeing subjects",
                 "[observer][raii][crash_hardening]") {
    ManagedOwner owner;
    owner.init_subjects();

    auto token = owner.subjects_.get_subjects_lifetime();
    REQUIRE(token != nullptr);
    REQUIRE(*token);

    // A guard built the way observe_*() builds one: observer attached, token
    // set. Its reset() after the teardown below must skip
    // lv_observer_remove() on the node lv_subject_deinit() freed.
    ObserverGuard guard(&owner.value_, noop_observer_cb, nullptr);
    guard.set_alive_token(token);
    REQUIRE(lv_ll_get_len(&owner.value_.subs_ll) == 1);

    owner.subjects_.deinit_all();

    CHECK_FALSE(*token);
    CHECK(lv_ll_get_len(&owner.value_.subs_ll) == 0);

    // The crash-hardening half: this reset() dereferences nothing the
    // teardown freed. With the flip missing it calls lv_observer_remove()
    // on a freed node.
    guard.reset();
    CHECK(lv_ll_get_len(&owner.value_.subs_ll) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "SubjectManager flips its death signal even with nothing registered",
                 "[observer][raii]") {
    // The flip must precede deinit_all()'s empty-list early return: an empty
    // manager still ends a generation. A token fetched before the call is the
    // only witness a holder has.
    SubjectManager empty;
    auto token = empty.get_subjects_lifetime();
    REQUIRE(*token);

    empty.deinit_all();
    CHECK_FALSE(*token);
}

TEST_CASE_METHOD(LVGLTestFixture, "a reused SubjectManager hands out a live token, never empty",
                 "[observer][raii]") {
    ManagedOwner owner;
    owner.init_subjects();

    auto first = owner.subjects_.get_subjects_lifetime();
    REQUIRE(*first);

    owner.subjects_.deinit_all();
    CHECK_FALSE(*first);

    // Between teardown and re-init an observer may attach against the NEXT
    // generation. The token it fetches here must be live — an empty one reads
    // as "dead" and suppresses that observer's removals for its whole life.
    auto second = owner.subjects_.get_subjects_lifetime();
    REQUIRE(second != nullptr);
    REQUIRE(*second);
    REQUIRE(second != first);

    // The second generation dies with its own flip; the first stays dead.
    owner.init_subjects();
    auto after_reinit = owner.subjects_.get_subjects_lifetime();
    REQUIRE(after_reinit == second);

    owner.subjects_.deinit_all();
    CHECK_FALSE(*second);
    CHECK_FALSE(*first);
}

TEST_CASE_METHOD(LVGLTestFixture, "a moved-from SubjectManager still hands out a live token",
                 "[observer][raii]") {
    SubjectManager source;
    auto token = source.get_subjects_lifetime();
    REQUIRE(token != nullptr);
    REQUIRE(*token);

    SubjectManager moved(std::move(source));

    // The death signal follows the subjects into the moved-to manager.
    REQUIRE(*token);

    // The moved-from manager keeps the documented invariant. An empty token is
    // not inert: observe_*() builds a guard with no defence at all, and
    // set_alive_token() reads null as already-dead.
    auto fresh = source.get_subjects_lifetime();
    REQUIRE(fresh != nullptr);
    REQUIRE(*fresh);
    REQUIRE(fresh != token);

    // Only the manager that now owns the subjects flips the pre-move token.
    moved.deinit_all();
    CHECK_FALSE(*token);
    CHECK(*fresh);
}
