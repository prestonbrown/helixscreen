// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Duplicate-title suppression depends on a NAME AGREEMENT between C++ and the
// XML. ui_notification.cpp's error paths suppress a second error modal when
// the top modal already shows the same title, reading it under
// kModalTitleWidgetName (ui_modal.h). ActionPromptModal's XML named its title
// widget "title" instead, so the lookup found nothing and identical error
// modals stacked (issue #1389). All C++ sites share the constant; the XML
// literal is the one remaining copy, and this test pins their agreement: it
// looks up the title under the constant, against the real modal tree.
//
// The contract is pinned against the real modal tree, as
// test_fault_modal_dismiss.cpp does. The assertions below are the two prerequisites of the
// suppression predicate, asserted per modal type: the top modal's title label is reachable under
// the canonical widget name, and it carries the title text.
//
// A ui_dialog alert (what ui_notification_error itself pushes) is the control;
// ActionPromptModal (a Klipper action:prompt) and RecoveryFaultModal (what
// RecoveryModalPresenter puts on top for a recoverable jam) are the dialogs that
// must agree with it.

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "action_prompt_modal.h"
#include "core/update_queue.h"
#include "recovery_modal_presenter.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

class DuplicateTitleFixture : public LVGLUITestFixture {
  public:
    DuplicateTitleFixture() {
        modal_.set_gcode_callback([](const std::string&) { /* no-op */ });
    }

    helix::ui::ActionPromptModal modal_;
};

} // namespace

TEST_CASE_METHOD(DuplicateTitleFixture,
                 "An error alert's title is reachable under the canonical widget name",
                 "[1389][notification][modal]") {
    // Control half: modal_alert (what ui_notification_error pushes) is
    // built from modal_dialog.xml, which names its title "dialog_title". This
    // case proves the canonical name works at all, so the ActionPromptModal
    // case cannot pass by some other accident.
    lv_obj_t* dialog =
        helix::ui::modal_alert("Printer Error", "Move out of range", ModalSeverity::Error, "OK");
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(dialog != nullptr);
    lv_obj_t* top = helix::ui::modal_get_top();
    REQUIRE(top != nullptr);
    lv_obj_t* label = lv_obj_find_by_name(top, helix::ui::kModalTitleWidgetName);
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Printer Error");

    Modal::hide(dialog);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(DuplicateTitleFixture,
                 "An ActionPromptModal on top is recognised by the duplicate-title check",
                 "[1389][notification][modal]") {
    // A second error with the same title as the prompt on top must be
    // suppressed, which the dedup can only do if this modal's title is
    // reachable under the SAME canonical name.
    helix::PromptData prompt;
    prompt.title = "Toolhead jam";
    prompt.text_lines.push_back("Clear the jam and resume.");
    helix::PromptButton resume;
    resume.label = "Resume";
    resume.gcode = "RESUME";
    prompt.buttons.push_back(resume);
    prompt.severity = "error";

    REQUIRE(modal_.show_prompt(test_screen(), prompt));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(Modal::any_visible());
    lv_obj_t* top = helix::ui::modal_get_top();
    REQUIRE(top != nullptr);
    lv_obj_t* label = lv_obj_find_by_name(top, helix::ui::kModalTitleWidgetName);
    // The assertion that was red before #1389: the lookup found nothing.
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Toolhead jam");

    modal_.hide();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(DuplicateTitleFixture,
                 "A recovery fault dialog on top is recognised by the duplicate-title check",
                 "[1389][notification][modal]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::ErrorEvent e;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "Clear the jam and resume.";
    e.recovery_actions = {{"Resume", "RESUME", "t::resume", "primary"}};
    presenter.present(e);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(presenter.is_visible());
    lv_obj_t* top = helix::ui::modal_get_top();
    REQUIRE(top != nullptr);
    lv_obj_t* label = lv_obj_find_by_name(top, helix::ui::kModalTitleWidgetName);
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Toolhead jam");

    presenter.dismiss();
    helix::ui::UpdateQueue::instance().drain();
}
