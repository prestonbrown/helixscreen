// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "helix/xml/scoped_subject_registry.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "scoped_subject_registry defaults to global scope",
                 "[xml][scope]") {
    lv_subject_t s;
    lv_subject_init_int(&s, 42);

    // With no active scope, registration goes to global
    helix::xml::register_subject_in_current_scope("test_default_scope", &s);

    REQUIRE(lv_xml_get_subject(nullptr, "test_default_scope") == &s);

    lv_subject_deinit(&s);
}

TEST_CASE_METHOD(LVGLTestFixture, "scoped_subject_registry uses active scope when pushed",
                 "[xml][scope]") {
    REQUIRE(lv_xml_register_component_from_data("test_scope_stub",
                                                "<component><view/></component>") == LV_RESULT_OK);
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("test_scope_stub");
    REQUIRE(scope != nullptr);

    // Heap-allocate the subject. NOTE: the scope does NOT take ownership -
    // lv_xml_subject_record_release() frees a record only when its `owned` flag
    // is set, which is true for parser-created <subject>/<subject_expr> and
    // false for anything handed in through lv_xml_register_subject(). This one
    // is BORROWED, so freeing it is our job (see the provenance rule above
    // lv_xml_subject_record_release, and lib/helix-xml's test_subject_provenance).
    auto* s = static_cast<lv_subject_t*>(lv_malloc(sizeof(lv_subject_t)));
    REQUIRE(s != nullptr);
    lv_subject_init_int(s, 7);

    {
        helix::xml::ScopedSubjectRegistryOverride push(scope);
        helix::xml::register_subject_in_current_scope("scoped_int", s);
    }

    REQUIRE(lv_xml_get_subject(scope, "scoped_int") == s);
    REQUIRE(lv_xml_get_subject(nullptr, "scoped_int") == nullptr);

    lv_xml_component_unregister("test_scope_stub");

    // Ours to release: the record was borrowed, so retiring the scope dropped
    // the reference without touching the storage. Omitting this leaks 72 bytes.
    lv_subject_deinit(s);
    lv_free(s);
}
