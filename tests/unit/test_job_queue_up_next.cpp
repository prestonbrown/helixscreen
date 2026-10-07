// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_up_next.cpp
 * @brief The home panel's "Up next" line and the completion modal's
 * "Start next" secondary.
 *
 * Three layers, each pinned separately:
 *  - the pure pieces: text composition (format_up_next_text /
 *    format_start_next_text) and the tap routing decision
 *    (decide_up_next_tap);
 *  - the subject wiring: JobQueueState publishes the composed strings
 *    settled-before-count, so a count observer never reads a half-updated
 *    line;
 *  - the XML wiring: the completion modal's secondary and the "Up next"
 *    rows follow job_queue_count, and the modal secondary's label follows
 *    job_queue_start_next_text.
 */

#include "ui_panel_print_status.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/job_queue_subjects_fixture.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_start.h"
#include "job_queue_state.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// ScopedJobQueueSubjects / status_with: shared test_helpers/job_queue_subjects_fixture.h

/// The print status panel owns most of the subjects its XML binds, so the
/// panel test builds the real owner (mirrors SubjectOwner<PrintStatusPanel>
/// in test_ui_panel_bindings.cpp).
struct ScopedPrintStatusPanelSubjects {
    explicit ScopedPrintStatusPanelSubjects(PrinterState& st) : panel_(st, nullptr) {
        panel_.init_subjects();
    }
    ~ScopedPrintStatusPanelSubjects() {
        panel_.deinit_subjects();
    }
    ScopedPrintStatusPanelSubjects(const ScopedPrintStatusPanelSubjects&) = delete;
    ScopedPrintStatusPanelSubjects& operator=(const ScopedPrintStatusPanelSubjects&) = delete;

  private:
    PrintStatusPanel panel_;
};

lv_obj_t* require_named(lv_obj_t* root, const char* name) {
    REQUIRE(root != nullptr);
    lv_obj_t* found = lv_obj_find_by_name(root, name);
    INFO("looking for widget named '" << name << "'");
    REQUIRE(found != nullptr);
    return found;
}

/// ui_button's internal label is unnamed; walk the button's children for the
/// one carrying text.
std::string button_label_text(lv_obj_t* btn) {
    uint32_t n = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* t = lv_label_get_text(child);
            if (t && t[0]) {
                return t;
            }
        }
    }
    return {};
}

} // namespace

// ============================================================================
// Pure pieces
// ============================================================================

TEST_CASE("format_up_next_text composes prefix, name and remaining count", "[job_queue][up_next]") {
    SECTION("empty queue yields no line at all") {
        REQUIRE(format_up_next_text("benchy", 0).empty());
        REQUIRE(format_up_next_text("benchy", -1).empty());
    }
    SECTION("an empty name is not a line either") {
        REQUIRE(format_up_next_text("", 3).empty());
    }
    SECTION("one job names it with no count suffix") {
        const std::string text = format_up_next_text("benchy", 1);
        // Literals, not lv_tr compositions: the whole format string is the
        // translation key now, so what the function returns under the base
        // locale is exactly this text.
        REQUIRE(text == "Up next: benchy");
        REQUIRE(text.find(" (+") == std::string::npos);
    }
    SECTION("more than one job appends the remaining count") {
        const std::string text = format_up_next_text("benchy", 3);
        REQUIRE(text == "Up next: benchy (+2)");
    }
}

TEST_CASE("format_start_next_text composes the completion modal's button label",
          "[job_queue][up_next][print_completion]") {
    SECTION("empty queue yields no label (the button hides with it)") {
        REQUIRE(format_start_next_text("benchy", 0).empty());
    }
    SECTION("a queued job is named after the verb") {
        REQUIRE(format_start_next_text("benchy", 2) == "Start next: benchy");
    }
    SECTION("no count suffix leaks into the button") {
        REQUIRE(format_start_next_text("benchy", 5).find(" (+") == std::string::npos);
    }
}

TEST_CASE("decide_up_next_tap routes by whether the printer can take a job",
          "[job_queue][up_next]") {
    SECTION("a free printer starts the next job") {
        REQUIRE(decide_up_next_tap(true) == UpNextTapAction::StartNextJob);
    }
    SECTION("a busy or blocked printer gets the queue modal") {
        // job_holds_machine and a host-side pre-print block both land here.
        REQUIRE(decide_up_next_tap(false) == UpNextTapAction::OpenQueueModal);
    }
}

// ============================================================================
// Subject wiring
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "up next subjects publish settled before the count",
                 "[job_queue][up_next][subjects]") {
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();

    lv_subject_t* count = lv_xml_get_subject(nullptr, "job_queue_count");
    REQUIRE(count != nullptr);
    lv_subject_t* up_next = lv_xml_get_subject(nullptr, "job_queue_up_next_text");
    REQUIRE(up_next != nullptr);
    lv_subject_t* start_next = lv_xml_get_subject(nullptr, "job_queue_start_next_text");
    REQUIRE(start_next != nullptr);
    REQUIRE(std::string(lv_subject_get_string(up_next)).empty());
    REQUIRE(std::string(lv_subject_get_string(start_next)).empty());

    // Whatever a count observer reads at publish time must already be the
    // settled value — count is the rebuild trigger the queue surfaces use.
    std::string up_next_seen_on_count;
    lv_observer_t* count_observer = lv_subject_add_observer(
        count,
        [](lv_observer_t* observer, lv_subject_t*) {
            auto* dest = static_cast<std::string*>(lv_observer_get_user_data(observer));
            lv_subject_t* up_next = lv_xml_get_subject(nullptr, "job_queue_up_next_text");
            if (up_next != nullptr) {
                const char* text = lv_subject_get_string(up_next);
                *dest = text ? text : "";
            }
        },
        &up_next_seen_on_count);

    JobQueueStateTestAccess::deliver_status(jqs, status_with(3));
    CHECK(lv_subject_get_int(count) == 3);
    CHECK(std::string(lv_subject_get_string(up_next)) == format_up_next_text("file-0", 3));
    CHECK(std::string(lv_subject_get_string(start_next)) == format_start_next_text("file-0", 3));
    CHECK(up_next_seen_on_count == std::string(lv_subject_get_string(up_next)));

    JobQueueStateTestAccess::deliver_status(jqs, status_with(0));
    CHECK(lv_subject_get_int(count) == 0);
    CHECK(std::string(lv_subject_get_string(up_next)).empty());
    CHECK(std::string(lv_subject_get_string(start_next)).empty());
    CHECK(up_next_seen_on_count.empty());

    lv_observer_remove(count_observer);
}

// ============================================================================
// XML wiring
// ============================================================================

TEST_CASE("JobQueueState subject init registers the queue-start XML callbacks",
          "[job_queue][up_next]") {
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();

    // The "Up next" rows and the completion modal's secondary resolve these
    // names; an unbound callback fails silently at XML creation, so the
    // lookup itself is the only observable that pins the registration.
    REQUIRE(lv_xml_get_event_cb(nullptr, "on_up_next_tap") != nullptr);
    REQUIRE(lv_xml_get_event_cb(nullptr, "on_print_complete_start_next") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "completion modal secondary appears only with a queued job",
                 "[job_queue][up_next][print_completion][xml]") {
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();

    lv_obj_t* modal =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_completion_modal", nullptr));
    REQUIRE(modal != nullptr);

    lv_obj_t* secondary = require_named(modal, "btn_secondary");
    lv_obj_t* primary = require_named(modal, "btn_primary");

    // Empty queue: OK alone, exactly the pre-queue modal.
    CHECK(lv_obj_has_flag(secondary, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(primary, LV_OBJ_FLAG_HIDDEN));

    JobQueueStateTestAccess::deliver_status(jqs, status_with(3));
    CHECK_FALSE(lv_obj_has_flag(secondary, LV_OBJ_FLAG_HIDDEN));
    CHECK(button_label_text(secondary) == format_start_next_text("file-0", 3));
    CHECK_FALSE(lv_obj_has_flag(primary, LV_OBJ_FLAG_HIDDEN));

    JobQueueStateTestAccess::deliver_status(jqs, status_with(0));
    CHECK(lv_obj_has_flag(secondary, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(modal);
}

TEST_CASE_METHOD(LVGLUITestFixture, "home print status widget shows the up next line while queued",
                 "[job_queue][up_next][print_status_widget][xml]") {
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();

    // The up next line belongs to the active views, which the card builds only during a print.
    lv_subject_t* view = lv_xml_get_subject(nullptr, "print_status_view");
    static lv_subject_t local_view; // outlives its registration
    if (!view) {
        lv_subject_init_int(&local_view, 0);
        lv_xml_register_subject(nullptr, "print_status_view", &local_view);
        view = &local_view;
    }
    const int32_t saved_view = lv_subject_get_int(view);
    lv_subject_set_int(view, 3);

    lv_obj_t* card =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_print_status", nullptr));
    REQUIRE(card != nullptr);

    lv_obj_t* row = require_named(card, "up_next_row");
    lv_obj_t* label = require_named(card, "up_next_label");

    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    JobQueueStateTestAccess::deliver_status(jqs, status_with(2));
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(label)) == format_up_next_text("file-0", 2));

    JobQueueStateTestAccess::deliver_status(jqs, status_with(0));
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(card);
    lv_subject_set_int(view, saved_view);
}

TEST_CASE_METHOD(LVGLUITestFixture, "print status panel shows the up next line while queued",
                 "[job_queue][up_next][print_status_panel][xml]") {
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();
    ScopedPrintStatusPanelSubjects panel_subjects(get_printer_state());

    lv_obj_t* panel =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_status_panel", nullptr));
    REQUIRE(panel != nullptr);

    lv_obj_t* row = require_named(panel, "up_next_row");
    lv_obj_t* label = require_named(panel, "up_next_label");

    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    JobQueueStateTestAccess::deliver_status(jqs, status_with(3));
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(label)) == format_up_next_text("file-0", 3));

    lv_obj_delete(panel);
}
