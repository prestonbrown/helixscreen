// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: the shipped code under test is the helix-xml engine in
// lib/helix-xml/, so the includes are engine headers, not include/ or src/.
//
// A <repeat> or <if> nested inside a <repeat> body is not supported. The
// engine logs it and skips the nested fragment, and the outer repeat and the
// rest of the parse stay intact.

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr const char* NESTED = R"xml(
<component>
  <view name="nested_root" extends="lv_obj">
    <lv_obj name="units">
      <repeat count="2">
        <lv_obj name="unit_row">
          <repeat count="3">
            <lv_label name="cell" text="c"/>
          </repeat>
          <lv_label name="row_tail" text="t"/>
        </lv_obj>
      </repeat>
      <lv_label name="after" text="a"/>
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

TEST_CASE_METHOD(LVGLTestFixture, "nested repeat is skipped and the outer still expands",
                 "[xml][repeat]") {
    REQUIRE(lv_xml_register_component_from_data("nested_repeat_guard", NESTED) == LV_RESULT_OK);

    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "nested_repeat_guard", nullptr));
    REQUIRE(root != nullptr);
    process_lvgl(20);

    lv_obj_t* units = lv_obj_find_by_name(root, "units");
    REQUIRE(units != nullptr);

    // The outer body expands twice, minus the nested repeat.
    CHECK(count_by_name(units, "unit_row") == 2);
    CHECK(count_by_name(units, "cell") == 0);
    lv_obj_t* row = lv_obj_find_by_name(units, "unit_row");
    REQUIRE(row != nullptr);
    CHECK(count_by_name(row, "row_tail") == 1);
    CHECK(count_by_name(row, "cell") == 0);

    // The parse stays balanced: the sibling after the repeat lands in `units`.
    CHECK(count_by_name(units, "after") == 1);

    lv_obj_delete(root);
}
