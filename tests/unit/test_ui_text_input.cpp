// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_text_input.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include "catch_amalgamated.hpp"

/**
 * @brief Unit tests for ui_text_input.cpp - Custom text input widget
 *
 * These drive the shipped widget: src/ui/ui_text_input.cpp is linked into the test
 * binary and registers <text_input> through its own ui_text_input_init(). It used to
 * be excluded from the link while ui_test_utils.cpp registered a hand-written copy,
 * so every assertion here described the copy rather than the widget that ships.
 *
 * Tests cover:
 * - placeholder attribute (shorthand for placeholder_text)
 * - max_length attribute for limiting input length
 * - keyboard_hint attribute
 */

/**
 * @brief Helper to create a text_input widget with attributes
 * @param parent Parent object
 * @param attrs NULL-terminated key-value pairs
 * @return Created text_input widget or nullptr
 */
static lv_obj_t* create_text_input(lv_obj_t* parent, const char** attrs) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "text_input", attrs));
}

// ============================================================================
// Placeholder Attribute Tests
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "text_input placeholder attribute sets placeholder text",
                 "[text_input][xml][placeholder]") {
    SECTION("placeholder attribute works as shorthand for placeholder_text") {
        const char* attrs[] = {"placeholder", "Enter value...", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        const char* placeholder = lv_textarea_get_placeholder_text(text_input);
        REQUIRE(placeholder != nullptr);
        REQUIRE(std::string(placeholder) == "Enter value...");
    }

    SECTION("placeholder_text attribute also works (inherited from textarea)") {
        const char* attrs[] = {"placeholder_text", "Type here", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        const char* placeholder = lv_textarea_get_placeholder_text(text_input);
        REQUIRE(placeholder != nullptr);
        REQUIRE(std::string(placeholder) == "Type here");
    }

    SECTION("empty placeholder") {
        const char* attrs[] = {"placeholder", "", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        const char* placeholder = lv_textarea_get_placeholder_text(text_input);
        // Empty string is valid
        REQUIRE(placeholder != nullptr);
        REQUIRE(std::string(placeholder).empty());
    }
}

// ============================================================================
// placeholder_tag Translation Tests
// ============================================================================

#if LV_USE_TRANSLATION
TEST_CASE_METHOD(LVGLUITestFixture, "text_input placeholder_tag translates the placeholder",
                 "[text_input][xml][placeholder][i18n]") {
    // Self-contained translation pack with a unique tag that can't collide with
    // the app's real translations.
    const char* pack =
        "<translations languages=\"en de\">"
        "  <translation tag=\"ti_test_placeholder\" en=\"Search...\" de=\"Suchen...\"/>"
        "</translations>";
    REQUIRE(lv_xml_register_translation_from_data(pack) == LV_RESULT_OK);

    SECTION("resolves to the active language's translation") {
        lv_translation_set_language("de");
        const char* attrs[] = {"placeholder_tag", "ti_test_placeholder", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        const char* placeholder = lv_textarea_get_placeholder_text(text_input);
        REQUIRE(placeholder != nullptr);
        REQUIRE(std::string(placeholder) == "Suchen...");
    }

    SECTION("falls back to the base language string") {
        lv_translation_set_language("en");
        const char* attrs[] = {"placeholder_tag", "ti_test_placeholder", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        const char* placeholder = lv_textarea_get_placeholder_text(text_input);
        REQUIRE(placeholder != nullptr);
        REQUIRE(std::string(placeholder) == "Search...");
    }

    SECTION("placeholder_tag overrides placeholder_text when both are present") {
        lv_translation_set_language("de");
        // placeholder_text first, placeholder_tag second — the order the XML uses
        const char* attrs[] = {"placeholder_text", "literal fallback", "placeholder_tag",
                               "ti_test_placeholder", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        const char* placeholder = lv_textarea_get_placeholder_text(text_input);
        REQUIRE(placeholder != nullptr);
        REQUIRE(std::string(placeholder) == "Suchen...");
    }
}
#endif // LV_USE_TRANSLATION

// ============================================================================
// Max Length Attribute Tests
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "text_input max_length attribute limits input",
                 "[text_input][xml][max_length]") {
    SECTION("max_length of 10 limits characters") {
        const char* attrs[] = {"max_length", "10", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        uint32_t max_len = lv_textarea_get_max_length(text_input);
        REQUIRE(max_len == 10);
    }

    SECTION("max_length of 9 for hex colors") {
        // Real use case from color_picker.xml: #RRGGBBAA = 9 chars
        const char* attrs[] = {"max_length", "9", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        uint32_t max_len = lv_textarea_get_max_length(text_input);
        REQUIRE(max_len == 9);
    }

    SECTION("max_length of 0 means unlimited") {
        const char* attrs[] = {"max_length", "0", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        uint32_t max_len = lv_textarea_get_max_length(text_input);
        REQUIRE(max_len == 0);
    }

    SECTION("large max_length value") {
        const char* attrs[] = {"max_length", "1000", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        uint32_t max_len = lv_textarea_get_max_length(text_input);
        REQUIRE(max_len == 1000);
    }

    SECTION("no max_length attribute defaults to unlimited") {
        const char* attrs[] = {"width", "100", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        // Default max_length in LVGL is 0 (unlimited)
        uint32_t max_len = lv_textarea_get_max_length(text_input);
        REQUIRE(max_len == 0);
    }
}

// ============================================================================
// Combined Attribute Tests
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "text_input combined attributes work together",
                 "[text_input][xml]") {
    SECTION("placeholder and max_length together") {
        const char* attrs[] = {"placeholder", "Enter G-code...", "max_length", "100", nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        const char* placeholder = lv_textarea_get_placeholder_text(text_input);
        REQUIRE(placeholder != nullptr);
        REQUIRE(std::string(placeholder) == "Enter G-code...");

        uint32_t max_len = lv_textarea_get_max_length(text_input);
        REQUIRE(max_len == 100);
    }

    SECTION("all custom attributes together") {
        const char* attrs[] = {"placeholder",   "#RRGGBB", "max_length", "9",
                               "keyboard_hint", "text",    nullptr};
        lv_obj_t* text_input = create_text_input(test_screen(), attrs);
        REQUIRE(text_input != nullptr);

        const char* placeholder = lv_textarea_get_placeholder_text(text_input);
        REQUIRE(placeholder != nullptr);
        REQUIRE(std::string(placeholder) == "#RRGGBB");

        uint32_t max_len = lv_textarea_get_max_length(text_input);
        REQUIRE(max_len == 9);
    }
}
