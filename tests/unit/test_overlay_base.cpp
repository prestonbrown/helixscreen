// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_base.cpp
 * @brief Regression tests for OverlayBase deletion semantics
 *
 * destroy_overlay_ui is invoked as an overlay close callback on
 * memory-constrained devices. safe_delete (sync) inside a close callback
 * corrupts LVGL's global event list when chained from a UpdateQueue batch —
 * see #776 / #190 / #80 / #840 and the "No sync widget deletion in queued
 * callbacks" section of CLAUDE.md.
 *
 * These tests pin the deferral contract so future edits can't silently
 * regress back to sync safe_delete.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "misc/lv_timer_private.h"
#include "overlay_base.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Process pending lv_async_call / lv_obj_delete_async one-shot timers.
/// TEST_MIRROR_OK: lv_timer_handler() is LVGL's own API, not our code, and this
/// helper mirrors another TEST helper (test_cleanup_helpers.cpp) rather than any
/// production symbol. The gate matched the LVGL name in the vendored headers.
/// Mirrors the helper in test_cleanup_helpers.cpp — lv_timer_handler() loops
/// indefinitely on display refresh timers in the fixture.
void process_async_timers() {
    for (int safety = 0; safety < 100; ++safety) {
        bool fired = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && t->timer_cb) {
                t->timer_cb(t);
                fired = true;
                break;
            }
            t = next;
        }
        if (!fired)
            break;
    }
}

/// Minimal concrete OverlayBase for testing the base class contract.
/// Exposes overlay_root_ write access and a test-only builder that bypasses
/// the usual XML factory path.
class TestOverlay : public OverlayBase {
  public:
    void init_subjects() override {
        subjects_initialized_ = true;
    }

    lv_obj_t* create(lv_obj_t* parent) override {
        overlay_root_ = lv_obj_create(parent);
        return overlay_root_;
    }

    const char* get_name() const override {
        return "TestOverlay";
    }

    lv_obj_t* raw_root() const {
        return overlay_root_;
    }

    bool on_ui_destroyed_called = false;

  protected:
    void on_ui_destroyed() override {
        on_ui_destroyed_called = true;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "destroy_overlay_ui defers widget deletion (regression #840)",
                 "[overlay_base][L081]") {
    auto overlay = std::make_unique<TestOverlay>();
    lv_obj_t* root = overlay->create(test_screen());
    REQUIRE(root != nullptr);
    REQUIRE(lv_obj_is_valid(root));

    lv_obj_t* cached = root;
    overlay->destroy_overlay_ui(cached);

    // Contract: cached pointer + overlay_root_ nulled immediately (both live
    // through the caller's reference — the test overlay's raw_root() should
    // also be nulled via safe_delete_deferred's pointer clearing).
    REQUIRE(cached == nullptr);
    REQUIRE(overlay->raw_root() == nullptr);

    // Contract: on_ui_destroyed() runs synchronously so the derived class can
    // null its child-widget pointers before they become invalid.
    REQUIRE(overlay->on_ui_destroyed_called);

    // Contract: underlying widget is NOT sync-deleted — still live (hidden,
    // reparented to top layer) until the async tick fires. This is what
    // prevents the L081 event-list-corruption crash: no sync widget delete
    // inside the queue-callback call chain.
    REQUIRE(lv_obj_is_valid(root));
    REQUIRE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

    // After the async tick, the widget is actually gone.
    process_async_timers();
    REQUIRE_FALSE(lv_obj_is_valid(root));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "destroy_overlay_ui inside UpdateQueue callback is safe (regression #840)",
                 "[overlay_base][L081]") {
    auto overlay = std::make_unique<TestOverlay>();
    lv_obj_t* root = overlay->create(test_screen());
    REQUIRE(root != nullptr);

    // Simulate the #840 trigger path: destroy_overlay_ui runs as a close
    // callback that was itself scheduled from an observer (queue-backed).
    // The sync safe_delete regression would corrupt the event list here;
    // deferred delete escapes the batch via LVGL's own async list.
    lv_obj_t* cached = root;
    helix::ui::UpdateQueue::instance().queue(
        "test_destroy_overlay_in_batch",
        [&overlay, &cached]() { overlay->destroy_overlay_ui(cached); });

    // Drain — this is the batch that used to corrupt the event list.
    REQUIRE_NOTHROW(helix::ui::UpdateQueue::instance().drain());

    // After drain: pointer nulled, widget hidden but still valid (async pending).
    REQUIRE(cached == nullptr);
    REQUIRE(lv_obj_is_valid(root));
    REQUIRE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

    // Async tick completes the deletion.
    REQUIRE_NOTHROW(process_async_timers());
    REQUIRE_FALSE(lv_obj_is_valid(root));
}
