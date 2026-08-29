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

#include "ui_button.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

// LVGLUITestFixture registers ALL XML components, so print_cancel_confirm_modal
// (a self-contained dialog with no subject bindings) builds its real widget
// tree here. The fixture also forces animations off, which makes modal exit
// teardown synchronous apart from the deferred widget delete.
#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"

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

// The buttons that matter are NOT direct children. print_cancel_confirm_modal
// mounts a modal_button_row, whose own view is an lv_obj wrapping a divider and
// an inner flex row, so btn_primary sits three levels below the dialog. The
// earlier version of this test walked only to grandchildren and asserted on the
// divider and the row container - it passed while every real button stayed
// armed. Assert the depth explicitly so a shallower recursion cannot pass.
TEST_CASE_METHOD(LVGLUITestFixture, "Owner-less hide disarms deeply nested buttons",
                 "[modal][teardown]") {
    lv_obj_t* dialog = Modal::show("print_cancel_confirm_modal");
    REQUIRE(dialog != nullptr);
    REQUIRE(ModalStack::instance().owner_for(dialog) == nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);

    // Prove this component really does nest the button below grandchild depth,
    // so the assertion below exercises deep recursion rather than depth 1 or 2.
    int depth = 0;
    for (lv_obj_t* p = lv_obj_get_parent(primary); p && p != dialog; p = lv_obj_get_parent(p)) {
        depth++;
    }
    REQUIRE(depth >= 2); // >= 2 ancestors between button and dialog
    REQUIRE(lv_obj_has_flag(primary, LV_OBJ_FLAG_CLICKABLE));

    Modal::hide(dialog);

    REQUIRE(lv_obj_is_valid(primary));
    CHECK_FALSE(lv_obj_has_flag(primary, LV_OBJ_FLAG_CLICKABLE));

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// Clearing CLICKABLE only stops a NEW press. LVGL dispatches LV_EVENT_CLICKED on
// release to the object captured at press time, gated on LV_STATE_DISABLED and
// not on the flag, so a finger already down when the modal closes would still
// fire the confirm callback on lift-off.
//
// The first half of this test is a known-positive: it drives a full press and
// release through the virtual indev and REQUIREs the callback fired. Without it
// a mis-aimed press (wrong coordinates, dialog still mid-entrance-animation)
// would make the real assertion below pass for the wrong reason - removing
// lv_indev_reset would not turn it red. The confirm callback here deliberately
// does not hide, so the dialog is still up for the second half.
TEST_CASE_METHOD(LVGLUITestFixture, "A press held across hide does not fire the confirm callback",
                 "[modal][teardown]") {
    helix::ui::modal_init_subjects();
    UITest::init(test_screen());

    static int confirms = 0;
    confirms = 0;

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page and all its widgets?", ModalSeverity::Warning, "Delete",
        [](lv_event_t*) { confirms++; }, nullptr, nullptr);
    REQUIRE(dialog != nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);

    // Let the entrance animation finish before measuring - MODAL_ENTRANCE_DURATION_MS
    // is 250, and coordinates taken mid-animation point at a scaled dialog.
    process_lvgl(400);
    // process_lvgl() pumps timers but does not force a layout pass, and without
    // one every widget still reads back as a zero-area rect at the origin - which
    // would send the press below to (0,0) and quietly make this test vacuous.
    lv_obj_update_layout(lv_screen_active());

    lv_area_t coords;
    lv_obj_get_coords(primary, &coords);
    const int32_t cx = coords.x1 + lv_area_get_width(&coords) / 2;
    const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;

    // Known-positive: this press/release MUST reach the callback, or everything
    // below is vacuous.
    REQUIRE(UITest::press_at(cx, cy));
    REQUIRE(UITest::release());
    process_lvgl(50);
    REQUIRE(confirms == 1);

    // Now the real case. Finger down on the confirm button...
    REQUIRE(UITest::press_at(cx, cy));

    // ...modal torn down underneath it (an emergency-stop state change, the
    // remote-control server clearing modals, a hot-reload rebuild)...
    Modal::hide(dialog);

    // ...finger lifts. The press was captured before the teardown, so LVGL will
    // still deliver CLICKED to it unless disarm_tree dropped the captured object.
    REQUIRE(UITest::release());
    process_lvgl(50);

    CHECK(confirms == 1); // still 1, not 2

    UITest::cleanup();
    process_lvgl(50);
}

// The disarm nulls user_data across the dialog tree so a subclass that stashed
// `this` there cannot be reached after teardown. ui_button is the exception: it
// owns that slot, and button_delete_cb frees the allocation by reading it back.
TEST_CASE_METHOD(LVGLUITestFixture, "Disarm preserves ui_button user_data but clears the rest",
                 "[modal][teardown]") {
    lv_obj_t* dialog = Modal::show("print_cancel_confirm_modal");
    REQUIRE(dialog != nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    REQUIRE(ui_button_owns_user_data(primary));

    // A non-button carrying a stale owner pointer, the case the clear exists for.
    lv_obj_t* content = lv_obj_get_child(dialog, 0);
    REQUIRE(content != nullptr);
    int sentinel = 0;
    lv_obj_set_user_data(content, &sentinel);

    Modal::hide(dialog);

    REQUIRE(lv_obj_is_valid(primary));
    // Still ui_button's own allocation - button_delete_cb can still free it.
    CHECK(ui_button_owns_user_data(primary));
    // The foreign pointer is gone.
    CHECK(lv_obj_get_user_data(content) == nullptr);

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// The extraction must not have weakened the two paths that already disarmed.
TEST_CASE_METHOD(LVGLUITestFixture, "Instance hide disarms through the shared path",
                 "[modal][teardown]") {
    TrackingModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* dialog = modal.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    REQUIRE(lv_obj_has_flag(primary, LV_OBJ_FLAG_CLICKABLE));

    modal.hide();

    REQUIRE(lv_obj_is_valid(primary));
    CHECK_FALSE(lv_obj_has_flag(primary, LV_OBJ_FLAG_CLICKABLE));
    CHECK(modal.hide_calls == 1);

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// ============================================================================
// Dismissal has to resolve the caller's state (#1380)
// ============================================================================

// show_low_ram_resonance_warning()'s two callers both gate re-entry on their
// dialog handle ("a second entry while the warning modal is open is a no-op")
// and only their confirm/cancel callbacks cleared it. A backdrop tap or ESC
// fires neither, so the handle stayed set and every later resonance calibration
// became a silent no-op - on exactly the low-RAM machines the warning exists
// for. The clear lives in the wrapper because it is one rule with two callers.
TEST_CASE_METHOD(LVGLUITestFixture, "Dismissing the low-RAM warning clears the caller's handle",
                 "[modal][teardown][1380]") {
    helix::ui::modal_init_subjects();

    lv_obj_t* slot = nullptr;
    lv_obj_t* dialog =
        helix::ui::show_low_ram_resonance_warning(256, nullptr, nullptr, nullptr, &slot);
    REQUIRE(dialog != nullptr);
    REQUIRE(slot == dialog); // wrapper populates the caller's handle

    // Dismiss the way a backdrop tap or ESC does: neither callback runs.
    Modal::hide(dialog);
    process_lvgl(50);

    CHECK(slot == nullptr);
    CHECK(ModalStack::instance().stack_empty());
}

// The clear must not fire blind. A dismissed dialog's DELETE arrives at the end
// of its exit animation, by which point the owner may already have opened a new
// warning - clearing then would blank the handle for a dialog that is still up,
// re-opening the very re-entry hole this fixes.
TEST_CASE_METHOD(LVGLUITestFixture, "A dying low-RAM dialog does not clear a re-opened one",
                 "[modal][teardown][1380]") {
    helix::ui::modal_init_subjects();

    lv_obj_t* slot = nullptr;
    lv_obj_t* first =
        helix::ui::show_low_ram_resonance_warning(256, nullptr, nullptr, nullptr, &slot);
    REQUIRE(first != nullptr);

    // Close the first and immediately open a second, before the first's exit
    // animation has completed and delivered its DELETE.
    Modal::hide(first);
    lv_obj_t* second =
        helix::ui::show_low_ram_resonance_warning(256, nullptr, nullptr, nullptr, &slot);
    REQUIRE(second != nullptr);
    REQUIRE(second != first);
    REQUIRE(slot == second);

    // Now let the first one finish dying.
    process_lvgl(50);

    // The handle must still name the live dialog, not have been blanked.
    CHECK(slot == second);

    Modal::hide(second);
    process_lvgl(50);
    CHECK(slot == nullptr);
    CHECK(ModalStack::instance().stack_empty());
}
