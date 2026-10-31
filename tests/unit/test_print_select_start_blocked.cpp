// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_start_blocked.cpp
 * @brief Starting a print from print select stays blocked while one runs.
 *
 * print_select_can_print drives the detail view's Print button (disabled at 0)
 * and print_select_blocked_reason carries the one-line why shown beside it.
 * The state-transition observers are registered by setup(), so the fixture
 * builds the real XML tree and runs setup() the way the app does; without it
 * the panel still computes a correct initial state but never reacts to a
 * transition. The second case pins the init-time compute alone, which is the
 * mid-print entry path: the panel's subjects are created while the enum
 * already holds PRINTING, so no transition ever fires
 * (prestonbrown/helixscreen#1395).
 */

#include "ui_panel_print_select.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <lvgl.h>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// The real panel over the real XML: subjects, widget tree and the
/// state-transition observers that setup() registers.
struct StartBlockedFixture : public LVGLUITestFixture {
    StartBlockedFixture() {
        panel_ = std::make_unique<PrintSelectPanel>(state(), nullptr);
        panel_->init_subjects();
        root_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_select_panel", nullptr));
        REQUIRE(root_ != nullptr);
        panel_->setup(root_, test_screen());
    }

    ~StartBlockedFixture() override {
        // XML tree first: its bindings observe subjects the panel owns.
        UpdateQueue::instance().drain();
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
    }

    void set_print_state(PrintState s) {
        lv_subject_set_int(state().print_state().get_print_lifecycle_subject(),
                           static_cast<int>(s));
        UpdateQueue::instance().drain();
    }

    /// set_job_queue_available() defers its subject write through UpdateQueue
    void set_job_queue(bool available) {
        state().capabilities_state().set_job_queue_available(available);
        UpdateQueue::instance().drain();
    }

    int can_print() const {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "print_select_can_print"));
    }

    int button_mode() const {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "print_select_button_mode"));
    }

    std::string button_label() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "print_select_button_label");
        return s ? lv_subject_get_string(s) : std::string("<missing>");
    }

    std::string button_icon() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "print_select_button_icon");
        return s ? lv_subject_get_string(s) : std::string("<missing>");
    }

    std::string blocked_reason() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "print_select_blocked_reason");
        return s ? lv_subject_get_string(s) : std::string("<missing>");
    }

    std::unique_ptr<PrintSelectPanel> panel_;
    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(StartBlockedFixture, "print select blocks start and states why while printing",
                 "[print_select][1395]") {
    // Idle baseline: printable, no reason on screen.
    set_print_state(PrintState::Idle);
    REQUIRE(can_print() == 1);
    CHECK(blocked_reason().empty());

    // Every state that holds the machine blocks the start and says why.
    for (PrintState s : {PrintState::Printing, PrintState::Paused}) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        CHECK(can_print() == 0);
        CHECK_FALSE(blocked_reason().empty());
    }

    // Terminal states release the block and clear the reason.
    for (PrintState s : {PrintState::Complete, PrintState::Cancelled, PrintState::Error}) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        CHECK(can_print() == 1);
        CHECK(blocked_reason().empty());
    }
}

TEST_CASE_METHOD(StartBlockedFixture,
                 "print select offers Add to Queue while printing when "
                 "Moonraker has the job_queue component",
                 "[print_select][queue]") {
    set_job_queue(true);

    for (PrintState s : {PrintState::Printing, PrintState::Paused}) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        // Queue mode: the button stays enabled, relabels, swaps to the
        // queue's clock icon, and the reason line becomes the queue hint
        // instead of a block.
        CHECK(can_print() == 1);
        CHECK(button_mode() == 1);
        CHECK(button_label() == "Add to Queue");
        CHECK(button_icon() == "progress_clock");
        CHECK(blocked_reason() == "Starts after the current print");
    }

    // The print ending while the detail view is open flips everything back.
    set_print_state(PrintState::Complete);
    CHECK(can_print() == 1);
    CHECK(button_mode() == 0);
    CHECK(button_label() == "Print");
    CHECK(button_icon() == "print");
    CHECK(blocked_reason().empty());
}

TEST_CASE_METHOD(StartBlockedFixture,
                 "print select stays blocked while printing without the "
                 "job_queue component",
                 "[print_select][queue]") {
    set_print_state(PrintState::Printing);
    CHECK(can_print() == 0);
    CHECK(button_mode() == 0);
    CHECK(button_label() == "Print");
    CHECK(button_icon() == "print");
    CHECK_FALSE(blocked_reason().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "queue mode hides the cards the queue cannot honor",
                 "[print_select][queue][xml]") {
    // The detail view's card hiding is declarative: two subjects drive
    // bind_flag bindings in print_file_detail.xml. Neither owner (panel,
    // JobQueueState) is part of the fixture's init, so register both the way
    // the header_bar subject tests do and drive the real XML. Static storage:
    // the global registry keeps the pointer after the test ends, and a later
    // test resolving these production names must never land on a dead stack.
    static lv_subject_t mode_subj;
    static lv_subject_t transition_subj;
    lv_subject_init_int(&mode_subj, 0);
    lv_xml_register_subject(nullptr, "print_select_button_mode", &mode_subj);
    lv_subject_init_int(&transition_subj, 0);
    lv_xml_register_subject(nullptr, "job_queue_automatic_transition", &transition_subj);

    // Without preprint options the options card is hidden for an unrelated
    // reason; assert the queue-mode hiding from the state where it shows.
    lv_subject_t* const any_options = lv_xml_get_subject(nullptr, "has_any_preprint_options");
    REQUIRE(any_options != nullptr);
    lv_subject_set_int(any_options, 1);

    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_file_detail", nullptr));
    REQUIRE(root != nullptr);
    lv_obj_t* const filament_card = lv_obj_find_by_name(root, "filament_mapping_card");
    lv_obj_t* const options_card = lv_obj_find_by_name(root, "options_card");
    REQUIRE(filament_card != nullptr);
    REQUIRE(options_card != nullptr);

    auto set_mode = [&](int mode, int automatic) {
        lv_subject_set_int(&mode_subj, mode);
        lv_subject_set_int(&transition_subj, automatic);
        UpdateQueue::instance().drain();
    };

    // Print mode: both cards stay.
    set_mode(0, 0);
    CHECK_FALSE(lv_obj_has_flag(filament_card, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(options_card, LV_OBJ_FLAG_HIDDEN));

    // Queue mode, manual transition: the mapping is dropped (it is recomputed
    // at start time) but the options card stays — those ARE saved with the job.
    set_mode(1, 0);
    CHECK(lv_obj_has_flag(filament_card, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(options_card, LV_OBJ_FLAG_HIDDEN));

    // Queue mode with automatic_transition: nobody ever reads saved options,
    // so both go.
    set_mode(1, 1);
    CHECK(lv_obj_has_flag(filament_card, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(options_card, LV_OBJ_FLAG_HIDDEN));

    // Print ending while the view is open brings both back.
    set_mode(0, 1);
    CHECK_FALSE(lv_obj_has_flag(filament_card, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(options_card, LV_OBJ_FLAG_HIDDEN));

    // Tree first: its bindings observe the static subjects above.
    UpdateQueue::instance().drain();
    lv_obj_delete(root);
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a panel initialized mid-print starts blocked",
                 "[print_select][1395]") {
    // The state holds BEFORE the panel's subjects exist, which is the mid-print
    // entry path: no transition ever fires after init, so only the init-time
    // update_print_button_state() call can have published the blocked state.
    lv_subject_set_int(state().print_state().get_print_lifecycle_subject(),
                       static_cast<int>(PrintState::Printing));

    auto panel = std::make_unique<PrintSelectPanel>(state(), nullptr);
    panel->init_subjects();

    lv_subject_t* can_print = lv_xml_get_subject(nullptr, "print_select_can_print");
    REQUIRE(can_print != nullptr);
    CHECK(lv_subject_get_int(can_print) == 0);

    lv_subject_t* reason = lv_xml_get_subject(nullptr, "print_select_blocked_reason");
    REQUIRE(reason != nullptr);
    CHECK_FALSE(std::string(lv_subject_get_string(reason)).empty());

    UpdateQueue::instance().drain();
    panel.reset();
    UpdateQueue::instance().drain();
}
