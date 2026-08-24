// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_manager_overlay.cpp
 * @brief Unit tests for PrinterManagerOverlay
 *
 * Tests subject initialization, lifecycle guards, and global accessor pattern.
 * Uses LVGLTestFixture for LVGL-dependent subject operations.
 *
 * @see ui_printer_manager_overlay.h
 */

#include "ui_printer_manager_overlay.h"

#include "../lvgl_test_fixture.h"
#include "subject_debug_registry.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Observer body is irrelevant - the test only cares that attaching the
/// observer to a widget installs an LV_EVENT_DELETE hook on that widget, and
/// that lv_subject_deinit() takes it back off again.
void sentinel_observer_cb(lv_observer_t*, lv_subject_t*) {}

/// One of the four subjects PrinterManagerOverlay::init_subjects() publishes.
constexpr const char* OWNED_SUBJECT_NAME = "pm_name_editing";

} // namespace

// =============================================================================
// Basic Properties
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: get_name returns expected value",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;
    REQUIRE(std::string(overlay.get_name()) == "Printer Manager");
}

// =============================================================================
// Subject Initialization
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: init_subjects sets initialized flag",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;

    REQUIRE_FALSE(overlay.are_subjects_initialized());

    overlay.init_subjects();

    REQUIRE(overlay.are_subjects_initialized());
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: double init_subjects does not crash",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;

    overlay.init_subjects();
    REQUIRE(overlay.are_subjects_initialized());

    // Second call should be a no-op (guarded)
    overlay.init_subjects();
    REQUIRE(overlay.are_subjects_initialized());
}

// =============================================================================
// Global Accessor Pattern
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: global accessor returns valid reference",
                 "[printer_manager]") {
    PrinterManagerOverlay& overlay = get_printer_manager_overlay();
    REQUIRE(std::string(overlay.get_name()) == "Printer Manager");

    // Cleanup for other tests
    destroy_printer_manager_overlay();
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: global accessor returns same instance",
                 "[printer_manager]") {
    PrinterManagerOverlay& first = get_printer_manager_overlay();
    PrinterManagerOverlay& second = get_printer_manager_overlay();

    REQUIRE(&first == &second);

    destroy_printer_manager_overlay();
}

// =============================================================================
// Destructor / Cleanup
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterManagerOverlay: destructor cleans up initialized subjects",
                 "[printer_manager]") {
    // ~PrinterManagerOverlay() -> deinit_subjects_base() -> lv_subject_deinit()
    // on every subject the overlay published. lv_subject_deinit() walks the
    // subject's observer list and calls lv_observer_remove(), which for an
    // obj-bound observer strips the LV_EVENT_DELETE hook that
    // lv_subject_add_observer_obj() installed on the target widget.
    //
    // So a sentinel widget's event count is direct, checkable evidence: it goes
    // up by one when we observe the overlay's subject, and only comes back down
    // if the destructor actually deinitialized that subject. A destructor that
    // skips cleanup leaves the hook - and a dangling observer pointing at freed
    // subject memory - in place, with no crash to give it away.
    lv_obj_t* sentinel = lv_obj_create(test_screen());
    const uint32_t baseline_events = lv_obj_get_event_count(sentinel);
    lv_observer_t* observer = nullptr;

    {
        PrinterManagerOverlay overlay;
        overlay.init_subjects();
        REQUIRE(overlay.are_subjects_initialized());

        lv_subject_t* owned = SubjectDebugRegistry::instance().lookup_by_name(OWNED_SUBJECT_NAME);
        REQUIRE(owned != nullptr);

        observer = lv_subject_add_observer_obj(owned, sentinel_observer_cb, sentinel, nullptr);
        REQUIRE(observer != nullptr);
        REQUIRE(lv_obj_get_event_count(sentinel) == baseline_events + 1);
        // Destructor runs here.
    }

    const uint32_t after_events = lv_obj_get_event_count(sentinel);

    if (after_events != baseline_events) {
        // Broken build: the hook still points at a freed observer/subject, so
        // deleting the sentinel would fault before Catch2 could report. Strip it
        // by user_data (a null cb means "any callback") and let the check below
        // do the reporting.
        lv_obj_remove_event_cb_with_user_data(sentinel, nullptr, observer);
    }
    lv_obj_delete(sentinel);

    REQUIRE(after_events == baseline_events);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: destructor safe without init_subjects",
                 "[printer_manager]") {
    // An overlay that never called init_subjects() owns nothing, so neither its
    // construction nor its destruction may disturb the subject another instance
    // published under the same XML name. Pin that with a live owner: the registry
    // entry must still resolve to the owner's subject afterwards, and the owner's
    // observer hook must still be attached.
    PrinterManagerOverlay owner;
    owner.init_subjects();
    lv_subject_t* owned = SubjectDebugRegistry::instance().lookup_by_name(OWNED_SUBJECT_NAME);
    REQUIRE(owned != nullptr);

    lv_obj_t* sentinel = lv_obj_create(test_screen());
    const uint32_t baseline_events = lv_obj_get_event_count(sentinel);
    lv_observer_t* observer =
        lv_subject_add_observer_obj(owned, sentinel_observer_cb, sentinel, nullptr);
    REQUIRE(observer != nullptr);
    REQUIRE(lv_obj_get_event_count(sentinel) == baseline_events + 1);

    {
        PrinterManagerOverlay uninitialized;
        REQUIRE_FALSE(uninitialized.are_subjects_initialized());
        // Destructor runs here - it must be a no-op.
    }

    REQUIRE(SubjectDebugRegistry::instance().lookup_by_name(OWNED_SUBJECT_NAME) == owned);
    REQUIRE(lv_obj_get_event_count(sentinel) == baseline_events + 1);

    // Owner is still alive; drop the observer before the sentinel so teardown
    // order cannot matter.
    lv_observer_remove(observer);
    lv_obj_delete(sentinel);
}

// =============================================================================
// Visibility / Lifecycle State
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: initially not visible",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;
    REQUIRE_FALSE(overlay.is_visible());
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: cleanup sets flag", "[printer_manager]") {
    PrinterManagerOverlay overlay;
    REQUIRE_FALSE(overlay.cleanup_called());

    overlay.cleanup();

    REQUIRE(overlay.cleanup_called());
}

// =============================================================================
// Overlay Root State
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: root is null before create",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;
    REQUIRE(overlay.get_root() == nullptr);
}
