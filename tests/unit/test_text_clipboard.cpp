// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_text_clipboard.cpp
 * @brief The in-app clipboard's rules and the Cut / Copy / Paste menu on text fields
 */

#include "ui_keyboard_manager.h"
#include "ui_text_edit_menu.h"

#include "../test_fixtures.h"
#include "core/update_queue.h"
#include "text_clipboard.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::TextEditAction;

TEST_CASE("clipboard: size cap, field sanitizing and offered actions", "[clipboard]") {
    SECTION("text over the cap is cut at a character boundary") {
        CHECK(clipboard::clip_to_limit("abc", 3) == "abc");
        CHECK(clipboard::clip_to_limit("abcd", 3) == "abc");
        // "é" is two bytes; a cut through it backs off to before it.
        CHECK(clipboard::clip_to_limit("a\xC3\xA9", 2) == "a");
        CHECK(clipboard::clip_to_limit(std::string(clipboard::kMaxBytes + 10, 'x')).size() ==
              clipboard::kMaxBytes);
    }

    SECTION("a one-line field gets line breaks and tabs as spaces") {
        CHECK(clipboard::sanitize_for_field("a\r\nb\nc\rd\te", true) == "a b c d e");
        CHECK(clipboard::sanitize_for_field("a\r\nb\rc\td", false) == "a\nb\nc\td");
        CHECK(clipboard::sanitize_for_field(std::string("x\x01y\x7Fz\0", 6), true) == "xyz");
    }

    SECTION("a password field offers nothing; otherwise each action needs its text") {
        CHECK_FALSE(clipboard::actions_for_field(true, true, true).any());
        const auto empty_field = clipboard::actions_for_field(false, false, true);
        CHECK_FALSE(empty_field.cut);
        CHECK_FALSE(empty_field.copy);
        CHECK(empty_field.paste);
        const auto empty_clipboard = clipboard::actions_for_field(false, true, false);
        CHECK(empty_clipboard.cut);
        CHECK(empty_clipboard.copy);
        CHECK_FALSE(empty_clipboard.paste);
    }
}

namespace {

lv_obj_t* make_field(lv_obj_t* parent, const char* text, bool one_line = true) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, one_line);
    lv_textarea_set_text(ta, text);
    return ta;
}

std::string field_text(lv_obj_t* ta) {
    return lv_textarea_get_text(ta);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "clipboard: edit actions on a text field", "[clipboard]") {
    clipboard::clear();

    SECTION("copy, cut and paste move text between fields") {
        lv_obj_t* source = make_field(test_screen(), "192.168.1.20");
        lv_obj_t* target = make_field(test_screen(), "host ");

        REQUIRE(ui::apply_text_edit_action(source, TextEditAction::Copy));
        CHECK(clipboard::get_text() == "192.168.1.20");
        CHECK(field_text(source) == "192.168.1.20");

        REQUIRE(ui::apply_text_edit_action(target, TextEditAction::Paste));
        CHECK(field_text(target) == "host 192.168.1.20");

        REQUIRE(ui::apply_text_edit_action(target, TextEditAction::Cut));
        CHECK(field_text(target).empty());
        CHECK(clipboard::get_text() == "host 192.168.1.20");
    }

    SECTION("a paste keeps to the field: one line, its max length") {
        clipboard::set_text("G28\nG1 Z5");
        lv_obj_t* one_line = make_field(test_screen(), "");
        REQUIRE(ui::apply_text_edit_action(one_line, TextEditAction::Paste));
        CHECK(field_text(one_line) == "G28 G1 Z5");

        lv_obj_t* short_field = make_field(test_screen(), "");
        lv_textarea_set_max_length(short_field, 4);
        REQUIRE(ui::apply_text_edit_action(short_field, TextEditAction::Paste));
        CHECK(field_text(short_field) == "G28 ");
    }

    SECTION("a password field neither gives up its text nor takes a paste") {
        clipboard::set_text("clip");
        lv_obj_t* secret = make_field(test_screen(), "hunter2");
        lv_textarea_set_password_mode(secret, true);

        CHECK_FALSE(ui::apply_text_edit_action(secret, TextEditAction::Copy));
        CHECK_FALSE(ui::apply_text_edit_action(secret, TextEditAction::Cut));
        CHECK_FALSE(ui::apply_text_edit_action(secret, TextEditAction::Paste));
        CHECK(clipboard::get_text() == "clip");
        CHECK(field_text(secret) == "hunter2");
    }

    clipboard::clear();
}

TEST_CASE_METHOD(XMLTestFixture, "clipboard: the menu shows only what the field offers",
                 "[clipboard][context-menu]") {
    REQUIRE(register_component("text_edit_menu"));
    clipboard::clear();
    ui::TextEditMenu menu;

    lv_obj_t* field = make_field(test_screen(), "PLA");
    REQUIRE(menu.show_for(field));
    lv_obj_t* backdrop = lv_obj_get_child(lv_obj_get_screen(field), -1);
    lv_obj_t* paste_row = lv_obj_find_by_name(backdrop, "text_edit_paste");
    lv_obj_t* copy_row = lv_obj_find_by_name(backdrop, "text_edit_copy");
    REQUIRE(paste_row != nullptr);
    REQUIRE(copy_row != nullptr);
    CHECK(lv_obj_is_hidden(paste_row));
    CHECK_FALSE(lv_obj_is_hidden(copy_row));

    // Tapping Copy closes the menu and copies the field it was raised for.
    lv_obj_send_event(copy_row, LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(menu.is_visible());
    CHECK(clipboard::get_text() == "PLA");

    lv_obj_t* secret = make_field(test_screen(), "hunter2");
    lv_textarea_set_password_mode(secret, true);
    CHECK_FALSE(menu.show_for(secret));
    CHECK_FALSE(menu.is_visible());

    clipboard::clear();
}

TEST_CASE_METHOD(XMLTestFixture, "clipboard: holding a managed field raises the menu",
                 "[clipboard][context-menu]") {
    REQUIRE(register_component("text_edit_menu"));
    auto& keyboard = KeyboardManager::instance();
    keyboard.reset();
    keyboard.init(test_screen());

    lv_obj_t* field = make_field(test_screen(), "PETG");
    keyboard.register_textarea(field);
    lv_obj_send_event(field, LV_EVENT_LONG_PRESSED, nullptr);
    CHECK(ui::ContextMenu::active_as<ui::TextEditMenu>() != nullptr);
    ui::text_edit_menu_reset();

    // A field that opted out of typing does not offer a paste either.
    keyboard.unregister_textarea(field);
    lv_obj_send_event(field, LV_EVENT_LONG_PRESSED, nullptr);
    CHECK(ui::ContextMenu::active() == nullptr);

    keyboard.reset();
    helix::ui::UpdateQueue::instance().drain();
}
