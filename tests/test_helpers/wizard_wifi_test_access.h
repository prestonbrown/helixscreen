// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_wizard_wifi.h"

/**
 * @brief Reaches the wizard WiFi step's cached password-modal pointer.
 *
 * The deferred connect-result callback null-checks this pointer and then walks
 * it with lv_obj_find_by_name(). "The pointer is null" is the whole invariant,
 * because a dangling pointer and a live one are indistinguishable at the call
 * site (prestonbrown/helixscreen#1579).
 */
class WizardWifiStepTestAccess {
  public:
    static lv_obj_t*& password_modal(WizardWifiStep& step) {
        return step.password_modal_;
    }
    /// Arm the production DELETE handler on an arbitrary object, exactly as
    /// show_password_modal() does.
    static void watch(WizardWifiStep& step, lv_obj_t* modal) {
        lv_obj_add_event_cb(modal, WizardWifiStep::on_modal_deleted, LV_EVENT_DELETE, &step);
    }
};
