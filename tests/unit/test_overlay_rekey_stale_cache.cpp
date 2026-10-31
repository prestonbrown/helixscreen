// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_rekey_stale_cache.cpp
 * @brief An overlay reopens after a hot-reload rebuild, and work queued across the
 *        rebuild reaches the rebuilt root (prestonbrown/helixscreen#1729, #1983).
 *
 * rebuild_active_views() rebuilds closed overlays too and frees every old root.
 * Navigation takes nullptr or a live widget, so an owner holds a handle.
 */

#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_widget_ref.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "overlay_base.h"

#include "../catch_amalgamated.hpp"

using helix::PanelId;

namespace {

class CachedOverlay : public OverlayBase {
  public:
    void init_subjects() override {
        subjects_initialized_ = true;
    }

    lv_obj_t* create(lv_obj_t* parent) override {
        parent_screen_ = parent;
        overlay_root_ = lv_obj_create(parent);
        lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
        return overlay_root_;
    }

    const char* get_name() const override {
        return "CachedOverlay";
    }
};

class StaleCacheFixture : public LVGLUITestFixture {
  protected:
    lv_obj_t* base_ = nullptr;

    StaleCacheFixture() {
        base_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = base_;
        NavigationManager::instance().set_panels(panels);
    }

    ~StaleCacheFixture() override {
        lv_obj_delete(base_);
    }

    static void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
            lv_timer_handler();
        }
    }

    /// Close the overlay, then rebuild everything as a hot reload does. The handle
    /// to the old root reads null; the owner's reads the rebuilt one.
    void close_and_rebuild(OverlayBase& overlay) {
        auto& nav = NavigationManager::instance();
        helix::ui::WeakWidget old_root(overlay.get_root());
        REQUIRE(nav.is_panel_on_top(old_root));
        nav.go_back();
        settle();
        REQUIRE_FALSE(nav.is_panel_in_stack(old_root));

        nav.rebuild_active_views();
        settle();
        REQUIRE(old_root.get() == nullptr);
        REQUIRE(overlay.get_root() != nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(StaleCacheFixture, "An owner's handle reopens the rebuilt root",
                 "[1729][navigation][overlay][hot_reload]") {
    auto& nav = NavigationManager::instance();
    CachedOverlay overlay;
    overlay.init_subjects();
    overlay.create(test_screen());
    nav.register_overlay_instance(overlay.get_root(), &overlay);
    nav.push_overlay(overlay.get_root());
    settle();

    close_and_rebuild(overlay);

    nav.register_overlay_instance(overlay.get_root(), &overlay);
    nav.push_overlay(overlay.get_root());
    settle();

    CHECK(nav.is_panel_on_top(overlay.get_root()));
    CHECK_FALSE(lv_obj_has_flag(overlay.get_root(), LV_OBJ_FLAG_HIDDEN));

    nav.go_back();
    settle();
}

TEST_CASE_METHOD(StaleCacheFixture, "OverlayBase::show() reopens the rebuilt root",
                 "[1729][navigation][overlay][hot_reload]") {
    auto& nav = NavigationManager::instance();
    CachedOverlay overlay;
    REQUIRE(overlay.show(test_screen()));
    settle();

    close_and_rebuild(overlay);

    REQUIRE(overlay.show(test_screen()));
    settle();

    CHECK(nav.is_panel_on_top(overlay.get_root()));

    nav.go_back();
    settle();
}

// A rebuild lands between each queued operation and its drain.
TEST_CASE_METHOD(StaleCacheFixture, "Operations queued before a rebuild reach the rebuilt root",
                 "[1729][navigation][overlay][hot_reload]") {
    auto& nav = NavigationManager::instance();
    CachedOverlay overlay;
    overlay.init_subjects();
    lv_obj_t* old_root = overlay.create(test_screen());
    nav.register_overlay_instance(old_root, &overlay);

    SECTION("a push opens it") {
        nav.push_overlay(old_root);
        nav.rebuild_active_views();
        REQUIRE(overlay.get_root() != old_root);
        settle();

        CHECK(nav.is_panel_on_top(overlay.get_root()));
        nav.go_back();
        settle();
    }

    SECTION("a close pops it") {
        nav.push_overlay(old_root);
        settle();
        nav.close_overlay(old_root);
        nav.rebuild_active_views();
        REQUIRE(overlay.get_root() != old_root);
        settle();

        CHECK_FALSE(nav.is_panel_in_stack(overlay.get_root()));
    }

    SECTION("a cancelled push retires it, running its close callback") {
        int closes = 0;
        nav.register_overlay_close_callback(old_root, [&closes] { ++closes; });
        nav.push_overlay(old_root);
        nav.close_overlay(old_root);
        nav.rebuild_active_views();
        REQUIRE(overlay.get_root() != old_root);
        settle();

        CHECK(closes == 1);
        CHECK_FALSE(nav.is_panel_in_stack(overlay.get_root()));
    }
}

TEST_CASE_METHOD(StaleCacheFixture,
                 "A dead root's handle opens nothing, least of all a rebuilt overlay",
                 "[1729][navigation][overlay][hot_reload]") {
    auto& nav = NavigationManager::instance();
    CachedOverlay overlay;
    overlay.init_subjects();
    overlay.create(test_screen());
    nav.register_overlay_instance(overlay.get_root(), &overlay);
    nav.push_overlay(overlay.get_root());
    settle();
    close_and_rebuild(overlay);

    helix::ui::WidgetRef other(lv_obj_create(test_screen()));
    lv_obj_add_flag(other, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(other, nullptr);

    SECTION("deleted with a push queued") {
        nav.push_overlay(other);
        lv_obj_delete(other);
    }
    SECTION("deleted, then pushed through the handle") {
        lv_obj_delete(other);
        nav.push_overlay(other);
    }
    settle();

    CHECK(other.get() == nullptr);
    CHECK_FALSE(nav.is_panel_in_stack(overlay.get_root()));
}
