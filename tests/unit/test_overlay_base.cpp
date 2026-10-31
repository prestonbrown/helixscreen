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

#include "ui_nav.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/process_async_timers.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../test_helpers/widget_in_tree.h"
#include "overlay_base.h"
#include "ui/ui_widget_helpers.h"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

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
    REQUIRE(helix::test::widget_in_tree(root));

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
    REQUIRE(helix::test::widget_in_tree(root));
    REQUIRE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

    // After the async tick, the widget is actually gone.
    process_async_timers();
    REQUIRE_FALSE(helix::test::widget_in_tree(root));
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
    REQUIRE(helix::test::widget_in_tree(root));
    REQUIRE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

    // Async tick completes the deletion.
    REQUIRE_NOTHROW(process_async_timers());
    REQUIRE_FALSE(helix::test::widget_in_tree(root));
}

// destroy_overlay_ui() names the overlay as the teardown's owner, so an overlay
// freed by the work its own teardown drains stops the teardown there: a
// contract breach, fatal under strict UI checks, logged and survived without.
TEST_CASE_METHOD(LVGLTestFixture, "destroy_overlay_ui stops when its drain frees the overlay",
                 "[overlay_base][overlay_teardown][drain_reentrancy]") {
    const bool strict = helix::ui::strict_ui_checks();
    helix::ui::set_strict_ui_checks(false);

    auto overlay = std::make_unique<TestOverlay>();
    lv_obj_t* const root = overlay->create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* cached = root;
    TestOverlay* const raw = overlay.get();
    helix::ui::UpdateQueue::instance().queue("test::free_overlay_in_teardown",
                                             [&overlay]() { overlay.reset(); });

    raw->destroy_overlay_ui(cached);

    // Stopped before the null-out, the free and the after hook.
    CHECK(cached == root);
    process_async_timers();
    CHECK(helix::test::widget_in_tree(root));

    helix::ui::set_strict_ui_checks(strict);
    lv_obj_delete(root);
}

// ============================================================================
// show(): the lazy create + register + push contract
// ============================================================================

namespace {

/// Counts every hook show() drives. xml_component() null means create() is
/// overridden with a plain lv_obj; non-null exercises the default create().
class ShowOverlay : public OverlayBase {
  public:
    explicit ShowOverlay(const char* component = nullptr, bool destroy = false)
        : component_(component), destroy_(destroy) {}

    void init_subjects() override {
        ++init_calls;
    }
    void register_callbacks() override {
        ++register_calls;
    }
    lv_obj_t* create(lv_obj_t* parent) override {
        ++create_calls;
        if (component_) {
            return OverlayBase::create(parent);
        }
        overlay_root_ = lv_obj_create(parent);
        lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
        return overlay_root_;
    }
    const char* xml_component() const override {
        return component_;
    }
    bool destroy_on_close() const override {
        return destroy_;
    }
    const char* get_name() const override {
        return "ShowOverlay";
    }

    int init_calls = 0;
    int register_calls = 0;
    int create_calls = 0;
    int before_show_calls = 0;
    int ui_destroyed_calls = 0;

  protected:
    void before_show() override {
        ++before_show_calls;
    }
    void on_ui_destroyed() override {
        ++ui_destroyed_calls;
    }

  private:
    const char* component_;
    bool destroy_;
};

class ShowFixture : public LVGLUITestFixture {
  protected:
    lv_obj_t* base_ = nullptr;

    ShowFixture() {
        base_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(helix::PanelId::Home)] = base_;
        NavigationManager::instance().set_panels(panels);
    }

    ~ShowFixture() override {
        lv_obj_delete(base_);
    }

    static void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
            lv_timer_handler();
        }
    }

    static void pop(OverlayBase& overlay) {
        auto& nav = NavigationManager::instance();
        REQUIRE(nav.is_panel_on_top(overlay.get_root()));
        nav.go_back();
        settle();
    }
};

} // namespace

TEST_CASE_METHOD(ShowFixture, "show() inits subjects once and creates once across shows",
                 "[overlay_base][overlay_show]") {
    ShowOverlay overlay;
    lv_obj_t* first = nullptr;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(overlay.show(test_screen()));
        settle();
        if (i == 0) {
            first = overlay.get_root();
        }
        CHECK(overlay.get_root() == first);
        CHECK(NavigationManager::instance().is_panel_on_top(first));
        pop(overlay);
    }
    CHECK(overlay.init_calls == 1);
    CHECK(overlay.are_subjects_initialized());
    CHECK(overlay.create_calls == 1);
    CHECK(overlay.register_calls == 1);
    CHECK(overlay.before_show_calls == 3);
    CHECK(helix::test::widget_in_tree(first));
}

TEST_CASE_METHOD(ShowFixture, "show() with destroy_on_close frees the tree on pop and re-creates",
                 "[overlay_base][overlay_show]") {
    ShowOverlay overlay(nullptr, /*destroy=*/true);
    REQUIRE(overlay.show(test_screen()));
    settle();
    lv_obj_t* first = overlay.get_root();
    REQUIRE(first != nullptr);

    pop(overlay);
    process_async_timers();
    CHECK(overlay.get_root() == nullptr);
    CHECK(overlay.ui_destroyed_calls == 1);
    CHECK_FALSE(NavigationManager::instance().has_overlay_close_callback(first));

    REQUIRE(overlay.show(test_screen()));
    settle();
    CHECK(overlay.get_root() != nullptr);
    CHECK(overlay.create_calls == 2);
    CHECK(overlay.register_calls == 2); // callbacks re-registered with every create
    CHECK(overlay.init_calls == 1);
    CHECK(NavigationManager::instance().is_panel_on_top(overlay.get_root()));
    pop(overlay);
    process_async_timers();
}

TEST_CASE_METHOD(ShowFixture,
                 "show()'s close callback is a no-op once the object's lifetime has ended",
                 "[overlay_base][overlay_show]") {
    ShowOverlay overlay(nullptr, /*destroy=*/true);
    REQUIRE(overlay.show(test_screen()));
    settle();
    lv_obj_t* root = overlay.get_root();

    // What a printer switch does before destroying the object; the close
    // callback it registered is still pending on the root.
    overlay.cleanup();
    NavigationManager::instance().go_back();
    settle();
    process_async_timers();
    CHECK(overlay.ui_destroyed_calls == 0);
    CHECK(overlay.get_root() == root);
}

TEST_CASE_METHOD(ShowFixture, "show() re-creates a root deleted out from under it",
                 "[overlay_base][overlay_show]") {
    ShowOverlay overlay;
    REQUIRE(overlay.show(test_screen()));
    settle();
    lv_obj_t* first = overlay.get_root();
    pop(overlay);
    lv_obj_delete(first);

    REQUIRE(overlay.show(test_screen()));
    settle();
    CHECK(overlay.create_calls == 2);
    CHECK(overlay.ui_destroyed_calls == 1);
    CHECK(helix::test::widget_in_tree(overlay.get_root()));
    pop(overlay);
}

TEST_CASE_METHOD(ShowFixture, "the default create() builds xml_component() and survives rebuild()",
                 "[overlay_base][overlay_show]") {
    lv_xml_register_component_from_data(
        "test_overlay_show_component",
        "<component><view extends=\"lv_obj\"><lv_obj name=\"probe\"/></view></component>");
    ShowOverlay overlay("test_overlay_show_component");
    REQUIRE(overlay.show(test_screen()));
    settle();
    lv_obj_t* first = overlay.get_root();
    REQUIRE(first != nullptr);
    CHECK(helix::ui::find_required(first, "probe", "test") != nullptr);

    REQUIRE(overlay.rebuild());
    settle();
    CHECK(overlay.get_root() != first);
    CHECK(helix::ui::find_required(overlay.get_root(), "probe", "test") != nullptr);
    CHECK(NavigationManager::instance().is_panel_on_top(overlay.get_root()));
    pop(overlay);
}

TEST_CASE_METHOD(ShowFixture, "show() with no xml_component() and no create() override fails",
                 "[overlay_base][overlay_show]") {
    struct Bare : OverlayBase {
        const char* get_name() const override {
            return "Bare";
        }
    } overlay;
    CHECK_FALSE(overlay.show(test_screen()));
    CHECK(overlay.get_root() == nullptr);
}

TEST_CASE_METHOD(ShowFixture, "close() closes a shown overlay and is a no-op before any show",
                 "[overlay_base][overlay_show]") {
    ShowOverlay overlay;
    overlay.close(); // no root yet
    REQUIRE(overlay.show(test_screen()));
    settle();
    REQUIRE(NavigationManager::instance().is_panel_in_stack(overlay.get_root()));
    overlay.close();
    settle();
    CHECK_FALSE(NavigationManager::instance().is_panel_in_stack(overlay.get_root()));
}

TEST_CASE_METHOD(ShowFixture,
                 "show() aborts on a root whose close callback another owner holds (strict)",
                 "[overlay_base][overlay_show][subprocess]") {
    ShowOverlay overlay;
    REQUIRE(overlay.show(test_screen()));
    settle();
    pop(overlay);
    // An owner-held overlay: the owner's callback deletes the object.
    NavigationManager::instance().register_overlay_close_callback(overlay.get_root(), [] {});

    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        std::signal(SIGABRT, SIG_DFL); // Catch2's handler would report instead of dying
        helix::ui::set_strict_ui_checks(true);
        overlay.show(test_screen());
        _exit(0);
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);

    NavigationManager::instance().unregister_overlay_close_callback(overlay.get_root());
}

namespace {

/// Sums the pixels every invalidation adds to the display while installed.
struct InvalidationMeter {
    int64_t pixels = 0;
    lv_display_t* disp = lv_display_get_default();

    InvalidationMeter() {
        lv_display_add_event_cb(disp, on_invalidate, LV_EVENT_INVALIDATE_AREA, this);
    }
    ~InvalidationMeter() {
        lv_display_remove_event_cb_with_user_data(disp, on_invalidate, this);
    }

    static void on_invalidate(lv_event_t* e) {
        auto* self = static_cast<InvalidationMeter*>(lv_event_get_user_data(e));
        auto* a = static_cast<const lv_area_t*>(lv_event_get_param(e));
        self->pixels += static_cast<int64_t>(lv_area_get_width(a)) * lv_area_get_height(a);
    }
};

// A full-size root whose construction runs a layout pass: a dropdown with a
// selection positions its list, which reads a scroll offset and so updates
// the screen's layout before lv_xml_create() returns.
constexpr const char* kLayoutDuringCreate =
    "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\">"
    "<lv_dropdown options=\"a&#10;b&#10;c\" selected=\"2\"/>"
    "</view></component>";

} // namespace

TEST_CASE_METHOD(ShowFixture, "show() leaves the screen undrawn until the queued push shows it",
                 "[overlay_base][overlay_show][render]") {
    lv_xml_register_component_from_data("test_overlay_layout_during_create", kLayoutDuringCreate);
    settle();
    lv_refr_now(lv_display_get_default());

    // The component really does lay out mid-create: built visible, its whole
    // area is already queued for redraw by the time the caller could hide it.
    {
        InvalidationMeter meter;
        lv_obj_t* visible = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "test_overlay_layout_during_create", nullptr));
        REQUIRE(visible != nullptr);
        CHECK(meter.pixels >= lv_obj_get_width(visible) * lv_obj_get_height(visible));
        lv_obj_delete(visible);
    }
    lv_refr_now(lv_display_get_default());

    ShowOverlay overlay("test_overlay_layout_during_create");
    {
        // The frame between show() and the queued push would draw only this.
        InvalidationMeter meter;
        REQUIRE(overlay.show(test_screen()));
        lv_obj_update_layout(test_screen());
        CHECK(meter.pixels == 0);
        CHECK(lv_obj_has_flag(overlay.get_root(), LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_parent(overlay.get_root()) == test_screen());
    }
    settle();
    CHECK_FALSE(lv_obj_has_flag(overlay.get_root(), LV_OBJ_FLAG_HIDDEN));
    CHECK(NavigationManager::instance().is_panel_on_top(overlay.get_root()));
    pop(overlay);
}

TEST_CASE_METHOD(ShowFixture, "create_xml_hidden() lays the tree out at the parent's size",
                 "[overlay_base][overlay_show][render]") {
    lv_xml_register_component_from_data("test_overlay_layout_during_create", kLayoutDuringCreate);
    settle();

    // No layout pass after the call: what a caller reading sizes synchronously
    // sees is what the build-time layout computed.
    lv_obj_t* root =
        helix::ui::create_xml_hidden(test_screen(), "test_overlay_layout_during_create");
    REQUIRE(root != nullptr);
    CHECK(lv_obj_get_width(root) == lv_obj_get_content_width(test_screen()));
    CHECK(lv_obj_get_height(root) == lv_obj_get_content_height(test_screen()));
    lv_obj_delete(root);
}

TEST_CASE_METHOD(ShowFixture, "create_xml_hidden() refuses a null parent",
                 "[overlay_base][overlay_show]") {
    lv_xml_register_component_from_data("test_overlay_layout_during_create", kLayoutDuringCreate);
    CHECK(helix::ui::create_xml_hidden(nullptr, "test_overlay_layout_during_create") == nullptr);
}

TEST_CASE_METHOD(ShowFixture, "is_push_pending() spans push_overlay() to its queued push",
                 "[overlay_base][overlay_show][nav]") {
    auto& nav = NavigationManager::instance();
    lv_obj_t* overlay = lv_obj_create(test_screen());
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(overlay, nullptr);

    CHECK_FALSE(nav.is_push_pending(overlay));
    nav.push_overlay(overlay);
    // Neither visible nor stacked yet: only this says an open is under way.
    CHECK(nav.is_push_pending(overlay));
    CHECK_FALSE(nav.is_panel_in_stack(overlay));

    settle();
    CHECK_FALSE(nav.is_push_pending(overlay));
    CHECK(nav.is_panel_on_top(overlay));
    nav.go_back();
    settle();
}

TEST_CASE_METHOD(ShowFixture, "is_showing() covers queued, stacked and sliding-out overlays",
                 "[overlay_base][overlay_show][nav]") {
    lv_obj_t* overlay = lv_obj_create(test_screen());
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    helix::nav::register_overlay(overlay, nullptr);
    CHECK_FALSE(helix::nav::is_showing(overlay));
    CHECK_FALSE(helix::nav::is_showing(nullptr));

    helix::nav::push_overlay(overlay);
    CHECK(helix::nav::is_showing(overlay)); // queued, still hidden
    settle();
    CHECK(helix::nav::is_showing(overlay)); // in the stack

    // Off the stack but still drawn: what a slide-out animation leaves until it ends.
    helix::nav::go_back();
    settle();
    REQUIRE_FALSE(helix::nav::is_in_stack(overlay));
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    CHECK(helix::nav::is_showing(overlay));
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    CHECK_FALSE(helix::nav::is_showing(overlay));

    // An owner's handle reads null once the widget is deleted.
    helix::nav::unregister_overlay(overlay);
    helix::ui::WidgetRef handle(overlay);
    lv_obj_delete(overlay);
    CHECK_FALSE(helix::nav::is_showing(handle));
}
