// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_application_background_resume.cpp
 * @brief Android pause/resume must run panel lifecycle (prestonbrown/helixscreen#1245)
 *
 * Issue #1245 item 4: "some UI elements like the camera feed or print status don't
 * refresh unless you change the active tab then back again - this occurs more when
 * switching apps and/or waking the screen from standby".
 *
 * Changing tabs fixes it because set_active() runs on_deactivate()/on_activate();
 * a resume used to repaint pixels and nothing else. on_activate() is what re-seeds
 * subjects, rebinds observers, reloads content and restarts per-panel timers
 * (PrintStatusPanel::on_activate does ensure_preview_current + update_button_states
 * + bind_fan_observers + temp graph resume), so without it the panel keeps whatever
 * it had when Android froze the process.
 *
 * These tests pin the wiring: Application::on_enter_background/on_enter_foreground
 * must drive NavigationManager::suspend_active()/resume_active(), for both branches
 * that pair covers (bare active panel, and topmost overlay), and must reset LVGL's
 * inactivity clock so the first check_display_sleep() after resume does not see the
 * whole backgrounded interval as idle time and drop straight back into sleep.
 *
 * The Application here is never run(); the pause/resume hooks are reached through
 * ApplicationTestAccess because they are private and production only calls them from
 * inside run()'s main loop.
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "application_test_fixture.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "runtime_config.h"
#include "test_helpers/application_test_access.h"
#include "theme_manager.h"

#include <array>

#include "../../catch_amalgamated.hpp"

namespace {

/// Bare lifecycle counter for the overlay branch of suspend_active/resume_active.
class CountingOverlay : public IPanelLifecycle {
  public:
    void on_activate() override {
        ++activates;
    }
    void on_deactivate() override {
        ++deactivates;
    }
    const char* get_name() const override {
        return "CountingOverlay";
    }

    int activates = 0;
    int deactivates = 0;
};

/// Same, for the panel branch — NavigationManager's panel slots are typed
/// PanelBase*, not IPanelLifecycle*, so the panel case needs a real subclass.
class CountingPanel : public PanelBase {
  public:
    CountingPanel() : PanelBase(get_printer_state(), nullptr) {}

    void init_subjects() override {}
    const char* get_name() const override {
        return "CountingPanel";
    }
    const char* get_xml_component_name() const override {
        return "counting_panel";
    }
    void on_activate() override {
        ++activates;
    }
    void on_deactivate() override {
        ++deactivates;
    }

    int activates = 0;
    int deactivates = 0;
};

constexpr helix::PanelId TEST_PANEL = helix::PanelId::Home;

/**
 * @brief Application + a NavigationManager seeded the way the running app has it
 *
 * panel_stack_[0] must hold the active root panel (see tests/CLAUDE.md) or the
 * overlay branch of suspend_active() never resolves what is beneath the overlay.
 */
class BackgroundResumeFixture : public ApplicationTestFixture {
  public:
    BackgroundResumeFixture() {
        // on_enter_foreground() re-initialises SoundManager. Nothing in this test
        // wants an SDL/ALSA device opened inside the Catch2 process.
        sound_was_disabled_ = get_runtime_config()->disable_sound;
        get_runtime_config()->disable_sound = true;

        // on_enter_foreground() ends in lv_refr_now(nullptr), which refreshes EVERY
        // display. Several unrelated test translation units create a bare
        // lv_display_create() in a static initialiser (test_helix_print_api.cpp,
        // test_metadata_and_usb_symlink.cpp, test_moonraker_api_domain.cpp, ...) with
        // no buffers and no flush callback, so their disp->flushing is set and never
        // cleared and lv_refr.c's wait_for_flushing() busy-waits forever — the hang
        // tests/test_helpers/display_manager_test_access.h warns about. Giving every
        // display a no-op flush_wait_cb takes the branch that clears the flag instead
        // of spinning, so the real production repaint runs here rather than being
        // stubbed out of the test.
        for (lv_display_t* d = lv_display_get_next(nullptr); d != nullptr;
             d = lv_display_get_next(d)) {
            lv_display_set_flush_wait_cb(d, [](lv_display_t*) {});
        }

        display_ = lv_display_get_default();
        REQUIRE(display_ != nullptr);
        theme_manager_register_responsive_spacing(display_);

        auto& nav = NavigationManager::instance();
        nav.init();
        for (auto& p : root_panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        nav.set_panels(root_panels_.data());
        nav.register_panel_instance(TEST_PANEL, &panel_);
        nav.set_active(TEST_PANEL);
        helix::ui::UpdateQueue::instance().drain();

        // NavigationManager is a process singleton and suspend/resume is latched on
        // a single bool. Clear it against our own freshly registered panel so a test
        // that left the latch set cannot make this one's suspend a silent no-op.
        nav.resume_active();

        // ~Application() calls shutdown(), which tears down process singletons
        // shared with the rest of the shard. This one was never run().
        ApplicationTestAccess::neutralize_destructor(app_);

        // set_active() legitimately fires lifecycle; only the transitions the test
        // itself drives should be visible in the counters.
        panel_.activates = 0;
        panel_.deactivates = 0;
    }

    ~BackgroundResumeFixture() override {
        // Never leave a test with rendering suppressed for the next one.
        lv_display_enable_invalidation(nullptr, true);
        for (lv_display_t* d = lv_display_get_next(nullptr); d != nullptr;
             d = lv_display_get_next(d)) {
            lv_display_set_flush_wait_cb(d, nullptr);
        }
        auto& nav = NavigationManager::instance();
        // Leave the suspend latch cleared for the next test, whatever this one did.
        nav.resume_active();
        nav.register_panel_instance(TEST_PANEL, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        nav.deinit_subjects();
        get_runtime_config()->disable_sound = sound_was_disabled_;
    }

    /// Register + push an overlay so panel_stack_.back() is the overlay, which is
    /// the branch suspend_active()/resume_active() take when anything is stacked.
    lv_obj_t* push_overlay(CountingOverlay& overlay) {
        auto& nav = NavigationManager::instance();
        lv_obj_t* w = lv_obj_create(lv_screen_active());
        nav.register_overlay_instance(w, &overlay);
        nav.push_overlay(w);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        return w;
    }

    void background() {
        ApplicationTestAccess::on_enter_background(app_);
    }
    void foreground() {
        ApplicationTestAccess::on_enter_foreground(app_);
    }

    Application app_;
    CountingPanel panel_;
    std::array<lv_obj_t*, UI_PANEL_COUNT> root_panels_{};
    lv_display_t* display_ = nullptr;
    bool sound_was_disabled_ = false;
};

} // namespace

// ============================================================================
// Panel branch
// ============================================================================

TEST_CASE_METHOD(BackgroundResumeFixture,
                 "Application background/foreground runs the active panel's lifecycle",
                 "[1245][application][lifecycle][navigation]") {
    // Negative case first: constructing and doing nothing must not move anything,
    // and a foreground with no preceding background is a no-op (the m_backgrounded
    // guard). Without this the counters below could be satisfied by any stray call.
    CHECK(panel_.deactivates == 0);
    CHECK(panel_.activates == 0);
    foreground();
    CHECK(panel_.activates == 0);
    CHECK_FALSE(ApplicationTestAccess::backgrounded(app_));

    background();
    CHECK(ApplicationTestAccess::backgrounded(app_));
    CHECK(panel_.deactivates == 1);
    // Suspending must not smuggle in an activate.
    CHECK(panel_.activates == 0);

    foreground();
    CHECK_FALSE(ApplicationTestAccess::backgrounded(app_));
    CHECK(panel_.activates == 1);
    CHECK(panel_.deactivates == 1);
}

TEST_CASE_METHOD(BackgroundResumeFixture, "Application pause/resume lifecycle is idempotent",
                 "[1245][application][lifecycle]") {
    background();
    background();
    CHECK(panel_.deactivates == 1);

    foreground();
    foreground();
    CHECK(panel_.activates == 1);

    // A second full cycle still works — suspend_active()'s internal latch must have
    // been cleared by the resume, not left stuck.
    background();
    foreground();
    CHECK(panel_.deactivates == 2);
    CHECK(panel_.activates == 2);
}

// ============================================================================
// Overlay branch — whatever is on top is what has to come back
// ============================================================================

TEST_CASE_METHOD(BackgroundResumeFixture,
                 "Application background/foreground runs the topmost overlay's lifecycle",
                 "[1245][application][lifecycle][navigation]") {
    CountingOverlay overlay;
    lv_obj_t* w = push_overlay(overlay);
    REQUIRE(w != nullptr);
    overlay.activates = 0;
    overlay.deactivates = 0;
    panel_.activates = 0;
    panel_.deactivates = 0;

    background();
    foreground();

    CHECK(overlay.deactivates == 1);
    CHECK(overlay.activates == 1);
    // The panel underneath stays suspended — it is not the visible view.
    CHECK(panel_.deactivates == 0);
    CHECK(panel_.activates == 0);

    NavigationManager::instance().unregister_overlay_instance(w);
}

// ============================================================================
// Inactivity clock
// ============================================================================

TEST_CASE_METHOD(BackgroundResumeFixture, "Resuming from background resets LVGL's idle clock",
                 "[1245][application][lifecycle][display]") {
    background();

    // LVGL's inactivity time is tick-based, and the tick keeps advancing while the
    // app is paused. Without a reset on resume, the first check_display_sleep()
    // after coming back sees the entire backgrounded interval as idle and drops
    // straight into sleep or the screensaver.
    process_lvgl(600);
    uint32_t idle_while_paused = lv_display_get_inactive_time(nullptr);
    REQUIRE(idle_while_paused >= 500);

    foreground();

    // Nothing advances the tick between the reset and here, so the clock must read
    // zero — not merely "smaller".
    CHECK(lv_display_get_inactive_time(nullptr) == 0);
}
