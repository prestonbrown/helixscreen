// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_snapmaker_screws_tilt_collector.cpp
 * @brief AutoScrewsTiltCollector driven through the mock client
 *
 * The collector chains five dependent RPCs against the firmware, and the
 * invariant these tests pin is the exit discipline: EVERY terminal path -
 * success, a failed command anywhere in the chain, a refused plate check -
 * releases the firmware's SCREWS_TILT_ADJUST state through the gated
 * AUTO_SCREWS_TILT_ADJUST_EXIT. A leaked state makes unrelated filament
 * operations refuse forever.
 *
 * Also pinned: the command order, the plate gate's fail-closed refusal
 * (DETECT_BED_PLATE PRESENCE=0 is the verdict itself - its not-removed error
 * refuses with instructions, and so does any error we cannot classify), the
 * dialect dispatch in calculate_screws_tilt(), and connect-time
 * reconciliation of a leftover state.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "core/update_queue.h"
#include "snapmaker_screws_tilt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

class AutoScrewsCollectorTestFixture : public LVGLTestFixture {
  public:
    AutoScrewsCollectorTestFixture()
        : mock_client_(MoonrakerClientMock::PrinterType::GENERIC_COREXY) {
        state_.init_subjects(false);
        // execute_gcode() halted gate would otherwise reject every command.
        state_.set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);

        // The U1's module as the sole screws-tilt object is what makes the
        // dialect SnapmakerAuto.
        mock_client_.set_additional_objects({"auto_screws_tilt_adjust"});
        // The firmware holds the calibration state while the collector runs;
        // the gated exit asks about exactly this.
        mock_client_.set_object_status("machine_state_manager", {{"main_state", 8}});
    }

    ~AutoScrewsCollectorTestFixture() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        api_.reset();
    }

    /// The U1's probed points: a clearly tilted bed.
    void set_module_results() {
        mock_client_.set_object_status("auto_screws_tilt_adjust", {{"target_z", 0.4},
                                                                   {"base_point1", 0.15},
                                                                   {"base_point2", 0.55},
                                                                   {"base_point3", 0.325},
                                                                   {"base_point4", 0.575}});
    }

    void drop_held_state() {
        mock_client_.set_object_status("machine_state_manager", json::object());
    }

    [[nodiscard]] bool sent(const std::string& command) const {
        const auto& history = mock_client_.gcode_script_history();
        return std::find(history.begin(), history.end(), command) != history.end();
    }

    [[nodiscard]] bool sent_containing(const std::string& fragment) const {
        for (const auto& script : mock_client_.gcode_script_history()) {
            if (script.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    void start_collection() {
        api_->advanced().calculate_screws_tilt(
            [this](const std::vector<ScrewTiltResult>& screws) {
                captured_screw_count_ = screws.size();
                result_received_.store(true);
            },
            [this](const MoonrakerError& err) {
                captured_error_ = err.message;
                error_received_.store(true);
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;

    std::atomic<bool> result_received_{false};
    std::atomic<bool> error_received_{false};
    size_t captured_screw_count_ = 0;
    std::string captured_error_;
};

// ============================================================================
// The exit discipline
// ============================================================================

TEST_CASE_METHOD(AutoScrewsCollectorTestFixture,
                 "auto screws collector sends the gated exit on success",
                 "[calibration][screws_tilt][auto_screws]") {
    REQUIRE(mock_client_.hardware().screws_tilt_dialect() == ScrewsTiltDialect::SnapmakerAuto);
    set_module_results();

    start_collection();

    SECTION("the five commands run in order, then the state-gated exit") {
        REQUIRE(result_received_.load());
        REQUIRE_FALSE(error_received_.load());
        REQUIRE(captured_screw_count_ == 4);

        const auto& history = mock_client_.gcode_script_history();
        REQUIRE(history.size() == 5);
        REQUIRE(history[0] == snapmaker::screws_tilt::kCmdEntry);
        REQUIRE(history[1] == snapmaker::screws_tilt::kCmdHoming);
        // The plate gate is DETECT_BED_PLATE PRESENCE=0 itself: success means
        // the sheet is off, so the chain walks straight into probing.
        REQUIRE(history[2] == snapmaker::screws_tilt::kCmdDetectBedPlate);
        REQUIRE(history[3] == snapmaker::screws_tilt::kCmdProbeReferencePoints);
        REQUIRE(history[4] == snapmaker::screws_tilt::kCmdExit);
        REQUIRE_FALSE(sent_containing("AUTO_SCREWS_TILT_ADJUST_DETECT_PLATE"));
    }

    SECTION("no exit when the firmware state is not ours to leave") {
        // Re-run with the state object unreadable: the gated exit must hold
        // its fire rather than send the command the firmware macro would
        // throw on.
        mock_client_.clear_gcode_script_history();
        result_received_.store(false);
        drop_held_state();
        set_module_results();

        start_collection();

        REQUIRE(result_received_.load());
        REQUIRE_FALSE(sent(snapmaker::screws_tilt::kCmdExit));
    }
}

TEST_CASE_METHOD(AutoScrewsCollectorTestFixture,
                 "auto screws collector sends the gated exit when any step fails",
                 "[calibration][screws_tilt][auto_screws]") {
    set_module_results();

    const struct {
        const char* command;
        MoonrakerErrorType type;
    } steps[] = {
        {snapmaker::screws_tilt::kCmdEntry, MoonrakerErrorType::JSON_RPC_ERROR},
        {snapmaker::screws_tilt::kCmdHoming, MoonrakerErrorType::TIMEOUT},
        {snapmaker::screws_tilt::kCmdDetectBedPlate, MoonrakerErrorType::JSON_RPC_ERROR},
        {snapmaker::screws_tilt::kCmdProbeReferencePoints, MoonrakerErrorType::JSON_RPC_ERROR},
    };

    for (const auto& step : steps) {
        CAPTURE(step.command);
        mock_client_.clear_gcode_script_history();
        error_received_.store(false);
        captured_error_.clear();

        mock_client_.force_next_gcode_error(step.type, "step failed", step.command);

        start_collection();

        REQUIRE(error_received_.load());
        REQUIRE_FALSE(result_received_.load());
        REQUIRE(captured_error_.find(step.command) != std::string::npos);

        // The run failed, and the firmware state is still released.
        REQUIRE(sent(snapmaker::screws_tilt::kCmdExit));

        // A failure ends the chain: nothing runs past the failed command
        // except the exit.
        const bool failed_before_probe =
            std::string(step.command) != snapmaker::screws_tilt::kCmdProbeReferencePoints;
        if (failed_before_probe) {
            REQUIRE_FALSE(sent(snapmaker::screws_tilt::kCmdProbeReferencePoints));
        }
    }
}

// ============================================================================
// The plate gate: fail closed, never probe through the sheet
// ============================================================================

TEST_CASE_METHOD(AutoScrewsCollectorTestFixture,
                 "auto screws plate gate classifies the DETECT_BED_PLATE verdict",
                 "[calibration][screws_tilt][auto_screws]") {
    set_module_results();

    SECTION("the not-removed error refuses with removal instructions") {
        mock_client_.force_next_gcode_error(
            MoonrakerErrorType::JSON_RPC_ERROR,
            "Klippy Host Error: '0003-0530-0000-0011: The plate has not been removed'",
            "DETECT_BED_PLATE");

        start_collection();

        REQUIRE(error_received_.load());
        REQUIRE_FALSE(result_received_.load());
        REQUIRE(captured_error_.find("Remove the PEI sheet") != std::string::npos);
        REQUIRE_FALSE(sent(snapmaker::screws_tilt::kCmdProbeReferencePoints));
        REQUIRE(sent(snapmaker::screws_tilt::kCmdExit));
    }

    SECTION("an error we cannot classify also refuses - it is not evidence the sheet is off") {
        mock_client_.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                            "inductance coil fault", "DETECT_BED_PLATE");

        start_collection();

        REQUIRE(error_received_.load());
        REQUIRE_FALSE(result_received_.load());
        REQUIRE(captured_error_.find("DETECT_BED_PLATE") != std::string::npos);
        REQUIRE(captured_error_.find("inductance coil fault") != std::string::npos);
        REQUIRE_FALSE(captured_error_.find("Remove the PEI sheet") != std::string::npos);
        REQUIRE_FALSE(sent(snapmaker::screws_tilt::kCmdProbeReferencePoints));
        REQUIRE(sent(snapmaker::screws_tilt::kCmdExit));
    }
}

// ============================================================================
// Dialect dispatch in calculate_screws_tilt()
// ============================================================================

TEST_CASE_METHOD(AutoScrewsCollectorTestFixture,
                 "calculate_screws_tilt dispatches on the screws-tilt dialect",
                 "[calibration][screws_tilt][auto_screws]") {
    SECTION("SnapmakerAuto runs the five-command sequence") {
        REQUIRE(mock_client_.hardware().screws_tilt_dialect() == ScrewsTiltDialect::SnapmakerAuto);
        set_module_results();

        start_collection();

        REQUIRE(result_received_.load());
        REQUIRE(sent(snapmaker::screws_tilt::kCmdEntry));
        REQUIRE_FALSE(sent_containing("SCREWS_TILT_CALCULATE"));
    }

    SECTION("Standard sends SCREWS_TILT_CALCULATE and no auto commands") {
        // A printer without the U1's module: the default mock hardware.
        MoonrakerClientMock stock_client(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
        PrinterState stock_state;
        stock_state.init_subjects(false);
        stock_state.set_klippy_state_sync(helix::KlippyState::READY);
        MoonrakerAPI stock_api(stock_client, stock_state);
        REQUIRE(stock_client.hardware().screws_tilt_dialect() == ScrewsTiltDialect::Standard);

        std::atomic<bool> stock_done{false};
        stock_api.advanced().calculate_screws_tilt(
            [&](const std::vector<ScrewTiltResult>&) { stock_done.store(true); },
            [](const MoonrakerError&) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // The standard command may carry probe preparation, so match on the
        // command appearing in a script rather than an exact history entry.
        bool saw_standard = false;
        for (const auto& script : stock_client.gcode_script_history()) {
            if (script.find("SCREWS_TILT_CALCULATE") != std::string::npos) {
                saw_standard = true;
            }
            REQUIRE(script.find("AUTO_SCREWS_TILT_ADJUST") == std::string::npos);
        }
        REQUIRE(saw_standard);
    }
}

// ============================================================================
// Connect-time reconciliation of a leftover state
// ============================================================================

TEST_CASE_METHOD(AutoScrewsCollectorTestFixture,
                 "reconcile_on_connect clears stale states and leaves in-flight ones",
                 "[calibration][screws_tilt][auto_screws]") {
    const json held = {{"machine_state_manager", {{"main_state", 8}}}};

    SECTION("a stale probe step gets the wizard's own exit, not a bare state restore") {
        mock_client_.set_object_status("machine_state_manager", {{"main_state", 8}});
        mock_client_.set_object_status("auto_screws_tilt_adjust", {{"probe_step", "adjust_idle"}});

        snapmaker::screws_tilt::reconcile_on_connect(mock_client_, held);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // The bare EXIT_TO_IDLE form leaves idle_timeout at the wizard's
        // pause value, so heaters and steppers never idle out. The macro
        // exit restores the timeout, clears probe_step and lifts Z as well.
        REQUIRE(sent(snapmaker::screws_tilt::kCmdExit));
        REQUIRE_FALSE(sent_containing("EXIT_TO_IDLE REQ_FROM_STATE"));
    }

    SECTION("work in flight is left to its driver") {
        mock_client_.set_object_status("machine_state_manager", {{"main_state", 8}});
        mock_client_.set_object_status("auto_screws_tilt_adjust",
                                       {{"probe_step", "adjust_probing"}});

        snapmaker::screws_tilt::reconcile_on_connect(mock_client_, held);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        REQUIRE(mock_client_.gcode_script_history().empty());
    }

    SECTION("a state that is not held sends nothing") {
        const json idle = {{"machine_state_manager", {{"main_state", 0}}}};
        snapmaker::screws_tilt::reconcile_on_connect(mock_client_, idle);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(mock_client_.gcode_script_history().empty());
    }

    SECTION("no machine_state_manager in the snapshot sends nothing") {
        snapmaker::screws_tilt::reconcile_on_connect(mock_client_, json::object());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(mock_client_.gcode_script_history().empty());
    }
}
