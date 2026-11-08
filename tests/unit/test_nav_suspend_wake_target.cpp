// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_suspend_wake_target.cpp
 * @brief The screensaver wake reaches the view the suspend put to sleep
 *
 * A suspended view stays on the stack and wakes into on_activate(), so the
 * suspend reports DeactivateReason::Suspended: reported as NavigateAway, every
 * reason-aware on_deactivating() would cancel work that still has a UI to
 * report into.
 *
 * Navigation keeps running behind the screensaver: a queued push lands, an
 * overlay closes. Every view that becomes visible while the screen is asleep is
 * activated by that navigation and must be suspended again so it stops
 * drawing; the wake then resumes that one view, exactly once. A view that is
 * activated twice without a deactivation in between double-registers whatever
 * its on_activate() starts (prestonbrown/helixscreen#2062).
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Lifecycle bookkeeping shared by the panel and overlay stand-ins.
struct ActivationLog {
    int activations = 0;
    int wakes = 0;    ///< activations that ended a Suspended deactivation
    int suspends = 0; ///< Suspended deactivations
    int double_activations = 0;
    bool active = false;
    bool asleep = false;
    DeactivateReason last_reason = DeactivateReason::Shutdown;

    void activated() {
        if (active) {
            ++double_activations;
        }
        ++activations;
        if (asleep) {
            ++wakes;
        }
        active = true;
        asleep = false;
    }
    void deactivated(DeactivateReason reason) {
        active = false;
        last_reason = reason;
        asleep = reason == DeactivateReason::Suspended;
        if (asleep) {
            ++suspends;
        }
    }
    int fresh_activations() const {
        return activations - wakes;
    }
};

class LoggingPanel : public PanelBase {
  public:
    LoggingPanel() : PanelBase(get_printer_state(), nullptr) {}

    void init_subjects() override {}
    const char* get_name() const override {
        return "LoggingPanel";
    }
    const char* get_xml_component_name() const override {
        return "logging_panel";
    }
    void on_activate() override {
        log.activated();
    }
    void on_deactivating(DeactivateReason reason) override {
        log.deactivated(reason);
    }

    ActivationLog log;
};

class LoggingOverlay : public IPanelLifecycle {
  public:
    explicit LoggingOverlay(const char* name) : name_(name) {}

    void on_activate() override {
        log.activated();
    }
    void on_deactivate(DeactivateReason reason) override {
        log.deactivated(reason);
    }
    const char* get_name() const override {
        return name_;
    }

    ActivationLog log;

  private:
    const char* name_;
};

class SuspendWakeFixture : public LVGLUITestFixture {
  public:
    SuspendWakeFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        auto& nav = NavigationManager::instance();

        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
        panels[static_cast<int>(PanelId::Controls)] = lv_obj_create(test_screen());
        nav.set_panels(panels);
        nav.register_panel_instance(PanelId::Home, &home_);
        nav.register_panel_instance(PanelId::Controls, &controls_);

        overlay_a_ = make_overlay(&a_);
        overlay_b_ = make_overlay(&b_);
        overlay_bare_ = make_overlay(nullptr);
    }

    ~SuspendWakeFixture() override {
        auto& nav = NavigationManager::instance();
        // NavigationManager is a singleton and suspend_active() early-returns
        // while it is suspended, so a case that ends asleep would silently
        // no-op every later one.
        nav.resume_active();
        drain();
        while (nav.has_open_overlays()) {
            nav.go_back();
            drain();
        }
        nav.set_active(PanelId::Home);
        nav.register_panel_instance(PanelId::Home, nullptr);
        nav.register_panel_instance(PanelId::Controls, nullptr);
        nav.unregister_overlay_instance(overlay_bare_);
        if (overlay_a_) {
            nav.unregister_overlay_instance(overlay_a_);
        }
        if (overlay_b_) {
            nav.unregister_overlay_instance(overlay_b_);
        }
        drain();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    lv_obj_t* make_overlay(IPanelLifecycle* lifecycle) {
        lv_obj_t* w = lv_obj_create(test_screen());
        lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
        NavigationManager::instance().register_overlay_instance(w, lifecycle);
        return w;
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static void push(lv_obj_t* overlay) {
        NavigationManager::instance().push_overlay(overlay);
        drain();
    }

    static void suspend() {
        NavigationManager::instance().suspend_active();
        drain();
    }

    static void wake() {
        NavigationManager::instance().resume_active();
        drain();
    }

    lv_obj_t* overlay_a_ = nullptr;
    lv_obj_t* overlay_b_ = nullptr;
    lv_obj_t* overlay_bare_ = nullptr; ///< registered with no lifecycle
    LoggingPanel home_;
    LoggingPanel controls_;
    LoggingOverlay a_{"OverlayA"};
    LoggingOverlay b_{"OverlayB"};
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(SuspendWakeFixture, "A plain suspend and wake resumes the top view once",
                 "[nav][suspend_wake]") {
    SECTION("main panel") {
        suspend();
        REQUIRE(home_.log.suspends == 1);

        wake();
        REQUIRE(home_.log.wakes == 1);
        REQUIRE(home_.log.active);
        REQUIRE(home_.log.double_activations == 0);
    }

    SECTION("overlay") {
        push(overlay_a_);
        REQUIRE(a_.log.active);

        suspend();
        REQUIRE(a_.log.suspends == 1);

        wake();
        REQUIRE(a_.log.wakes == 1);
        REQUIRE(a_.log.activations == 2);
        REQUIRE(a_.log.double_activations == 0);
        // The panel beneath stays covered.
        REQUIRE_FALSE(home_.log.active);
    }
}

TEST_CASE_METHOD(SuspendWakeFixture,
                 "A view pushed while asleep is suspended and the wake resumes it once",
                 "[nav][suspend_wake]") {
    push(overlay_a_);
    suspend();

    push(overlay_b_);
    // The push activated it; the screen is still asleep, so it must stop drawing.
    REQUIRE(b_.log.fresh_activations() == 1);
    REQUIRE(b_.log.suspends == 1);
    REQUIRE_FALSE(b_.log.active);

    wake();
    REQUIRE(b_.log.wakes == 1);
    REQUIRE(b_.log.fresh_activations() == 1);
    REQUIRE(b_.log.double_activations == 0);
    REQUIRE(b_.log.active);

    // The view the suspend put to sleep is covered now: the push deactivated it
    // as a navigation, so it is not left suspended, and the wake leaves it alone.
    REQUIRE(a_.log.wakes == 0);
    REQUIRE_FALSE(a_.log.asleep);
    REQUIRE_FALSE(a_.log.active);
}

TEST_CASE_METHOD(SuspendWakeFixture, "A covered view uncovered while asleep wakes once",
                 "[nav][suspend_wake]") {
    push(overlay_a_);
    suspend();
    push(overlay_b_);

    NavigationManager::instance().go_back();
    drain();
    // Restored by the close, then put back to sleep.
    REQUIRE(a_.log.fresh_activations() == 2);
    REQUIRE_FALSE(a_.log.active);
    REQUIRE(a_.log.asleep);

    wake();
    REQUIRE(a_.log.wakes == 1);
    REQUIRE(a_.log.active);
    REQUIRE(a_.log.double_activations == 0);
    // B was closed while asleep and has nothing to wake into.
    REQUIRE(b_.log.wakes == 0);
    REQUIRE_FALSE(b_.log.active);
}

TEST_CASE_METHOD(SuspendWakeFixture,
                 "The suspended view closed onto a lifecycle-less overlay is not woken",
                 "[nav][suspend_wake]") {
    push(overlay_bare_);
    push(overlay_a_);
    suspend();
    const int a_activations = a_.log.activations;

    NavigationManager::instance().go_back();
    drain();
    wake();
    REQUIRE(a_.log.activations == a_activations);
    REQUIRE_FALSE(a_.log.active);
}

TEST_CASE_METHOD(SuspendWakeFixture, "A panel switched in under the suspended overlay stays put",
                 "[nav][suspend_wake]") {
    // The connection-change path swaps the base panel beneath an open overlay.
    push(overlay_a_);
    suspend();

    NavigationManager::instance().set_active(PanelId::Controls);
    drain();
    REQUIRE(controls_.log.suspends == 0);

    wake();
    REQUIRE(a_.log.wakes == 1);
    REQUIRE(a_.log.active);
}

TEST_CASE_METHOD(SuspendWakeFixture, "The suspended view closed while asleep is not woken",
                 "[nav][suspend_wake]") {
    push(overlay_a_);
    suspend();
    const int a_activations = a_.log.activations;

    NavigationManager::instance().go_back();
    drain();
    REQUIRE_FALSE(a_.log.active);
    // The close restored Home; asleep, it is suspended again rather than drawing.
    REQUIRE(home_.log.suspends == 1);
    REQUIRE_FALSE(home_.log.active);

    wake();
    REQUIRE(a_.log.activations == a_activations);
    REQUIRE(home_.log.wakes == 1);
    REQUIRE(home_.log.active);
    REQUIRE(home_.log.double_activations == 0);
}

TEST_CASE_METHOD(SuspendWakeFixture, "Deleting the suspended view's widget forgets the record",
                 "[nav][suspend_wake]") {
    auto& nav = NavigationManager::instance();
    push(overlay_a_);
    suspend();
    REQUIRE(nav.suspended_view() == &a_);

    SECTION("widget deleted") {
        lv_obj_delete(overlay_a_);
        overlay_a_ = nullptr;
    }
    SECTION("instance unregistered") {
        nav.unregister_overlay_instance(overlay_a_);
    }
    REQUIRE(nav.suspended_view() == nullptr);

    const int a_activations = a_.log.activations;
    wake();
    REQUIRE(a_.log.activations == a_activations);
}

TEST_CASE_METHOD(SuspendWakeFixture, "Replacing the suspended panel instance forgets the record",
                 "[nav][suspend_wake]") {
    auto& nav = NavigationManager::instance();
    suspend();
    REQUIRE(nav.suspended_view() == &home_);

    nav.register_panel_instance(PanelId::Home, nullptr);
    REQUIRE(nav.suspended_view() == nullptr);
    nav.register_panel_instance(PanelId::Home, &home_);
}

TEST_CASE_METHOD(SuspendWakeFixture, "Navigating away still reports NavigateAway",
                 "[nav][suspend_wake]") {
    // A deliberate navigation is still the case where work should stop.
    NavigationManager::instance().set_active(PanelId::Controls);
    drain();

    REQUIRE(home_.log.last_reason == DeactivateReason::NavigateAway);
    REQUIRE_FALSE(home_.log.asleep);
}
