// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_suspend_deactivate_reason.cpp
 * @brief suspend_active() reports a suspension, not a navigation
 *
 * A view suspended for the screensaver, display sleep or app backgrounding
 * stays on the stack and is reactivated on the next wake. Reporting that as
 * NavigateAway makes every reason-aware on_deactivating() behave as if the user
 * walked away, which cancels work that still has a UI to report into.
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "printer_state.h"

#include <optional>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Records the reason it was last deactivated with.
class ReasonRecordingPanel : public PanelBase {
  public:
    ReasonRecordingPanel() : PanelBase(get_printer_state(), nullptr) {}

    void init_subjects() override {}
    const char* get_name() const override {
        return "ReasonRecordingPanel";
    }
    const char* get_xml_component_name() const override {
        return "reason_recording_panel";
    }
    void on_deactivating(DeactivateReason reason) override {
        last_reason = reason;
    }

    std::optional<DeactivateReason> last_reason;
};

class ReasonRecordingOverlay : public IPanelLifecycle {
  public:
    void on_activate() override {}
    void on_deactivate(DeactivateReason reason) override {
        last_reason = reason;
    }
    const char* get_name() const override {
        return "ReasonRecordingOverlay";
    }

    std::optional<DeactivateReason> last_reason;
};

class SuspendReasonFixture : public LVGLUITestFixture {
  public:
    SuspendReasonFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        auto& nav = NavigationManager::instance();

        home_widget_ = lv_obj_create(test_screen());
        controls_widget_ = lv_obj_create(test_screen());

        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        panels[static_cast<int>(PanelId::Controls)] = controls_widget_;
        nav.set_panels(panels);

        nav.register_panel_instance(PanelId::Home, &home_panel_);
        nav.register_panel_instance(PanelId::Controls, &controls_panel_);

        overlay_ = lv_obj_create(test_screen());
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        nav.register_overlay_instance(overlay_, &overlay_lifecycle_);
    }

    ~SuspendReasonFixture() override {
        auto& nav = NavigationManager::instance();
        // NavigationManager is a singleton and suspend_active() early-returns
        // while suspended_ is set, so a case that suspends would silently
        // no-op every later one. Clear it while this fixture's panels are still
        // the registered ones.
        nav.resume_active();
        drain();
        nav.register_panel_instance(PanelId::Home, nullptr);
        nav.register_panel_instance(PanelId::Controls, nullptr);
        nav.unregister_overlay_instance(overlay_);
        drain();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    lv_obj_t* home_widget_ = nullptr;
    lv_obj_t* controls_widget_ = nullptr;
    lv_obj_t* overlay_ = nullptr;
    ReasonRecordingPanel home_panel_;
    ReasonRecordingPanel controls_panel_;
    ReasonRecordingOverlay overlay_lifecycle_;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(SuspendReasonFixture, "Suspending the active panel reports Suspended",
                 "[suspend_reason][lifecycle]") {
    auto& nav = NavigationManager::instance();

    nav.suspend_active();
    drain();

    REQUIRE(home_panel_.last_reason.has_value());
    REQUIRE(*home_panel_.last_reason == DeactivateReason::Suspended);
}

TEST_CASE_METHOD(SuspendReasonFixture, "Suspending reports Suspended to the topmost overlay",
                 "[suspend_reason][lifecycle]") {
    auto& nav = NavigationManager::instance();

    nav.push_overlay(overlay_);
    drain();
    // The overlay branch of suspend_active() only runs with a stacked overlay.
    REQUIRE(NavigationManagerTestAccess::panel_stack(nav).size() > 1);
    overlay_lifecycle_.last_reason.reset();

    nav.suspend_active();
    drain();

    REQUIRE(overlay_lifecycle_.last_reason.has_value());
    REQUIRE(*overlay_lifecycle_.last_reason == DeactivateReason::Suspended);
}

TEST_CASE_METHOD(SuspendReasonFixture, "Navigating away still reports NavigateAway",
                 "[suspend_reason][lifecycle]") {
    // The new reason must not swallow the real one: a deliberate navigation is
    // still the case where work should stop.
    auto& nav = NavigationManager::instance();

    nav.set_active(PanelId::Controls);
    drain();

    REQUIRE(home_panel_.last_reason.has_value());
    REQUIRE(*home_panel_.last_reason == DeactivateReason::NavigateAway);
}
