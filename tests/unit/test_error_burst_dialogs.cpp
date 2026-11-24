// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A burst of one failure must not leave a stack of dialogs to dismiss.

#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_toast_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
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
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

} // namespace

TEST_CASE("a fault repeated with new coordinates is the same fault", "[dup_dialogs]") {
    using helix::ui::fault_dedup_key;
    CHECK(fault_dedup_key("T", "Must home axis first: 94.700 86.700 59.000") ==
          fault_dedup_key("T", "Must home axis first: 60.000 105.000 61.000"));
    // A different cause, a different title, or a different integer is another fault.
    CHECK(fault_dedup_key("T", "Must home axis first") != fault_dedup_key("T", "Heater failed"));
    CHECK(fault_dedup_key("A", "x") != fault_dedup_key("B", "x"));
    CHECK(fault_dedup_key("T", "MCU 1 lost") != fault_dedup_key("T", "MCU 2 lost"));
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
    REQUIRE(count_labels(screen, "homed") == 1);
    // Another dialog lands on top, so the next refusal finds the wrong one on top.
    ui_notification_printer_fault("Other fault", "Something else broke first");
    process_lvgl(200);
    REQUIRE(count_labels(screen, "Something else broke first") == 1);

    for (const char* coords : {"60.000 105.000 61.000", "60.000 105.000 61.200"}) {
        refuse(coords);
    }
    CHECK(count_labels(screen, "homed") == 1);
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
