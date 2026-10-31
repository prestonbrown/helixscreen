// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_remote_viewer_sleep.cpp
 * @brief A display someone watches through the remote screen server never sleeps, and a
 *        viewer arriving wakes one that is asleep.
 *
 * The rule is decide_idle()'s (test_display_idle_decision.cpp); this pins that
 * DisplayManager feeds it the service's viewer signal.
 */

#include "ui_update_queue.h"

#include "../../test_helpers/config_test_access.h"
#include "config.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "lvgl_test_fixture.h"
#include "remote_screen_server_sink.h"
#include "remote_screen_service.h"
#include "remote_screen_settings_manager.h"
#include "screensaver.h"
#include "test_helpers/display_manager_test_access.h"

#include "../../catch_amalgamated.hpp"

using helix::Config;
using helix::ConfigTestAccess;
using helix::DisplaySettingsManager;
using helix::RemoteScreenService;
using helix::RemoteScreenSettingsManager;

namespace {

/// The screen server running on an ephemeral port, a one-second sleep timeout and no dim
/// stage, so the only branch a tick can take past the timeout is sleep entry.
class RemoteViewerSleepFixture : public LVGLTestFixture {
  public:
    RemoteViewerSleepFixture() {
        ::unsetenv("HELIX_REMOTE_SCREEN");
        ::unsetenv("HELIX_REMOTE_SCREEN_FB0");
        auto& data = ConfigTestAccess::data(*Config::get_instance());
        data.erase("remote_screen");
        Config::get_instance()->set<int>("/remote_screen/port", 0);
        auto& m = RemoteScreenSettingsManager::instance();
        m.deinit_subjects();
        m.init_subjects();
        m.set_enabled(true);
        RemoteScreenService::instance().init(nullptr);
        REQUIRE(RemoteScreenService::instance().running());

        auto& ds = DisplaySettingsManager::instance();
        prev_dim_sec_ = ds.get_display_dim_sec();
        prev_sleep_sec_ = ds.get_display_sleep_sec();
        prev_sleep_while_printing_ = ds.get_sleep_while_printing();
        ds.set_display_dim_sec(1);
        ds.set_display_sleep_sec(1);
        ds.set_sleep_while_printing(true);
    }

    ~RemoteViewerSleepFixture() override {
        // A display held out of sleep runs the screensaver; it outlives the case's
        // DisplayManager, and the next start() would take it for its own.
#if HELIX_ENABLE_SCREENSAVER
        ScreensaverManager::instance().stop();
#endif
        auto& ds = DisplaySettingsManager::instance();
        ds.set_display_dim_sec(prev_dim_sec_);
        ds.set_display_sleep_sec(prev_sleep_sec_);
        ds.set_sleep_while_printing(prev_sleep_while_printing_);
        RemoteScreenService::instance().shutdown();
        ConfigTestAccess::data(*Config::get_instance()).erase("remote_screen");
        helix::ui::UpdateQueue::instance().drain();
    }

    void idle_past_sleep_timeout(DisplayManager& mgr) {
        mgr.set_dim_timeout(0);
        lv_display_trigger_activity(nullptr);
        process_lvgl(1500);
        REQUIRE(lv_display_get_inactive_time(nullptr) >= 1000);
        REQUIRE_FALSE(mgr.is_display_sleeping());
    }

  private:
    int prev_dim_sec_ = 0;
    int prev_sleep_sec_ = 0;
    bool prev_sleep_while_printing_ = true;
};

} // namespace

TEST_CASE_METHOD(RemoteViewerSleepFixture, "a remote viewer keeps the display out of sleep",
                 "[application][display][sleep][remote_viewer]") {
    DisplayManager mgr;
    idle_past_sleep_timeout(mgr);

    RemoteScreenService::instance().store()->note_page_view();
    REQUIRE(RemoteScreenService::instance().viewer_active());

    mgr.check_display_sleep();
    CHECK_FALSE(mgr.is_display_sleeping());
}

TEST_CASE_METHOD(RemoteViewerSleepFixture, "with no remote viewer the display sleeps on timeout",
                 "[application][display][sleep][remote_viewer]") {
    // Negative control: the server is up, nobody has fetched a frame.
    DisplayManager mgr;
    idle_past_sleep_timeout(mgr);
    REQUIRE_FALSE(RemoteScreenService::instance().viewer_active());

    mgr.check_display_sleep();
    CHECK(mgr.is_display_sleeping());

    // The overlay is parented to lv_layer_top(), which outlives this never-init()'d manager.
    DisplayManagerTestAccess::restore_display_output(mgr);
}

TEST_CASE_METHOD(RemoteViewerSleepFixture, "a remote viewer arriving wakes a sleeping display",
                 "[application][display][sleep][remote_viewer]") {
    DisplayManager mgr;
    idle_past_sleep_timeout(mgr);
    mgr.check_display_sleep();
    REQUIRE(mgr.is_display_sleeping());

    RemoteScreenService::instance().store()->note_page_view();
    mgr.check_display_sleep();
    CHECK_FALSE(mgr.is_display_sleeping());
    // The wake restarts the idle clock, as a touch does.
    CHECK(lv_display_get_inactive_time(nullptr) < 500);
}

TEST_CASE_METHOD(RemoteViewerSleepFixture,
                 "a remote viewer that stays does not re-wake the display",
                 "[application][display][sleep][remote_viewer]") {
    DisplayManager mgr;
    idle_past_sleep_timeout(mgr);
    RemoteScreenService::instance().store()->note_page_view();
    mgr.check_display_sleep();
    REQUIRE_FALSE(mgr.is_display_sleeping());

    // Asleep by some other path while the same viewer is still watching.
    DisplayManagerTestAccess::enter_sleep(mgr, 1);
    REQUIRE(mgr.is_display_sleeping());
    RemoteScreenService::instance().store()->note_page_view();
    process_lvgl(1500);
    mgr.check_display_sleep();
    CHECK(mgr.is_display_sleeping());

    DisplayManagerTestAccess::restore_display_output(mgr);
}
