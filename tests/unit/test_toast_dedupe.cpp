// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_panel_common.h"
#include "ui_toast_manager.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/toast_manager_test_access.h"
#include "core/update_queue.h"
#include "lvgl/src/core/lv_obj_draw_private.h"
#include "lvgl/src/misc/lv_area_private.h"
#include "pending_startup_warnings.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

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

namespace {
void action_a(void*) {}
void action_b(void*) {}
} // namespace

TEST_CASE("Toast dedupe: an action toast folds only into the same action on the same target",
          "[toast][dedupe]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    int lane_5 = 5;
    int lane_6 = 6;
    ToastManagerTestAccess::inject_action(tm, ToastSeverity::INFO, "Same spool in Gate 5?",
                                          action_a, &lane_5);

    CHECK(ToastManagerTestAccess::refresh_action_duplicate(
        tm, ToastSeverity::INFO, "Same spool in Gate 5?", action_a, &lane_5));
    // Same words, different question: each keeps its own button.
    CHECK_FALSE(ToastManagerTestAccess::refresh_action_duplicate(
        tm, ToastSeverity::INFO, "Same spool in Gate 5?", action_a, &lane_6));
    CHECK_FALSE(ToastManagerTestAccess::refresh_action_duplicate(
        tm, ToastSeverity::INFO, "Same spool in Gate 5?", action_b, &lane_5));
    // A plain toast never absorbs an action toast's message, nor the reverse.
    CHECK_FALSE(ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::INFO,
                                                          "Same spool in Gate 5?"));
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

namespace {

int g_unknown_attr_lines = 0;

void unknown_attr_log_cb(lv_log_level_t level, const char* buf) {
    if (level == LV_LOG_LEVEL_WARN &&
        std::string(buf).find("Unknown attribute") != std::string::npos) {
        ++g_unknown_attr_lines;
    }
}

// Counts WARN lines about attributes no XML parser claims while alive — the
// only trace a typo'd attribute name leaves. Same contract as the fan-mark
// counter in test_fan_settings_xml.cpp: registered for the body only, and
// nullptr (LVGL's default path) restored after.
class ScopedUnknownAttrCounter {
  public:
    ScopedUnknownAttrCounter() {
        g_unknown_attr_lines = 0;
        lv_log_register_print_cb(unknown_attr_log_cb);
    }
    ~ScopedUnknownAttrCounter() {
        lv_log_register_print_cb(nullptr);
    }
    ScopedUnknownAttrCounter(const ScopedUnknownAttrCounter&) = delete;
    ScopedUnknownAttrCounter& operator=(const ScopedUnknownAttrCounter&) = delete;

    static int count() {
        return g_unknown_attr_lines;
    }
};

} // namespace

// A typo'd attribute name in a component's XML is silently ignored by the
// engine: the style value never applies and the only trace is an "Unknown
// attribute ... (typo?)" warning per render. Build the registered component
// the way ToastManager does — lv_xml_create with the toast's props — and pin
// the engine log clean: every
// style attribute on the card, shadow offset included, must be a name a
// parser claims.
TEST_CASE_METHOD(LVGLUITestFixture, "toast_notification renders without engine attribute warnings",
                 "[toast][xml]") {
    ScopedUnknownAttrCounter counter;

    const char* attrs[] = {"message", "attribute warning pin", "hide_action", "false", nullptr};
    lv_obj_t* toast =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "toast_notification", attrs));

    REQUIRE(toast != nullptr); // the component really built
    CHECK(ScopedUnknownAttrCounter::count() == 0);

    lv_obj_delete(toast);
}

// The toast is built the way ToastManager builds one, into a stack sized and
// anchored like its own, so the redraw can be measured without its animation.
TEST_CASE_METHOD(LVGLUITestFixture, "A new toast redraws only the area it ends up in",
                 "[toast][xml]") {
    lv_obj_t* stack = lv_obj_create(lv_layer_top());
    lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(stack, 0, LV_PART_MAIN);
    lv_obj_set_size(stack, 460, LV_SIZE_CONTENT);
    lv_obj_align(stack, LV_ALIGN_TOP_RIGHT, -24, 24);
    lv_refr_now(nullptr);

    std::vector<lv_area_t> invalidated;
    lv_display_t* disp = lv_display_get_default();
    const lv_event_cb_t record = [](lv_event_t* e) {
        static_cast<std::vector<lv_area_t>*>(lv_event_get_user_data(e))
            ->push_back(*static_cast<lv_area_t*>(lv_event_get_param(e)));
    };
    lv_display_add_event_cb(disp, record, LV_EVENT_INVALIDATE_AREA, &invalidated);

    const char* attrs[] = {
        "message", "Showing the 50 newest files. See more in the printer's web UI.", nullptr};
    lv_obj_t* toast = helix::ui::create_xml_laid_out(stack, "toast_notification", attrs);
    REQUIRE(toast != nullptr);
    lv_obj_update_layout(stack);
    lv_display_remove_event_cb_with_user_data(disp, record, &invalidated);

    // What the toast draws, its shadow included.
    lv_area_t drawn;
    lv_obj_get_coords(toast, &drawn);
    lv_area_increase(&drawn, lv_obj_get_ext_draw_size(toast), lv_obj_get_ext_draw_size(toast));
    CHECK(lv_area_get_height(&drawn) < 200);
    REQUIRE_FALSE(invalidated.empty());
    for (const lv_area_t& a : invalidated) {
        INFO(a.x1 << "," << a.y1 << "-" << a.x2 << "," << a.y2);
        CHECK(lv_area_is_in(&a, &drawn, 0));
    }

    lv_obj_delete(stack);
}

// A toast shown before init() waits in PendingStartupWarnings for the drain
// that follows init(), rather than being built before init() has registered
// its close-button callback and teardown. The component is registered here,
// so a toast built anyway would be visible.
TEST_CASE_METHOD(LVGLUITestFixture, "ToastManager queues a toast shown before init()", "[toast]") {
    auto& tm = ToastManager::instance();
    REQUIRE_FALSE(tm.is_initialized());

    tm.show_with_detail(ToastSeverity::WARNING, "before init", "detail line", 4000);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(tm.is_visible());

    std::vector<std::string> queued;
    helix::PendingStartupWarnings::instance().drain(
        [&queued](helix::PendingStartupWarnings::Severity sev, const std::string& m, uint32_t d) {
            CHECK(sev == helix::PendingStartupWarnings::Severity::WARNING);
            CHECK(d == 4000);
            queued.push_back(m);
        });
    CHECK(queued == std::vector<std::string>{"before init - detail line"});

    tm.init();
    tm.show(ToastSeverity::INFO, "after init", 4000);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(tm.is_visible());

    tm.deinit_subjects();
}
