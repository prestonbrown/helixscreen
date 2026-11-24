// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A burst of one failure must not leave a stack of dialogs to dismiss.

#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "fault_modal_registry.h"
#include "gcode_error_router.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

namespace {

/// Labels under @p node whose text contains @p needle: one per dialog showing it.
int count_labels(lv_obj_t* node, const char* needle) {
    if (!node) {
        return 0;
    }
    int n = 0;
    if (lv_obj_check_type(node, &lv_label_class)) {
        const char* text = lv_label_get_text(node);
        n += text && std::strstr(text, needle) ? 1 : 0;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        n += count_labels(lv_obj_get_child(node, i), needle);
    }
    return n;
}

} // namespace

namespace {

struct ToastGuard {
    ToastGuard() {
        ToastManager::instance().init();
    }
    ~ToastGuard() {
        ToastManager::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

} // namespace

TEST_CASE("a fault repeated with new coordinates is the same fault", "[dup_dialogs]") {
    using helix::ui::fault_dedup_key;
    CHECK(fault_dedup_key("T", "Must home axis first: 94.700 86.700 59.000") ==
          fault_dedup_key("T", "Must home axis first: 60.000 105.000 61.000"));
    // Counters, line numbers and clock ticks fold too.
    CHECK(fault_dedup_key("T", "Timer too close at 1234567 (line 12)") ==
          fault_dedup_key("T", "Timer too close at 7654321 (line 98)"));
    // A different cause or title is another fault, and so is another quoted name.
    CHECK(fault_dedup_key("T", "Must home axis first") != fault_dedup_key("T", "Heater failed"));
    CHECK(fault_dedup_key("A", "x") != fault_dedup_key("B", "x"));
    CHECK(fault_dedup_key("T", "Lost communication with MCU 'mcu1'") !=
          fault_dedup_key("T", "Lost communication with MCU 'mcu2'"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "a burst of refused moves shows one dialog even over another",
                 "[error-center][dup_dialogs][ui_integration]") {
    get_printer_state().update_from_status(
        nlohmann::json{{"print_stats", {{"state", "printing"}}}});
    helix::ui::UpdateQueue::instance().drain();
    ToastGuard toasts;

    helix::ui::RecoveryModalPresenter presenter(api());
    helix::GcodeErrorRouter router(api(), client(), presenter);
    const auto refuse = [&](const char* coords) {
        GcodeErrorRouterTestAccess::process_line(router,
                                                 std::string("!! Must home axis first: ") + coords);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(200);
    };

    refuse("94.700 86.700 59.000");
    lv_obj_t* screen = lv_screen_active();
    REQUIRE(count_labels(screen, "home axes") == 1);
    // Another dialog lands on top, so the next refusal finds the wrong one on top.
    ui_notification_printer_fault("Other fault", "Something else broke first");
    process_lvgl(200);
    REQUIRE(count_labels(screen, "Something else broke first") == 1);

    for (const char* coords : {"60.000 105.000 61.000", "60.000 105.000 61.200"}) {
        refuse(coords);
    }
    CHECK(count_labels(screen, "home axes") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "two faults under one generic title both show",
                 "[error-center][dup_dialogs][ui_integration]") {
    ToastGuard toasts;
    ui_notification_printer_fault("Printer Error", "First cause alpha");
    process_lvgl(200);
    ui_notification_printer_fault("Printer Error", "Second cause beta");
    process_lvgl(200);
    lv_obj_t* screen = lv_screen_active();
    CHECK(count_labels(screen, "First cause alpha") == 1);
    CHECK(count_labels(screen, "Second cause beta") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a fault whose text keeps changing cannot stack without bound",
                 "[error-center][dup_dialogs][ui_integration]") {
    ToastGuard toasts;
    const char* causes[] = {"cause alpha", "cause beta", "cause gamma", "cause delta", "cause eps"};
    for (const char* cause : causes) {
        ui_notification_printer_fault("Printer Error", cause);
        process_lvgl(600);
    }
    lv_obj_t* screen = lv_screen_active();
    int shown = 0;
    for (const char* cause : causes) {
        shown += count_labels(screen, cause);
    }
    CHECK(shown == helix::ui::kMaxFaultModalsPerTitle);
    // The newest stay; the oldest were retired.
    CHECK(count_labels(screen, "cause eps") == 1);
    CHECK(count_labels(screen, "cause alpha") == 0);
}
