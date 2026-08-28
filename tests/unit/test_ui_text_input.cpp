// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_text_input.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include <cstring>
#include <string>

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

// ============================================================================
// Two-way bind_text, multiline ordering, clear button, keyboard hint
//
// None of the below could run until src/ui/ui_text_input.cpp was linked into the
// test binary. While ui_test_utils.cpp registered a hand-written <text_input>,
// every case in this file exercised that copy, and the copy implemented none of
// these four features — so they shipped across 113 bind_text files, 11
// keyboard_hint files, 3 multiline files and both search boxes with no coverage
// at all.
// ============================================================================

namespace {

int g_clear_cb_calls = 0;

void test_clear_callback(lv_event_t* /*e*/) {
    ++g_clear_cb_calls;
}

} // namespace

class TextInputBindingFixture : public LVGLUITestFixture {
  public:
    TextInputBindingFixture() {
        std::memset(text_buf_, 0, sizeof(text_buf_));
        lv_subject_init_string(&text_subject_, text_buf_, nullptr, sizeof(text_buf_), "initial");
        lv_xml_register_subject(nullptr, "ti_text_subject", &text_subject_);

        // A deliberately wrong type: bind_text must refuse it rather than
        // reinterpret an int subject's value as a char*.
        lv_subject_init_int(&int_subject_, 7);
        lv_xml_register_subject(nullptr, "ti_int_subject", &int_subject_);

        g_clear_cb_calls = 0;
        lv_xml_register_event_cb(nullptr, "ti_clear_cb", test_clear_callback);
    }

    ~TextInputBindingFixture() override {
        lv_subject_deinit(&text_subject_);
        lv_subject_deinit(&int_subject_);
    }

    lv_subject_t* text_subject() {
        return &text_subject_;
    }

  private:
    char text_buf_[64]{};
    lv_subject_t text_subject_{};
    lv_subject_t int_subject_{};
};

TEST_CASE_METHOD(TextInputBindingFixture,
                 "text_input bind_text drives the textarea from the subject",
                 "[text_input][xml][bind_text]") {
    const char* attrs[] = {"bind_text", "ti_text_subject", nullptr};
    lv_obj_t* ta = create_text_input(test_screen(), attrs);
    REQUIRE(ta != nullptr);

    // The observer fires on attach, so the subject's current value lands immediately.
    REQUIRE(std::string(lv_textarea_get_text(ta)) == "initial");

    lv_subject_copy_string(text_subject(), "from subject");
    CHECK(std::string(lv_textarea_get_text(ta)) == "from subject");
}

TEST_CASE_METHOD(TextInputBindingFixture, "text_input bind_text writes edits back to the subject",
                 "[text_input][xml][bind_text]") {
    const char* attrs[] = {"bind_text", "ti_text_subject", nullptr};
    lv_obj_t* ta = create_text_input(test_screen(), attrs);
    REQUIRE(ta != nullptr);

    // What a keystroke amounts to: the text changes, then VALUE_CHANGED fires.
    lv_textarea_set_text(ta, "typed by user");
    lv_obj_send_event(ta, LV_EVENT_VALUE_CHANGED, nullptr);

    REQUIRE(lv_subject_get_string(text_subject()) != nullptr);
    CHECK(std::string(lv_subject_get_string(text_subject())) == "typed by user");

    // The write-back must not re-enter through the observer and clobber the
    // textarea; g_updating_from_textarea exists for exactly this.
    CHECK(std::string(lv_textarea_get_text(ta)) == "typed by user");
}

TEST_CASE_METHOD(TextInputBindingFixture, "text_input bind_text refuses a bad subject",
                 "[text_input][xml][bind_text]") {
    SECTION("an unknown subject name leaves the widget usable but unbound") {
        const char* attrs[] = {"bind_text", "no_such_subject", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        REQUIRE(ta != nullptr); // warns and continues — must not abort the parse

        lv_textarea_set_text(ta, "still editable");
        lv_obj_send_event(ta, LV_EVENT_VALUE_CHANGED, nullptr);
        CHECK(std::string(lv_textarea_get_text(ta)) == "still editable");
    }

    SECTION("an int subject is rejected rather than read as a char*") {
        // Binding this would hand lv_textarea_set_text() the integer 7 as a
        // pointer on the very first notify.
        const char* attrs[] = {"bind_text", "ti_int_subject", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        REQUIRE(ta != nullptr);
        CHECK(std::string(lv_textarea_get_text(ta)).empty());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "text_input multiline is applied before sizing",
                 "[text_input][xml][multiline]") {
    SECTION("one_line is the default for form inputs") {
        const char* attrs[] = {nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        REQUIRE(ta != nullptr);
        CHECK(lv_textarea_get_one_line(ta));
    }

    SECTION("multiline=true clears one_line mode") {
        const char* attrs[] = {"multiline", "true", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        REQUIRE(ta != nullptr);
        CHECK_FALSE(lv_textarea_get_one_line(ta));
    }

    SECTION("an explicit height survives alongside multiline") {
        // The whole reason multiline is pre-scanned ahead of lv_xml_textarea_apply:
        // in one_line mode the textarea sizes itself to its content, so handling
        // multiline in the ordinary attribute loop would let auto-sizing overwrite
        // the height that was just applied. Moving the pre-scan after the standard
        // apply fails this SECTION and no other.
        const char* attrs[] = {"multiline", "true", "height", "120", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        REQUIRE(ta != nullptr);
        lv_obj_update_layout(ta);
        CHECK_FALSE(lv_textarea_get_one_line(ta));
        CHECK(lv_obj_get_height(ta) == 120);
    }
}

TEST_CASE_METHOD(TextInputBindingFixture, "text_input clear button tracks content",
                 "[text_input][xml][clear_button]") {
    SECTION("no button unless asked for") {
        const char* attrs[] = {nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        REQUIRE(ta != nullptr);
        CHECK(lv_obj_find_by_name(ta, "text_input_clear_btn") == nullptr);
    }

    SECTION("hidden while empty, shown once there is text") {
        const char* attrs[] = {"show_clear_button", "true", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        REQUIRE(ta != nullptr);

        lv_obj_t* btn = lv_obj_find_by_name(ta, "text_input_clear_btn");
        REQUIRE(btn != nullptr);
        CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

        lv_textarea_set_text(ta, "query");
        lv_obj_send_event(ta, LV_EVENT_VALUE_CHANGED, nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("clicking it empties the field and hides itself") {
        const char* attrs[] = {"show_clear_button", "true", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        lv_obj_t* btn = lv_obj_find_by_name(ta, "text_input_clear_btn");
        REQUIRE(btn != nullptr);

        lv_textarea_set_text(ta, "query");
        lv_obj_send_event(ta, LV_EVENT_VALUE_CHANGED, nullptr);
        REQUIRE_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));

        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        CHECK(std::string(lv_textarea_get_text(ta)).empty());
        CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
    }
}

TEST_CASE_METHOD(TextInputBindingFixture, "text_input clear_callback fires on clear",
                 "[text_input][xml][clear_button]") {
    SECTION("a registered callback runs when the button is clicked") {
        const char* attrs[] = {"show_clear_button", "true", "clear_callback", "ti_clear_cb",
                               nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        lv_obj_t* btn = lv_obj_find_by_name(ta, "text_input_clear_btn");
        REQUIRE(btn != nullptr);
        REQUIRE(g_clear_cb_calls == 0);

        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);

        // The panel repopulates from here; without it the search list keeps
        // showing the filtered results after the box has been emptied.
        CHECK(g_clear_cb_calls == 1);
    }

    SECTION("an unknown callback name warns but leaves the button working") {
        const char* attrs[] = {"show_clear_button", "true", "clear_callback", "no_such_cb",
                               nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        lv_obj_t* btn = lv_obj_find_by_name(ta, "text_input_clear_btn");
        REQUIRE(btn != nullptr);

        lv_textarea_set_text(ta, "query");
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        CHECK(std::string(lv_textarea_get_text(ta)).empty());
        CHECK(g_clear_cb_calls == 0);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "text_input keyboard_hint round-trips through user_data",
                 "[text_input][xml][keyboard_hint]") {
    SECTION("numeric") {
        const char* attrs[] = {"keyboard_hint", "numeric", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        CHECK(ui_text_input_get_keyboard_hint(ta) == KeyboardHint::NUMERIC);
    }

    SECTION("explicit text") {
        const char* attrs[] = {"keyboard_hint", "text", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        CHECK(ui_text_input_get_keyboard_hint(ta) == KeyboardHint::TEXT);
    }

    SECTION("absent defaults to text") {
        const char* attrs[] = {nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        CHECK(ui_text_input_get_keyboard_hint(ta) == KeyboardHint::TEXT);
    }

    SECTION("an unrecognised value warns and falls back to text") {
        const char* attrs[] = {"keyboard_hint", "emoji", nullptr};
        lv_obj_t* ta = create_text_input(test_screen(), attrs);
        CHECK(ui_text_input_get_keyboard_hint(ta) == KeyboardHint::TEXT);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "text_input keyboard_hint guards foreign user_data",
                 "[text_input][xml][keyboard_hint]") {
    // The hint is packed into user_data behind a magic value because user_data is
    // shared territory — ui_button stores a UiButtonData* there. Without the magic
    // check, any textarea carrying an unrelated pointer would answer with whatever
    // its low nibble happened to be.
    SECTION("a null object is safe") {
        CHECK(ui_text_input_get_keyboard_hint(nullptr) == KeyboardHint::TEXT);
    }

    SECTION("a plain textarea is not mistaken for a text_input") {
        lv_obj_t* plain = lv_textarea_create(test_screen());
        REQUIRE(plain != nullptr);
        CHECK(ui_text_input_get_keyboard_hint(plain) == KeyboardHint::TEXT);
    }

    SECTION("foreign user_data does not leak through as a hint") {
        lv_obj_t* plain = lv_textarea_create(test_screen());
        int owner_payload = 0;
        lv_obj_set_user_data(plain, &owner_payload);
        CHECK(ui_text_input_get_keyboard_hint(plain) == KeyboardHint::TEXT);
    }
}
