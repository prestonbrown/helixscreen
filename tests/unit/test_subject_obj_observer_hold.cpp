// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_obj_observer_hold.cpp
 * @brief Widget observers held while the display sleeps (prestonbrown/helixscreen#2037).
 *
 * lv_subject_hold_obj_observers() holds the notifications of observers bound to a widget
 * and leaves every other observer alone, so data consumers keep seeing each value while
 * nobody can see the widgets. The release brings every held widget to the subject's
 * current value, once.
 */

#include "../lvgl_test_fixture.h"
#include "core/subject.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Ends a hold a failing test left on, so the next test starts released.
struct HoldScope {
    HoldScope() {
        lv_subject_hold_obj_observers(true);
    }
    ~HoldScope() {
        lv_subject_hold_obj_observers(false);
    }
    HoldScope(const HoldScope&) = delete;
    HoldScope& operator=(const HoldScope&) = delete;
};

void record_cb(lv_observer_t* observer, lv_subject_t* subject) {
    static_cast<std::vector<int>*>(lv_observer_get_user_data(observer))
        ->push_back(lv_subject_get_int(subject));
}

std::string text_of(lv_obj_t* label) {
    return lv_label_get_text(label);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "held widget observers wait while data observers see every value",
                 "[observer_hold][sleep]") {
    helix::Subject subject;
    subject.init_int(1);
    lv_obj_t* label = lv_label_create(test_screen());
    lv_label_bind_text(label, &subject, "%d");
    std::vector<int> widget_values;
    lv_subject_add_observer_obj(&subject, record_cb, label, &widget_values);
    std::vector<int> data_values;
    lv_subject_add_observer(&subject, record_cb, &data_values);
    REQUIRE(text_of(label) == "1");
    widget_values.clear();
    data_values.clear();

    {
        HoldScope hold;
        for (int v : {2, 3, 4}) {
            lv_subject_set_int(&subject, v);
        }
        CHECK(text_of(label) == "1");
        CHECK(widget_values.empty());
        CHECK(data_values == std::vector<int>{2, 3, 4});
        CHECK(lv_subject_obj_observers_held_count() == 2);
    }

    CHECK(text_of(label) == "4");
    CHECK(widget_values == std::vector<int>{4});
    CHECK(lv_subject_obj_observers_held_count() == 0);

    lv_subject_set_int(&subject, 5);
    CHECK(text_of(label) == "5");
    CHECK(widget_values == std::vector<int>{4, 5});
}

TEST_CASE_METHOD(LVGLTestFixture, "a widget deleted while held is not notified by the release",
                 "[observer_hold][sleep]") {
    helix::Subject subject;
    subject.init_int(0);
    lv_obj_t* kept = lv_label_create(test_screen());
    lv_obj_t* doomed = lv_label_create(test_screen());
    lv_label_bind_text(kept, &subject, "%d");
    std::vector<int> doomed_values;
    lv_subject_add_observer_obj(&subject, record_cb, doomed, &doomed_values);
    doomed_values.clear();

    {
        HoldScope hold;
        lv_subject_set_int(&subject, 9);
        REQUIRE(lv_subject_obj_observers_held_count() == 2);
        lv_obj_delete(doomed);
        CHECK(lv_subject_obj_observers_held_count() == 1);
    }

    CHECK(doomed_values.empty());
    CHECK(text_of(kept) == "9");
}

TEST_CASE_METHOD(LVGLTestFixture, "a release from inside a notification waits for the next one",
                 "[observer_hold][sleep]") {
    helix::Subject trigger;
    trigger.init_int(0);
    helix::Subject value;
    value.init_int(0);
    lv_obj_t* label = lv_label_create(test_screen());
    lv_label_bind_text(label, &value, "%d");
    lv_subject_add_observer(
        &trigger,
        [](lv_observer_t*, lv_subject_t* s) {
            if (lv_subject_get_int(s) == 1) {
                lv_subject_hold_obj_observers(false);
            }
        },
        nullptr);

    HoldScope hold;
    lv_subject_set_int(&value, 7);
    lv_subject_set_int(&trigger, 1);
    CHECK(lv_subject_obj_observers_held());
    CHECK(text_of(label) == "0");

    lv_subject_hold_obj_observers(false);
    CHECK(text_of(label) == "7");
}
