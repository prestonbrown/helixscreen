// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_display_cleanup.cpp
 * @brief Deleting a temp_display detaches every observer from its bound subjects.
 *
 * The widget's child labels observe external subjects. Those observers must be
 * removed before the widget's owned subjects are deinited, or a later notify on
 * the external subject walks freed memory.
 */

#include "ui_temp_display.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

/**
 * Helper: count observers on a subject by walking its linked list.
 * LVGL does not expose a public observer count API, so we inspect
 * the internal subs_ll linked list directly.
 */
static int count_subject_observers(lv_subject_t* subject) {
    int count = 0;
    void* node = lv_ll_get_head(&subject->subs_ll);
    while (node != nullptr) {
        count++;
        node = lv_ll_get_next(&subject->subs_ll, node);
    }
    return count;
}

TEST_CASE_METHOD(LVGLTestFixture, "external subject has no observers after temp_display deletion",
                 "[temp_display]") {
    // Register the temp_display custom widget
    ui_temp_display_init();

    // Create an external integer subject simulating a temperature value
    lv_subject_t current_temp_subject;
    lv_subject_init_int(&current_temp_subject, 0);

    // Register it as a global XML subject so temp_display can find it via bind_current
    lv_xml_register_subject(nullptr, "test_current_temp", &current_temp_subject);

    // Create a container to hold the temp_display widget
    lv_obj_t* container = lv_obj_create(test_screen());

    // Create temp_display with bind_current pointing to our external subject
    const char* attrs[] = {"bind_current", "test_current_temp", nullptr};
    lv_obj_t* td = static_cast<lv_obj_t*>(lv_xml_create(container, "temp_display", attrs));
    REQUIRE(td != nullptr);

    // Verify the external subject has at least one observer (from temp_display binding)
    REQUIRE(count_subject_observers(&current_temp_subject) > 0);

    // Delete the container (which deletes the temp_display and all children)
    lv_obj_delete(container);

    // After deletion, the external subject should have zero observers remaining
    CHECK(count_subject_observers(&current_temp_subject) == 0);

    // Clean up the external subject
    lv_subject_deinit(&current_temp_subject);
}
