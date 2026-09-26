// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: the shipped code under test is the helix-xml engine in
// lib/helix-xml/, so the includes are engine headers, not include/ or src/.
//
// Does a <repeat> nested inside a <repeat> expand and rebind? No XML in ui_xml/
// nests one, so nothing exercised the path. These tests pin what the engine
// actually does, in both the literal-count and subject-bound forms.

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Two unit rows of three cells each. If nesting worked this tree would hold
// 2 unit_row objects and 6 cell labels; anything else is the recorded answer.
constexpr const char* NESTED = R"xml(
<component>
  <view name="spike_root" extends="lv_obj">
    <lv_obj name="units">
      <repeat count="2">
        <lv_obj name="unit_row">
          <repeat count="3">
            <lv_label name="cell" text="c"/>
          </repeat>
        </lv_obj>
      </repeat>
    </lv_obj>
  </view>
</component>
)xml";

int count_by_name(lv_obj_t* root, const char* name) {
    int n = 0;
    for (lv_obj_t* c = lv_obj_get_child(root, 0); c; c = lv_obj_get_sibling(c, 1)) {
        const char* cn = lv_obj_get_name(c);
        if (cn && strcmp(cn, name) == 0)
            ++n;
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "nested repeat: literal counts", "[xml][repeat][spike]") {
    REQUIRE(lv_xml_register_component_from_data("spike_nested_repeat", NESTED) == LV_RESULT_OK);

    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spike_nested_repeat", nullptr));
    REQUIRE(root != nullptr);
    process_lvgl(20);

    lv_obj_t* units = lv_obj_find_by_name(root, "units");
    REQUIRE(units != nullptr);

    INFO("unit_row children of units: " << count_by_name(units, "unit_row"));
    INFO("cell labels directly under units: " << count_by_name(units, "cell"));

    // Pins the engine's actual answer: the outer repeat never expands (its
    // capture is replaced when the inner repeat opens), the inner expands
    // once at the wrong level (cells directly under `units`, no unit_row),
    // and only the inner count's worth of cells exist.
    CHECK(count_by_name(units, "unit_row") == 0);
    CHECK(count_by_name(units, "cell") == 3);

    lv_obj_delete(root);
}

TEST_CASE_METHOD(LVGLTestFixture, "nested repeat: subject-bound inner", "[xml][repeat][spike]") {
    lv_subject_t units_n, slots_n;
    lv_subject_init_int(&units_n, 2);
    lv_subject_init_int(&slots_n, 3);
    lv_xml_register_subject(nullptr, "spike_units", &units_n);
    lv_xml_register_subject(nullptr, "spike_slots", &slots_n);

    constexpr const char* NESTED_SUB = R"xml(
<component>
  <view name="spike_root" extends="lv_obj">
    <lv_obj name="units">
      <repeat count="spike_units">
        <lv_obj name="unit_row">
          <repeat count="spike_slots">
            <lv_label name="cell" text="c"/>
          </repeat>
        </lv_obj>
      </repeat>
    </lv_obj>
  </view>
</component>
)xml";
    REQUIRE(lv_xml_register_component_from_data("spike_nested_repeat_sub", NESTED_SUB) ==
            LV_RESULT_OK);

    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spike_nested_repeat_sub", nullptr));
    REQUIRE(root != nullptr);
    process_lvgl(20);

    lv_obj_t* units = lv_obj_find_by_name(root, "units");
    REQUIRE(units != nullptr);

    // Rebind half of the question: does the inner re-expand when its count
    // subject changes? (It does, still at the wrong level.)
    lv_subject_set_int(&slots_n, 1);
    process_lvgl(20);
    INFO("cells after slots_n=1: " << count_by_name(units, "cell"));
    INFO("unit_rows: " << count_by_name(units, "unit_row"));
    CHECK(count_by_name(units, "cell") == 1);
    CHECK(count_by_name(units, "unit_row") == 0);

    lv_obj_delete(root);
    lv_xml_unregister_subject(nullptr, "spike_units");
    lv_xml_unregister_subject(nullptr, "spike_slots");
}
