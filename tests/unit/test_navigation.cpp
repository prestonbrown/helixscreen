// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../../include/theme_manager.h"
#include "../../include/ui_nav_manager.h"
#include "../helix_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

// Test fixture for navigation tests.
//
// Derives from HelixTestFixture so the UpdateQueue is drained on the way out:
// panel switching notifies subjects, and observe_int_sync defers every apply,
// so a hand-rolled fixture returns with those applies still queued and hands
// them to whichever test runs next (tests/CLAUDE.md, "inherit, don't hand-roll").
class NavigationTestFixture : public HelixTestFixture {
  public:
    NavigationTestFixture() {
        // Initialize LVGL for testing (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Create a display for testing (headless)
        // LVGL 9 requires aligned buffers - use alignas(64) for portability
        lv_display_t* disp = lv_display_create(800, 480);
        alignas(64) static lv_color_t buf1[800 * 10];
        lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

        // Initialize navigation system
        NavigationManager::instance().init();
    }

    ~NavigationTestFixture() override {
        // Drain BEFORE deinit_subjects(), while the subjects those deferred
        // applies read are still alive. The base destructor drains again, but
        // that one runs after teardown — too late to be the only drain.
        helix::ui::UpdateQueue::instance().drain();

        // Reset singleton state so it doesn't leak between test cases.
        // deinit_subjects() clears observers, panel/overlay tracking,
        // and resets subjects_initialized_ so init() works in the next fixture.
        NavigationManager::instance().deinit_subjects();
    }
};

TEST_CASE_METHOD(NavigationTestFixture, "Navigation initialization", "[core][navigation]") {
    SECTION("Default active panel is HOME") {
        REQUIRE(NavigationManager::instance().get_active() == PanelId::Home);
    }
}

TEST_CASE_METHOD(NavigationTestFixture, "Panel switching", "[core][navigation]") {
    SECTION("Switch to CONTROLS panel") {
        NavigationManager::instance().set_active(PanelId::Controls);
        REQUIRE(NavigationManager::instance().get_active() == PanelId::Controls);
    }

    SECTION("Switch to FILAMENT panel") {
        NavigationManager::instance().set_active(PanelId::Filament);
        REQUIRE(NavigationManager::instance().get_active() == PanelId::Filament);
    }

    SECTION("Switch to SETTINGS panel") {
        NavigationManager::instance().set_active(PanelId::Settings);
        REQUIRE(NavigationManager::instance().get_active() == PanelId::Settings);
    }

    SECTION("Switch to ADVANCED panel") {
        NavigationManager::instance().set_active(PanelId::Advanced);
        REQUIRE(NavigationManager::instance().get_active() == PanelId::Advanced);
    }

    SECTION("Switch back to HOME panel") {
        NavigationManager::instance().set_active(PanelId::Controls);
        NavigationManager::instance().set_active(PanelId::Home);
        REQUIRE(NavigationManager::instance().get_active() == PanelId::Home);
    }
}

TEST_CASE_METHOD(NavigationTestFixture, "Invalid panel handling", "[core][navigation]") {
    SECTION("Setting invalid panel ID does not change active panel") {
        PanelId original = NavigationManager::instance().get_active();
        NavigationManager::instance().set_active((PanelId)99); // Invalid panel ID
        REQUIRE(NavigationManager::instance().get_active() == original);
    }
}

TEST_CASE_METHOD(NavigationTestFixture, "Repeated panel selection", "[core][navigation]") {
    SECTION("Setting same panel multiple times is safe") {
        NavigationManager::instance().set_active(PanelId::Controls);
        NavigationManager::instance().set_active(PanelId::Controls);
        NavigationManager::instance().set_active(PanelId::Controls);
        REQUIRE(NavigationManager::instance().get_active() == PanelId::Controls);
    }
}

TEST_CASE_METHOD(NavigationTestFixture, "All panels are accessible", "[core][navigation]") {
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        NavigationManager::instance().set_active((PanelId)i);
        REQUIRE(NavigationManager::instance().get_active() == (PanelId)i);
    }
}

// ============================================================================
// L081 Mech D defense: indev reset primitive
// ============================================================================
// The nav teardown paths (go_back, switch_to_panel_impl, clear_overlay_stack)
// each call lv_indev_reset(nullptr, nullptr) before destroying widgets, so
// any in-flight pointer event can't dispatch to memory we're about to free.
// This is the per-widget event_dsc cb-slot corruption family (Mech D / bundle
// 3XNZQB2R / #937). Integration is verified by code review + on-device test
// per [L060]; this case pins the LVGL primitive's behavior we rely on so a
// future LVGL upgrade can't silently regress the invariant.

#include "../../lib/lvgl/src/indev/lv_indev_private.h"

TEST_CASE_METHOD(NavigationTestFixture, "lv_indev_reset(NULL, NULL) clears in-flight press",
                 "[core][navigation][indev][l081]") {
    lv_indev_t* indev = lv_indev_create();
    REQUIRE(indev != nullptr);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    lv_obj_t* widget = lv_obj_create(lv_screen_active());
    REQUIRE(widget != nullptr);

    // Stage a press in flight: indev tracks widget as the active obj.
    indev->pointer.act_obj = widget;
    indev->pointer.last_pressed = widget;

    // The exact call shape used by go_back / switch_to_panel_impl / clear_overlay_stack.
    lv_indev_reset(nullptr, nullptr);

    // Invariant the nav paths depend on: act_obj/last_pressed are nulled, so
    // the next indev tick can't dispatch LV_EVENT_* to a freed widget.
    REQUIRE(indev->pointer.act_obj == nullptr);
    REQUIRE(indev->pointer.last_pressed == nullptr);

    lv_obj_delete(widget);
}

// ============================================================================
// Navbar Icon Visibility Tests (XML Integration)
// ============================================================================
// These tests verify that navbar icons show/hide correctly based on
// connection state and klippy state. They require full XML registration.

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"

/// Puts an int subject back the way it was found, on every exit path. Catch2
/// aborts a case at its first failed REQUIRE, so a restore written as the last
/// statement of the case is skipped exactly when it matters and leaves the
/// subject raised for every test that follows in the shard.
class ScopedSubjectInt {
  public:
    explicit ScopedSubjectInt(lv_subject_t* subject)
        : subject_(subject), saved_(subject ? lv_subject_get_int(subject) : 0) {}
    ~ScopedSubjectInt() {
        if (subject_ != nullptr) {
            lv_subject_set_int(subject_, saved_);
        }
    }

    ScopedSubjectInt(const ScopedSubjectInt&) = delete;
    ScopedSubjectInt& operator=(const ScopedSubjectInt&) = delete;

  private:
    lv_subject_t* subject_;
    int32_t saved_;
};

/**
 * @brief Test fixture for navbar XML binding tests
 *
 * Tests the dual-icon pattern where:
 * - Active/Inactive icons show when connected AND klippy ready
 * - Disabled icons show when disconnected OR klippy not ready
 */
class NavbarIconTestFixture : public LVGLUITestFixture {
  public:
    NavbarIconTestFixture() {
        // Create the navigation bar component
        navbar_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "navigation_bar", nullptr));
        if (!navbar_) {
            spdlog::error("[NavbarIconTestFixture] Failed to create navigation_bar!");
            return;
        }

        // NOTE: Don't call process_lvgl() in constructor - mDNS timer processing
        // causes test hangs. Subject changes trigger binding updates synchronously.
    }

    ~NavbarIconTestFixture() override {
        if (navbar_) {
            lv_obj_delete(navbar_);
            navbar_ = nullptr;
        }
    }

    /**
     * @brief Check if an object would be visible (no hidden flag on self or ancestors)
     *
     * Unlike lv_obj_is_visible(), this doesn't require an active screen -
     * it just checks the hidden flag chain, which is what we need for testing
     * XML binding behavior.
     */
    bool is_visible(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(navbar_, name);
        if (!obj) {
            spdlog::warn("[NavbarIconTestFixture] Could not find object: {}", name);
            return false;
        }

        // Check hidden flag on object itself
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }

        // Check all ancestors for hidden flag
        lv_obj_t* parent = lv_obj_get_parent(obj);
        while (parent) {
            if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) {
                return false;
            }
            parent = lv_obj_get_parent(parent);
        }

        return true;
    }

    /**
     * @brief Helper to check if an icon is hidden (not visible)
     */
    bool is_hidden(const char* name) {
        return !is_visible(name);
    }

    /**
     * @brief Check whether an icon carries LV_STATE_CHECKED
     */
    bool is_checked(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(navbar_, name);
        if (!obj) {
            spdlog::warn("[NavbarIconTestFixture] Could not find object: {}", name);
            return false;
        }
        return lv_obj_has_state(obj, LV_STATE_CHECKED);
    }

    /**
     * @brief Set nav buttons enabled state directly (combined subject)
     */
    void set_nav_buttons_enabled(bool enabled) {
        lv_subject_set_int(state().get_nav_buttons_enabled_subject(), enabled ? 1 : 0);
    }

    /**
     * @brief Set active panel
     */
    void set_active_panel(PanelId panel_id) {
        NavigationManager::instance().set_active(panel_id);
    }

    lv_obj_t* navbar_ = nullptr;
};

TEST_CASE_METHOD(NavbarIconTestFixture, "Navbar: checked state marks the active icon",
                 "[navbar][ui_integration]") {
    REQUIRE(navbar_ != nullptr);

    SECTION("Enabled + On Home: inactive icon is checked") {
        set_nav_buttons_enabled(true);
        set_active_panel(PanelId::Home); // Not on controls or filament

        // Both icons of a pair resolve and stay unhidden; the inactive one
        // carries the checked state.
        REQUIRE(is_visible("nav_icon_controls_inactive"));
        REQUIRE(is_visible("nav_icon_controls_active"));
        REQUIRE(is_checked("nav_icon_controls_inactive"));
        REQUIRE_FALSE(is_checked("nav_icon_controls_active"));
        REQUIRE(is_hidden("nav_icon_controls_disabled"));

        // Filament button: same pattern
        REQUIRE(is_visible("nav_icon_filament_inactive"));
        REQUIRE(is_visible("nav_icon_filament_active"));
        REQUIRE(is_checked("nav_icon_filament_inactive"));
        REQUIRE_FALSE(is_checked("nav_icon_filament_active"));
        REQUIRE(is_hidden("nav_icon_filament_disabled"));
    }

    SECTION("Enabled + On Controls: active icon is checked") {
        set_nav_buttons_enabled(true);
        set_active_panel(PanelId::Controls);

        // Controls button: active icon carries the checked state.
        REQUIRE(is_visible("nav_icon_controls_active"));
        REQUIRE(is_visible("nav_icon_controls_inactive"));
        REQUIRE(is_checked("nav_icon_controls_active"));
        REQUIRE_FALSE(is_checked("nav_icon_controls_inactive"));
        REQUIRE(is_hidden("nav_icon_controls_disabled"));
    }

    SECTION("Disabled: shows only disabled icon") {
        set_nav_buttons_enabled(false);
        set_active_panel(PanelId::Home);

        // Controls button: only disabled should be visible
        REQUIRE(is_visible("nav_icon_controls_disabled"));
        REQUIRE(is_hidden("nav_icon_controls_inactive"));
        REQUIRE(is_hidden("nav_icon_controls_active"));

        // Filament button: same pattern
        REQUIRE(is_visible("nav_icon_filament_disabled"));
        REQUIRE(is_hidden("nav_icon_filament_inactive"));
        REQUIRE(is_hidden("nav_icon_filament_active"));
    }
}

TEST_CASE_METHOD(NavbarIconTestFixture, "Navbar: State transitions work correctly",
                 "[navbar][ui_integration]") {
    REQUIRE(navbar_ != nullptr);

    SECTION("Transition: Enabled -> Disabled -> Enabled") {
        // Start enabled
        set_nav_buttons_enabled(true);
        set_active_panel(PanelId::Home);

        REQUIRE(is_visible("nav_icon_controls_inactive"));
        REQUIRE(is_hidden("nav_icon_controls_disabled"));

        // Disable (simulate disconnect or klippy shutdown)
        set_nav_buttons_enabled(false);
        REQUIRE(is_hidden("nav_icon_controls_inactive"));
        REQUIRE(is_visible("nav_icon_controls_disabled"));

        // Re-enable
        set_nav_buttons_enabled(true);
        REQUIRE(is_visible("nav_icon_controls_inactive"));
        REQUIRE(is_hidden("nav_icon_controls_disabled"));
    }

    SECTION("Transition: Panel switch while enabled") {
        set_nav_buttons_enabled(true);
        set_active_panel(PanelId::Home);

        // Neither icon is ever hidden any more; checked state carries which
        // one reads as active.
        REQUIRE(is_visible("nav_icon_controls_inactive"));
        REQUIRE(is_visible("nav_icon_controls_active"));
        REQUIRE(is_checked("nav_icon_controls_inactive"));
        REQUIRE_FALSE(is_checked("nav_icon_controls_active"));

        // Switch to controls panel
        set_active_panel(PanelId::Controls);
        REQUIRE(is_checked("nav_icon_controls_active"));
        REQUIRE_FALSE(is_checked("nav_icon_controls_inactive"));

        // Switch back to home
        set_active_panel(PanelId::Home);
        REQUIRE(is_checked("nav_icon_controls_inactive"));
        REQUIRE_FALSE(is_checked("nav_icon_controls_active"));
    }
}

TEST_CASE_METHOD(NavbarIconTestFixture, "Navbar: icon visibility holds in portrait",
                 "[navbar][ui_integration]") {
    REQUIRE(navbar_ != nullptr);

    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    ScopedSubjectInt restore_portrait(portrait);
    lv_subject_set_int(portrait, 1);

    set_nav_buttons_enabled(true);
    set_active_panel(PanelId::Home);

    // One component serves both orientations, so every invariant the landscape
    // cases assert has to hold with ui_is_portrait raised too. Neither icon of
    // a pair is ever hidden; checked state carries which one is active.
    REQUIRE(is_visible("nav_icon_controls_inactive"));
    REQUIRE(is_visible("nav_icon_controls_active"));
    REQUIRE(is_checked("nav_icon_controls_inactive"));
    REQUIRE_FALSE(is_checked("nav_icon_controls_active"));
    REQUIRE(is_hidden("nav_icon_controls_disabled"));

    REQUIRE(is_visible("nav_icon_filament_inactive"));
    REQUIRE(is_visible("nav_icon_filament_active"));
    REQUIRE(is_checked("nav_icon_filament_inactive"));
    REQUIRE_FALSE(is_checked("nav_icon_filament_active"));
    REQUIRE(is_hidden("nav_icon_filament_disabled"));

    set_active_panel(PanelId::Controls);
    REQUIRE(is_checked("nav_icon_controls_active"));
    REQUIRE_FALSE(is_checked("nav_icon_controls_inactive"));
}

TEST_CASE_METHOD(NavbarIconTestFixture, "Navbar: the bar swaps axes with ui_is_portrait",
                 "[navbar][ui_integration]") {
    REQUIRE(navbar_ != nullptr);

    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    ScopedSubjectInt restore_portrait(portrait);

    // nav_btn_controls is the next visible child after nav_btn_home: the two
    // edit-mode buttons ahead of them and the printer badge behind them are
    // hidden until their subjects say otherwise.
    lv_obj_t* home = lv_obj_find_by_name(navbar_, "nav_btn_home");
    lv_obj_t* controls = lv_obj_find_by_name(navbar_, "nav_btn_controls");
    REQUIRE(home != nullptr);
    REQUIRE(controls != nullptr);

    // Orientation reaches the bar as two complementary bound styles. An inline
    // width/height/flex_flow on the view would be a local style, which outranks
    // both and pins the bar to one axis while every other assertion still
    // passes. Measuring the boxes is what catches that.
    lv_subject_set_int(portrait, 0);
    lv_obj_update_layout(navbar_);
    const int32_t land_w = lv_obj_get_width(navbar_);
    const int32_t land_h = lv_obj_get_height(navbar_);
    const int32_t land_btn_w = lv_obj_get_width(home);
    const int32_t land_btn_h = lv_obj_get_height(home);
    const int32_t land_step = lv_obj_get_y(controls) - lv_obj_get_y(home);

    lv_subject_set_int(portrait, 1);
    lv_obj_update_layout(navbar_);
    const int32_t port_w = lv_obj_get_width(navbar_);
    const int32_t port_h = lv_obj_get_height(navbar_);
    const int32_t port_btn_w = lv_obj_get_width(home);
    const int32_t port_btn_h = lv_obj_get_height(home);
    const int32_t port_step = lv_obj_get_x(controls) - lv_obj_get_x(home);

    INFO("bar: landscape " << land_w << "x" << land_h << ", portrait " << port_w << "x" << port_h);
    INFO("home button: landscape " << land_btn_w << "x" << land_btn_h << " step " << land_step
                                   << ", portrait " << port_btn_w << "x" << port_btn_h << " step "
                                   << port_step);

    REQUIRE(land_h > land_w); // vertical strip down one edge
    REQUIRE(port_w > port_h); // horizontal bar across the bottom
    REQUIRE(port_w > land_w);
    REQUIRE(port_h < land_h);

    // A grow item spans the bar's cross axis and takes a share of its main axis,
    // so the home button's box swaps with the bar's.
    REQUIRE(land_btn_w == land_w);
    REQUIRE(port_btn_h == port_h);
    REQUIRE(land_btn_h < land_h);
    REQUIRE(port_btn_w < port_w);

    // Only a flex container distributes its children along an axis. Without a
    // layout every child sits at the content origin, so the next button starting
    // where this one ends is what proves the bar is laying them out at all, and
    // on the axis the live orientation style asks for.
    REQUIRE(land_step >= land_btn_h);
    REQUIRE(port_step >= port_btn_w);
}

/**
 * @brief Test fixture for the app shell, built from ui_xml/app_layout.xml
 *
 * One component serves both orientations, so the shell's axis and the order it
 * places the nav bar in are bound styles rather than two files. Only the shell
 * root is instantiated here; the panel subtrees come with it.
 */
class AppShellTestFixture : public LVGLUITestFixture {
  public:
    AppShellTestFixture() {
        shell_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "app_layout", nullptr));
        if (shell_ == nullptr) {
            spdlog::error("[AppShellTestFixture] Failed to create app_layout!");
        }
    }

    ~AppShellTestFixture() override {
        if (shell_ != nullptr) {
            lv_obj_delete(shell_);
            shell_ = nullptr;
        }
    }

    lv_obj_t* shell_ = nullptr;
};

TEST_CASE_METHOD(AppShellTestFixture, "App shell: the nav bar changes edges with ui_is_portrait",
                 "[navbar][ui_integration]") {
    REQUIRE(shell_ != nullptr);

    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    ScopedSubjectInt restore_portrait(portrait);

    lv_obj_t* navbar = lv_obj_find_by_name(shell_, "navbar");
    lv_obj_t* content = lv_obj_find_by_name(shell_, "content_area");
    REQUIRE(navbar != nullptr);
    REQUIRE(content != nullptr);

    // Where each child LANDS is the only thing that separates a working shell
    // from the three ways this can break: no layout at all leaves both children
    // at the content origin, a row flow in portrait starves content_area to zero
    // width, and a forward column flow puts the bar at the top. Sizes alone hold
    // in all three.
    lv_subject_set_int(portrait, 0);
    lv_obj_update_layout(shell_);
    // The shell is width/height 100%, so its own box only resolves once a layout
    // pass has run.
    const int32_t shell_w = lv_obj_get_width(shell_);
    const int32_t shell_h = lv_obj_get_height(shell_);
    const int32_t land_nav_x = lv_obj_get_x(navbar);
    const int32_t land_nav_y = lv_obj_get_y(navbar);
    const int32_t land_nav_w = lv_obj_get_width(navbar);
    const int32_t land_content_x = lv_obj_get_x(content);
    const int32_t land_content_y = lv_obj_get_y(content);
    const int32_t land_content_w = lv_obj_get_width(content);
    const int32_t land_content_h = lv_obj_get_height(content);

    lv_subject_set_int(portrait, 1);
    lv_obj_update_layout(shell_);
    const int32_t port_nav_x = lv_obj_get_x(navbar);
    const int32_t port_nav_y = lv_obj_get_y(navbar);
    const int32_t port_nav_h = lv_obj_get_height(navbar);
    const int32_t port_content_x = lv_obj_get_x(content);
    const int32_t port_content_y = lv_obj_get_y(content);
    const int32_t port_content_w = lv_obj_get_width(content);
    const int32_t port_content_h = lv_obj_get_height(content);

    INFO("shell " << shell_w << "x" << shell_h);
    INFO("landscape navbar at " << land_nav_x << "," << land_nav_y << " w " << land_nav_w
                                << "; content at " << land_content_x << "," << land_content_y << " "
                                << land_content_w << "x" << land_content_h);
    INFO("portrait navbar at " << port_nav_x << "," << port_nav_y << " h " << port_nav_h
                               << "; content at " << port_content_x << "," << port_content_y << " "
                               << port_content_w << "x" << port_content_h);

    // Landscape: strip on the leading edge, content filling the rest of the row.
    REQUIRE(land_nav_x == 0);
    REQUIRE(land_nav_y == 0);
    REQUIRE(land_content_y == 0);
    REQUIRE(land_content_x >= land_nav_x + land_nav_w);
    REQUIRE(land_content_x + land_content_w == shell_w);
    REQUIRE(land_content_h == shell_h);

    // Portrait: content on top, bar flush against the bottom edge. The reverse
    // flow is what puts the first-declared child last, so the bar starting below
    // where content_area ends is the assertion that proves it.
    REQUIRE(port_content_x == 0);
    REQUIRE(port_content_y == 0);
    REQUIRE(port_content_w == shell_w);
    REQUIRE(port_nav_x == 0);
    REQUIRE(port_nav_y >= port_content_y + port_content_h);
    REQUIRE(port_nav_y + port_nav_h == shell_h);
}

// ============================================================================
// Overlay Instance Registration Tests
// ============================================================================

#include "ui_nav_manager.h"

#include "panel_lifecycle.h"

/**
 * @brief Mock implementation of IPanelLifecycle for testing overlay registration
 *
 * Tests that NavigationManager::register_overlay_instance accepts any
 * IPanelLifecycle implementation, not just OverlayBase.
 */
class MockPanelLifecycle : public IPanelLifecycle {
  public:
    void on_activate() override {
        activate_count_++;
    }
    void on_deactivate(DeactivateReason) override {
        deactivate_count_++;
    }
    const char* get_name() const override {
        return "MockPanel";
    }

    int activate_count_ = 0;
    int deactivate_count_ = 0;
};

TEST_CASE_METHOD(NavbarIconTestFixture, "Overlay registration accepts IPanelLifecycle",
                 "[navigation][overlay]") {
    MockPanelLifecycle mock_panel;

    // Create a test widget to serve as overlay root
    lv_obj_t* test_overlay = lv_obj_create(test_screen());
    REQUIRE(test_overlay != nullptr);

    SECTION("Can register IPanelLifecycle implementation") {
        // Should not throw - IPanelLifecycle is accepted, not just OverlayBase
        NavigationManager::instance().register_overlay_instance(test_overlay, &mock_panel);

        // Verify it was registered by checking we can unregister without error
        NavigationManager::instance().unregister_overlay_instance(test_overlay);
    }

    // Cleanup
    lv_obj_delete(test_overlay);
}

// ============================================================================
// Self-healing panel_stack_ against out-of-band widget deletion (bundle ZW6ATWSL)
// ============================================================================
// Regression for the SIGSEGV where OverlayBase::destroy_overlay_ui() deletes an
// overlay that is still on panel_stack_ (the klippy MCU-shutdown teardown path),
// leaving a dangling pointer that the next push_overlay() dereferences via
// lv_obj_add_flag(panel_stack_.back(), LV_OBJ_FLAG_HIDDEN). The fix attaches an
// LV_EVENT_DELETE hook so a deleted widget is scrubbed from panel_stack_ (and the
// other bookkeeping maps) synchronously, before its memory is freed.

#include "../test_helpers/update_queue_test_access.h"

TEST_CASE_METHOD(NavbarIconTestFixture, "Out-of-band backdrop deletion scrubs overlay_backdrop_",
                 "[navigation][overlay][backdrop]") {
    auto& nav = NavigationManager::instance();

    // Same base-stack seeding as the sibling scrub test — push_overlay only
    // builds a backdrop when it sees itself as the first overlay.
    lv_obj_t* base = lv_obj_create(test_screen());
    REQUIRE(base != nullptr);
    lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
    panels[static_cast<int>(PanelId::Home)] = base;
    nav.set_panels(panels);

    MockPanelLifecycle mock_panel;
    lv_obj_t* overlay = lv_obj_create(test_screen());
    REQUIRE(overlay != nullptr);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(overlay, &mock_panel);
    nav.push_overlay(overlay);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);

    // Delete the backdrop OUT-OF-BAND. In the suite this happens when a fixture
    // destructor deletes the screen the backdrop is parented to (XMLTestFixture
    // does exactly that), which frees it with no go_back() involved.
    lv_obj_delete(backdrop);

    // Regression assertion: overlay_backdrop_ is a scalar, so scrub_deleted_widget()
    // must clear it explicitly. Without that, deinit_subjects() reaches
    // lv_obj_del(overlay_backdrop_) on freed memory — a heap-use-after-free that
    // detonates in whatever unrelated test runs next.
    REQUIRE(NavigationManagerTestAccess::overlay_backdrop(nav) == nullptr);

    lv_obj_delete(overlay);
    lv_obj_delete(base);
}

TEST_CASE_METHOD(NavbarIconTestFixture, "Out-of-band widget deletion scrubs panel_stack_",
                 "[navigation][overlay][l081]") {
    auto& nav = NavigationManager::instance();

    // Seed a base main panel into the stack the way production does — via
    // set_panels(), which pushes the active (Home) panel widget onto slot 0.
    // push_overlay's is_first_overlay logic assumes a non-empty base stack.
    lv_obj_t* base = lv_obj_create(test_screen());
    REQUIRE(base != nullptr);
    lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
    panels[static_cast<int>(PanelId::Home)] = base; // active panel is Home by default
    nav.set_panels(panels);
    REQUIRE(nav.is_panel_in_stack(base) == true);

    // Build the overlay that will be torn down out-of-band, stacked on the base.
    MockPanelLifecycle mock_panel;
    lv_obj_t* overlay = lv_obj_create(test_screen());
    REQUIRE(overlay != nullptr);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(overlay, &mock_panel);
    nav.push_overlay(overlay);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // Precondition: the overlay is tracked as the top of the stack.
    REQUIRE(nav.is_panel_in_stack(overlay) == true);

    // Delete the overlay OUT-OF-BAND (not via go_back) — mirrors
    // OverlayBase::destroy_overlay_ui() during a klippy-shutdown teardown.
    lv_obj_delete(overlay);

    // Regression assertion: the stack must no longer reference the freed widget.
    // Without the LV_EVENT_DELETE self-heal, the stale pointer stays in
    // panel_stack_ and the next push dereferences freed memory.
    REQUIRE(nav.is_panel_in_stack(overlay) == false);
    REQUIRE(nav.is_panel_in_stack(base) == true); // base untouched

    // Prove no UAF on the next push: pushing a new overlay must not touch the
    // freed previous-top widget (this is exactly the production crash chain —
    // a reconnect push_overlay("Print Status") after the teardown).
    MockPanelLifecycle mock_panel2;
    lv_obj_t* overlay2 = lv_obj_create(test_screen());
    REQUIRE(overlay2 != nullptr);
    lv_obj_add_flag(overlay2, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(overlay2, &mock_panel2);
    nav.push_overlay(overlay2);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(nav.is_panel_in_stack(overlay2) == true);

    // overlay2 went through register_overlay_instance + push, so it is hooked
    // and must also be scrubbed on out-of-band deletion.
    lv_obj_delete(overlay2);
    REQUIRE(nav.is_panel_in_stack(overlay2) == false);

    // Cleanup: base is a main-panel widget (set_panels), not an overlay, so it
    // is not delete-hooked; the fixture's deinit_subjects() clears panel_stack_
    // without dereferencing, so leaving a stale base pointer is harmless.
    lv_obj_delete(base);
}

// ============================================================================
// Backdrop tap must not dismiss the overlay while the on-screen keyboard is up
// ============================================================================
// Reported on Snapmaker U1 v0.91: entering the Spoolman IP was impossible — the
// on-screen keyboard closed and the settings overlay popped on nearly every
// keystroke. Overlays are narrower than the screen (hor_res - nav_width) and sit
// over a full-screen clickable dismiss-backdrop, so a stray tap outside the
// keyboard/field lands on the exposed backdrop column and calls go_back().
//
// LVGL's indev_proc_press sends LV_EVENT_PRESSED before the click-focus DEFOCUS
// that hides the keyboard (lv_indev.c: PRESSED at :1344, indev_click_focus at
// :1351), so is_visible() already reads false by the CLICKED handler. The fix
// latches keyboard visibility at PRESSED and consumes that first backdrop tap
// for the keyboard dismiss only — the overlay stays; a second tap dismisses it.
TEST_CASE_METHOD(NavbarIconTestFixture,
                 "Backdrop tap with keyboard up dismisses keyboard, keeps overlay",
                 "[navigation][overlay][keyboard]") {
    auto& nav = NavigationManager::instance();

    // Seed a base panel so push_overlay's is_first_overlay logic has a stack.
    lv_obj_t* base = lv_obj_create(test_screen());
    REQUIRE(base != nullptr);
    lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
    panels[static_cast<int>(PanelId::Home)] = base;
    nav.set_panels(panels);

    MockPanelLifecycle mock_panel;
    lv_obj_t* overlay = lv_obj_create(test_screen());
    REQUIRE(overlay != nullptr);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(overlay, &mock_panel);
    nav.push_overlay(overlay);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(nav.is_panel_in_stack(overlay) == true);

    // Keyboard was visible when the backdrop was pressed: the CLICKED handler
    // consumes this tap for the keyboard dismiss and returns before go_back(), so
    // the overlay is kept.
    NavigationManagerTestAccess::set_backdrop_press_keyboard_visible(nav, true);
    REQUIRE(nav.take_backdrop_keyboard_dismiss() == true);
    REQUIRE(nav.is_panel_in_stack(overlay) == true);

    // The latch is one-shot: the next tap (keyboard now down) is NOT consumed, so
    // the handler falls through to go_back() and dismisses the overlay.
    REQUIRE(nav.take_backdrop_keyboard_dismiss() == false);
    nav.go_back();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(nav.is_panel_in_stack(overlay) == false);

    lv_obj_delete(base);
}

// ============================================================================
// Overlay freed BEFORE the deferred push drains (bundle MBUX7WUN)
// ============================================================================
// push_overlay() queues its whole body via queue_update, capturing the raw
// overlay_panel pointer. If the overlay is destroyed between the queue call and
// the drain — e.g. a print that fails Klipper config validation
// ('sense_resistor' not valid in 'tmc2240 stepper_z') and immediately tears its
// Print Status overlay back down — the deferred lambda dereferences a freed
// pointer at lv_obj_get_screen(overlay_panel), SIGSEGV with a wild fault_addr.
//
// This differs from the ZW6ATWSL self-heal above: there the overlay was already
// ON panel_stack_ when deleted; here it is deleted while the push is still
// pending, so it never reached the stack and the delete-hook scrub does not
// apply. The fix is an lv_obj_is_valid(overlay_panel) guard at the top of the
// deferred lambda — lv_obj_is_valid searches the display tree rather than
// dereferencing, so it is safe on a freed pointer.

TEST_CASE_METHOD(NavbarIconTestFixture,
                 "push_overlay skips a target freed before the deferred push drains",
                 "[navigation][overlay][l081]") {
    auto& nav = NavigationManager::instance();

    // Seed a base main panel so push_overlay's is_first_overlay logic has a
    // non-empty base stack (matches production: slot 0 holds the active panel).
    lv_obj_t* base = lv_obj_create(test_screen());
    REQUIRE(base != nullptr);
    lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
    panels[static_cast<int>(PanelId::Home)] = base;
    nav.set_panels(panels);
    REQUIRE(nav.is_panel_in_stack(base) == true);

    // Register + push an overlay, but do NOT drain yet — the push lambda is now
    // pending in the UpdateQueue with a raw pointer captured by value.
    MockPanelLifecycle mock_panel;
    lv_obj_t* overlay = lv_obj_create(test_screen());
    REQUIRE(overlay != nullptr);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(overlay, &mock_panel);
    nav.push_overlay(overlay);

    // Destroy the overlay BEFORE the deferred push runs — this is the
    // push→teardown race from the bundle.
    lv_obj_delete(overlay);

    // Draining must not crash on the freed pointer, and the dead overlay must
    // not be admitted to the stack. Without the lv_obj_is_valid guard the lambda
    // calls lv_obj_get_screen on freed memory (SIGSEGV under ASAN) or pushes the
    // dangling pointer onto panel_stack_ (is_panel_in_stack would be true).
    REQUIRE_NOTHROW(
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance()));
    REQUIRE(nav.is_panel_in_stack(overlay) == false);
    REQUIRE(nav.is_panel_in_stack(base) == true); // base untouched

    lv_obj_delete(base);
}

// ============================================================================
// push_overlay onto an EMPTY stack must not deref panel_stack_.back() (UB)
// ============================================================================
// push_overlay's "deactivate previous overlay" else-branch derefs
// panel_stack_.back(). With the old `is_first_overlay = (size() == 1)`, an empty
// stack (size 0) fell into that else-branch and called back() on an empty vector
// — undefined behavior (observed as a SIGSEGV under hardened libstdc++). The fix
// (`size() <= 1`) treats an empty stack as the first overlay, skipping the deref.
// Production never pushes onto an empty stack (slot 0 holds the active panel),
// so this guards a latent path rather than a reachable user flow.

TEST_CASE_METHOD(NavbarIconTestFixture, "push_overlay onto empty stack does not deref back()",
                 "[navigation][overlay]") {
    auto& nav = NavigationManager::instance();

    // Fixture starts with an empty panel_stack_ (no set_panels()).
    REQUIRE(nav.is_panel_in_stack(nullptr) == false);

    MockPanelLifecycle mock_panel;
    lv_obj_t* overlay = lv_obj_create(test_screen());
    REQUIRE(overlay != nullptr);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(overlay, &mock_panel);

    // Pushing onto the empty stack must not crash on panel_stack_.back().
    nav.push_overlay(overlay);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // The overlay is now the (only) entry — survived the empty-stack push.
    REQUIRE(nav.is_panel_in_stack(overlay) == true);

    lv_obj_delete(overlay);
    REQUIRE(nav.is_panel_in_stack(overlay) == false);
}

// ============================================================================
// is_panel_on_top: "will go_back() pop THIS panel?" (#1221)
// ============================================================================
// A deferred callback that pushed an overlay earlier cannot assume it is still
// the top when it finally runs. PrintStartController's failure handler pushed
// print status, then called go_back() up to a minute later after a preparation
// timeout — by which point the user had opened two more overlays, so the pop
// closed THEIR screen and the follow-up re-show rebuilt widgets that same pop
// had just destroyed (SIGSEGV, bundle SREQ5VQN). is_panel_in_stack() is not
// enough to catch this: the pusher's panel is usually still IN the stack, just
// buried.
TEST_CASE_METHOD(NavbarIconTestFixture, "is_panel_on_top distinguishes top from merely-in-stack",
                 "[navigation][overlay][1221]") {
    auto& nav = NavigationManager::instance();

    lv_obj_t* base = lv_obj_create(test_screen());
    REQUIRE(base != nullptr);
    lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
    panels[static_cast<int>(PanelId::Home)] = base;
    nav.set_panels(panels);

    REQUIRE(nav.is_panel_on_top(nullptr) == false);
    REQUIRE(nav.is_panel_on_top(base) == true); // only entry

    MockPanelLifecycle mock_first;
    lv_obj_t* first = lv_obj_create(test_screen());
    lv_obj_add_flag(first, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(first, &mock_first);
    nav.push_overlay(first);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE(nav.is_panel_on_top(first) == true);
    REQUIRE(nav.is_panel_on_top(base) == false); // in the stack, but buried

    // The user navigates on — this is the state the timed-out callback wakes to.
    MockPanelLifecycle mock_second;
    lv_obj_t* second = lv_obj_create(test_screen());
    lv_obj_add_flag(second, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(second, &mock_second);
    nav.push_overlay(second);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // The discrimination that matters: `first` is still in the stack, so an
    // is_panel_in_stack() guard would wave the blind go_back() through.
    REQUIRE(nav.is_panel_in_stack(first) == true);
    REQUIRE(nav.is_panel_on_top(first) == false);
    REQUIRE(nav.is_panel_on_top(second) == true);

    lv_obj_delete(second);
    lv_obj_delete(first);
    lv_obj_delete(base);
}
