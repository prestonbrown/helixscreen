// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_low_ram_warning_handle.cpp
 * @brief show_low_ram_resonance_warning clears the caller's handle on every close.
 *
 * Run with: ./build/bin/helix-tests "[modal][low_ram]"
 *
 * The stored handle is the caller's re-entry guard: both call sites open the
 * dialog only `if (!handle)`. A close path that leaves the handle set makes
 * resonance calibration a silent no-op for the rest of the panel's life, so
 * every path the user can take has to clear it.
 */

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"

#include <functional>

#include "../catch_amalgamated.hpp"

namespace {

class LowRamWarningFixture : public LVGLUITestFixture {
  public:
    LowRamWarningFixture() {
        helix::ui::modal_init_subjects();
    }

    ~LowRamWarningFixture() override {
        if (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
        }
        settle();
    }

    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    static void press(lv_obj_t* dialog, const char* name) {
        lv_obj_t* button = lv_obj_find_by_name(dialog, name);
        REQUIRE(button != nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* handle = nullptr;
};

} // namespace

TEST_CASE_METHOD(LowRamWarningFixture, "Low-RAM warning clears its handle when confirmed",
                 "[modal][low_ram]") {
    helix::ui::ConfirmOptions opts;
    helix::ui::show_low_ram_resonance_warning(256, &handle, []() {}, opts);
    REQUIRE(handle != nullptr);

    press(handle, "btn_primary");
    settle();

    CHECK(handle == nullptr);
}

TEST_CASE_METHOD(LowRamWarningFixture,
                 "Low-RAM warning clears its handle when cancelled by a caller with on_cancel",
                 "[modal][low_ram]") {
    // A caller that supplies on_cancel latches the dialog's answered_ flag,
    // which suppresses the dismissal report. The handle still has to be cleared,
    // or the caller's `if (!handle)` guard blocks every later attempt.
    bool cancelled = false;
    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [&cancelled]() { cancelled = true; };
    helix::ui::show_low_ram_resonance_warning(256, &handle, []() {}, opts);
    REQUIRE(handle != nullptr);

    press(handle, "btn_secondary");
    settle();

    CHECK(cancelled);
    CHECK(handle == nullptr);
}

TEST_CASE_METHOD(LowRamWarningFixture, "Low-RAM warning clears its handle on a backdrop dismissal",
                 "[modal][low_ram]") {
    helix::ui::ConfirmOptions opts;
    helix::ui::show_low_ram_resonance_warning(256, &handle, []() {}, opts);
    REQUIRE(handle != nullptr);

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(handle);
    REQUIRE(backdrop != nullptr);
    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
    settle();

    CHECK(handle == nullptr);
}

TEST_CASE_METHOD(LowRamWarningFixture, "Low-RAM warning still reports a dismissal to its caller",
                 "[modal][low_ram]") {
    // The helper clears the handle itself, but a caller holding its own state
    // has to be able to learn about the dismissal too.
    bool dismissed = false;
    helix::ui::ConfirmOptions opts;
    opts.on_dismiss = [&dismissed]() { dismissed = true; };
    helix::ui::show_low_ram_resonance_warning(256, &handle, []() {}, opts);
    REQUIRE(handle != nullptr);

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(handle);
    REQUIRE(backdrop != nullptr);
    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
    settle();

    CHECK(handle == nullptr);
    CHECK(dismissed);
}
