// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_sleep_while_printing.cpp
 * @brief sleep_while_printing=false must inhibit sleep entry for the WHOLE job.
 *
 * Run with: ./build/bin/helix-tests "[sleep-while-printing]"
 *
 * DisplayManager::check_display_sleep() asks job_holds_machine() on the
 * published print_lifecycle rather than comparing print_stats.state. The
 * difference is the pre-print block: a user who turned sleep-while-printing off
 * wants the screen up through homing, heating and meshing — which is exactly
 * when they are standing over the printer watching — and print_stats still
 * reads "standby" for all of it, so no comparison against the raw job state can
 * see it.
 *
 * The lifecycle is driven through PrinterState's real publish path
 * (update_from_status / set_print_start_state); writing print_state_enum
 * directly leaves print_lifecycle stale and every assertion here would pass for
 * the wrong reason.
 */

#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "lvgl_test_fixture.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "test_helpers/display_manager_test_access.h"
#include "test_helpers/printer_state_test_access.h"

#include "../../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::DisplaySettingsManager;
using json = nlohmann::json;

namespace {

/// A one-second sleep timeout, no dim stage, and sleep-while-printing turned
/// off — the reporter's configuration reduced to the smallest thing that can
/// reach the inhibit branch. Previous settings are restored on teardown because
/// DisplaySettingsManager is a process singleton.
class SleepWhilePrintingFixture : public LVGLTestFixture {
  public:
    helix::PrinterState& printer_state = get_printer_state();

    SleepWhilePrintingFixture() {
        helix::PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);

        auto& ds = DisplaySettingsManager::instance();
        prev_sleep_while_printing_ = ds.get_sleep_while_printing();
        prev_dim_sec_ = ds.get_display_dim_sec();
        prev_sleep_sec_ = ds.get_display_sleep_sec();

        // Dim first: the Sleep>=Dim coupling clamps a sleep shorter than dim on
        // devices that dim or run a screensaver, and equal values never clamp.
        ds.set_display_dim_sec(1);
        ds.set_display_sleep_sec(1);
        ds.set_sleep_while_printing(false);
    }

    ~SleepWhilePrintingFixture() override {
        auto& ds = DisplaySettingsManager::instance();
        ds.set_display_dim_sec(prev_dim_sec_);
        ds.set_display_sleep_sec(prev_sleep_sec_);
        ds.set_sleep_while_printing(prev_sleep_while_printing_);
        helix::ui::UpdateQueue::instance().drain();
        helix::PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);
    }

    /// Drive the published print_lifecycle subject the way production does.
    /// A live pre-print phase with print_stats still at "standby" is the
    /// host-side pre-start block; it derives to Preparing.
    void drive_lifecycle(const char* wire_state,
                         helix::PrintStartPhase phase = helix::PrintStartPhase::IDLE) {
        printer_state.update_from_status(json{{"print_stats", {{"state", wire_state}}}});
        printer_state.set_print_start_state(phase, "", 0);
        // set_print_start_state defers; its callback is what publishes.
        process_lvgl(10);
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    PrintState lifecycle() const {
        return printer_state.get_print_lifecycle();
    }

    /// Park a fresh manager past the sleep timeout with the dim stage disabled,
    /// so the only branch left in check_display_sleep() is sleep entry.
    void idle_past_sleep_timeout(DisplayManager& mgr) {
        mgr.set_dim_timeout(0);
        lv_display_trigger_activity(nullptr);
        process_lvgl(1500);
        REQUIRE(lv_display_get_inactive_time(nullptr) >= 1000);
        REQUIRE_FALSE(mgr.is_display_sleeping());
    }

  private:
    bool prev_sleep_while_printing_ = true;
    int prev_dim_sec_ = 0;
    int prev_sleep_sec_ = 0;
};

} // namespace

TEST_CASE_METHOD(SleepWhilePrintingFixture, "sleep is inhibited during the pre-print block",
                 "[application][display][sleep][sleep-while-printing][job-holds-machine]") {
    drive_lifecycle("standby", helix::PrintStartPhase::HOMING);
    REQUIRE(lifecycle() == PrintState::Preparing);

    DisplayManager mgr;
    idle_past_sleep_timeout(mgr);

    mgr.check_display_sleep();

    // Reverting the guard to `job_state == PRINTING || job_state == PAUSED`
    // fails both of these: print_stats reads "standby" for the whole host-side
    // pre-start block, so the old comparison sees an idle printer and the
    // screen goes dark on top of the user watching it home.
    CHECK_FALSE(mgr.is_display_sleeping());
    // The inhibit also resets LVGL's idle clock, so sleep does not fire the
    // instant the print ends.
    CHECK(lv_display_get_inactive_time(nullptr) < 500);
}

TEST_CASE_METHOD(SleepWhilePrintingFixture, "sleep is inhibited while paused",
                 "[application][display][sleep][sleep-while-printing][job-holds-machine]") {
    drive_lifecycle("paused");
    REQUIRE(lifecycle() == PrintState::Paused);

    DisplayManager mgr;
    idle_past_sleep_timeout(mgr);

    mgr.check_display_sleep();
    CHECK_FALSE(mgr.is_display_sleeping());
}

TEST_CASE_METHOD(SleepWhilePrintingFixture, "an idle printer still sleeps on timeout",
                 "[application][display][sleep][sleep-while-printing][job-holds-machine]") {
    // Negative control. Without it, a check_display_sleep() that never sleeps
    // for any reason would satisfy the two tests above.
    drive_lifecycle("standby");
    REQUIRE(lifecycle() == PrintState::Idle);

    DisplayManager mgr;
    idle_past_sleep_timeout(mgr);

    mgr.check_display_sleep();
    CHECK(mgr.is_display_sleeping());

    // Drop the software sleep overlay. It is parented to lv_layer_top(), which
    // outlives the manager — and ~DisplayManager() short-circuits because this
    // instance was never init()'d, so nothing else removes it.
    DisplayManagerTestAccess::restore_display_output(mgr);
}

TEST_CASE_METHOD(SleepWhilePrintingFixture,
                 "sleep_while_printing=true lets an active print sleep normally",
                 "[application][display][sleep][sleep-while-printing][job-holds-machine]") {
    // The setting is what gates the whole branch; job_holds_machine() must not
    // start inhibiting sleep for users who never asked for it.
    DisplaySettingsManager::instance().set_sleep_while_printing(true);

    drive_lifecycle("printing");
    REQUIRE(lifecycle() == PrintState::Printing);

    DisplayManager mgr;
    idle_past_sleep_timeout(mgr);

    mgr.check_display_sleep();
    CHECK(mgr.is_display_sleeping());

    DisplayManagerTestAccess::restore_display_output(mgr);
}
