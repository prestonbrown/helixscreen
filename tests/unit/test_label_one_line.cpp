// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_label_one_line.cpp
 * @brief A dotted label holds exactly one line of its own font.
 *
 * LVGL's DOTS mode ellipsizes only when the wrapped text is taller than the
 * label. A label at content height grows to fit whatever it wraps to, so it
 * never ellipsizes: the text wraps onto two or three lines and pushes its
 * siblings out of the way (prestonbrown/helixscreen#1775). text_* and ui_button
 * cap a dotted label with lv_label_set_max_lines() (prestonbrown/helixscreen#2110).
 * These tests pin the height that cap gives, and the cases that opt out of it.
 */

#include "ui_text.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix/ui/shared_font_style.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Far wider than any test container in any font the suite loads.
constexpr const char* kLongText =
    "An extremely long spool name that cannot possibly fit on one line of a narrow row";

/// Registers a one-off component from an XML string and instantiates it.
/// Returns the named child, or nullptr.
lv_obj_t* build(XMLTestFixture& fx, const char* comp_name, const std::string& body,
                const char* child_name) {
    const std::string xml = "<component><view extends=\"lv_obj\" width=\"160\" height=\"300\""
                            " style_pad_all=\"0\" flex_flow=\"column\">" +
                            body + "</view></component>";
    lv_xml_component_unregister(comp_name);
    if (lv_xml_register_component_from_data(comp_name, xml.c_str()) != LV_RESULT_OK)
        return nullptr;
    lv_obj_t* root = fx.create_component(comp_name);
    if (!root)
        return nullptr;
    lv_obj_update_layout(root);
    return lv_obj_find_by_name(root, child_name);
}

int32_t line_height(const char* font_const) {
    const lv_font_t* font = theme_manager_get_font(font_const);
    REQUIRE(font != nullptr);
    return lv_font_get_line_height(font);
}

bool ends_with_dots(lv_obj_t* label) {
    const char* text = lv_label_get_text(label);
    const size_t len = text ? strlen(text) : 0;
    return len >= 3 && strcmp(text + len - 3, "...") == 0;
}

void settle(lv_obj_t* label) {
    lv_obj_update_layout(lv_obj_get_screen(label));
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "text_small with dots and no height holds one line",
                 "[ui_text][one_line]") {
    lv_obj_t* label = build(*this, "one_line_default",
                            std::string("<text_small name=\"l\" width=\"100%\" long_mode=\"dots\""
                                        " text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_height(label) == line_height("font_small"));
    CHECK(ends_with_dots(label));
}

TEST_CASE_METHOD(XMLTestFixture, "text_body with dots and height=content holds one line",
                 "[ui_text][one_line]") {
    lv_obj_t* label = build(*this, "one_line_content",
                            std::string("<text_body name=\"l\" width=\"100%\" height=\"content\""
                                        " long_mode=\"dots\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_height(label) == line_height("font_body"));
    CHECK(ends_with_dots(label));
}

TEST_CASE_METHOD(XMLTestFixture, "an explicit px height opts a dotted label out",
                 "[ui_text][one_line]") {
    lv_obj_t* label = build(*this, "one_line_px",
                            std::string("<text_small name=\"l\" width=\"100%\" height=\"57\""
                                        " long_mode=\"dots\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_height(label) == 57);
}

TEST_CASE_METHOD(XMLTestFixture, "style_max_height opts a dotted label out and lets it wrap",
                 "[ui_text][one_line]") {
    lv_obj_t* label = build(*this, "one_line_maxh",
                            std::string("<text_small name=\"l\" width=\"100%\""
                                        " style_max_height=\"200\" long_mode=\"dots\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_height(label) > line_height("font_small"));
}

TEST_CASE_METHOD(XMLTestFixture, "max_lines lets a dotted label wrap to N lines, then dots",
                 "[ui_text][one_line][max_lines]") {
    lv_obj_t* label = build(*this, "max_lines_long",
                            std::string("<text_small name=\"l\" width=\"100%\" long_mode=\"dots\""
                                        " max_lines=\"2\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    settle(label);
    const int32_t two_lines =
        line_height("font_small") * 2 + lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    CHECK(lv_obj_get_height(label) == two_lines);
    CHECK(ends_with_dots(label));
}

TEST_CASE_METHOD(XMLTestFixture, "max_lines leaves text that fits at its own height",
                 "[ui_text][one_line][max_lines]") {
    lv_obj_t* label = build(*this, "max_lines_short",
                            "<text_small name=\"l\" width=\"100%\" long_mode=\"dots\""
                            " max_lines=\"2\" text=\"Short\"/>",
                            "l");
    REQUIRE(label != nullptr);
    settle(label);
    CHECK(lv_obj_get_height(label) == line_height("font_small"));
    CHECK_FALSE(ends_with_dots(label));
}

TEST_CASE_METHOD(XMLTestFixture, "max_lines below 1 still holds a dotted label to one line",
                 "[ui_text][one_line][max_lines]") {
    lv_obj_t* label = build(*this, "max_lines_zero",
                            std::string("<text_small name=\"l\" width=\"100%\" long_mode=\"dots\""
                                        " max_lines=\"0\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    settle(label);
    CHECK(lv_obj_get_height(label) == line_height("font_small"));
    CHECK(ends_with_dots(label));
}

TEST_CASE_METHOD(XMLTestFixture, "an explicit height opts a dotted label out of max_lines",
                 "[ui_text][one_line][max_lines]") {
    lv_obj_t* label = build(*this, "max_lines_px",
                            std::string("<text_small name=\"l\" width=\"100%\" height=\"57\""
                                        " long_mode=\"dots\" max_lines=\"2\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    settle(label);
    CHECK(lv_obj_get_height(label) == 57);
    CHECK(lv_label_get_max_lines(label) == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "text_button honours max_lines",
                 "[ui_text][one_line][max_lines]") {
    lv_obj_t* label = build(*this, "max_lines_text_button",
                            std::string("<text_button name=\"l\" width=\"100%\" long_mode=\"dots\""
                                        " max_lines=\"2\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    settle(label);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    REQUIRE(font != nullptr);
    CHECK(lv_obj_get_height(label) == lv_font_get_line_height(font) * 2 +
                                          lv_obj_get_style_text_line_space(label, LV_PART_MAIN));
    CHECK(ends_with_dots(label));
}

TEST_CASE_METHOD(XMLTestFixture, "wrap and scroll long modes are left at content height",
                 "[ui_text][one_line]") {
    SECTION("wrap") {
        lv_obj_t* label = build(*this, "one_line_wrap",
                                std::string("<text_small name=\"l\" width=\"100%\""
                                            " long_mode=\"wrap\" text=\"") +
                                    kLongText + "\"/>",
                                "l");
        REQUIRE(label != nullptr);
        CHECK(lv_obj_get_height(label) > line_height("font_small"));
        CHECK(lv_obj_get_style_height(label, LV_PART_MAIN) == LV_SIZE_CONTENT);
    }
    SECTION("scroll") {
        lv_obj_t* label = build(*this, "one_line_scroll",
                                std::string("<text_small name=\"l\" width=\"100%\""
                                            " long_mode=\"scroll\" text=\"") +
                                    kLongText + "\"/>",
                                "l");
        REQUIRE(label != nullptr);
        CHECK(lv_obj_get_style_height(label, LV_PART_MAIN) == LV_SIZE_CONTENT);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "the one-line height includes the label's padding",
                 "[ui_text][one_line]") {
    lv_obj_t* label = build(*this, "one_line_pad",
                            std::string("<text_small name=\"l\" width=\"100%\""
                                        " style_pad_top=\"5\" style_pad_bottom=\"3\""
                                        " long_mode=\"dots\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_height(label) == line_height("font_small") + 8);
    CHECK(ends_with_dots(label));
}

TEST_CASE_METHOD(XMLTestFixture, "the one-line height follows a font and padding change",
                 "[ui_text][one_line]") {
    lv_obj_t* label = build(*this, "one_line_refont",
                            std::string("<text_small name=\"l\" width=\"100%\""
                                        " long_mode=\"dots\" text=\"") +
                                kLongText + "\"/>",
                            "l");
    REQUIRE(label != nullptr);
    REQUIRE(line_height("font_heading") != line_height("font_small"));

    SECTION("local font") {
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_heading"), LV_PART_MAIN);
        settle(label);
        CHECK(lv_obj_get_height(label) == line_height("font_heading"));
        CHECK(ends_with_dots(label));
    }
    SECTION("shared face style") {
        helix::ui::apply_font_style(label, theme_manager_get_font("font_heading"));
        settle(label);
        CHECK(lv_obj_get_height(label) == line_height("font_heading"));
    }
    SECTION("padding") {
        lv_obj_set_style_pad_bottom(label, 6, LV_PART_MAIN);
        settle(label);
        CHECK(lv_obj_get_height(label) == line_height("font_small") + 6);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "ui_button with dots holds its label to one line",
                 "[ui_text][one_line][ui_button]") {
    lv_obj_t* btn = build(*this, "one_line_button",
                          std::string("<ui_button name=\"b\" width=\"90\" height=\"80\""
                                      " long_mode=\"dots\" text=\"") +
                              kLongText + "\"/>",
                          "b");
    REQUIRE(btn != nullptr);
    lv_obj_t* label = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
        lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class)) {
            label = child;
        }
    }
    REQUIRE(label != nullptr);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    CHECK(lv_obj_get_height(label) == lv_font_get_line_height(font) +
                                          lv_obj_get_style_space_top(label, LV_PART_MAIN) +
                                          lv_obj_get_style_space_bottom(label, LV_PART_MAIN));
    CHECK(ends_with_dots(label));
}
