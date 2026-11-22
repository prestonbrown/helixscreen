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
