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
// ui_notification.o is excluded from the test link (mk/tests.mk) and its stub
// builds no modal at all - the same constraint test_fault_modal_dismiss.cpp
// documents, answered the same way: pin the contract against the real modal
// tree. The assertions below are the two prerequisites of the suppression
// predicate, asserted per modal type: the top modal's title label is reachable
// under the canonical widget name, and it carries the title text.
//
// Two cases, because the contract has two halves: a ui_dialog alert (what
// ui_notification_error itself pushes) is the control; ActionPromptModal (what
// RecoveryModalPresenter puts on top for a recoverable jam) is the half that
// was broken.

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "action_prompt_modal.h"

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
    // Control half: modal_show_alert (what ui_notification_error pushes) is
    // built from modal_dialog.xml, which names its title "dialog_title". This
    // case proves the canonical name works at all, so the ActionPromptModal
    // case cannot pass by some other accident.
    lv_obj_t* dialog = helix::ui::modal_show_alert("Printer Error", "Move out of range",
                                                   ModalSeverity::Error, "OK");
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
    // The broken half: RecoveryModalPresenter shows an ActionPromptModal for a
    // recoverable jam carrying the error's title. A second error with the same
    // title must be suppressed, which the dedup can only do if this modal's
    // title is reachable under the SAME canonical name.
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
