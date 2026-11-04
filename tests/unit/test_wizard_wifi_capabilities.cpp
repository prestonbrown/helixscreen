// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_wifi_capabilities.cpp
 * @brief The setup wizard offers the WiFi radio toggle only when the backend
 *        can move the radio (prestonbrown/helixscreen#1429).
 */

#include "ui_wizard_wifi.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wizard_wifi_test_access.h"
#include "core/update_queue.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using Access = WizardWifiStepTestAccess;

namespace {

/// A backend whose radio belongs to something else, the shape of a printer
/// whose network daemon manages WiFi.
class NoToggleBackend : public WifiBackendMock {
  public:
    bool supports_radio_toggle() const override {
        return false;
    }
};

class WizardCapabilityFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;
    WizardWifiStep step_;

    /// The wizard's WiFi screen, published against a manager on @p backend.
    lv_obj_t* toggle_for(std::unique_ptr<WifiBackend> backend) {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        auto manager = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
        manager->init_self_reference(manager);
        helix::ui::UpdateQueue::instance().drain();

        step_.init_subjects();
        REQUIRE(step_.create(test_screen()) != nullptr);
        Access::wifi_manager(step_) = manager;
        Access::publish_wifi_capabilities(step_);

        lv_obj_t* toggle = lv_obj_find_by_name(test_screen(), "wifi_toggle");
        REQUIRE(toggle != nullptr);
        return toggle;
    }
};

} // namespace

TEST_CASE_METHOD(WizardCapabilityFixture,
                 "The wizard offers the WiFi toggle when the radio can move", "[1429][wizard]") {
    CHECK_FALSE(
        lv_obj_has_flag(toggle_for(std::make_unique<WifiBackendMock>()), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(WizardCapabilityFixture,
                 "The wizard hides the WiFi toggle when the backend cannot move the radio",
                 "[1429][wizard]") {
    CHECK(lv_obj_has_flag(toggle_for(std::make_unique<NoToggleBackend>()), LV_OBJ_FLAG_HIDDEN));
}
