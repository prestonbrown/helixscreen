// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_button_defer_reuse.cpp
 * @brief Identity-guard regression for deferred contrast under address reuse (#924)
 *
 * button_style_changed_cb defers update_button_text_contrast through the
 * UpdateQueue. lv_obj_is_valid() + the magic field only prove the captured
 * address is *a* live ui_button; under heap address reuse a freed button's
 * address can be reallocated to a DIFFERENT live ui_button before the deferred
 * tick fires, passing both checks and recomputing contrast against the foreign
 * button (reading freed-then-realloc'd memory in production -> SIGSEGV).
 *
 * defer_button_contrast_update() captures the button's unique id at defer time
 * and re-verifies identity on the main thread before touching style/state.
 *
 * This file includes ui_button.cpp directly (the ui_switch test pattern) so the
 * anonymous-namespace internals — defer_button_contrast_update(), UiButtonData,
 * next_button_id() — are reachable. ui_button.o is filtered out of the test
 * link in mk/tests.mk to avoid duplicate symbols.
 *
 * The "rejects a foreign reused-address button" case FAILS if the `d->id != gen`
 * check is removed from defer_button_contrast_update().
 */

#include "../test_fixtures.h"
#include "../test_helpers/update_queue_test_access.h"

#include "../catch_amalgamated.hpp"

// Direct include AFTER the headers above so the test sees the real internals.
#include "../../src/ui/ui_button.cpp"

namespace {

// A sentinel opacity that update_button_text_contrast() never produces
// (it forces LV_OPA_COVER=255 when enabled, LV_OPA_50=128 when disabled).
constexpr lv_opa_t SENTINEL_OPA = 173;

// Build a minimal ui_button-shaped object: a real lv_button carrying a freshly
// allocated UiButtonData with the production magic and a unique id. Mirrors the
// data installed by ui_button_create() without the full XML/theme path.
lv_obj_t* make_fake_button(lv_obj_t* parent, UiButtonData** out_data) {
    lv_obj_t* btn = lv_button_create(parent);
    auto* data = new UiButtonData{.magic = UiButtonData::MAGIC,
                                  .id = next_button_id(),
                                  .icon = nullptr,
                                  .label = nullptr,
                                  .icon_on_right = false};
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

TEST_CASE_METHOD(XMLTestFixture, "ui_button next_button_id yields distinct tokens",
                 "[ui_button][crash][quick]") {
    uint64_t a = next_button_id();
    uint64_t b = next_button_id();
    uint64_t c = next_button_id();
    REQUIRE(a != b);
    REQUIRE(b != c);
    REQUIRE(a != c);
}

TEST_CASE_METHOD(XMLTestFixture, "ui_button deferred contrast runs on a matching-identity button",
                 "[ui_button][crash][quick]") {
    // Baseline: when the captured identity still matches the live button, the
    // deferred contrast recompute MUST run. Proves the guard isn't over-broad.
    UiButtonData* data = nullptr;
    lv_obj_t* btn = make_fake_button(test_screen(), &data);
    REQUIRE(btn != nullptr);

    // Plant the sentinel; a real contrast recompute overwrites it.
    lv_obj_set_style_opa(btn, SENTINEL_OPA, LV_PART_MAIN);

    defer_button_contrast_update(btn);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    lv_opa_t after = lv_obj_get_style_opa(btn, LV_PART_MAIN);
    INFO("sentinel=" << int(SENTINEL_OPA) << " after=" << int(after));
    REQUIRE(after != SENTINEL_OPA); // contrast ran -> opacity forced to COVER/50

    lv_obj_delete(btn);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ui_button deferred contrast rejects a foreign reused-address button",
                 "[ui_button][crash]") {
    // Simulate the #924 race deterministically: a deferred contrast update is
    // queued capturing a button's address + id, then that button's slot is
    // "reused" by a different live ui_button (different id) at the SAME address
    // before the deferred tick fires.
    //
    // We force the address-reuse condition directly by swapping the UiButtonData
    // behind the captured address for one with a different id — exactly what a
    // freed-then-reallocated foreign button looks like to the deferred lambda.
    UiButtonData* original_data = nullptr;
    lv_obj_t* btn = make_fake_button(test_screen(), &original_data);
    REQUIRE(btn != nullptr);
    const uint64_t original_id = original_data->id;

    // Queue the deferred contrast update — captures (btn, original_id).
    defer_button_contrast_update(btn);

    // The slot is now "reused" by a foreign button: same address, new identity.
    // Free the original data and install a fresh UiButtonData with a new id,
    // matching magic, mimicking a distinct live ui_button at this address.
    delete original_data;
    auto* foreign_data = new UiButtonData{.magic = UiButtonData::MAGIC,
                                          .id = next_button_id(),
                                          .icon = nullptr,
                                          .label = nullptr,
                                          .icon_on_right = false};
    REQUIRE(foreign_data->id != original_id);
    lv_obj_set_user_data(btn, foreign_data);

    // Plant the sentinel on the foreign button. If the stale defer runs, the
    // contrast recompute overwrites it; the identity guard must skip it.
    lv_obj_set_style_opa(btn, SENTINEL_OPA, LV_PART_MAIN);

    REQUIRE_NOTHROW(
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance()));

    lv_opa_t after = lv_obj_get_style_opa(btn, LV_PART_MAIN);
    INFO("sentinel=" << int(SENTINEL_OPA) << " after=" << int(after)
                     << " original_id=" << original_id << " foreign_id=" << foreign_data->id);
    // With the `d->id != gen` guard the stale defer is rejected and the
    // sentinel survives. Remove that check and update_button_text_contrast()
    // runs against the foreign button, forcing opacity to COVER/50 -> FAIL.
    REQUIRE(after == SENTINEL_OPA);

    lv_obj_delete(btn); // button_delete_cb frees foreign_data
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ui_button deferred contrast rejects a reused address holding a foreign widget",
                 "[ui_button][crash]") {
    // The #1111 crash: the button is freed and its address is reused by a
    // NON-ui_button widget whose user_data is a small non-pointer sentinel
    // (observed value: 0x2 — many LVGL widgets stash small ints/enums there).
    // lv_obj_is_valid(btn) is TRUE (a live foreign object occupies the address),
    // !d is false (0x2 is non-null), so the old guard dereferenced d->magic at
    // address 0x2 -> SIGSEGV. The live-ui_button registry must reject btn before
    // any user_data dereference.
    UiButtonData* original_data = nullptr;
    lv_obj_t* btn = make_fake_button(test_screen(), &original_data);
    REQUIRE(btn != nullptr);

    // Queue the deferred contrast update — captures (btn, original id).
    defer_button_contrast_update(btn);

    // Simulate delete-then-reuse: the original ui_button is gone (untracked,
    // its data freed) but its address now hosts a foreign widget carrying a
    // small non-pointer user_data. btn stays a live lv_obj so lv_obj_is_valid
    // still passes, reproducing the exact production condition.
    delete original_data;
    live_ui_buttons().erase(btn); // what button_delete_cb does on real deletion
    lv_obj_set_user_data(btn, reinterpret_cast<void*>(0x2));

    // Plant the sentinel; if the stale defer runs it overwrites it.
    lv_obj_set_style_opa(btn, SENTINEL_OPA, LV_PART_MAIN);

    REQUIRE_NOTHROW(
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance()));

    lv_opa_t after = lv_obj_get_style_opa(btn, LV_PART_MAIN);
    INFO("sentinel=" << int(SENTINEL_OPA) << " after=" << int(after));
    // The registry check rejects btn before touching user_data: sentinel survives.
    REQUIRE(after == SENTINEL_OPA);

    // Restore a null user_data so button_delete_cb doesn't dereference 0x2.
    lv_obj_set_user_data(btn, nullptr);
    lv_obj_delete(btn);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}
