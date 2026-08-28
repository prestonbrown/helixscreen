// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_modal_lifetime.cpp
 * @brief #1341 — cached modal pointers must not outlive their modal.
 *
 * NetworkSettingsOverlay caches raw lv_obj_t* for its three modals and its
 * async WiFi callbacks null-check those pointers before walking them with
 * lv_obj_find_by_name(). The overlay nulls each pointer for the dismissals it
 * drives itself, but a modal closed by the modal system (back gesture,
 * ModalStack unwinding) left a dangling NON-NULL pointer, so the null check
 * passed and the walk went into freed memory — SIGBUS on the AD5X's MIPS core,
 * silent corruption elsewhere.
 *
 * The lifetime token those callbacks already carry cannot cover this: it guards
 * the overlay, which is a singleton that outlives every modal it opens.
 */

#include "ui_overlay_network_settings.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/network_settings_overlay_test_access.h"
#include "../ui_test_utils.h"

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace {

class NetworkModalLifetimeFixture : public LVGLTestFixture {
  protected:
    NetworkSettingsOverlay& overlay() {
        return get_network_settings_overlay();
    }

    /// A stand-in for a modal: a real screen child, watched by the real handler.
    lv_obj_t* make_watched_modal() {
        lv_obj_t* modal = lv_obj_create(lv_screen_active());
        REQUIRE(modal != nullptr);
        Access::watch(overlay(), modal);
        return modal;
    }
};

} // namespace

TEST_CASE_METHOD(NetworkModalLifetimeFixture,
                 "Deleting the password modal clears the overlay's cached pointer",
                 "[network_settings][modal_lifetime][1341]") {
    lv_obj_t* modal = make_watched_modal();
    Access::password_modal(overlay()) = modal;

    // The modal system destroys it — NOT hide_password_modal().
    lv_obj_delete(modal);

    REQUIRE(Access::password_modal(overlay()) == nullptr);
}

TEST_CASE_METHOD(NetworkModalLifetimeFixture,
                 "Deleting the hidden-network modal clears its cached pointer",
                 "[network_settings][modal_lifetime][1341]") {
    lv_obj_t* modal = make_watched_modal();
    Access::hidden_network_modal(overlay()) = modal;

    lv_obj_delete(modal);

    REQUIRE(Access::hidden_network_modal(overlay()) == nullptr);
}

TEST_CASE_METHOD(NetworkModalLifetimeFixture,
                 "Deleting the network-test modal clears its cached pointer",
                 "[network_settings][modal_lifetime][1341]") {
    lv_obj_t* modal = make_watched_modal();
    Access::test_modal(overlay()) = modal;

    lv_obj_delete(modal);

    REQUIRE(Access::test_modal(overlay()) == nullptr);
}

TEST_CASE_METHOD(NetworkModalLifetimeFixture, "Deleting the step widget clears its cached pointer",
                 "[network_settings][modal_lifetime][1341]") {
    // Widest exposure of the family: the network-test progress callback fires
    // repeatedly over seconds and dereferences step_widget_ nine times behind a
    // single null check, so a modal dismissed mid-test lands right in it.
    lv_obj_t* widget = make_watched_modal();
    Access::step_widget(overlay()) = widget;

    lv_obj_delete(widget);

    REQUIRE(Access::step_widget(overlay()) == nullptr);
}

TEST_CASE_METHOD(NetworkModalLifetimeFixture, "A step widget dies with the modal that contains it",
                 "[network_settings][modal_lifetime][1341]") {
    // step_widget_ is a grandchild of test_modal_. Deleting the ancestor must
    // clear BOTH cached pointers - LVGL fires DELETE on descendants too.
    lv_obj_t* modal = make_watched_modal();
    lv_obj_t* container = lv_obj_create(modal);
    lv_obj_t* widget = lv_obj_create(container);
    Access::watch(overlay(), widget);
    Access::test_modal(overlay()) = modal;
    Access::step_widget(overlay()) = widget;

    lv_obj_delete(modal);

    REQUIRE(Access::test_modal(overlay()) == nullptr);
    REQUIRE(Access::step_widget(overlay()) == nullptr);
}

TEST_CASE_METHOD(NetworkModalLifetimeFixture, "A dying modal clears only its own cached pointer",
                 "[network_settings][modal_lifetime][1341]") {
    // All three pointers share one handler, so a careless implementation could
    // null whichever slot it looked at first.
    lv_obj_t* password = make_watched_modal();
    lv_obj_t* hidden = make_watched_modal();
    Access::password_modal(overlay()) = password;
    Access::hidden_network_modal(overlay()) = hidden;

    lv_obj_delete(password);

    REQUIRE(Access::password_modal(overlay()) == nullptr);
    REQUIRE(Access::hidden_network_modal(overlay()) == hidden);

    lv_obj_delete(hidden);
    REQUIRE(Access::hidden_network_modal(overlay()) == nullptr);
}

TEST_CASE_METHOD(NetworkModalLifetimeFixture,
                 "A late DELETE from a replaced modal leaves the live one alone",
                 "[network_settings][modal_lifetime][1341]") {
    // Modals are deferred-deleted, so a DELETE for the PREVIOUS password modal
    // can land after the next one is already on screen. Clearing the pointer
    // then would blank a modal the user is looking at.
    lv_obj_t* first = make_watched_modal();
    Access::password_modal(overlay()) = first;

    lv_obj_t* second = make_watched_modal();
    Access::password_modal(overlay()) = second; // replaced before `first` dies

    lv_obj_delete(first);

    REQUIRE(Access::password_modal(overlay()) == second);

    lv_obj_delete(second);
    Access::password_modal(overlay()) = nullptr;
}
