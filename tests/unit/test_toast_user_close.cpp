// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toast_user_close.cpp
 * @brief show_with_action()'s user-closed callback: runs on the X button's
 * removal path only, never on hide(), timeout or stack eviction.
 */

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/toast_manager_test_access.h"
#include "display_settings_manager.h"

#include <cstdint>
#include <iterator>

#include "../catch_amalgamated.hpp"

namespace {

int g_closed_count = 0;
void* g_closed_data = nullptr;

void counting_closed_cb(void* user_data) {
    ++g_closed_count;
    g_closed_data = user_data;
}

void noop_action_cb(void*) {}

void* data_42() {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(42));
}

void drain() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

} // namespace

namespace {

struct UserCloseFixture : LVGLUITestFixture {
    UserCloseFixture() {
        animations_were_enabled_ =
            helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        g_closed_count = 0;
        g_closed_data = nullptr;
    }
    ~UserCloseFixture() override {
        ToastManagerTestAccess::clear(ToastManager::instance());
        helix::DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
        drain();
    }

    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(UserCloseFixture, "Toast user close: the X path runs the closed callback once",
                 "[toast][user-close]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_t* widget = lv_obj_create(host);

    ToastManagerTestAccess::inject_closeable(tm, widget, noop_action_cb, data_42(),
                                             counting_closed_cb);
    ToastManagerTestAccess::user_close(tm, widget);
    ToastManagerTestAccess::user_close(tm, widget); // already exiting: no second fire
    drain(); // the closed callback runs deferred, like the action button's

    CHECK(g_closed_count == 1);
    CHECK(g_closed_data == data_42());

    lv_obj_delete(host);
    drain();
}

// The whole wiring, not the pieces: a toast shown through show_with_action
// with a closed callback, closed through the XML close button like a user
// does, runs the callback once.
TEST_CASE_METHOD(UserCloseFixture, "Toast user close: show_with_action wires the X end to end",
                 "[toast][user-close]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    tm.init();
    tm.show_with_action(ToastSeverity::WARNING, "seam check", "OK", noop_action_cb, data_42(), 0,
                        counting_closed_cb);
    drain();
    REQUIRE(tm.is_visible());

    lv_obj_t* btn = lv_obj_find_by_name(lv_layer_top(), "toast_close_btn");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    drain();

    CHECK(g_closed_count == 1);
    CHECK(g_closed_data == data_42());
    CHECK_FALSE(tm.is_visible());

    tm.deinit_subjects();
    drain();
}

TEST_CASE_METHOD(UserCloseFixture, "Toast user close: hide() does not run the closed callback",
                 "[toast][user-close]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_t* widget = lv_obj_create(host);

    ToastManagerTestAccess::inject_closeable(tm, widget, noop_action_cb, data_42(),
                                             counting_closed_cb);
    tm.hide(noop_action_cb, data_42());
    drain();

    CHECK(g_closed_count == 0);

    lv_obj_delete(host);
    drain();
}
