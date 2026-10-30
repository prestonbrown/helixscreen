// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_macros.cpp
 * @brief Unit tests for INIT_SUBJECT_INT/STRING macros
 *
 * These macros consolidate the 3-line subject initialization pattern:
 * 1. lv_subject_init_*(subject, value)
 * 2. subjects.register_subject(subject)
 * 3. if (register_xml) lv_xml_register_subject(NULL, "name", subject)
 */

#include "../lvgl_test_fixture.h"
#include "state/subject_macros.h"
#include "subject_managed_panel.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// INIT_SUBJECT_INT Tests
// ============================================================================

TEST_CASE("INIT_SUBJECT_INT macro", "[state][subject][macro]") {
    // Use LVGLTestFixture to ensure LVGL is initialized
    LVGLTestFixture fixture;

    SECTION("skips XML registration when flag is false") {
        SubjectManager subjects;
        lv_subject_t no_xml_subject_;

        INIT_SUBJECT_INT(no_xml_subject, 456, subjects, false);

        // Subject should NOT be in XML registry
        lv_subject_t* found = lv_xml_get_subject(NULL, "no_xml_subject");
        REQUIRE(found == nullptr);

        // But the subject should still be initialized
        REQUIRE(lv_subject_get_int(&no_xml_subject_) == 456);
    }
}

// ============================================================================
// INIT_SUBJECT_STRING Tests
// ============================================================================

TEST_CASE("INIT_SUBJECT_STRING macro", "[state][subject][macro]") {
    // Use LVGLTestFixture to ensure LVGL is initialized
    LVGLTestFixture fixture;

    SECTION("skips XML registration when flag is false") {
        SubjectManager subjects;
        lv_subject_t no_xml_str_;
        char no_xml_str_buf_[64];

        INIT_SUBJECT_STRING(no_xml_str, "Not in XML", subjects, false);

        // Subject should NOT be in XML registry
        lv_subject_t* found = lv_xml_get_subject(NULL, "no_xml_str");
        REQUIRE(found == nullptr);

        // But the subject should still be initialized
        REQUIRE(std::string(lv_subject_get_string(&no_xml_str_)) == "Not in XML");
    }
}

// ============================================================================
// XML-name withdrawal on teardown
// ============================================================================

// Regression test for the nightly TSan SIGSEGV in the panel-widget gate
// coalescing test (2026-08-17 through 08-19).
//
// SubjectManager::deinit_all() withdraws each subject's XML-scope name before
// freeing the subject, but it can only do that for names it was told about.
// The INIT_SUBJECT_* macros used to register the name in the XML scope while
// calling register_subject() WITHOUT it — and the parameter is defaulted, so
// the omission was silent. Every managed subject therefore kept resolving by
// name after lv_subject_deinit().
//
// The consequence was a live crash, not a leak: an owner that does not outlive
// the process (a PrinterState held by a stack-allocated XMLTestFixture) left
// "klippy_state" pointing at a destroyed fixture member, and
// PanelWidgetManager::setup_gate_observers null-checks its lookup — the check
// passed, and lv_subject_add_observer walked a garbage subs_ll.
TEST_CASE("INIT_SUBJECT macros withdraw their XML names when the owner dies",
          "[state][subject][macro][regression]") {
    LVGLTestFixture fixture;

    // Storage outlives the SubjectManager so deinit_all() has something valid
    // to deinit; only the manager's lifetime is under test.
    lv_subject_t doomed_int_{};
    lv_subject_t doomed_text_{};
    char doomed_text_buf_[32];

    {
        SubjectManager subjects;
        INIT_SUBJECT_INT(doomed_int, 7, subjects, true);
        INIT_SUBJECT_STRING(doomed_text, "alive", subjects, true);

        REQUIRE(lv_xml_get_subject(NULL, "doomed_int") == &doomed_int_);
        REQUIRE(lv_xml_get_subject(NULL, "doomed_text") == &doomed_text_);
    } // ~SubjectManager -> deinit_all()

    // A non-null answer here IS the bug: the name resolves to a subject that
    // lv_subject_deinit() has already torn down, and every caller that guards
    // with `if (!subject) continue;` sails straight past into freed memory.
    CHECK(lv_xml_get_subject(NULL, "doomed_int") == nullptr);
    CHECK(lv_xml_get_subject(NULL, "doomed_text") == nullptr);
}

// The withdrawal must be driven by what was actually published, not by the
// macro firing at all — a subject initialized with register_xml=false never
// entered the scope, so teardown must not go looking for its name (and must
// not disturb an unrelated registration that happens to share it).
TEST_CASE("INIT_SUBJECT macros withdraw nothing when XML registration was skipped",
          "[state][subject][macro][regression]") {
    LVGLTestFixture fixture;

    lv_subject_t shared_name_{};
    lv_subject_init_int(&shared_name_, 99);
    lv_xml_register_subject(NULL, "unmanaged_name", &shared_name_);

    {
        lv_subject_t unmanaged_name_{};
        SubjectManager subjects;
        INIT_SUBJECT_INT(unmanaged_name, 1, subjects, false);
        REQUIRE(lv_xml_get_subject(NULL, "unmanaged_name") == &shared_name_);
    }

    // The independently-registered subject still owns the name.
    CHECK(lv_xml_get_subject(NULL, "unmanaged_name") == &shared_name_);

    lv_xml_unregister_subject(NULL, "unmanaged_name");
    lv_subject_deinit(&shared_name_);
}
