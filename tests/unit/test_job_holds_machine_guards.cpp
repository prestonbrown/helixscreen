// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_job_holds_machine_guards.cpp
 * @brief Print-state guards that must count Preparing, not just PRINTING/PAUSED.
 *
 * Run with: ./build/bin/helix-tests "[job-holds-machine]"
 *
 * Two consumers of the published `print_lifecycle` subject that used to compare
 * the raw job state against PRINTING (|| PAUSED) and now ask
 * job_holds_machine():
 *
 *  1. FilamentSensorManager's AD5X-IFS idle-unload toast suppression. The
 *     firmware pulls filament back out of the toolhead between prints, so a
 *     head-sensor-empty while nothing is running is noise. A head-empty during
 *     a host-side pre-print block is NOT that, and swallowing it hides a real
 *     failure at the worst possible moment.
 *
 *  2. UpgradeNudge's visibility gate. A user who has just committed to a print
 *     is the last person who wants an upgrade prompt, and a paused print is
 *     still mid-print.
 *
 * Both are driven through the real subject, never by writing
 * print_state_enum directly: the lifecycle is published by
 * PrinterPrintState::publish_lifecycle_state(), which only runs from
 * update_from_status() and set_print_start_state(). Poking the raw job-state
 * subject leaves print_lifecycle stale, and every assertion here would then
 * pass for the wrong reason.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "system/update_checker.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/update_checker_test_access.h"
#include "test_helpers/upgrade_nudge_test_access.h"
#include "upgrade_nudge.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

// Friend shim reaching FilamentSensorManager's private state. Per-TU class (same
// idiom as BypassArmingTestAccess / RunoutScopeTestAccess) to avoid an ODR clash
// with the other suites' shims.

namespace {

/// Drive the published print_lifecycle subject the way production does.
///
/// @param wire_state print_stats.state as Moonraker spells it
/// @param phase      live pre-print phase; IDLE means none is running
///
/// A host-side pre-print block is (standby, phase != IDLE): the printer has not
/// been handed the job yet, so the wire still reads standby while the lifecycle
/// is already Preparing. That combination is exactly what the old
/// `PRINTING || PAUSED` comparison could not express.
void drive_lifecycle(PrinterState& ps, const char* wire_state,
                     PrintStartPhase phase = PrintStartPhase::IDLE) {
    ps.update_from_status(json{{"print_stats", {{"state", wire_state}}}});
    ps.set_print_start_state(phase, "", 0);
    // set_print_start_state defers, and its callback publishes the lifecycle.
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

PrintState published_lifecycle(PrinterState& ps) {
    return ps.get_print_lifecycle();
}

/// AD5X-IFS backend installed as the active AMS backend, one RUNOUT-role
/// toolhead sensor seeded with filament present, and the warning-toast hook
/// captured. Everything the idle-unload suppression reads.
class Ad5xToastFixture : public LVGLTestFixture {
  public:
    PrinterState& printer_state = get_printer_state();
    FilamentSensorManager& mgr = FilamentSensorManager::instance();
    std::vector<std::string> warnings;

    Ad5xToastFixture() {
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);

        AmsState::instance().deinit_subjects();
        AmsState::instance().init_subjects(false);
        AmsState::instance().set_backend(std::make_unique<AmsBackendAd5xIfs>(nullptr, nullptr));

        PostUnloadGraceTestAccess::reset(mgr);
        mgr.discover_sensors({"filament_switch_sensor toolhead_sensor"});
        mgr.set_sensor_role("filament_switch_sensor toolhead_sensor", FilamentSensorRole::RUNOUT);
        // Seed "filament present" so the empty reading below is a real edge.
        mgr.update_from_status(json{{"filament_switch_sensor toolhead_sensor",
                                     {{"filament_detected", true}, {"enabled", true}}}});
        PostUnloadGraceTestAccess::clear_startup_grace(mgr);

        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings.push_back(msg); });
    }

    ~Ad5xToastFixture() override {
        helix::ui::set_test_notification_warning_hook(nullptr);
        PostUnloadGraceTestAccess::reset(mgr);
        AmsState::instance().set_backend(nullptr);
        AmsState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);
    }

    /// Report the toolhead sensor as empty and return how many "filament gone"
    /// toasts that produced. Filtered on the message rather than counting every
    /// warning so an unrelated toast from another subsystem cannot be mistaken
    /// for the one under test.
    size_t report_head_empty() {
        warnings.clear();
        mgr.update_from_status(json{{"filament_switch_sensor toolhead_sensor",
                                     {{"filament_detected", false}, {"enabled", true}}}});
        helix::ui::UpdateQueue::instance().drain();
        size_t n = 0;
        for (const auto& w : warnings) {
            if (w.find("Filament removed") != std::string::npos) {
                ++n;
            }
        }
        return n;
    }
};

/// UpgradeNudge asks UpdateChecker whether anything is worth nudging about
/// before it ever looks at print state, so the gate is unreachable until a
/// release is cached.
class UpgradeNudgeFixture : public LVGLTestFixture {
  public:
    PrinterState& printer_state = get_printer_state();

    UpgradeNudgeFixture() {
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);
        UpdateCheckerTestAccess::seed_available_update(UpdateChecker::instance());
    }

    ~UpgradeNudgeFixture() override {
        UpdateCheckerTestAccess::clear(UpdateChecker::instance());
        helix::ui::UpdateQueue::instance().drain();
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);
    }

    bool visible() const {
        return UpgradeNudgeTestAccess::is_update_visible_now(UpgradeNudge::instance());
    }
};

} // namespace

// ============================================================================
// 1. AD5X-IFS idle-unload toast suppression
// ============================================================================

TEST_CASE_METHOD(Ad5xToastFixture,
                 "AD5X head-empty during a pre-print block is NOT swallowed as an idle unload",
                 "[ams][ad5x][filament-sensor][job-holds-machine]") {
    // Guard against a leaked filament operation from an earlier test: that
    // suppresses the toast through a different arm of the same expression and
    // would make this pass for the wrong reason.
    REQUIRE_FALSE(AmsState::instance().is_filament_operation_active());

    drive_lifecycle(printer_state, "standby", PrintStartPhase::BED_MESH);
    REQUIRE(published_lifecycle(printer_state) == PrintState::Preparing);

    // Reverting the guard to `job_state == PRINTING || job_state == PAUSED`
    // makes this 0: print_stats still reads standby for the whole host-side
    // pre-start block, so the old comparison classifies a Preparing print as
    // "between prints" and eats the runout warning.
    CHECK(report_head_empty() == 1);
}

TEST_CASE_METHOD(Ad5xToastFixture, "AD5X head-empty while idle is swallowed as the firmware unload",
                 "[ams][ad5x][filament-sensor][job-holds-machine]") {
    // The behaviour the suppression exists for, and the negative control that
    // proves the test above is measuring the guard rather than a toast path
    // that always fires.
    drive_lifecycle(printer_state, "standby");
    REQUIRE(published_lifecycle(printer_state) == PrintState::Idle);

    CHECK(report_head_empty() == 0);
}

TEST_CASE_METHOD(Ad5xToastFixture, "AD5X head-empty mid-print is a real runout",
                 "[ams][ad5x][filament-sensor][job-holds-machine]") {
    drive_lifecycle(printer_state, "printing");
    REQUIRE(published_lifecycle(printer_state) == PrintState::Printing);

    CHECK(report_head_empty() == 1);
}

// ============================================================================
// 2. UpgradeNudge visibility gate
// ============================================================================

TEST_CASE_METHOD(UpgradeNudgeFixture, "upgrade nudge is visible when the printer is idle",
                 "[updates][upgrade-nudge][job-holds-machine]") {
    // Establishes that the seam works at all: without this, every "false"
    // below could just be an unseeded UpdateChecker.
    REQUIRE(UpdateChecker::instance().has_update_available());

    drive_lifecycle(printer_state, "standby");
    REQUIRE(published_lifecycle(printer_state) == PrintState::Idle);

    CHECK(visible());
}

TEST_CASE_METHOD(UpgradeNudgeFixture, "upgrade nudge is hidden while a job owns the machine",
                 "[updates][upgrade-nudge][job-holds-machine]") {
    REQUIRE(UpdateChecker::instance().has_update_available());

    SECTION("printing") {
        // The one arm the old `== PRINTING` guard already covered.
        drive_lifecycle(printer_state, "printing");
        REQUIRE(published_lifecycle(printer_state) == PrintState::Printing);
        CHECK_FALSE(visible());
    }

    SECTION("paused") {
        // Deliberate behaviour change: a paused print is still mid-print, and
        // the old guard had no PAUSED arm at all. Reverting to
        // `job_state == PRINTING || job_state == PAUSED` keeps this passing,
        // but reverting to the original `== PRINTING` does not.
        drive_lifecycle(printer_state, "paused");
        REQUIRE(published_lifecycle(printer_state) == PrintState::Paused);
        CHECK_FALSE(visible());
    }

    SECTION("preparing") {
        // The arm no PrintJobState comparison can express. Reverting the guard
        // to either historical form makes this visible() == true, because
        // print_stats.state is still standby throughout the pre-print block.
        drive_lifecycle(printer_state, "standby", PrintStartPhase::HOMING);
        REQUIRE(published_lifecycle(printer_state) == PrintState::Preparing);
        CHECK_FALSE(visible());
    }
}

TEST_CASE_METHOD(UpgradeNudgeFixture, "upgrade nudge returns after the print finishes",
                 "[updates][upgrade-nudge][job-holds-machine]") {
    // job_holds_machine() is false for the terminal states, so the nudge must
    // come back rather than staying suppressed for the rest of the session.
    drive_lifecycle(printer_state, "printing");
    REQUIRE_FALSE(visible());

    drive_lifecycle(printer_state, "complete");
    REQUIRE(published_lifecycle(printer_state) == PrintState::Complete);
    CHECK(visible());
}
