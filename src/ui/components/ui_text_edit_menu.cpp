// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_text_edit_menu.h"

#include "ui_callback_helpers.h"
#include "ui_utils.h"

#include "core/static_subject_registry.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace helix::ui {

namespace {

constexpr const char* kCutSubject = "text_edit_can_cut";
constexpr const char* kCopySubject = "text_edit_can_copy";
constexpr const char* kPasteSubject = "text_edit_can_paste";

lv_subject_t s_can_cut{};
lv_subject_t s_can_copy{};
lv_subject_t s_can_paste{};
bool s_subjects_ready = false;

std::unique_ptr<TextEditMenu>& shared_menu() {
    static std::unique_ptr<TextEditMenu> menu;
    return menu;
}

void ensure_subjects() {
    if (s_subjects_ready) {
        return;
    }
    lv_subject_init_int(&s_can_cut, 0);
    lv_subject_init_int(&s_can_copy, 0);
    lv_subject_init_int(&s_can_paste, 0);
    lv_xml_register_subject(nullptr, kCutSubject, &s_can_cut);
    lv_xml_register_subject(nullptr, kCopySubject, &s_can_copy);
    lv_xml_register_subject(nullptr, kPasteSubject, &s_can_paste);
    s_subjects_ready = true;
    StaticSubjectRegistry::instance().register_deinit(
        "TextEditMenu",
        []() {
            // The menu's widgets bind these subjects, so it goes first.
            text_edit_menu_reset();
            if (s_subjects_ready) {
                lv_xml_unregister_subject(nullptr, kCutSubject);
                lv_xml_unregister_subject(nullptr, kCopySubject);
                lv_xml_unregister_subject(nullptr, kPasteSubject);
                lv_subject_deinit(&s_can_cut);
                lv_subject_deinit(&s_can_copy);
                lv_subject_deinit(&s_can_paste);
                s_subjects_ready = false;
            }
        },
        StaticSubjectRegistry::Lifetime::Process);
}

bool field_has_text(lv_obj_t* textarea) {
    const char* text = lv_textarea_get_text(textarea);
    return text != nullptr && text[0] != '\0';
}

} // namespace

clipboard::EditActions text_edit_actions(lv_obj_t* textarea) {
    if (textarea == nullptr) {
        return {};
    }
    return clipboard::actions_for_field(lv_textarea_get_password_mode(textarea),
                                        field_has_text(textarea), clipboard::has_text());
}

bool apply_text_edit_action(lv_obj_t* textarea, TextEditAction action) {
    const clipboard::EditActions offered = text_edit_actions(textarea);
    switch (action) {
    case TextEditAction::Copy:
    case TextEditAction::Cut:
        if (!(action == TextEditAction::Cut ? offered.cut : offered.copy)) {
            return false;
        }
        clipboard::set_text(lv_textarea_get_text(textarea));
        if (action == TextEditAction::Cut) {
            lv_textarea_set_text(textarea, "");
        }
        return true;
    case TextEditAction::Paste: {
        if (!offered.paste) {
            return false;
        }
        const std::string text = clipboard::sanitize_for_field(clipboard::get_text(),
                                                               lv_textarea_get_one_line(textarea));
        if (text.empty()) {
            return false;
        }
        lv_textarea_add_text(textarea, text.c_str());
        return true;
    }
    }
    return false;
}

void TextEditMenu::register_callbacks() {
    ensure_subjects();
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_xml_callbacks({
        {"text_edit_cut_cb", [](lv_event_t*) { pick(TextEditAction::Cut); }},
        {"text_edit_copy_cb", [](lv_event_t*) { pick(TextEditAction::Copy); }},
        {"text_edit_paste_cb", [](lv_event_t*) { pick(TextEditAction::Paste); }},
    });
}

bool TextEditMenu::show_for(lv_obj_t* textarea) {
    register_callbacks();
    const clipboard::EditActions offered = text_edit_actions(textarea);
    if (!offered.any()) {
        return false;
    }
    lv_subject_set_int(&s_can_cut, offered.cut ? 1 : 0);
    lv_subject_set_int(&s_can_copy, offered.copy ? 1 : 0);
    lv_subject_set_int(&s_can_paste, offered.paste ? 1 : 0);
    target_ = textarea;
    set_action_callback([this](int action, int /*item*/) {
        lv_obj_t* field = target_.get();
        target_ = nullptr;
        if (action < 0 || field == nullptr) {
            return;
        }
        apply_text_edit_action(field, static_cast<TextEditAction>(action));
    });
    return show_around_widget(lv_obj_get_screen(textarea), -1, textarea);
}

void TextEditMenu::pick(TextEditAction action) {
    auto* self = ContextMenu::active_as<TextEditMenu>();
    if (!self) {
        spdlog::warn("[TextEditMenu] No active instance for a row tap");
        return;
    }
    spdlog::debug("[TextEditMenu] Action {}", static_cast<int>(action));
    self->dispatch_action(static_cast<int>(action));
}

void text_edit_long_press_cb(lv_event_t* e) {
    lv_obj_t* textarea = lv_event_get_current_target_obj(e);
    if (textarea == nullptr) {
        return;
    }
    auto& menu = shared_menu();
    if (!menu) {
        menu = std::make_unique<TextEditMenu>();
    }
    if (menu->is_visible()) {
        return;
    }
    if (menu->show_for(textarea)) {
        // The release would otherwise click the field and move its cursor.
        end_press(e);
    }
}

void text_edit_menu_reset() {
    shared_menu().reset();
}

} // namespace helix::ui
