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

#include "async_lifetime_guard.h"

// LVGLUITestFixture registers ALL XML components, so print_cancel_confirm_modal
// (a self-contained dialog with no subject bindings) builds its real widget
// tree here. The fixture also forces animations off, which makes modal exit
// teardown synchronous apart from the deferred widget delete.
#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"

#include <memory>
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

/// Simulates a user tap on a dialog's backdrop - the dismissal path a caller
/// did not initiate. Modal::hide(dialog) is no longer one: hide() distinguishes
/// caller closes (Programmatic) from user/environment ones, so a test that
/// means "dismissed" must drive this, not a programmatic close.
void tap_backdrop(lv_obj_t* dialog) {
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
}

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
// The confirmation helpers are owned by an instance (#1379)
// ============================================================================

// The helpers used to push through the static factory with owner = nullptr, so
// nothing observed their close: no on_hide(), no lifetime_, and a dismissal
// reported nothing to the caller. Re-homing them on an internal Modal subclass
// makes on_hide() the always-fires resolve point without changing any of the 63
// call sites' signatures.
TEST_CASE_METHOD(LVGLUITestFixture, "A confirmation dialog is owned by a Modal instance",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    lv_obj_t* dialog = helix::ui::modal_show_confirmation("Delete Page", "Remove this page?",
                                                          ModalSeverity::Warning, "Delete", nullptr,
                                                          nullptr, nullptr);
    REQUIRE(dialog != nullptr);

    // The whole point of #1379: there is now something to delegate to.
    CHECK(ModalStack::instance().owner_for(dialog) != nullptr);

    Modal::hide(dialog);
    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// A backdrop tap or ESC fires neither button. That is the case every #1380 site
// was stranded by, and it is now reportable.
TEST_CASE_METHOD(LVGLUITestFixture, "Dismissing a confirmation invokes on_dismiss",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    int dismissed = 0;
    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete", nullptr, nullptr,
        nullptr, nullptr, [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);
    REQUIRE(dismissed == 0);

    tap_backdrop(dialog); // user dismissed - neither button
    process_lvgl(50);

    CHECK(dismissed == 1);
    CHECK(ModalStack::instance().stack_empty());
}

// ...but answering must NOT look like a dismissal, or every caller that resolves
// its own state in a button handler would resolve it twice.
TEST_CASE_METHOD(LVGLUITestFixture, "Answering a confirmation does not invoke on_dismiss",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    int dismissed = 0;
    static int confirms = 0;
    confirms = 0;

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete",
        [](lv_event_t*) { ++confirms; }, nullptr, nullptr, nullptr,
        [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    lv_obj_send_event(primary, LV_EVENT_CLICKED, nullptr);
    REQUIRE(confirms == 1);

    Modal::hide(dialog);
    process_lvgl(50);

    CHECK(dismissed == 0); // answered, not dismissed
    CHECK(ModalStack::instance().stack_empty());
}

// The owner is what makes the static hide() overload run a real teardown, so a
// helper dialog closed through it now disarms like any instance-backed modal.
TEST_CASE_METHOD(LVGLUITestFixture, "A helper dialog disarms through its owner",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    lv_obj_t* dialog = helix::ui::modal_show_confirmation("Delete Page", "Remove this page?",
                                                          ModalSeverity::Warning, "Delete", nullptr,
                                                          nullptr, nullptr);
    REQUIRE(dialog != nullptr);
    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    REQUIRE(lv_obj_has_flag(primary, LV_OBJ_FLAG_CLICKABLE));

    Modal::hide(dialog);

    REQUIRE(lv_obj_is_valid(primary));
    CHECK_FALSE(lv_obj_has_flag(primary, LV_OBJ_FLAG_CLICKABLE));

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// ============================================================================
// The declarative form attaches nothing to a widget (#1383)
// ============================================================================

// modal_confirm() takes std::function, so on_ok() invokes it directly and the
// modal closes itself. No lv_event_cb_t reaches a button and there is no
// user_data to outlive the dialog.
TEST_CASE_METHOD(LVGLUITestFixture, "modal_confirm invokes the callback and closes itself",
                 "[modal][teardown][1383]") {
    helix::ui::modal_init_subjects();

    int confirmed = 0, cancelled = 0, dismissed = 0;
    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [&cancelled] { ++cancelled; };
    opts.on_dismiss = [&dismissed] { ++dismissed; };
    lv_obj_t* dialog = helix::ui::modal_confirm(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete",
        [&confirmed]() { ++confirmed; }, opts);
    REQUIRE(dialog != nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    lv_obj_send_event(primary, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(confirmed == 1);
    CHECK(cancelled == 0);
    CHECK(dismissed == 0);                       // answered, not dismissed
    CHECK(ModalStack::instance().stack_empty()); // closed itself, no caller hide()
}

TEST_CASE_METHOD(LVGLUITestFixture, "modal_confirm reports a dismissal to on_dismiss",
                 "[modal][teardown][1383]") {
    helix::ui::modal_init_subjects();

    int confirmed = 0, dismissed = 0;
    helix::ui::ConfirmOptions opts;
    opts.on_dismiss = [&dismissed] { ++dismissed; };
    lv_obj_t* dialog = helix::ui::modal_confirm(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete",
        [&confirmed]() { ++confirmed; }, opts);
    REQUIRE(dialog != nullptr);

    tap_backdrop(dialog); // user dismissed - neither button
    process_lvgl(50);

    CHECK(confirmed == 0);
    CHECK(dismissed == 1);
    CHECK(ModalStack::instance().stack_empty());
}

// The cancel hook is the ESC route too, so it must resolve the same way.
TEST_CASE_METHOD(LVGLUITestFixture, "modal_confirm cancel is an answer, not a dismissal",
                 "[modal][teardown][1383]") {
    helix::ui::modal_init_subjects();

    int cancelled = 0, dismissed = 0;
    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [&cancelled] { ++cancelled; };
    opts.on_dismiss = [&dismissed] { ++dismissed; };
    lv_obj_t* dialog = helix::ui::modal_confirm("Delete Page", "Remove this page?",
                                                ModalSeverity::Warning, "Delete", nullptr, opts);
    REQUIRE(dialog != nullptr);

    lv_obj_t* secondary = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(secondary != nullptr);
    lv_obj_send_event(secondary, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(cancelled == 1);
    CHECK(dismissed == 0);
    CHECK(ModalStack::instance().stack_empty());
}

// ESC routes to on_cancel() for instance-backed modals. Binding that hook to the
// cancel BUTTON would make the two indistinguishable, and a legacy caller that
// supplied a cancel callback would then leave ESC unable to close the dialog at
// all - it would mark the modal answered and leave it on screen. The legacy path
// therefore leaves the hooks unwired so Modal::on_cancel()'s default still runs.
TEST_CASE_METHOD(LVGLUITestFixture, "ESC closes a legacy confirmation that has a cancel callback",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    static int cancels = 0;
    cancels = 0;
    int dismissed = 0;

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete", nullptr,
        [](lv_event_t*) { ++cancels; }, nullptr, nullptr, [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);

    // ESC, as esc_key_cb delivers it.
    uint32_t key = LV_KEY_ESC;
    lv_obj_send_event(backdrop, LV_EVENT_KEY, &key);
    process_lvgl(50);

    // It must actually close...
    CHECK(ModalStack::instance().stack_empty());
    // ...report a dismissal, since no button was pressed...
    CHECK(dismissed == 1);
    // ...and NOT invoke the caller's cancel callback, which is bound to the
    // button. That would be a behaviour change across 63 call sites.
    CHECK(cancels == 0);
}

// ============================================================================
// The dismissal callback's lifetime tie (#1379 review follow-up)
// ============================================================================

// on_dismiss is a std::function the caller supplies, and the dialog outlives its
// exit animation - so a capture whose owner dies first is a use-after-free. That
// is the shape that got #1380 reverted. dismiss_token is the tie: it is checked
// when the callback actually fires, not when it was scheduled.
TEST_CASE_METHOD(LVGLUITestFixture, "An expired dismiss token suppresses on_dismiss",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    int dismissed = 0;
    auto guard = std::make_unique<helix::AsyncLifetimeGuard>();

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete", nullptr, nullptr,
        nullptr, nullptr, [&dismissed]() { ++dismissed; }, guard->token());
    REQUIRE(dialog != nullptr);

    // The owner dies while the dialog is still up - the exact race the tie exists
    // for. A panel destroyed by StaticPanelRegistry teardown does this.
    guard.reset();

    tap_backdrop(dialog); // dismissal on a dead owner - the reason the tie exists
    process_lvgl(50);

    CHECK(dismissed == 0); // callback skipped rather than run on a dead owner
    CHECK(ModalStack::instance().stack_empty());
}

// ...and a live token must not suppress it, or the tie would silently disable
// the whole mechanism.
TEST_CASE_METHOD(LVGLUITestFixture, "A live dismiss token still fires on_dismiss",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    int dismissed = 0;
    helix::AsyncLifetimeGuard guard;

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete", nullptr, nullptr,
        nullptr, nullptr, [&dismissed]() { ++dismissed; }, guard.token());
    REQUIRE(dialog != nullptr);

    tap_backdrop(dialog);
    process_lvgl(50);

    CHECK(dismissed == 1);
}

// The declarative form's callbacks are invoked by the modal itself, so unlike a
// legacy lv_event_cb_t they CAN be called off. The token therefore gates all
// three, which is what makes a `this` capture safe to hand to modal_confirm():
// the legacy form's void* still has to outlive the dialog on its own.
TEST_CASE_METHOD(LVGLUITestFixture, "An expired owner token suppresses modal_confirm's confirm",
                 "[modal][teardown][1383]") {
    helix::ui::modal_init_subjects();

    int confirmed = 0;
    auto guard = std::make_unique<helix::AsyncLifetimeGuard>();

    helix::ui::ConfirmOptions opts;
    opts.owner_token = guard->token();
    lv_obj_t* dialog = helix::ui::modal_confirm(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete",
        [&confirmed]() { ++confirmed; }, opts);
    REQUIRE(dialog != nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);

    guard.reset(); // owner dies with the dialog still on screen

    lv_obj_send_event(primary, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(confirmed == 0);                       // not run on a dead owner
    CHECK(ModalStack::instance().stack_empty()); // but the dialog still closes
}

// Same gate on the other side. The two handlers are separate functions, so a
// mutation that drops the check from only one of them has to show up somewhere.
TEST_CASE_METHOD(LVGLUITestFixture, "An expired owner token suppresses modal_confirm's cancel",
                 "[modal][teardown][1383]") {
    helix::ui::modal_init_subjects();

    int cancelled = 0;
    auto guard = std::make_unique<helix::AsyncLifetimeGuard>();

    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [&cancelled] { ++cancelled; };
    opts.owner_token = guard->token();
    lv_obj_t* dialog = helix::ui::modal_confirm("Delete Page", "Remove this page?",
                                                ModalSeverity::Warning, "Delete", nullptr, opts);
    REQUIRE(dialog != nullptr);

    lv_obj_t* secondary = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(secondary != nullptr);

    guard.reset();

    lv_obj_send_event(secondary, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(cancelled == 0);
    CHECK(ModalStack::instance().stack_empty());
}

// Known-positive for the two above: with the guard alive the same click DOES
// reach the callback, so their zeros mean "suppressed" and not "never wired".
TEST_CASE_METHOD(LVGLUITestFixture, "A live owner token still runs modal_confirm's callback",
                 "[modal][teardown][1383]") {
    helix::ui::modal_init_subjects();

    int confirmed = 0;
    helix::AsyncLifetimeGuard guard;

    helix::ui::ConfirmOptions opts;
    opts.owner_token = guard.token();
    lv_obj_t* dialog = helix::ui::modal_confirm(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete",
        [&confirmed]() { ++confirmed; }, opts);
    REQUIRE(dialog != nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    lv_obj_send_event(primary, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(confirmed == 1);
}

// modal_alert routes its single button through the same primary handler, so the
// gate has to hold for the one-button shape too.
TEST_CASE_METHOD(LVGLUITestFixture, "An expired owner token suppresses modal_alert's callback",
                 "[modal][teardown][1383]") {
    helix::ui::modal_init_subjects();

    int acked = 0;
    auto guard = std::make_unique<helix::AsyncLifetimeGuard>();

    helix::ui::AlertOptions alert_opts;
    alert_opts.owner_token = guard->token();
    lv_obj_t* dialog = helix::ui::modal_alert(
        "Heads up", "Something happened", ModalSeverity::Info, "OK", [&acked]() { ++acked; },
        alert_opts);
    REQUIRE(dialog != nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);

    guard.reset();

    lv_obj_send_event(primary, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(acked == 0);
    CHECK(ModalStack::instance().stack_empty());
}

// The owner frees itself one tick after on_hide(), while the dialog lives out
// MODAL_EXIT_DURATION_MS. Every callback carrying that pointer must be gone
// before then, or a programmatic click in the window dispatches into freed
// memory - lv_obj_send_event ignores the CLICKABLE flag that
// disable_clicks_recursive removes, which is how `ctl click` reaches disabled
// widgets.
//
// Scope note: this strips the OWNER's callbacks. The caller's own
// lv_event_cb_t is registered with the caller's user_data and is left in place
// until the widget is deleted - it is the caller's pointer, not ours, and
// removing arbitrary third-party callbacks during teardown is not this
// function's business. Real input cannot reach it (disable_clicks_recursive +
// lv_indev_reset); only a synthetic lv_obj_send_event can.
TEST_CASE_METHOD(LVGLUITestFixture, "Teardown strips the owner's callbacks from the dialog",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    static int confirms = 0;
    confirms = 0;

    // Non-null user_data matters: the caller's callback must NOT be registered
    // with the owner pointer, or the strip would take it too and this test
    // could not tell "removed ours" from "removed everything".
    static int marker_data = 0;
    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete",
        [](lv_event_t*) { ++confirms; }, nullptr, &marker_data);
    REQUIRE(dialog != nullptr);

    lv_obj_t* primary = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(primary != nullptr);
    // The owner's answered_ marker plus the caller's callback are both here.
    const uint32_t before = lv_obj_get_event_count(primary);
    REQUIRE(before >= 2);

    Modal::hide(dialog);

    REQUIRE(lv_obj_is_valid(primary));
    const uint32_t after = lv_obj_get_event_count(primary);
    // Exactly one removed: ours. The caller's callback carries its own
    // user_data and is deliberately left alone.
    CHECK(after == before - 1);

    process_lvgl(50);
    CHECK(ModalStack::instance().stack_empty());
}

// on_hide() runs mid-teardown: the stack entry has been un-owned but is not yet
// marked exiting. Invoking caller code there lets the natural "clear my handle
// and make sure it's closed" shape (ui_print_start_controller.cpp does exactly
// this) re-enter Modal::hide on the SAME backdrop, which then finds the entry
// present, un-owned and not exiting, and runs a second full teardown - two
// animate_exit calls on one object, and with animations off two deferred
// deletes. Deferring the callback to the next tick closes that window: by the
// time it runs, the entry is exiting and the reentrant hide is a no-op.
TEST_CASE_METHOD(LVGLUITestFixture, "A dismissal callback may close its own dialog",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    static lv_obj_t* handle = nullptr;
    static int dismissed = 0;
    static lv_obj_t* top_when_dismissed = nullptr;
    static bool top_was_captured = false;
    handle = nullptr;
    dismissed = 0;
    top_when_dismissed = nullptr;
    top_was_captured = false;

    handle = helix::ui::modal_show_confirmation("Delete Page", "Remove this page?",
                                                ModalSeverity::Warning, "Delete", nullptr, nullptr,
                                                nullptr, nullptr, []() {
                                                    ++dismissed;
                                                    // Deferral is what makes the reentrant call
                                                    // below safe, and this is the observable
                                                    // difference: by the time a DEFERRED callback
                                                    // runs, the dialog is already marked exiting,
                                                    // so it is no longer the top modal and a
                                                    // reentrant hide cannot target it. Fired
                                                    // synchronously from on_hide() it would still
                                                    // be top - which is precisely the window where
                                                    // a second full teardown lands on the same
                                                    // backdrop.
                                                    top_when_dismissed = Modal::get_top();
                                                    top_was_captured = true;
                                                    if (handle) {
                                                        helix::ui::modal_hide(handle);
                                                        handle = nullptr;
                                                    }
                                                });
    REQUIRE(handle != nullptr);

    tap_backdrop(handle);
    process_lvgl(50);

    CHECK(dismissed == 1);
    REQUIRE(top_was_captured);
    // The load-bearing assertion: the dialog was NOT the top modal when the
    // callback ran, so its reentrant hide had nothing to re-tear-down.
    CHECK(top_when_dismissed == nullptr);
    CHECK(ModalStack::instance().stack_empty());
    CHECK(Modal::get_top() == nullptr);
}

// ESC must report a dismissal on EVERY dialog shape, not just the ones whose
// cancel button carries a caller callback. Previously on_cancel() served double
// duty as the cancel-button hook, so a confirmation with a cancel button but no
// cancel callback latched answered_ on ESC and on_dismiss never fired - the
// #1380 leak surviving on the ESC path while working on a backdrop tap. Routing
// every button to the class's own handler leaves on_cancel() reachable only
// from esc_key_cb, which makes "closed with no button pressed" decidable.
TEST_CASE_METHOD(LVGLUITestFixture, "ESC reports a dismissal with a cancel button but no callback",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    static int confirms = 0;
    confirms = 0;
    int dismissed = 0;

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete",
        [](lv_event_t*) { ++confirms; }, /*on_cancel=*/nullptr, nullptr, nullptr,
        [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);
    // The cancel button exists - this is the shape that used to swallow ESC.
    REQUIRE(lv_obj_find_by_name(dialog, "btn_secondary") != nullptr);

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    uint32_t key = LV_KEY_ESC;
    lv_obj_send_event(backdrop, LV_EVENT_KEY, &key);
    process_lvgl(50);

    CHECK(dismissed == 1);
    CHECK(confirms == 0);
    CHECK(ModalStack::instance().stack_empty());
}

// An alert has no cancel button at all, so ESC is the only way into on_cancel().
TEST_CASE_METHOD(LVGLUITestFixture, "ESC reports a dismissal on an alert",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    int dismissed = 0;
    lv_obj_t* dialog =
        helix::ui::modal_show_alert("Heads up", "Something happened", ModalSeverity::Info, "OK",
                                    nullptr, nullptr, [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    uint32_t key = LV_KEY_ESC;
    lv_obj_send_event(backdrop, LV_EVENT_KEY, &key);
    process_lvgl(50);

    CHECK(dismissed == 1);
    CHECK(ModalStack::instance().stack_empty());
}

// A button press only "answers" if it reaches a caller callback. Pressing a
// Cancel button that has no callback behind it tells the caller nothing, so
// suppressing on_dismiss there would strand exactly the state on_dismiss exists
// to clear - the #1380 leak, on the button path instead of the ESC path.
TEST_CASE_METHOD(LVGLUITestFixture, "A button with no callback reports a dismissal",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    static int confirms = 0;
    confirms = 0;
    int dismissed = 0;

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete",
        [](lv_event_t*) { ++confirms; }, /*on_cancel=*/nullptr, nullptr, nullptr,
        [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);

    lv_obj_t* secondary = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(secondary != nullptr);
    lv_obj_send_event(secondary, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(confirms == 0);
    CHECK(dismissed == 1); // nothing else told the caller
    CHECK(ModalStack::instance().stack_empty());
}

// ...but a press that DOES reach a callback is an answer, not a dismissal.
TEST_CASE_METHOD(LVGLUITestFixture, "A button with a callback is not a dismissal",
                 "[modal][teardown][1379]") {
    helix::ui::modal_init_subjects();

    static int cancels = 0;
    cancels = 0;
    int dismissed = 0;

    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete", nullptr,
        [](lv_event_t*) { ++cancels; }, nullptr, nullptr, [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);

    lv_obj_t* secondary = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(secondary != nullptr);
    lv_obj_send_event(secondary, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(cancels == 1);
    CHECK(dismissed == 0);
}

// ============================================================================
// A caller's own close is not a dismissal
// ============================================================================

// on_dismiss fired on ANY close where no button callback ran - including the
// caller's own Modal::hide() from teardown. Deferred through async_call, the
// callback landed a tick AFTER a destructor finished, which is the UAF shape
// that got the #1380 per-site fix reverted. The two callers that could hit it
// were safe only because each nulled its handle first and passed a token;
// neither is required by the signature. hide() now defaults to Programmatic,
// and on_dismiss means "closed by something other than you".
TEST_CASE_METHOD(LVGLUITestFixture, "A caller's own close does not invoke on_dismiss",
                 "[modal][teardown][1380]") {
    helix::ui::modal_init_subjects();

    int dismissed = 0;
    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete", nullptr, nullptr,
        nullptr, nullptr, [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);

    Modal::hide(dialog); // the caller's teardown shape: null the handle, close
    process_lvgl(50);

    CHECK(dismissed == 0);
    CHECK(ModalStack::instance().stack_empty());
}

// Same contract on the declarative form, whose callers are the #1380 sweep's
// target: converting a site must not arm a deferred callback against its own
// teardown.
TEST_CASE_METHOD(LVGLUITestFixture, "modal_confirm's programmatic close does not invoke on_dismiss",
                 "[modal][teardown][1380]") {
    helix::ui::modal_init_subjects();

    int dismissed = 0;
    helix::ui::ConfirmOptions opts;
    opts.on_dismiss = [&dismissed] { ++dismissed; };
    lv_obj_t* dialog = helix::ui::modal_confirm("Delete Page", "Remove this page?",
                                                ModalSeverity::Warning, "Delete", nullptr, opts);
    REQUIRE(dialog != nullptr);

    Modal::hide(dialog);
    process_lvgl(50);

    CHECK(dismissed == 0);
    CHECK(ModalStack::instance().stack_empty());
}

// A hot-reload rebuild closes the dialog from outside the caller, so it still
// reports - and this pins the HotReload reason so a later regression that
// routes every close through the Programmatic default cannot silently eat it.
TEST_CASE_METHOD(LVGLUITestFixture, "A hot-reload rebuild still reports a dismissal",
                 "[modal][teardown][1380]") {
    helix::ui::modal_init_subjects();

    int dismissed = 0;
    lv_obj_t* dialog = helix::ui::modal_show_confirmation(
        "Delete Page", "Remove this page?", ModalSeverity::Warning, "Delete", nullptr, nullptr,
        nullptr, nullptr, [&dismissed]() { ++dismissed; });
    REQUIRE(dialog != nullptr);

    // Instance-backed, so rebuild_top hides rather than resurrects a broken
    // copy - either way the caller did not close it.
    Modal::rebuild_top();
    process_lvgl(50);

    CHECK(dismissed == 1);
    CHECK(ModalStack::instance().stack_empty());
}
