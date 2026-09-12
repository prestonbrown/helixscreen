// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_wifi_teardown_invalidates.cpp
 * @brief cleanup() must expire the step's outstanding async work.
 *
 * Six async results in WizardWifiStep run on the main thread long after the
 * click that started them, writing subjects and walking widgets. Nothing in
 * those bodies re-checks that the step is still on screen; what makes them safe
 * is that cleanup() invalidates the lifetime guard, so LifetimeToken::defer
 * drops a body whose generation has moved on — at enqueue, and again at
 * dispatch.
 *
 * That is a property every one of those bodies depends on and none of them
 * states. This pins it at the connect result, which is the longest-lived of
 * them: a join can be in flight for the whole time a user needs to leave the
 * step.
 */

#include "ui_update_queue.h"
#include "ui_wizard_wifi.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wizard_wifi_test_access.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using Access = WizardWifiStepTestAccess;

namespace {

/// An SSID no mock backend stocks, so connect_network() refuses it
/// synchronously and the step's deferred failure body is queued on the spot —
/// an in-flight join with no thread and no wait.
constexpr const char* kUnknownSsid = "NoSuchNetwork";

class WizardWifiTeardownFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;

    /// A step with its screen up and a mock-backed manager, the state a user
    /// tapping Connect is in.
    WizardWifiStep& live_step() {
        if (!started_) {
            step_.init_subjects();
            step_.register_callbacks();
            REQUIRE(step_.create(test_screen()) != nullptr);

            get_runtime_config()->test_mode = true;
            get_runtime_config()->use_real_wifi = false;
            auto backend = std::make_unique<WifiBackendMock>();
            REQUIRE(backend->start().success());
            manager_ = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
            manager_->init_self_reference(manager_);
            Access::wifi_manager(step_) = manager_;

            Access::set_current_ssid(step_, kUnknownSsid);
            Access::password_modal(step_) = make_modal();
            started_ = true;
        }
        return step_;
    }

    /// Stands in for the XML dialog: the two widgets the connect handler looks
    /// up by name, and nothing else.
    lv_obj_t* make_modal() {
        lv_obj_t* modal = lv_obj_create(test_screen());
        REQUIRE(modal != nullptr);
        lv_obj_t* input = lv_textarea_create(modal);
        lv_obj_set_name(input, "password_input");
        lv_textarea_set_text(input, "some-password"); // the handler refuses an empty one
        lv_obj_t* status = lv_label_create(modal);
        lv_obj_set_name(status, "modal_status");
        lv_label_set_text(status, "");
        return modal;
    }

    /// 1 from the moment Connect is clicked until the deferred result clears
    /// it, so it reads "the body has not run yet" and nothing else writes it.
    static int connecting(WizardWifiStep& step) {
        return lv_subject_get_int(&Access::wifi_connecting(step));
    }

  private:
    WizardWifiStep step_;
    std::shared_ptr<helix::WiFiManager> manager_;
    bool started_ = false;
};

} // namespace

TEST_CASE_METHOD(WizardWifiTeardownFixture, "cleanup() expires a connect result still in flight",
                 "[wizard][wifi][threading]") {
    WizardWifiStep& s = live_step();

    // A live step first: the same click, the same queued body, no teardown.
    // Without this half the assertion below would also hold against a join that
    // never answered or a body that was never queued.
    Access::password_connect_clicked(s);
    REQUIRE(connecting(s) == 1); // queued, not yet run
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(connecting(s) == 0); // the deferred body ran and cleared it

    // Now the same sequence with the step torn down between the click and the
    // queue tick that would deliver the result.
    Access::password_connect_clicked(s);
    REQUIRE(connecting(s) == 1);

    s.cleanup();
    helix::ui::UpdateQueue::instance().drain();

    // Still 1: the body's first statement never ran. Delete
    // lifetime_.invalidate() from cleanup() and this reads 0.
    CHECK(connecting(s) == 1);

    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
}
