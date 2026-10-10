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

#include "ui_nav.h"
#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/snapshot_backdrops_mode.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "connection_state.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "platform_capabilities.h"
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
    void on_deactivating(DeactivateReason) override {
        ++deactivates;
    }

    lv_obj_t* widget = nullptr;
    int activates = 0;
    int deactivates = 0;
    bool visible_on_last_activate = false;
    bool navigate_to_home_on_activate = false;
};

/// Sets the platform_tier subject for one scope.
class ScopedTier {
  public:
    explicit ScopedTier(helix::PlatformTier tier)
        : subject_(lv_xml_get_subject(nullptr, "platform_tier")),
          saved_(lv_subject_get_int(subject_)) {
        lv_subject_set_int(subject_, static_cast<int>(tier));
    }
    ~ScopedTier() {
        lv_subject_set_int(subject_, saved_);
    }

  private:
    lv_subject_t* subject_;
    int saved_;
};

/// Unhidden children of the top layer, where the loading pill lives.
uint32_t shown_top_layer_children() {
    uint32_t shown = 0;
    lv_obj_t* layer = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_count(layer); ++i) {
        if (!lv_obj_has_flag(lv_obj_get_child(layer, i), LV_OBJ_FLAG_HIDDEN)) {
            ++shown;
        }
    }
    return shown;
}

class RecordingOverlay : public IPanelLifecycle {
  public:
    void on_activate() override {
        ++activates;
        top_layer_shown_on_activate = shown_top_layer_children();
    }
    void on_deactivate(DeactivateReason) override {
        ++deactivates;
    }
    const char* get_name() const override {
        return "RecordingOverlay";
    }
    bool is_destination() const override {
        return destination;
    }

    bool destination = false;
    int activates = 0;
    int deactivates = 0;
    uint32_t top_layer_shown_on_activate = 0;
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

TEST_CASE_METHOD(OverlayActivationFixture,
                 "set_active puts the new panel on screen before activating it",
                 "[navigation][lifecycle]") {
    auto& nav = NavigationManager::instance();
    controls_panel_.activates = 0;

    nav.set_active(PanelId::Controls);

    // Nothing drained: a switch that paints before it returns (the ESP32 loading
    // pill lifts with a forced refresh) must find the new panel already shown,
    // not wait for the queued active_panel observer to un-hide it.
    REQUIRE(controls_panel_.activates == 1);
    REQUIRE(controls_panel_.visible_on_last_activate);
    REQUIRE(lv_obj_has_flag(home_widget_, LV_OBJ_FLAG_HIDDEN));
}

namespace {
int count_of(const std::string& text, const std::string& needle) {
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

std::string last_line_with(const std::string& text, const std::string& needle) {
    const size_t at = text.rfind(needle);
    if (at == std::string::npos) {
        return {};
    }
    const size_t start = text.rfind('\n', at);
    const size_t end = text.find('\n', at);
    return text.substr(start == std::string::npos ? 0 : start + 1,
                       end == std::string::npos ? std::string::npos : end - start - 1);
}
} // namespace

// The trace is process-wide, so a line still pending from an earlier case may
// land first; the assertions read the last line and count deltas.
TEST_CASE_METHOD(OverlayActivationFixture,
                 "Switch trace names the frame after the switch, or says none rendered",
                 "[navigation][lifecycle][switch_trace]") {
    auto& nav = NavigationManager::instance();
    helix::TextLogCapture log;

    // No refresh between the two switches: the first one's line, written when
    // the second starts, must not report a frame.
    nav.set_active(PanelId::Controls);
    nav.set_active(PanelId::Home);
    const std::string no_frame = last_line_with(log.get_captured(), "Panel switch to 2 took");
    REQUIRE(no_frame.find("no frame rendered after the switch") != std::string::npos);
    REQUIRE(no_frame.find("next frame +") == std::string::npos);

    // A refresh after the switch is its frame: one line per switch, however
    // many switches have hooked the display before.
    const int lines_before = count_of(log.get_captured(), "Panel switch to 2 took");
    const int frames_before = count_of(log.get_captured(), "start-to-frame");
    nav.set_active(PanelId::Controls);
    lv_refr_now(nullptr);
    lv_refr_now(nullptr);
    REQUIRE(count_of(log.get_captured(), "Panel switch to 2 took") == lines_before + 1);
    REQUIRE(count_of(log.get_captured(), "start-to-frame") == frames_before + 1);
    REQUIRE(last_line_with(log.get_captured(), "Panel switch to 2 took").find("start-to-frame") !=
            std::string::npos);
}

TEST_CASE_METHOD(OverlayActivationFixture, "Switch trace splits each switch path into its phases",
                 "[navigation][lifecycle][switch_trace]") {
    auto& nav = NavigationManager::instance();
    helix::TextLogCapture log;

    nav.set_active(PanelId::Controls);
    lv_refr_now(nullptr);
    const std::string direct = last_line_with(log.get_captured(), "Panel switch to 2 took");
    for (const char* phase : {" build=", " deactivate=", " show=", " activate="}) {
        CAPTURE(phase, direct);
        REQUIRE(direct.find(phase) != std::string::npos);
    }

    // The navbar path clears overlays first and charges set_active's phases to
    // its own line.
    REQUIRE(nav.request_panel(PanelId::Home, NavigationManager::SwitchDispatch::Inline) ==
            NavigationManager::PanelRequest::Switched);
    nav.set_active(PanelId::Controls); // writes the Home switch's line
    const std::string navbar = last_line_with(log.get_captured(), "Panel switch to 0 took");
    for (const char* phase : {" build=", " overlays=", " deactivate=", " show=", " activate="}) {
        CAPTURE(phase, navbar);
        REQUIRE(navbar.find(phase) != std::string::npos);
    }
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
                 "Navbar tap during a close slide-out still runs that overlay's close callback",
                 "[navigation][overlay][lifecycle]") {
    auto& nav = NavigationManager::instance();
    // The close callback of a popped overlay waits for its slide-out, so this case
    // needs the real animations rather than the fixture's inline close path.
    DisplaySettingsManager::instance().set_animations_enabled(true);
    int closes = 0;
    nav.register_overlay_close_callback(overlay_, [&closes] { ++closes; });

    open_overlay();
    process_lvgl(500); // slide-in complete
    nav.go_back();
    drain(); // popped; the slide-out is animating and the callback waits for its end
    REQUIRE(closes == 0);

    NavigationManagerTestAccess::switch_to_panel(nav, PanelId::Controls);
    drain();
    process_lvgl(500); // deferred callbacks, then whatever is left of the slide-out
    CHECK(closes == 1);
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
// A queued push that is dismissed before it drains never shows
// ============================================================================

TEST_CASE_METHOD(OverlayActivationFixture, "go_back while a push is pending drops the push",
                 "[navigation][overlay][pending_push]") {
    auto& nav = NavigationManager::instance();

    nav.push_overlay(overlay_);
    nav.go_back();
    drain();

    CHECK(overlay_lifecycle_.activates == 0);
    // Owners release what show() primed in on_deactivate(), so a cancelled
    // open still closes.
    CHECK(overlay_lifecycle_.deactivates == 1);
    CHECK(home_panel_.deactivates == 0);
    CHECK_FALSE(nav.is_push_pending(overlay_));
    CHECK_FALSE(nav.has_open_overlays());
    CHECK(lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(OverlayActivationFixture, "go_back with a pending push still pops what is shown",
                 "[navigation][overlay][pending_push]") {
    auto& nav = NavigationManager::instance();
    open_overlay();

    // A duplicate push of a stacked overlay is ignored, so it must not swallow
    // the back that follows it.
    nav.push_overlay(overlay_);
    nav.go_back();
    drain();

    CHECK_FALSE(nav.is_push_pending(overlay_));
    CHECK_FALSE(nav.has_open_overlays());
}

TEST_CASE_METHOD(OverlayActivationFixture,
                 "close_overlay of a stacked overlay with a duplicate push pending closes it once",
                 "[navigation][overlay][pending_push]") {
    auto& nav = NavigationManager::instance();
    open_overlay();

    nav.push_overlay(overlay_);
    nav.close_overlay(overlay_);
    drain();

    REQUIRE(overlay_lifecycle_.deactivates == 1);
    CHECK_FALSE(nav.has_open_overlays());
}

TEST_CASE_METHOD(OverlayActivationFixture, "close_overlay while its push is pending drops the push",
                 "[navigation][overlay][pending_push]") {
    auto& nav = NavigationManager::instance();

    nav.push_overlay(overlay_);
    nav.close_overlay(overlay_);
    drain();

    CHECK(overlay_lifecycle_.activates == 0);
    // Owners release what show() primed in on_deactivate(), so a cancelled
    // open still closes.
    CHECK(overlay_lifecycle_.deactivates == 1);
    CHECK(home_panel_.deactivates == 0);
    CHECK_FALSE(nav.is_push_pending(overlay_));
    CHECK_FALSE(nav.has_open_overlays());
}

TEST_CASE_METHOD(OverlayActivationFixture, "Deleting a root with a pending push drops the push",
                 "[navigation][overlay][pending_push]") {
    auto& nav = NavigationManager::instance();

    nav.push_overlay(overlay_);
    lv_obj_t* deleted = overlay_;
    lv_obj_delete(overlay_);
    overlay_ = nullptr;

    // A new overlay allocated at the freed address must not read as already
    // opening, nor be pushed by the dead root's queued push.
    CHECK_FALSE(nav.is_push_pending(deleted));
    drain();
    CHECK(overlay_lifecycle_.activates == 0);
    CHECK_FALSE(nav.has_open_overlays());
}

TEST_CASE_METHOD(OverlayActivationFixture,
                 "An overlay built under the loading pill activates beneath it",
                 "[navigation][overlay][pending_push][loading_pill]") {
    ScopedTier tier(helix::PlatformTier::EMBEDDED);
    auto& nav = NavigationManager::instance();
    const uint32_t shown_before = shown_top_layer_children();

    int renders = 0;
    lv_display_t* disp = lv_display_get_default();
    lv_event_cb_t count_render = [](lv_event_t* e) {
        ++*static_cast<int*>(lv_event_get_user_data(e));
    };
    lv_display_add_event_cb(disp, count_render, LV_EVENT_RENDER_READY, &renders);

    uint32_t shown_in_build = 0;
    int renders_in_build = 0;
    helix::nav::build_under_loading_pill([&]() {
        shown_in_build = shown_top_layer_children();
        renders_in_build = renders;
        nav.push_overlay(overlay_);
    });
    lv_display_remove_event_cb_with_user_data(disp, count_render, &renders);

    // Painted before the build ran, not merely created.
    CHECK(shown_in_build == shown_before + 1);
    CHECK(renders_in_build > 0);

    drain();
    CHECK(overlay_lifecycle_.activates == 1);
    CHECK(overlay_lifecycle_.top_layer_shown_on_activate == shown_before + 1);
    CHECK(shown_top_layer_children() == shown_before);
}

TEST_CASE_METHOD(OverlayActivationFixture, "A push cancelled under the loading pill still lifts it",
                 "[navigation][overlay][pending_push][loading_pill]") {
    ScopedTier tier(helix::PlatformTier::BASIC);
    auto& nav = NavigationManager::instance();
    const uint32_t shown_before = shown_top_layer_children();

    helix::nav::build_under_loading_pill([&]() {
        nav.push_overlay(overlay_);
        nav.close_overlay(overlay_);
    });
    drain();

    CHECK(overlay_lifecycle_.activates == 0);
    CHECK_FALSE(nav.has_open_overlays());
    CHECK(shown_top_layer_children() == shown_before);
}

TEST_CASE_METHOD(OverlayActivationFixture,
                 "The loading pill stays off on the standard tier and under a switch's pill",
                 "[navigation][overlay][loading_pill]") {
    auto& nav = NavigationManager::instance();
    const uint32_t shown_before = shown_top_layer_children();
    uint32_t shown_in_build = 0;
    bool built = false;
    auto build = [&]() {
        built = true;
        shown_in_build = shown_top_layer_children();
    };

    SECTION("standard tier") {
        ScopedTier tier(helix::PlatformTier::STANDARD);
        nav.build_under_loading_pill(build);
    }
    SECTION("a panel switch's pill is already up") {
        ScopedTier tier(helix::PlatformTier::EMBEDDED);
        NavigationManagerTestAccess::set_nav_scrim_active(nav, true);
        nav.build_under_loading_pill(build);
        NavigationManagerTestAccess::set_nav_scrim_active(nav, false);
    }

    CHECK(built);
    CHECK(shown_in_build == shown_before);
    drain();
    CHECK(shown_top_layer_children() == shown_before);
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

        conn_ = get_printer_state().network_state().get_printer_connection_state_subject();
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

TEST_CASE_METHOD(OverlayActivationFixture,
                 "A cached overlay pushed after a navbar switch still gets on_deactivate",
                 "[navigation][overlay][lifecycle][1469]") {
    auto& nav = NavigationManager::instance();

    open_overlay();
    nav.go_back();
    drain();
    REQUIRE(overlay_lifecycle_.deactivates == 1);

    // A navbar tap tears the overlay stack down. The widget itself survives, so a
    // lazily created overlay is pushed again straight from its cache, with no
    // create() call and so no second register_overlay_instance().
    NavigationManagerTestAccess::switch_to_panel(nav, PanelId::Controls);
    drain();

    // Pushed without re-registering. The registration has to survive the switch,
    // or this push arrives unpaired: the fixture runs with
    // overlay_registration_strict() on, which aborts, and in production
    // on_deactivate() would simply never be called.
    nav.push_overlay(overlay_);
    drain();
    nav.go_back();
    drain();

    REQUIRE(overlay_lifecycle_.deactivates == 2);
}

TEST_CASE_METHOD(OverlayActivationFixture,
                 "Over a dim-layer backdrop only a transient overlay leaves the panel drawn",
                 "[navigation][backdrop][overlay]") {
    auto& nav = NavigationManager::instance();
    SnapshotBackdropsMode snapshots(false);

    SECTION("a transient overlay shows the panel through the dim layer") {
        open_overlay();
        lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
        REQUIRE(backdrop != nullptr);
        REQUIRE_FALSE(lv_obj_check_type(backdrop, &lv_image_class));
        REQUIRE(lv_obj_get_width(overlay_) < lv_obj_get_width(test_screen()));
        // Hidden, it would leave dimmed emptiness beside the narrow overlay.
        CHECK_FALSE(lv_obj_has_flag(home_widget_, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_index(backdrop) > lv_obj_get_index(home_widget_));
        CHECK(lv_obj_get_index(backdrop) < lv_obj_get_index(overlay_));
    }

    SECTION("a destination overlay (print status) hides the panel it covers") {
        overlay_lifecycle_.destination = true;
        open_overlay();
        REQUIRE_FALSE(
            lv_obj_check_type(NavigationManagerTestAccess::overlay_backdrop(nav), &lv_image_class));
        // Drawn under a full-width overlay, it would redraw for hours unseen.
        CHECK(lv_obj_has_flag(home_widget_, LV_OBJ_FLAG_HIDDEN));
    }

    nav.go_back();
    drain();
    CHECK_FALSE(lv_obj_has_flag(home_widget_, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(nav.has_open_overlays());
}

// A near-full-screen slide plus fade costs more per frame than the limited tiers
// can render, so they open and close overlays in their final state.
TEST_CASE_METHOD(OverlayActivationFixture, "Overlay slides follow the tier's style-effects rule",
                 "[navigation][overlay][modal_tier][platform_tier]") {
    auto& nav = NavigationManager::instance();
    DisplaySettingsManager::instance().set_animations_enabled(true);
    lv_subject_t* tier = lv_xml_get_subject(nullptr, "platform_tier");
    REQUIRE(tier != nullptr);
    struct RestoreTier {
        lv_subject_t* tier;
        int saved = lv_subject_get_int(tier);
        ~RestoreTier() {
            lv_subject_set_int(tier, saved);
        }
    } restore_tier{tier};
    int closes = 0;
    nav.register_overlay_close_callback(overlay_, [&closes] { ++closes; });

    SECTION("limited tier: no animation, close callback fires once") {
        lv_subject_set_int(tier, static_cast<int>(helix::PlatformTier::EMBEDDED));
        open_overlay();
        CHECK(lv_anim_get(overlay_, nullptr) == nullptr);
        CHECK(lv_obj_get_style_translate_x(overlay_, LV_PART_MAIN) == 0);
        CHECK(lv_obj_get_style_opa(overlay_, LV_PART_MAIN) == LV_OPA_COVER);

        nav.go_back();
        drain();
        CHECK(lv_anim_get(overlay_, nullptr) == nullptr);
        CHECK(lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(nav.has_open_overlays());
        process_lvgl(50); // the close callback runs on the next tick
        drain();
        CHECK(closes == 1);
        process_lvgl(500);
        CHECK(closes == 1);
    }

    SECTION("capable tier: slides in and out") {
        lv_subject_set_int(tier, static_cast<int>(helix::PlatformTier::STANDARD));
        open_overlay();
        CHECK(lv_anim_get(overlay_, nullptr) != nullptr);
        process_lvgl(500);
        nav.go_back();
        drain();
        CHECK(lv_anim_get(overlay_, nullptr) != nullptr);
        CHECK(closes == 0);
        process_lvgl(500);
        CHECK(closes == 1);
    }
}
