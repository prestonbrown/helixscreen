// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_overlay_activation.cpp
 * @brief Overlay-close activation and expected-disconnect latches (#1245)
 *
 * Two latches in NavigationManager, both of which used to be inferred from state
 * that other code paths rewrite:
 *
 * 1. Closing an overlay must activate the panel it restored EXACTLY ONCE, and
 *    only after that panel has been un-hidden. Four call sites used to race for
 *    it (animation-complete callback, the two animations-disabled paths, and
 *    go_back's own synchronous call), so a close with animations off activated
 *    twice — enough to trip PrintSelectPanel's Print-Last activation counter and
 *    to start FirstRunTour twice. Activating before the un-hide is worse: an
 *    on_activate() that navigates (PrintSelectPanel calls set_active(Home)) had
 *    its work undone by go_back's own lv_obj_remove_flag two lines later.
 *
 * 2. mark_disconnect_expected() must survive a still-undrained CONNECTED apply.
 *    It used to spoof previous_connection_state_, which every deferred
 *    connection handler overwrites — so a CONNECTED callback queued before the
 *    app backgrounded restored was_connected==true and the synthetic disconnect
 *    behind it cleared the overlay stack and bounced the user to Home.
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "connection_state.h"
#include "display_settings_manager.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Main-panel stand-in. Records how many times the close path activated it and
/// whether its widget was actually on screen at that moment — the two things
/// go_back has to get right.
class RecordingPanel : public PanelBase {
  public:
    RecordingPanel() : PanelBase(get_printer_state(), nullptr) {}

    void init_subjects() override {}
    const char* get_name() const override {
        return "RecordingPanel";
    }
    const char* get_xml_component_name() const override {
        return "recording_panel";
    }

    void on_activate() override {
        ++activates;
        if (widget) {
            visible_on_last_activate = !lv_obj_has_flag(widget, LV_OBJ_FLAG_HIDDEN);
        }
        if (navigate_to_home_on_activate) {
            // One-shot, mirroring PrintSelectPanel's Print-Last flow: on_activate
            // decides the panel is done and hands control to Home.
            navigate_to_home_on_activate = false;
            NavigationManager::instance().set_active(PanelId::Home);
        }
    }
    void on_deactivate() override {
        ++deactivates;
    }

    lv_obj_t* widget = nullptr;
    int activates = 0;
    int deactivates = 0;
    bool visible_on_last_activate = false;
    bool navigate_to_home_on_activate = false;
};

class RecordingOverlay : public IPanelLifecycle {
  public:
    void on_activate() override {
        ++activates;
    }
    void on_deactivate() override {
        ++deactivates;
    }
    const char* get_name() const override {
        return "RecordingOverlay";
    }

    int activates = 0;
    int deactivates = 0;
};

/**
 * @brief NavigationManager seeded the way the running app has it
 *
 * Two real main-panel widgets in the widget array, matching panel instances
 * registered, and panel_stack_[0] holding the active one (set_panels does that).
 * Animations are turned OFF for the whole fixture: that is the deterministic
 * path — the no-animation branch runs inline inside go_back, where a
 * double-activation shows up in the same drain instead of an lv_timer tick later.
 */
class OverlayActivationFixture : public LVGLUITestFixture {
  public:
    OverlayActivationFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        auto& nav = NavigationManager::instance();

        home_widget_ = lv_obj_create(test_screen());
        controls_widget_ = lv_obj_create(test_screen());

        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        panels[static_cast<int>(PanelId::Controls)] = controls_widget_;
        nav.set_panels(panels); // active is Home by default → stack = [home_widget_]

        home_panel_.widget = home_widget_;
        controls_panel_.widget = controls_widget_;
        nav.register_panel_instance(PanelId::Home, &home_panel_);
        nav.register_panel_instance(PanelId::Controls, &controls_panel_);

        overlay_ = lv_obj_create(test_screen());
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        nav.register_overlay_instance(overlay_, &overlay_lifecycle_);
    }

    ~OverlayActivationFixture() override {
        auto& nav = NavigationManager::instance();
        // Drop the registrations BEFORE the mock objects die: the base fixture's
        // cleanup still walks NavigationManager, and the panel slots are raw
        // pointers into this fixture.
        nav.register_panel_instance(PanelId::Home, nullptr);
        nav.register_panel_instance(PanelId::Controls, nullptr);
        nav.unregister_overlay_instance(overlay_);
        drain();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Open the overlay over whatever main panel is active, and settle.
    void open_overlay() {
        NavigationManager::instance().push_overlay(overlay_);
        drain();
    }

    lv_obj_t* home_widget_ = nullptr;
    lv_obj_t* controls_widget_ = nullptr;
    lv_obj_t* overlay_ = nullptr;
    RecordingPanel home_panel_;
    RecordingPanel controls_panel_;
    RecordingOverlay overlay_lifecycle_;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(OverlayActivationFixture, "Overlay close activates the main panel exactly once",
                 "[navigation][overlay][lifecycle][1245]") {
    auto& nav = NavigationManager::instance();

    open_overlay();
    REQUIRE(nav.has_open_overlays());
    // Premise: pushing the overlay hid the panel underneath it.
    REQUIRE(lv_obj_has_flag(home_widget_, LV_OBJ_FLAG_HIDDEN));

    home_panel_.activates = 0;
    home_panel_.visible_on_last_activate = false;

    nav.go_back();
    drain();

    // Exactly once. Before the latch, the no-animation close path activated
    // here AND go_back activated again a few lines later — two on_activate()
    // calls for one close, which is what tripped PrintSelectPanel's
    // return_home_activation_count_ safety timeout and double-started the tour.
    REQUIRE(home_panel_.activates == 1);

    // And late enough to matter: the restored panel must already be un-hidden
    // when it is told it is active, or anything on_activate() does to the
    // visible panel stack is undone by go_back's own un-hide right after.
    REQUIRE(home_panel_.visible_on_last_activate);
    REQUIRE_FALSE(lv_obj_has_flag(home_widget_, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(nav.has_open_overlays());
}

TEST_CASE_METHOD(OverlayActivationFixture,
                 "Overlay close activates the restored overlay exactly once",
                 "[navigation][overlay][lifecycle][1245]") {
    auto& nav = NavigationManager::instance();

    open_overlay();

    lv_obj_t* second = lv_obj_create(test_screen());
    lv_obj_add_flag(second, LV_OBJ_FLAG_HIDDEN);
    RecordingOverlay second_lifecycle;
    nav.register_overlay_instance(second, &second_lifecycle);
    nav.push_overlay(second);
    drain();

    overlay_lifecycle_.activates = 0;

    nav.go_back(); // pops `second`, restoring overlay_
    drain();

    REQUIRE(overlay_lifecycle_.activates == 1);
    REQUIRE(nav.has_open_overlays()); // still one overlay deep

    nav.unregister_overlay_instance(second);
    drain();
    lv_obj_delete(second);
}

TEST_CASE_METHOD(OverlayActivationFixture,
                 "Navigation from on_activate survives the overlay close that triggered it",
                 "[navigation][overlay][lifecycle][1245]") {
    auto& nav = NavigationManager::instance();

    nav.set_active(PanelId::Controls);
    drain();
    REQUIRE(nav.get_active() == PanelId::Controls);

    open_overlay();
    REQUIRE(lv_obj_has_flag(controls_widget_, LV_OBJ_FLAG_HIDDEN));

    // The Print-Last shape: the panel beneath the overlay decides, from its own
    // on_activate(), that it is finished and hands off to Home.
    controls_panel_.navigate_to_home_on_activate = true;
    controls_panel_.activates = 0;
    home_panel_.activates = 0;

    nav.go_back();
    drain();

    REQUIRE(controls_panel_.activates == 1);
    REQUIRE(nav.get_active() == PanelId::Home);
    REQUIRE(nav.is_panel_in_stack(home_widget_));
    REQUIRE_FALSE(lv_obj_has_flag(home_widget_, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_flag(controls_widget_, LV_OBJ_FLAG_HIDDEN));
}

// ============================================================================
// Navbar close path — the other way an overlay goes away
// ============================================================================

TEST_CASE_METHOD(OverlayActivationFixture,
                 "Navbar tap onto the already-active panel re-activates it",
                 "[navigation][overlay][lifecycle]") {
    auto& nav = NavigationManager::instance();

    open_overlay();
    REQUIRE(nav.has_open_overlays());
    // Premise: push_overlay() deactivated the panel underneath, and left
    // active_panel_ pointing at it. That is the state the navbar path inherits.
    REQUIRE(home_panel_.deactivates == 1);
    REQUIRE(nav.get_active() == PanelId::Home);

    home_panel_.activates = 0;
    home_panel_.visible_on_last_activate = false;

    // Tap the navbar button for the panel we are ALREADY on. The handler's
    // "already here, do nothing" guard does not fire while an overlay is open,
    // so this reaches switch_to_panel_impl — which clears the overlay, un-hides
    // the panel, then calls set_active(Home). set_active short-circuits on
    // panel_id == active_panel_, so nothing else is left to re-activate it.
    NavigationManagerTestAccess::switch_to_panel(nav, PanelId::Home);
    drain();

    REQUIRE_FALSE(nav.has_open_overlays());
    REQUIRE_FALSE(lv_obj_has_flag(home_widget_, LV_OBJ_FLAG_HIDDEN));

    // The panel is back on screen, so it must have been told it is active
    // again. Without this, it stays visible-but-deactivated forever and
    // anything on_activate() restarts — CameraWidget::start_stream() — never
    // runs, so the widget renders blank until the user visits another panel
    // and comes back.
    REQUIRE(home_panel_.activates == 1);
    REQUIRE(home_panel_.visible_on_last_activate);
}

TEST_CASE_METHOD(OverlayActivationFixture,
                 "Navbar tap onto a different panel activates only the target",
                 "[navigation][overlay][lifecycle]") {
    auto& nav = NavigationManager::instance();

    open_overlay();
    home_panel_.activates = 0;
    controls_panel_.activates = 0;

    // The panel id genuinely changes here, so set_active() does the activation
    // itself. Re-activating the restored panel must not double up on that.
    NavigationManagerTestAccess::switch_to_panel(nav, PanelId::Controls);
    drain();

    REQUIRE(nav.get_active() == PanelId::Controls);
    REQUIRE(controls_panel_.activates == 1);
    REQUIRE(home_panel_.activates == 0);
    REQUIRE_FALSE(nav.has_open_overlays());
}

TEST_CASE_METHOD(OverlayActivationFixture,
                 "Navbar tap does not re-activate a panel set_active already activated",
                 "[navigation][overlay][lifecycle]") {
    auto& nav = NavigationManager::instance();

    open_overlay();

    // The connection-change shape: set_active() runs while an overlay is still
    // up. It rebases the stack and activates Controls underneath the overlay,
    // so by the time the overlay goes away Controls is already active.
    nav.set_active(PanelId::Controls);
    drain();
    REQUIRE(controls_panel_.activates == 1);
    REQUIRE(nav.has_open_overlays());

    // Now tap the navbar button for Controls — the panel we are already on.
    // The re-activation is NOT owed here; paying it anyway would activate
    // Controls twice for one close, which is what tripped PrintSelectPanel's
    // Print-Last counter and double-started FirstRunTour before.
    NavigationManagerTestAccess::switch_to_panel(nav, PanelId::Controls);
    drain();

    REQUIRE_FALSE(nav.has_open_overlays());
    REQUIRE_FALSE(lv_obj_has_flag(controls_widget_, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(controls_panel_.activates == 1);
}

// ============================================================================
// mark_disconnect_expected() — one-shot, immune to callback ordering
// ============================================================================

namespace {

/// Adds the connection-state observer that only wire_events() installs. A bare
/// widget is enough: wire_events looks its nav buttons up by name and skips the
/// ones it cannot find, then registers the observers unconditionally.
class ConnectionGatingFixture : public OverlayActivationFixture {
  public:
    ConnectionGatingFixture() {
        fake_navbar_ = lv_obj_create(test_screen());
        NavigationManager::instance().wire_events(fake_navbar_);

        conn_ = get_printer_state().get_printer_connection_state_subject();
        REQUIRE(conn_ != nullptr);

        // Known-not-connected starting point, fully drained.
        lv_subject_set_int(conn_, static_cast<int>(ConnectionState::DISCONNECTED));
        drain();

        NavigationManager::instance().set_active(PanelId::Controls);
        drain();
    }

    void connect_undrained() {
        lv_subject_set_int(conn_, static_cast<int>(ConnectionState::CONNECTED));
    }
    void disconnect_undrained() {
        lv_subject_set_int(conn_, static_cast<int>(ConnectionState::DISCONNECTED));
    }

    lv_obj_t* fake_navbar_ = nullptr;
    lv_subject_t* conn_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(ConnectionGatingFixture, "Expected disconnect survives an undrained CONNECTED",
                 "[navigation][connection][overlay][1245]") {
    auto& nav = NavigationManager::instance();

    open_overlay();
    REQUIRE(nav.has_open_overlays());

    // Exactly the resume ordering from #1245: a CONNECTED notification is still
    // sitting in the queue when the app backgrounds, and the synthetic
    // DISCONNECTED is queued behind it. Both drain together on resume.
    connect_undrained();
    nav.mark_disconnect_expected();
    disconnect_undrained();
    drain();

    // The CONNECTED apply rewrote previous_connection_state_ on its way through,
    // so a latch stored there would already be gone by the time the disconnect
    // lands — that is the bug. The one-shot is consumed on the falling edge.
    REQUIRE(nav.get_active() == PanelId::Controls);
    REQUIRE(nav.has_open_overlays());
}

TEST_CASE_METHOD(ConnectionGatingFixture, "Unexpected disconnect still clears overlays and homes",
                 "[navigation][connection][overlay][1245]") {
    auto& nav = NavigationManager::instance();

    open_overlay();
    REQUIRE(nav.has_open_overlays());

    connect_undrained();
    drain();
    disconnect_undrained();
    drain();

    // No mark_disconnect_expected(): the gating must still fire. Without this
    // case, "always ignore the disconnect" would pass the test above.
    REQUIRE(nav.get_active() == PanelId::Home);
    REQUIRE_FALSE(nav.has_open_overlays());
}

TEST_CASE_METHOD(ConnectionGatingFixture, "Expected-disconnect latch is consumed, not sticky",
                 "[navigation][connection][overlay][1245]") {
    auto& nav = NavigationManager::instance();

    connect_undrained();
    nav.mark_disconnect_expected();
    disconnect_undrained();
    drain();
    REQUIRE(nav.get_active() == PanelId::Controls);

    // Second round, no new mark: the latch must have been spent by the first
    // falling edge, so this disconnect is treated as real.
    nav.set_active(PanelId::Controls);
    open_overlay();
    connect_undrained();
    drain();
    disconnect_undrained();
    drain();

    REQUIRE(nav.get_active() == PanelId::Home);
    REQUIRE_FALSE(nav.has_open_overlays());
}
