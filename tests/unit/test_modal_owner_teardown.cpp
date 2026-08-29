// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_modal_owner_teardown.cpp
 * @brief ModalStack owner tracking — static Modal::hide() must not bypass instance teardown
 *
 * Modal has two hide overloads with different semantics: the instance
 * Modal::hide() runs the full teardown (lifetime invalidation, user_data
 * clearing, on_hide()), the static Modal::hide(dialog) only animates the
 * widgets away. ~50 call sites reach for the static one — usually as
 * Modal::hide(Modal::get_top()) — with no idea whether the dialog on top
 * belongs to a C++ Modal instance. ModalStack therefore records the owning
 * instance for every modal shown through the instance API so the static
 * overload can delegate.
 */

#include "ui_modal.h"
#include "ui_update_queue.h"

// LVGLUITestFixture registers ALL XML components, so print_cancel_confirm_modal
// (a self-contained dialog with no subject bindings) builds its real widget
// tree here. The fixture also forces animations off, which makes modal exit
// teardown synchronous apart from the deferred widget delete.
#include "../lvgl_ui_test_fixture.h"

#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Minimal real modal component: no subject bindings, no owning C++ class.
constexpr const char* TEST_COMPONENT = "print_cancel_confirm_modal";

/// Counts on_hide() invocations so tests can prove the instance teardown ran.
class TrackingModal : public Modal {
  public:
    int hide_calls = 0;

    const char* get_name() const override {
        return "TrackingModal";
    }
    const char* component_name() const override {
        return TEST_COMPONENT;
    }
    void on_hide() override {
        ++hide_calls;
    }
};

/// Live-instance count for SelfDeletingModal, which mirrors the production
/// subclasses (DebugBundleModal, PreflightCheckModal, SpaghettiDetectionModal,
/// ...) that free themselves from on_hide().
int g_self_deleting_live = 0;

class SelfDeletingModal : public Modal {
  public:
    SelfDeletingModal() {
        ++g_self_deleting_live;
    }
    ~SelfDeletingModal() override {
        --g_self_deleting_live;
    }

    const char* get_name() const override {
        return "SelfDeletingModal";
    }
    const char* component_name() const override {
        return TEST_COMPONENT;
    }
    void on_hide() override {
        auto* self = this;
        helix::ui::async_call([](void* data) { delete static_cast<SelfDeletingModal*>(data); },
                              self);
    }
};

/// Calls the static overload on its own dialog from on_hide(), standing in for
/// a hook that reaches back into modal teardown — e.g. AmsLoadingErrorModal and
/// ColorPicker both invoke a caller-supplied dismiss callback from on_hide().
/// The owner must already be cleared by then or this recurses forever.
class ReentrantModal : public Modal {
  public:
    int hide_calls = 0;

    const char* get_name() const override {
        return "ReentrantModal";
    }
    const char* component_name() const override {
        return TEST_COMPONENT;
    }
    void on_hide() override {
        ++hide_calls;
        if (dialog_) {
            Modal::hide(dialog_);
        }
    }
};

/// True when `backdrop` is the last (topmost) child of its parent.
bool is_foreground_child(lv_obj_t* backdrop) {
    lv_obj_t* parent = lv_obj_get_parent(backdrop);
    if (!parent) {
        return false;
    }
    uint32_t count = lv_obj_get_child_count(parent);
    return count > 0 && lv_obj_get_child(parent, count - 1) == backdrop;
}

/// A plain screen child standing in for whatever the modal was raised above.
///
/// Without it the z-order assertions are vacuous: with the fixture's animations
/// off, hiding a modal reaches safe_delete_deferred_raw(), which reparents the
/// exiting backdrop onto lv_layer_top(). The modal beneath then ends up last
/// child of the screen whether or not anything raised it. The sentinel is
/// created between the two modals, so only an actual lv_obj_move_foreground()
/// on the lower backdrop can put it back on top.
lv_obj_t* make_z_order_sentinel(lv_obj_t* screen) {
    return lv_obj_create(screen);
}

} // namespace

// ============================================================================
// Owner-aware static hide
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Static Modal::hide runs instance teardown for owned modal",
                 "[modal][1230]") {
    TrackingModal modal;
    REQUIRE(modal.show(test_screen()));
    REQUIRE(modal.is_visible());

    lv_obj_t* dialog = modal.dialog();
    REQUIRE(dialog != nullptr);

    // The static overload is what ~50 call sites use. It must reach the owner.
    Modal::hide(dialog);

    CHECK(modal.hide_calls == 1);
    CHECK_FALSE(modal.is_visible());

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());

    // A second static hide on the (now dead) dialog must not re-fire on_hide().
    Modal::hide(dialog);
    CHECK(modal.hide_calls == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "rebuild_top hides an instance-backed modal instead of rebuilding",
                 "[modal][1230]") {
    TrackingModal modal;
    REQUIRE(modal.show(test_screen()));

    // Re-showing through the static path would build a bare XML copy with no
    // on_show() population and no button wiring, so rebuild_top must decline.
    CHECK_FALSE(Modal::rebuild_top());
    CHECK(modal.hide_calls == 1);
    CHECK_FALSE(modal.is_visible());

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Self-deleting modal destructs when torn down statically",
                 "[modal][1230]") {
    g_self_deleting_live = 0;

    auto* modal = new SelfDeletingModal();
    REQUIRE(modal->show(test_screen()));
    REQUIRE(g_self_deleting_live == 1);

    lv_obj_t* dialog = modal->dialog();
    REQUIRE(dialog != nullptr);

    // on_hide() queues `delete this`; without owner-aware delegation the static
    // overload never calls it and the heap object leaks.
    Modal::hide(dialog);
    process_lvgl(50);

    CHECK(g_self_deleting_live == 0);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "on_hide reaching back into static hide does not recurse",
                 "[modal][1230]") {
    ReentrantModal modal;
    REQUIRE(modal.show(test_screen()));

    lv_obj_t* dialog = modal.dialog();
    REQUIRE(dialog != nullptr);

    // Instance hide() clears the stack's owner before running the hook, so the
    // nested static hide takes the plain static path instead of delegating back
    // into the hide() that is already in progress.
    Modal::hide(dialog);

    CHECK(modal.hide_calls == 1);
    CHECK_FALSE(modal.is_visible());

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// ============================================================================
// Move semantics keep the stack's owner pointer live
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Move constructor reassigns the stack owner", "[modal][1230]") {
    TrackingModal source;
    REQUIRE(source.show(test_screen()));
    lv_obj_t* dialog = source.dialog();
    REQUIRE(dialog != nullptr);

    TrackingModal moved(std::move(source));
    REQUIRE(moved.dialog() == dialog);
    REQUIRE(moved.is_visible());

    Modal::hide(dialog);

    CHECK(moved.hide_calls == 1);
    CHECK(source.hide_calls == 0);
    CHECK_FALSE(moved.is_visible());

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Move assignment reassigns the stack owner", "[modal][1230]") {
    TrackingModal source;
    TrackingModal target;
    REQUIRE(source.show(test_screen()));
    lv_obj_t* dialog = source.dialog();
    REQUIRE(dialog != nullptr);

    target = std::move(source);
    REQUIRE(target.dialog() == dialog);
    REQUIRE(target.is_visible());

    Modal::hide(dialog);

    CHECK(target.hide_calls == 1);
    CHECK(source.hide_calls == 0);
    CHECK_FALSE(target.is_visible());

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// ============================================================================
// Stacking: the next modal down must be raised on the delegate path too
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Hiding an owned modal raises the modal beneath it",
                 "[modal][1230]") {
    lv_obj_t* lower = Modal::show(TEST_COMPONENT);
    REQUIRE(lower != nullptr);
    lv_obj_t* lower_backdrop = ModalStack::instance().backdrop_for(lower);
    REQUIRE(lower_backdrop != nullptr);

    lv_obj_t* sentinel = make_z_order_sentinel(test_screen());
    REQUIRE(sentinel != nullptr);

    TrackingModal upper;
    REQUIRE(upper.show(test_screen()));
    lv_obj_t* upper_dialog = upper.dialog();
    REQUIRE(upper_dialog != nullptr);
    REQUIRE_FALSE(is_foreground_child(lower_backdrop));

    Modal::hide(upper_dialog);

    CHECK(upper.hide_calls == 1);
    // The delegate path must still run the foreground fix-up, or the surviving
    // modal is left buried under ordinary screen content.
    CHECK(is_foreground_child(lower_backdrop));
    CHECK(Modal::get_top() == lower);

    process_lvgl(50);
    Modal::hide(lower);
    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// ============================================================================
// Regression: the static (unowned) path is unchanged
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Static show/hide still tears a modal down", "[modal][1230]") {
    lv_obj_t* dialog = Modal::show(TEST_COMPONENT);
    REQUIRE(dialog != nullptr);
    REQUIRE_FALSE(ModalStack::instance().stack_empty());
    REQUIRE(Modal::any_visible());

    Modal::hide(dialog);
    process_lvgl(50);

    CHECK(ModalStack::instance().stack_empty());
    CHECK_FALSE(Modal::any_visible());
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "rebuild_top hides a statically shown modal too",
                 "[modal][1230]") {
    lv_obj_t* original = Modal::show(TEST_COMPONENT);
    REQUIRE(original != nullptr);

    // A re-show cannot carry the runtime attrs or the post-show button wiring
    // the static factories attach, so hot reload closes the dialog rather than
    // bringing back a mislabelled, inert copy.
    CHECK_FALSE(Modal::rebuild_top());
    process_lvgl(50);

    CHECK(Modal::get_top() == nullptr);
    CHECK(ModalStack::instance().stack_empty());
}

// The concrete failure the hide policy replaces: a confirmation dialog carries
// its title/message as runtime attrs and its button callbacks as post-show
// event handlers, so a rebuild produced labels reading LVGL's default "Text"
// over buttons that no longer did anything.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "rebuild_top does not resurrect a confirmation dialog with default label text",
                 "[modal][1230]") {
    // modal_configure() no-ops without these, leaving the captions at defaults;
    // the app does this at startup. Idempotent — warns and returns if already up.
    helix::ui::modal_init_subjects();

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Printer type mismatch", "This printer looks like something else.", ModalSeverity::Warning,
        "Re-identify", nullptr, nullptr, nullptr, "Keep current");
    REQUIRE(dialog != nullptr);

    lv_obj_t* title = lv_obj_find_by_name(dialog, "dialog_title");
    REQUIRE(title != nullptr);
    REQUIRE(std::string(lv_label_get_text(title)) == "Printer type mismatch");

    CHECK_FALSE(Modal::rebuild_top());
    process_lvgl(50);

    // Closed outright — no second dialog left standing to read "Text".
    CHECK(Modal::get_top() == nullptr);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Hiding a statically shown top modal raises the one beneath",
                 "[modal][1230]") {
    lv_obj_t* lower = Modal::show(TEST_COMPONENT);
    REQUIRE(lower != nullptr);
    lv_obj_t* lower_backdrop = ModalStack::instance().backdrop_for(lower);
    REQUIRE(lower_backdrop != nullptr);

    lv_obj_t* sentinel = make_z_order_sentinel(test_screen());
    REQUIRE(sentinel != nullptr);

    lv_obj_t* upper = Modal::show(TEST_COMPONENT);
    REQUIRE(upper != nullptr);
    REQUIRE_FALSE(is_foreground_child(lower_backdrop));

    Modal::hide(upper);
    CHECK(is_foreground_child(lower_backdrop));
    CHECK(Modal::get_top() == lower);

    process_lvgl(50);
    Modal::hide(lower);
    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// ============================================================================
// The owner-less path has to disarm its own dialog
// ============================================================================

// modal_show_confirmation() / modal_show_alert() push with no owner, so the
// static hide() has nothing to delegate to. It still has to close the window
// the instance path closes: for MODAL_EXIT_DURATION_MS the buttons remain
// clickable while still holding the caller's per-callback user_data, and a
// second tap in that window re-enters the confirm handler with the first tap's
// state already consumed. Clearing LV_OBJ_FLAG_CLICKABLE is what stops it -
// LVGL's indev processing skips non-clickable objects, so the queued press
// never reaches the callback.
TEST_CASE_METHOD(LVGLUITestFixture, "Owner-less hide disarms the dialog buttons",
                 "[modal][teardown]") {
    // modal_configure() no-ops without these; the app does this at startup.
    helix::ui::modal_init_subjects();

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page and all its widgets?", ModalSeverity::Warning, "Delete",
        nullptr, nullptr, nullptr);
    REQUIRE(dialog != nullptr);
    // The whole point: this dialog has no Modal instance behind it.
    REQUIRE(ModalStack::instance().owner_for(dialog) == nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    lv_obj_t* secondary = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(primary != nullptr);
    REQUIRE(secondary != nullptr);
    REQUIRE(lv_obj_has_flag(primary, LV_OBJ_FLAG_CLICKABLE));
    REQUIRE(lv_obj_has_flag(secondary, LV_OBJ_FLAG_CLICKABLE));

    Modal::hide(dialog);

    // Deletion is deferred to the end of the exit animation, so the widgets are
    // still live here. This is precisely the window a second tap lands in.
    REQUIRE(lv_obj_is_valid(primary));
    REQUIRE(lv_obj_is_valid(secondary));
    CHECK_FALSE(lv_obj_has_flag(primary, LV_OBJ_FLAG_CLICKABLE));
    CHECK_FALSE(lv_obj_has_flag(secondary, LV_OBJ_FLAG_CLICKABLE));

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// The disarm has to reach nested XML components, not just the dialog's direct
// children - modal_dialog wraps its buttons in a row container.
TEST_CASE_METHOD(LVGLUITestFixture, "Owner-less hide disarms nested dialog children",
                 "[modal][teardown]") {
    lv_obj_t* dialog = Modal::show(TEST_COMPONENT);
    REQUIRE(dialog != nullptr);
    REQUIRE(ModalStack::instance().owner_for(dialog) == nullptr);

    // Collect every clickable descendant below the dialog's immediate children.
    std::vector<lv_obj_t*> nested;
    for (uint32_t i = 0; i < lv_obj_get_child_count(dialog); i++) {
        lv_obj_t* child = lv_obj_get_child(dialog, i);
        for (uint32_t j = 0; j < lv_obj_get_child_count(child); j++) {
            lv_obj_t* grandchild = lv_obj_get_child(child, j);
            if (lv_obj_has_flag(grandchild, LV_OBJ_FLAG_CLICKABLE)) {
                nested.push_back(grandchild);
            }
        }
    }
    REQUIRE_FALSE(nested.empty());

    Modal::hide(dialog);

    for (lv_obj_t* obj : nested) {
        REQUIRE(lv_obj_is_valid(obj));
        CHECK_FALSE(lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE));
    }

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}
