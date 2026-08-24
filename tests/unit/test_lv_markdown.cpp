// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_markdown.h"

#include "lv_markdown.h"
#include "lvgl_test_fixture.h"
#include "test_fixtures.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include "catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "lv_markdown participates in flex layout", "[markdown][layout]") {
    // Create a flex-column container (simulating a typical panel layout)
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_width(container, 400);
    lv_obj_set_height(container, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_pad_row(container, 0, 0);

    // Add a fixed-height child before the markdown widget
    lv_obj_t* spacer = lv_obj_create(container);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_height(spacer, 50);

    // Add a markdown widget
    lv_obj_t* md = lv_markdown_create(container);

    // Set some text so the markdown widget has content
    lv_markdown_set_text(md, "# Hello\n\nSome body text here.");

    // Force layout calculation
    lv_obj_update_layout(container);

    // The markdown widget should be positioned AFTER the spacer (y >= 50)
    int32_t md_y = lv_obj_get_y(md);
    int32_t md_h = lv_obj_get_height(md);

    spdlog::debug("[test_lv_markdown] spacer h={}, md y={}, md h={}", lv_obj_get_height(spacer),
                  md_y, md_h);

    REQUIRE(md_y >= 50);
    REQUIRE(md_h > 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "lv_markdown renders in non-flex container",
                 "[markdown][layout]") {
    // Verify the widget still works in non-flex containers (existing use case)
    lv_obj_t* wrapper = lv_obj_create(test_screen());
    lv_obj_set_width(wrapper, 400);
    lv_obj_set_height(wrapper, 600);

    lv_obj_t* md = lv_markdown_create(wrapper);
    lv_markdown_set_text(md, "## Test\n\n- Item 1\n- Item 2\n\n> A blockquote");

    lv_obj_update_layout(wrapper);

    int32_t md_h = lv_obj_get_height(md);
    REQUIRE(md_h > 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "lv_markdown with multiple siblings in flex",
                 "[markdown][layout]") {
    // Two markdown widgets in a flex column should not overlap
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_width(container, 400);
    lv_obj_set_height(container, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_pad_row(container, 0, 0);

    lv_obj_t* md1 = lv_markdown_create(container);
    lv_markdown_set_text(md1, "# First\n\nParagraph one.");

    lv_obj_t* md2 = lv_markdown_create(container);
    lv_markdown_set_text(md2, "# Second\n\nParagraph two.");

    lv_obj_update_layout(container);

    int32_t y1 = lv_obj_get_y(md1);
    int32_t h1 = lv_obj_get_height(md1);
    int32_t y2 = lv_obj_get_y(md2);
    int32_t h2 = lv_obj_get_height(md2);

    spdlog::debug("[test_lv_markdown] md1 y={} h={}, md2 y={} h={}", y1, h1, y2, h2);

    // Both should have content
    REQUIRE(h1 > 0);
    REQUIRE(h2 > 0);

    // Second markdown should start after first ends (no overlap)
    REQUIRE(y2 >= y1 + h1);
}

TEST_CASE_METHOD(XMLTestFixture, "ui_markdown theme style wires bold font and distinct headings",
                 "[markdown][style]") {
    // XMLTestFixture is required because theme_manager_get_font/get_color need
    // globals.xml parsed and AssetManager::register_fonts() run.
    lv_markdown_style_t style{};
    ui_markdown_get_theme_style(style);

    // Bold runs use a real bold glyph, not the letter-spacing faux-bold fallback
    REQUIRE(style.bold_font != nullptr);

    // Each heading tier should differ from body so the hierarchy actually reads
    REQUIRE(style.heading_font[0] != style.heading_font[1]); // H1 (font_xl) vs H2 (font_heading)
    REQUIRE(style.heading_font[2] != style.body_font);       // H3 (font_body_bold) vs body
    REQUIRE(style.heading_font[3] != style.body_font);       // H4 (font_body_bold) vs body

    // H3 must not fade into body text — its color is distinct from body_color
    REQUIRE_FALSE(lv_color_eq(style.heading_color[2], style.body_color));
}
