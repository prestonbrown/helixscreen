// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// helix::snapshot_borrowed_subjects() / restore_borrowed_subjects() -- the
// HelixScreen half of the hot-reload subject round trip.
//
// A component scope gets subjects from two places:
//   owned    -- the XML parser allocated the lv_subject_t for a `<subject>` /
//               `<subject_expr>` element; the scope frees it at teardown.
//   borrowed -- C++ called lv_xml_register_subject() with a pointer to storage
//               it owns (a `static inline lv_subject_t` member, a manager-owned
//               field, ...). The scope holds the pointer only.
//
// The ENGINE-side provenance rules -- that teardown must not free or deinit a
// borrowed subject, that it must free an owned one, that unregister behaves
// per-provenance -- migrated to the standalone helix-xml suite, in
// lib/helix-xml/tests/cases/test_subject_provenance.c. That file also covers the
// reload round trip, but with its own local snapshot_borrowed()/restore_borrowed()
// helpers: the engine suite cannot link HelixScreen code, so it pins the engine
// CONTRACT, not our implementation of it.
//
// This test is the other side of that seam. It drives the real
// src/application/xml_hot_reloader.cpp functions -- the ones XmlHotReloader
// actually calls on the LVGL thread around a re-registration -- and is the only
// coverage they have. Its real trigger: HELIX_HOT_RELOAD saving
// ui_xml/runout_guidance_modal.xml, whose scope borrows
// RunoutGuidanceModal::resume_blocked_subject_ / autofeed_capable_subject_. If
// the snapshot misses them or the restore fails to re-register, the component
// comes back live but inert -- every bind_* naming a borrowed subject silently
// resolves to nothing.

#include "../lvgl_test_fixture.h"
#include "xml_hot_reloader.h"

#include "../catch_amalgamated.hpp"

extern "C" {
#include "helix-xml/src/xml/lv_xml.h"
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "hot-reload cycle re-registers borrowed subjects into the fresh scope",
                 "[xml][hotreload]") {
    // What ui_xml/runout_guidance_modal.xml looks like to the reloader: some
    // XML-declared subjects, plus C++-registered ones bound by the view.
    const char* v1 = "<component>"
                     "  <subjects>"
                     "    <subject name='xml_owned' type='int' value='3'/>"
                     "  </subjects>"
                     "  <view>"
                     "    <lv_obj name='box'>"
                     "      <bind_flag_if_eq subject='cpp_flag' flag='hidden' ref_value='1'/>"
                     "    </lv_obj>"
                     "  </view>"
                     "</component>";
    const char* v2 = "<component>"
                     "  <subjects>"
                     "    <subject name='xml_owned' type='int' value='4'/>"
                     "  </subjects>"
                     "  <view>"
                     "    <lv_obj name='box' style_pad_all='8'>"
                     "      <bind_flag_if_eq subject='cpp_flag' flag='hidden' ref_value='1'/>"
                     "    </lv_obj>"
                     "  </view>"
                     "</component>";

    REQUIRE(lv_xml_register_component_from_data("t_reload_rt", v1) == LV_RESULT_OK);

    lv_subject_t cpp_flag; // C++-owned storage, as a modal's static member would be
    lv_subject_init_int(&cpp_flag, 0);
    REQUIRE(lv_xml_register_subject(lv_xml_component_get_scope("t_reload_rt"), "cpp_flag",
                                    &cpp_flag) == LV_RESULT_OK);

    // Sanity: v1 resolves both provenances.
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("t_reload_rt");
    REQUIRE(lv_xml_get_subject(scope, "cpp_flag") == &cpp_flag);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(scope, "xml_owned")) == 3);

    // --- the reload cycle XmlHotReloader runs on the LVGL thread ---
    auto borrowed = helix::snapshot_borrowed_subjects("t_reload_rt");
    REQUIRE(borrowed.size() == 1);
    REQUIRE(borrowed[0].first == "cpp_flag");
    REQUIRE(borrowed[0].second == &cpp_flag);

    REQUIRE(lv_xml_component_unregister("t_reload_rt") == LV_RESULT_OK);
    REQUIRE(lv_xml_register_component_from_data("t_reload_rt", v2) == LV_RESULT_OK);
    REQUIRE(helix::restore_borrowed_subjects("t_reload_rt", borrowed) == 1);
    // --- end of cycle ---

    // Deliberately no `fresh != scope` assertion: the old scope really is freed,
    // and the allocator hands the same block straight back, so the pointers
    // routinely match. The re-parsed value below is the honest proof of a fresh
    // registration.
    lv_xml_component_scope_t* fresh = lv_xml_component_get_scope("t_reload_rt");
    REQUIRE(fresh != nullptr);

    // The XML-declared subject was re-parsed from v2 (new object, new value).
    lv_subject_t* xml_owned = lv_xml_get_subject(fresh, "xml_owned");
    REQUIRE(xml_owned != nullptr);
    REQUIRE(lv_subject_get_int(xml_owned) == 4);

    // The borrowed subject is the SAME object, still resolvable by name — this
    // is what keeps the reloaded component's bind_* wiring alive.
    REQUIRE(lv_xml_get_subject(fresh, "cpp_flag") == &cpp_flag);

    // And it actually binds: an instance created from the reloaded definition
    // must react to the C++-owned subject.
    lv_obj_t* inst =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "t_reload_rt", nullptr));
    REQUIRE(inst != nullptr);
    lv_obj_t* box = lv_obj_find_by_name(inst, "box");
    REQUIRE(box != nullptr);

    lv_subject_set_int(&cpp_flag, 1);
    REQUIRE(lv_obj_is_hidden(box));
    lv_subject_set_int(&cpp_flag, 0);
    REQUIRE_FALSE(lv_obj_is_hidden(box));

    lv_obj_delete(inst);
    REQUIRE(lv_xml_component_unregister("t_reload_rt") == LV_RESULT_OK);
    lv_subject_deinit(&cpp_flag);
}
