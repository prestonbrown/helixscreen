// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_wifi_modal_lifetime.cpp
 * @brief The wizard WiFi step's cached modal pointer must not outlive its modal.
 *
 * WizardWifiStep keeps the raw lv_obj_t* that modal_show() returns for the
 * password modal, and the deferred connect-result callback walks it with
 * lv_obj_find_by_name(). Every modal carries an unconditional backdrop-tap and
 * ESC handler that closes it through Modal::hide() without telling the caller,
 * so a dismissal the step did not drive leaves that member pointing at freed
 * memory while the in-flight join is still running
 * (prestonbrown/helixscreen#1579).
 *
 * The lifetime token the callback already carries cannot cover this: it guards
 * the step, which outlives every modal it opens.
 */

#include "ui_modal.h"
#include "ui_wizard_wifi.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/wizard_wifi_test_access.h"
#include "../ui_test_utils.h"

#include "../catch_amalgamated.hpp"

using Access = WizardWifiStepTestAccess;

namespace {

/// Simulates a user tap on a dialog's backdrop, the dismissal route that
/// reaches Modal::hide() without passing through hide_password_modal().
void tap_backdrop(lv_obj_t* dialog) {
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
}

/// A step with its subjects live, which show_password_modal() writes the SSID
/// into before building the dialog.
class WizardWifiModalFixture : public LVGLUITestFixture {
  protected:
    WizardWifiStep& step() {
        if (!started_) {
            step_.init_subjects();
            started_ = true;
        }
        return step_;
    }

  private:
    WizardWifiStep step_;
    bool started_ = false;
};

} // namespace

TEST_CASE_METHOD(WizardWifiModalFixture,
                 "A backdrop tap on the password modal clears the step's cached pointer",
                 "[wizard][wifi][modal_lifetime][1579]") {
    WizardWifiStep& s = step();

    s.show_password_modal("TestNetwork");
    lv_obj_t* dialog = Access::password_modal(s);
    REQUIRE(dialog != nullptr);

    tap_backdrop(dialog);
    process_lvgl(50);

    // The in-flight connect callback null-checks this before walking it.
    REQUIRE(Access::password_modal(s) == nullptr);
}

TEST_CASE_METHOD(WizardWifiModalFixture,
                 "ESC on the password modal clears the step's cached pointer",
                 "[wizard][wifi][modal_lifetime][1579]") {
    WizardWifiStep& s = step();

    s.show_password_modal("TestNetwork");
    lv_obj_t* dialog = Access::password_modal(s);
    REQUIRE(dialog != nullptr);

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    uint32_t key = LV_KEY_ESC;
    lv_obj_send_event(backdrop, LV_EVENT_KEY, &key);
    process_lvgl(50);

    REQUIRE(Access::password_modal(s) == nullptr);
}

TEST_CASE_METHOD(WizardWifiModalFixture, "hide_password_modal still clears the cached pointer",
                 "[wizard][wifi][modal_lifetime][1579]") {
    WizardWifiStep& s = step();

    s.show_password_modal("TestNetwork");
    REQUIRE(Access::password_modal(s) != nullptr);

    s.hide_password_modal();
    process_lvgl(50);

    REQUIRE(Access::password_modal(s) == nullptr);
}

TEST_CASE_METHOD(WizardWifiModalFixture, "A dying modal clears only its own cached pointer",
                 "[wizard][wifi][modal_lifetime][1579]") {
    WizardWifiStep& s = step();

    lv_obj_t* live = lv_obj_create(lv_screen_active());
    lv_obj_t* stale = lv_obj_create(lv_screen_active());
    REQUIRE(live != nullptr);
    REQUIRE(stale != nullptr);
    Access::watch(s, live);
    Access::watch(s, stale);

    // The step is showing `live`; `stale` is a previous dialog on its way out.
    Access::password_modal(s) = live;
    lv_obj_delete(stale);
    process_lvgl(50);

    REQUIRE(Access::password_modal(s) == live);
}
