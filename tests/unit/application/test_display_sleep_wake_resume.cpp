// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_sleep_wake_resume.cpp
 * @brief Waking from sleep must resume the lifecycle the screensaver suspended.
 *
 * The idle chain is dim (screensaver starts, check_display_sleep suspends the
 * active panel/overlay lifecycle) -> sleep (enter_sleep stops the screensaver
 * and clears m_screensaver_active) -> wake. enter_sleep deliberately does not
 * resume: widget timers must stay stopped while the screen sleeps. The resume
 * therefore belongs on the wake side, and it must not be gated on
 * m_screensaver_active — enter_sleep already cleared that flag, so a gated
 * resume never fires. The view stays deactivated (stopped timers, emptied
 * graphs) while NavigationManager's suspend latch makes every later
 * screensaver cycle skip its suspend entirely.
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_test_utils.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "display/lv_display_private.h" // inv_en_cnt, restored after every test
#include "display_manager.h"
#include "display_settings_manager.h"
#include "lvgl_test_fixture.h"
#include "panel_lifecycle.h"
#include "theme_manager.h"

#include <array>

#include "../../catch_amalgamated.hpp"

using helix::DisplaySettingsManager;

#ifdef HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif

namespace {

/// Lifecycle counter driven through the panel branch of
/// suspend_active()/resume_active().
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
 * @brief NavigationManager seeded the way the running app has it
 *
 * panel_stack_[0] must hold the active root panel (see tests/CLAUDE.md) or the
 * suspend branch never resolves what is beneath the overlay.
 */
class SleepWakeFixture : public LVGLTestFixture {
  public:
    SleepWakeFixture() {
        // wake_display() from full sleep ends in lv_refr_now(nullptr), which
        // refreshes every display, including ones with no flush callback.
        ensure_displays_never_block_on_flush();

        display_ = lv_display_get_default();
        REQUIRE(display_ != nullptr);
        saved_inv_en_cnt_ = display_->inv_en_cnt;
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

        // NavigationManager is a process singleton and suspend/resume latches on
        // a single bool. Clear it against our own freshly registered panel so a
        // test that left the latch set cannot make this one's suspend a no-op.
        nav.resume_active();

        // set_active() legitimately fires lifecycle; only the idle-driven
        // transitions should be visible in the counters.
        panel_.activates = 0;
        panel_.deactivates = 0;
    }

    ~SleepWakeFixture() override {
#ifdef HELIX_ENABLE_SCREENSAVER
        ScreensaverManager::instance().stop();
#endif
        display_->inv_en_cnt = saved_inv_en_cnt_;
        auto& nav = NavigationManager::instance();
        // Leave the suspend latch cleared for the next test, whatever this one did.
        nav.resume_active();
        nav.register_panel_instance(TEST_PANEL, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        nav.deinit_subjects();
        // Leave DisplaySettingsManager initialized for the next fixture. A
        // deinitialised one withdraws settings_animations_enabled, so later
        // fixtures would run with animations re-enabled.
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();
    }

    /// Point the idle config at the shape this test wants and rebuild the subjects.
    void configure_idle(int screensaver_type, int dim_sec, int sleep_sec) {
        helix::Config* config = helix::Config::get_instance();
        config->set<int>("/display/screensaver_type", screensaver_type);
        config->set<int>("/display/dim_sec", dim_sec);
        config->set<int>("/display/sleep_sec", sleep_sec);
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();
    }

    CountingPanel panel_;
    std::array<lv_obj_t*, UI_PANEL_COUNT> root_panels_{};
    lv_display_t* display_ = nullptr;
    decltype(lv_display_t::inv_en_cnt) saved_inv_en_cnt_ = 0;

    /// Advance virtual LVGL time without the wall-clock pacing process_lvgl()
    /// adds for long waits: the sleep timeout's smallest valid option is 60s,
    /// which the idle path must be driven past.
    void advance_idle(int ms) {
        while (ms > 0) {
            const int chunk = ms > 40 ? 40 : ms;
            process_lvgl(chunk);
            ms -= chunk;
        }
    }
};

} // namespace

#ifdef HELIX_ENABLE_SCREENSAVER

TEST_CASE_METHOD(SleepWakeFixture, "waking from sleep resumes the panel the screensaver suspended",
                 "[application][display][sleep][screensaver][wake]") {
    // Valid settings options only: dim 30s / sleep 60s (sleep >= dim keeps the
    // coupling from clamping). The manager's own dim timeout is driven down to
    // 1s so the dim stage is reached quickly.
    configure_idle(/*screensaver_type=*/1, /*dim_sec=*/30, /*sleep_sec=*/60);

    DisplayManager mgr; // no init(): no backlight, software-overlay sleep
    mgr.set_dim_timeout(1);

    // Dim stage: 1.3s idle crosses the 1s dim timeout, starts the screensaver
    // and suspends the active panel's lifecycle.
    lv_display_trigger_activity(nullptr);
    process_lvgl(1300);
    mgr.check_display_sleep();
    REQUIRE(mgr.is_display_dimmed());
    REQUIRE_FALSE(mgr.is_display_sleeping());
    REQUIRE(panel_.deactivates == 1);
    REQUIRE(panel_.activates == 0);

    // Sleep stage: 61s total idle crosses the 60s sleep timeout. enter_sleep()
    // stops the screensaver but must not resume yet — the screen is asleep.
    advance_idle(60000);
    mgr.check_display_sleep();
    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(panel_.activates == 0);

    // Wake stage: fresh activity drives wake_display(), which must resume the
    // suspended lifecycle even though enter_sleep() already cleared
    // m_screensaver_active.
    lv_display_trigger_activity(nullptr);
    mgr.check_display_sleep();
    REQUIRE_FALSE(mgr.is_display_sleeping());
    REQUIRE_FALSE(mgr.is_display_dimmed());
    REQUIRE(panel_.activates == 1);

    // The latch must be clear: a second idle cycle suspends again instead of
    // silently skipping the suspend forever.
    lv_display_trigger_activity(nullptr);
    process_lvgl(1300);
    mgr.check_display_sleep();
    REQUIRE(mgr.is_display_dimmed());
    REQUIRE(panel_.deactivates == 2);
}

TEST_CASE_METHOD(SleepWakeFixture,
                 "waking from a screensaverless sleep activates nothing spuriously",
                 "[application][display][sleep][wake]") {
    configure_idle(/*screensaver_type=*/0, /*dim_sec=*/600, /*sleep_sec=*/60);

    DisplayManager mgr;
    mgr.set_dim_timeout(600);

    // 61s idle crosses the sleep timeout directly; with the screensaver off
    // nothing ever suspends.
    lv_display_trigger_activity(nullptr);
    advance_idle(61000);
    mgr.check_display_sleep();
    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(panel_.deactivates == 0);

    lv_display_trigger_activity(nullptr);
    mgr.check_display_sleep();
    REQUIRE_FALSE(mgr.is_display_sleeping());
    // The wake-side resume is a no-op when nothing is suspended — no spurious
    // activation.
    REQUIRE(panel_.activates == 0);
    REQUIRE(panel_.deactivates == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
