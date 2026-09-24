// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_toast_manager.h"

#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

// Friend accessor (L065): reach ToastManager privates without test-only
// methods on the production class.
class ToastManagerTestAccess {
  public:
    static void inject(ToastManager& tm, ToastSeverity sev, const char* msg, bool exiting) {
        ToastManager::ToastInstance inst;
        inst.severity = sev;
        inst.message = msg;
        inst.is_exiting = exiting;
        tm.active_.push_back(std::move(inst));
    }
    static void inject_widget(ToastManager& tm, lv_obj_t* widget, ToastSeverity sev,
                              const char* msg) {
        ToastManager::ToastInstance inst;
        inst.widget = widget;
        inst.severity = sev;
        inst.message = msg;
        tm.active_.push_back(std::move(inst));
    }
    static ToastManager::ToastList::iterator find_owning_toast(ToastManager& tm, lv_obj_t* node) {
        return tm.find_owning_toast(node);
    }
    static ToastManager::ToastList::iterator list_end(ToastManager& tm) {
        return tm.active_.end();
    }
    static bool refresh_duplicate(ToastManager& tm, ToastSeverity sev, const char* msg) {
        return tm.refresh_duplicate(sev, msg);
    }
    static void clear(ToastManager& tm) {
        tm.active_.clear();
    }
};

TEST_CASE("Toast dedupe: identical active toast is refreshed, not duplicated", "[toast][dedupe]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);

    ToastManagerTestAccess::inject(tm, ToastSeverity::ERROR, "Jog failed: busy", false);
    CHECK(ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::ERROR, "Jog failed: busy"));

    ToastManagerTestAccess::clear(tm);
}

TEST_CASE("Toast dedupe: different message or severity does not match", "[toast][dedupe]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    ToastManagerTestAccess::inject(tm, ToastSeverity::ERROR, "Jog failed: busy", false);

    CHECK_FALSE(
        ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::WARNING, "Jog failed: busy"));
    CHECK_FALSE(ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::ERROR, "Other error"));
    ToastManagerTestAccess::clear(tm);
}

TEST_CASE("Toast dedupe: exiting toasts don't match", "[toast][dedupe]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    ToastManagerTestAccess::inject(tm, ToastSeverity::ERROR, "Jog failed: busy", true);

    CHECK_FALSE(
        ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::ERROR, "Jog failed: busy"));
    ToastManagerTestAccess::clear(tm);
}

TEST_CASE_METHOD(LVGLTestFixture, "Toast lifecycle: nested button resolves to its toast",
                 "[toast][lifecycle]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);

    lv_obj_t* stack = lv_obj_create(lv_layer_top());
    lv_obj_t* toast = lv_obj_create(stack);
    lv_obj_t* action_btn = lv_obj_create(toast);
    ToastManagerTestAccess::inject_widget(tm, toast, ToastSeverity::ERROR, "Switched printers");

    auto it = ToastManagerTestAccess::find_owning_toast(tm, action_btn);
    REQUIRE(it != ToastManagerTestAccess::list_end(tm));
    CHECK(it->widget == toast);

    // The toast root itself resolves too (close-button walk starts there).
    CHECK(ToastManagerTestAccess::find_owning_toast(tm, toast)->widget == toast);

    ToastManagerTestAccess::clear(tm);
    lv_obj_delete(stack);
}

TEST_CASE_METHOD(LVGLTestFixture, "Toast lifecycle: node outside any toast resolves to end",
                 "[toast][lifecycle]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);

    lv_obj_t* stack = lv_obj_create(lv_layer_top());
    lv_obj_t* toast = lv_obj_create(stack);
    ToastManagerTestAccess::inject_widget(tm, toast, ToastSeverity::ERROR, "msg");
    lv_obj_t* outsider = lv_obj_create(lv_layer_top());
    lv_obj_t* outsider_child = lv_obj_create(outsider);

    CHECK(ToastManagerTestAccess::find_owning_toast(tm, outsider) ==
          ToastManagerTestAccess::list_end(tm));
    CHECK(ToastManagerTestAccess::find_owning_toast(tm, outsider_child) ==
          ToastManagerTestAccess::list_end(tm));
    CHECK(ToastManagerTestAccess::find_owning_toast(tm, nullptr) ==
          ToastManagerTestAccess::list_end(tm));

    ToastManagerTestAccess::clear(tm);
    lv_obj_delete(stack);
    lv_obj_delete(outsider);
}

TEST_CASE_METHOD(LVGLTestFixture, "Toast lifecycle: button of an erased toast resolves to end",
                 "[toast][lifecycle]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);

    lv_obj_t* stack = lv_obj_create(lv_layer_top());
    lv_obj_t* toast = lv_obj_create(stack);
    lv_obj_t* action_btn = lv_obj_create(toast);
    ToastManagerTestAccess::inject_widget(tm, toast, ToastSeverity::ERROR, "msg");

    // The entry is gone from active_ while the widget is still alive — the
    // printer-switch state. The button must resolve to end(), not into freed
    // list memory.
    ToastManagerTestAccess::clear(tm);
    CHECK(ToastManagerTestAccess::find_owning_toast(tm, action_btn) ==
          ToastManagerTestAccess::list_end(tm));

    lv_obj_delete(stack);
}
