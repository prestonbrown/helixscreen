// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// TEST_MIRROR_OK: exercises tests/test_helpers/label_fits.h, test infrastructure with no production
// header
#include "../lvgl_test_fixture.h"
#include "../test_helpers/label_fits.h"

#include "../catch_amalgamated.hpp"

namespace {

lv_obj_t* fixed_width_label(lv_obj_t* parent, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(label, 120);
    lv_label_set_text(label, text);
    lv_obj_update_layout(label);
    return label;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "label_fits: a short string in a fixed box fits",
                 "[label_fits][layout]") {
    lv_obj_t* label = fixed_width_label(test_screen(), "OK");
    CHECK(helix::test::label_text_findings(label).empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "label_fits: a string far wider than its box is a Text overflow",
                 "[label_fits][layout]") {
    lv_obj_t* label = fixed_width_label(
        test_screen(), "This string is much much wider than a one hundred and twenty pixel box");
    const auto findings = helix::test::label_text_findings(label);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings.front().kind == helix::OverflowKind::Text);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "label_fits: a wrapping label clipped by a fixed height overflows",
                 "[label_fits][layout]") {
    lv_obj_t* label = lv_label_create(test_screen());
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_size(label, 120, 20);
    lv_label_set_text(label,
                      "This string is much much wider than a one hundred and twenty pixel box");
    lv_obj_update_layout(label);
    const auto findings = helix::test::label_text_findings(label);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings.front().kind == helix::OverflowKind::Text);
}

TEST_CASE_METHOD(LVGLTestFixture, "label_fits: a zero-width label does not count as fitting",
                 "[label_fits][layout]") {
    lv_obj_t* label = fixed_width_label(test_screen(), "OK");
    REQUIRE(helix::test::label_fit_problems(label).empty());
    lv_obj_set_width(label, 0);
    CHECK_FALSE(helix::test::label_fit_problems(label).empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "label_fits: a truncating long mode is not a label that fits",
                 "[label_fits][layout]") {
    for (auto mode :
         {LV_LABEL_LONG_MODE_DOTS, LV_LABEL_LONG_MODE_SCROLL, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR}) {
        lv_obj_t* label = fixed_width_label(test_screen(), "OK");
        REQUIRE(helix::test::label_fit_problems(label).empty());
        lv_label_set_long_mode(label, mode);
        CHECK_FALSE(helix::test::label_fit_problems(label).empty());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "label_fits: a hidden or empty label does not count as fitting",
                 "[label_fits][layout]") {
    lv_obj_t* hidden = fixed_width_label(test_screen(), "OK");
    lv_obj_add_flag(hidden, LV_OBJ_FLAG_HIDDEN);
    CHECK_FALSE(helix::test::label_fit_problems(hidden).empty());
    lv_obj_t* empty = fixed_width_label(test_screen(), "");
    CHECK_FALSE(helix::test::label_fit_problems(empty).empty());
}
