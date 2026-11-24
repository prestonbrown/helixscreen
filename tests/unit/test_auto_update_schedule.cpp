// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_auto_update_schedule.cpp
 * @brief Automatic updates (#1304): the scheduling rule and UpdateChecker's tick.
 *
 * Run with: ./build/bin/helix-tests "[auto_update]"
 */

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "system/auto_update_schedule.h"
#include "system/update_checker.h"
#include "system/update_rollback.h"
#include "system_settings_manager.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/update_checker_test_access.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::update::AutoUpdateDecision;
using helix::update::AutoUpdateInputs;
using helix::update::AutoUpdateMode;
using helix::update::decide_auto_update;
using helix::update::effective_auto_update_mode;
using helix::update::in_nightly_window;
using helix::update::kAutoUpdateIdleSeconds;
using helix::update::NightlyWindow;
using json = nlohmann::json;

namespace {

constexpr int kThreeAm = 3 * 60;
constexpr int kNoon = 12 * 60;

/// Every condition for an overnight install met; each test breaks one.
AutoUpdateInputs ready_to_install() {
    AutoUpdateInputs in;
    in.mode = AutoUpdateMode::InstallOvernight;
    in.update_available = true;
    in.local_minute_of_day = kThreeAm + 10;
    in.lifecycle = PrintState::Idle;
    in.plr_pending = false;
    in.seconds_since_touch = kAutoUpdateIdleSeconds;
    return in;
}

} // namespace

TEST_CASE("Auto-update window: membership, edges and midnight wrap", "[auto_update]") {
    const NightlyWindow three_am{kThreeAm, 60};
    CHECK_FALSE(in_nightly_window(kThreeAm - 1, three_am));
    CHECK(in_nightly_window(kThreeAm, three_am));
    CHECK(in_nightly_window(kThreeAm + 59, three_am));
    CHECK_FALSE(in_nightly_window(kThreeAm + 60, three_am));
    CHECK_FALSE(in_nightly_window(kNoon, three_am));

    const NightlyWindow wraps{23 * 60 + 30, 60}; // 23:30 to 00:30
    CHECK(in_nightly_window(23 * 60 + 45, wraps));
    CHECK(in_nightly_window(15, wraps));
    CHECK_FALSE(in_nightly_window(30, wraps));
    CHECK_FALSE(in_nightly_window(23 * 60 + 29, wraps));

    CHECK_FALSE(in_nightly_window(kThreeAm, NightlyWindow{kThreeAm, 0}));
    CHECK(in_nightly_window(kNoon, NightlyWindow{0, 24 * 60}));
    CHECK_FALSE(in_nightly_window(-1, three_am));
    CHECK_FALSE(in_nightly_window(24 * 60, three_am));
}

TEST_CASE("Auto-update mode: Install overnight runs only where it is offered", "[auto_update]") {
    const int overnight = static_cast<int>(AutoUpdateMode::InstallOvernight);
    CHECK(effective_auto_update_mode(overnight, true) == AutoUpdateMode::InstallOvernight);
    CHECK(effective_auto_update_mode(overnight, false) == AutoUpdateMode::DownloadAndAsk);
    for (bool offered : {true, false}) {
        CHECK(effective_auto_update_mode(static_cast<int>(AutoUpdateMode::Off), offered) ==
              AutoUpdateMode::Off);
        CHECK(effective_auto_update_mode(static_cast<int>(AutoUpdateMode::DownloadAndAsk),
                                         offered) == AutoUpdateMode::DownloadAndAsk);
        CHECK(effective_auto_update_mode(-1, offered) == AutoUpdateMode::DownloadAndAsk);
        CHECK(effective_auto_update_mode(7, offered) == AutoUpdateMode::DownloadAndAsk);
    }
}

TEST_CASE("Auto-update decision: installs only when every condition holds", "[auto_update]") {
    CHECK(decide_auto_update(ready_to_install()) == AutoUpdateDecision::InstallNow);

    SECTION("nothing available waits in every mode") {
        for (auto mode : {AutoUpdateMode::InstallOvernight, AutoUpdateMode::DownloadAndAsk,
                          AutoUpdateMode::Off}) {
            auto in = ready_to_install();
            in.mode = mode;
            in.update_available = false;
            CHECK(decide_auto_update(in) == AutoUpdateDecision::Wait);
        }
    }
    SECTION("Off never acts") {
        auto in = ready_to_install();
        in.mode = AutoUpdateMode::Off;
        CHECK(decide_auto_update(in) == AutoUpdateDecision::Wait);
    }
    SECTION("Download and ask offers, whatever the hour or the printer") {
        auto in = ready_to_install();
        in.mode = AutoUpdateMode::DownloadAndAsk;
        CHECK(decide_auto_update(in) == AutoUpdateDecision::Download);
        in.local_minute_of_day = kNoon;
        in.lifecycle = PrintState::Printing;
        CHECK(decide_auto_update(in) == AutoUpdateDecision::Download);
    }
    SECTION("outside the window waits, even when idle") {
        auto in = ready_to_install();
        in.local_minute_of_day = kNoon;
        CHECK(decide_auto_update(in) == AutoUpdateDecision::Wait);
        in.local_minute_of_day = kThreeAm - 1;
        CHECK(decide_auto_update(in) == AutoUpdateDecision::Wait);
    }
    SECTION("a job holding the machine skips tonight") {
        for (auto state : {PrintState::Preparing, PrintState::Printing, PrintState::Paused}) {
            auto in = ready_to_install();
            in.lifecycle = state;
            CHECK(decide_auto_update(in) == AutoUpdateDecision::RetryTomorrow);
        }
    }
    SECTION("a finished, cancelled or failed job does not block") {
        for (auto state : {PrintState::Complete, PrintState::Cancelled, PrintState::Error}) {
            auto in = ready_to_install();
            in.lifecycle = state;
            CHECK(decide_auto_update(in) == AutoUpdateDecision::InstallNow);
        }
    }
    SECTION("a pending power-loss resume skips tonight") {
        auto in = ready_to_install();
        in.plr_pending = true;
        CHECK(decide_auto_update(in) == AutoUpdateDecision::RetryTomorrow);
    }
    SECTION("a recent touch waits inside the window") {
        auto in = ready_to_install();
        in.seconds_since_touch = kAutoUpdateIdleSeconds - 1;
        CHECK(decide_auto_update(in) == AutoUpdateDecision::Wait);
    }
    SECTION("busy printer outranks a recent touch") {
        auto in = ready_to_install();
        in.seconds_since_touch = 0;
        in.lifecycle = PrintState::Printing;
        CHECK(decide_auto_update(in) == AutoUpdateDecision::RetryTomorrow);
    }
}

namespace {

/// UpdateChecker's tick against the real settings, print state and cached release,
/// with the clock and touch reading supplied by the test and the installer hooked.
class AutoUpdateTickFixture : public LVGLTestFixture {
  public:
    PrinterState& printer_state = get_printer_state();
    UpdateChecker& checker = UpdateChecker::instance();
    int installs = 0;

    AutoUpdateTickFixture() {
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);
        SystemSettingsManager::instance().init_subjects();
        helix::update::set_auto_rollback_supported_for_test(true);
        SystemSettingsManager::instance().set_auto_update_mode(
            static_cast<int>(AutoUpdateMode::InstallOvernight));
        UpdateCheckerTestAccess::seed_available_update(checker);
        UpdateCheckerTestAccess::set_auto_install_hook(checker, [this] { ++installs; });
    }

    ~AutoUpdateTickFixture() override {
        UpdateCheckerTestAccess::set_auto_install_hook(checker, nullptr);
        helix::update::set_auto_rollback_supported_for_test(std::nullopt);
        SystemSettingsManager::instance().set_auto_update_mode(
            static_cast<int>(AutoUpdateMode::DownloadAndAsk));
        checker.clear_cache();
        helix::ui::UpdateQueue::instance().drain();
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);
    }

    static UpdateChecker::AutoUpdateNow at(int minute, int64_t idle_s = kAutoUpdateIdleSeconds) {
        return UpdateChecker::AutoUpdateNow{minute, idle_s};
    }

    void drive_lifecycle(const char* wire_state) {
        printer_state.update_from_status(json{{"print_stats", {{"state", wire_state}}}});
        printer_state.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

} // namespace

TEST_CASE_METHOD(AutoUpdateTickFixture, "Auto-update tick: installs once in the window when idle",
                 "[auto_update]") {
    CHECK(checker.auto_update_tick(at(kNoon)) == AutoUpdateDecision::Wait);
    CHECK(checker.auto_update_tick(at(kThreeAm, 10)) == AutoUpdateDecision::Wait);
    CHECK(installs == 0);

    CHECK(checker.auto_update_tick(at(kThreeAm + 5)) == AutoUpdateDecision::InstallNow);
    CHECK(installs == 1);

    // The same release is never retried unprompted, even with the window still open.
    CHECK(checker.auto_update_tick(at(kThreeAm + 10)) == AutoUpdateDecision::Wait);
    CHECK(installs == 1);
}

TEST_CASE_METHOD(AutoUpdateTickFixture,
                 "Auto-update tick: a stored Install overnight asks where rollback is missing",
                 "[auto_update]") {
    helix::update::set_auto_rollback_supported_for_test(false);
    CHECK(checker.auto_update_tick(at(kThreeAm + 5)) == AutoUpdateDecision::Download);
    CHECK(installs == 0);
}

TEST_CASE_METHOD(AutoUpdateTickFixture, "Auto-update tick: a print in progress waits for tomorrow",
                 "[auto_update]") {
    drive_lifecycle("printing");
    REQUIRE(printer_state.print_state().get_print_lifecycle() == PrintState::Printing);
    CHECK(checker.auto_update_tick(at(kThreeAm + 5)) == AutoUpdateDecision::RetryTomorrow);
    CHECK(installs == 0);
}

TEST_CASE_METHOD(AutoUpdateTickFixture, "Auto-update tick: a pending power-loss resume waits",
                 "[auto_update]") {
    lv_subject_set_int(printer_state.print_state().get_plr_snapshot_valid_subject(), 1);
    CHECK(checker.auto_update_tick(at(kThreeAm + 5)) == AutoUpdateDecision::RetryTomorrow);
    CHECK(installs == 0);
}

TEST_CASE_METHOD(AutoUpdateTickFixture, "Auto-update tick: only a qualifying release installs",
                 "[auto_update]") {
    SECTION("unsigned release") {
        UpdateCheckerTestAccess::seed_available_update(checker, "9.9.9", "", false);
    }
    SECTION("no release") {
        checker.clear_cache();
    }
    SECTION("mode is Download and ask") {
        SystemSettingsManager::instance().set_auto_update_mode(
            static_cast<int>(AutoUpdateMode::DownloadAndAsk));
    }
    SECTION("mode is Off") {
        SystemSettingsManager::instance().set_auto_update_mode(
            static_cast<int>(AutoUpdateMode::Off));
    }
    CHECK(checker.auto_update_tick(at(kThreeAm + 5)) != AutoUpdateDecision::InstallNow);
    CHECK(installs == 0);
}
