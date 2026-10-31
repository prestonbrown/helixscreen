// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_button_defer_reuse.cpp
 * @brief A deferred contrast update reaches only a button that is still alive
 *
 * button_style_changed_cb defers update_button_text_contrast until the next
 * render or queue drain, and the button can be deleted in between. Each queued
 * entry holds a WeakWidget, which reads nullptr once that button is gone, even
 * when a new button has taken its address.
 *
 * This file includes ui_button.cpp directly (the ui_switch test pattern) so the
 * anonymous-namespace internals are reachable. ui_button.o is filtered out of
 * the test link in mk/tests.mk to avoid duplicate symbols.
 */

#include "../test_fixtures.h"
#include "../test_helpers/update_queue_test_access.h"

#include "../catch_amalgamated.hpp"

// Direct include AFTER the headers above so the test sees the real internals.
#include "../../src/ui/ui_button.cpp"

namespace {

// A sentinel opacity that update_button_text_contrast() never produces
// (it forces LV_OPA_COVER=255 when enabled, LV_OPA_50=128 when disabled).
constexpr lv_opa_t kSentinelOpa = 173;

// Build a minimal ui_button-shaped object: a real lv_button carrying a freshly
// allocated UiButtonData with the production magic. Mirrors the
// data installed by ui_button_create() without the full XML/theme path.
lv_obj_t* make_fake_button(lv_obj_t* parent, UiButtonData** out_data) {
    lv_obj_t* btn = lv_button_create(parent);
    auto* data = new UiButtonData{
        .magic = UiButtonData::kMagic, .icon = nullptr, .label = nullptr, .icon_on_right = false};
    lv_obj_set_user_data(btn, data);
    // Mirror the real create path: register as a live ui_button (#1111).
    live_ui_buttons().insert(btn);
    // Match the real widget's delete cleanup so the UiButtonData is freed.
    lv_obj_add_event_cb(btn, button_delete_cb, LV_EVENT_DELETE, nullptr);
    if (out_data)
        *out_data = data;
    return btn;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ui_button deferred contrast runs on a live button",
                 "[ui_button][crash][quick]") {
    // Baseline: the deferred recompute reaches a button that is still alive.
    UiButtonData* data = nullptr;
    lv_obj_t* btn = make_fake_button(test_screen(), &data);
    REQUIRE(btn != nullptr);

    // Plant the sentinel; a real contrast recompute overwrites it.
    lv_obj_set_style_opa(btn, kSentinelOpa, LV_PART_MAIN);

    defer_button_contrast_update(btn);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    lv_opa_t after = lv_obj_get_style_opa(btn, LV_PART_MAIN);
    INFO("sentinel=" << int(kSentinelOpa) << " after=" << int(after));
    REQUIRE(after != kSentinelOpa); // contrast ran -> opacity forced to COVER/50

    lv_obj_delete(btn);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(XMLTestFixture, "ui_button deferred contrast skips a deleted button",
                 "[ui_button][crash]") {
    lv_obj_t* doomed = make_fake_button(test_screen(), nullptr);
    REQUIRE(doomed != nullptr);
    defer_button_contrast_update(doomed);
    lv_obj_delete(doomed);

    // The replacement may land at the same address; the update was not queued for it.
    lv_obj_t* replacement = make_fake_button(test_screen(), nullptr);
    lv_obj_set_style_opa(replacement, kSentinelOpa, LV_PART_MAIN);

    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(lv_obj_get_style_opa(replacement, LV_PART_MAIN) == kSentinelOpa);

    lv_obj_delete(replacement);
}
