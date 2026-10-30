// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_start_collector_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "moonraker_client_mock.h"
#include "print_start_collector.h"
#include "print_start_profile.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "start_macro_injection.h"

#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using helix::PrintStartPhase;
using helix::ui::UpdateQueueTestAccess;
using json = nlohmann::json;

namespace {

const json kInjecting = {{"virtual_sdcard", {{"auto_creator5_start", true}}}};

} // namespace

TEST_CASE("a firmware that starts the print before the file is named by its config",
          "[print][collector][start_macro]") {
    CHECK(helix::firmware_runs_start_macro_before_file(kInjecting));
    CHECK_FALSE(helix::firmware_runs_start_macro_before_file(
        json{{"virtual_sdcard", {{"auto_creator5_start", false}}}}));
    CHECK_FALSE(
        helix::firmware_runs_start_macro_before_file(json{{"virtual_sdcard", json::object()}}));
    CHECK_FALSE(helix::firmware_runs_start_macro_before_file(json::object()));
    CHECK_FALSE(helix::firmware_runs_start_macro_before_file(
        json{{"virtual_sdcard", {{"auto_creator5_start", "True"}}}}));
}

TEST_CASE("discovery keeps whether the firmware starts the print before the file",
          "[print][collector][start_macro]") {
    helix::PrinterDiscovery hw;
    CHECK_FALSE(hw.firmware_runs_start_macro_before_file());
    hw.parse_start_macro_injection(kInjecting);
    CHECK(hw.firmware_runs_start_macro_before_file());
    hw.clear();
    CHECK_FALSE(hw.firmware_runs_start_macro_before_file());
}

namespace {

class StartMacroGateFixture : public LVGLTestFixture {
  public:
    StartMacroGateFixture() {
        state_.init_subjects(false);
        lv_subject_set_int(state_.print_state().get_print_active_subject(), 1);
        client_ = std::make_unique<MoonrakerClientMock>();
        collector_ = std::make_shared<PrintStartCollector>(*client_, state_);
        collector_->set_profile(PrintStartProfile::load_default());
    }

    ~StartMacroGateFixture() override {
        if (collector_->is_active()) {
            collector_->stop();
        }
        collector_.reset();
        client_.reset();
    }

    /// Start a run with the printer reporting @p position before any frame.
    void start(bool injects, uint64_t position) {
        helix::PrinterDiscovery hw;
        hw.parse_start_macro_injection(injects ? kInjecting : json::object());
        state_.set_hardware(hw);
        drain();
        client_->set_object_status("virtual_sdcard", {{"file_position", position}});
        collector_->start();
        drain();
    }

    void stop() {
        collector_->stop();
        drain();
    }

    void report_position(uint64_t position) {
        client_->dispatch_status_update({{"virtual_sdcard", {{"file_position", position}}}});
        drain();
    }

    void drain() {
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    }

    PrintStartPhase phase() {
        return static_cast<PrintStartPhase>(
            lv_subject_get_int(state_.print_state().get_print_start_phase_subject()));
    }

    bool seen_zero() {
        return PrintStartCollectorTestAccess::start_macro_seen_zero(*collector_);
    }

  private:
    PrinterState state_;
    std::unique_ptr<MoonrakerClientMock> client_;
    std::shared_ptr<PrintStartCollector> collector_;
};

} // namespace

TEST_CASE_METHOD(StartMacroGateFixture,
                 "pre-print ends when a firmware-run start macro returns and the file streams",
                 "[print][collector][start_macro]") {
    start(/*injects=*/true, /*position=*/0);
    REQUIRE(wait_until([this] { return seen_zero(); }));

    // The macro, KAMP's purge line included, is still running at position 0.
    report_position(0);
    CHECK(phase() != PrintStartPhase::COMPLETE);

    report_position(1872);
    CHECK(phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(StartMacroGateFixture,
                 "the last print's end position does not hide the next start macro",
                 "[print][collector][start_macro]") {
    // The collector starts before the new file loads, while the position is
    // still where the previous job finished.
    start(/*injects=*/true, /*position=*/1234567);
    CHECK_FALSE(seen_zero());

    report_position(0);
    REQUIRE(seen_zero());
    CHECK(phase() != PrintStartPhase::COMPLETE);

    report_position(1872);
    CHECK(phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(StartMacroGateFixture,
                 "a streaming file says nothing about pre-print without the firmware's start macro",
                 "[print][collector][start_macro]") {
    start(/*injects=*/false, /*position=*/0);
    report_position(0);
    CHECK_FALSE(seen_zero());

    report_position(1872);
    CHECK(phase() != PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(StartMacroGateFixture, "a run that never shows position 0 does not end pre-print",
                 "[print][collector][start_macro]") {
    // A resume from a saved position streams from there, and a file that calls
    // the macro itself stands on that line past 0.
    start(/*injects=*/true, /*position=*/4096);
    report_position(5120);
    CHECK_FALSE(seen_zero());
    CHECK(phase() != PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(StartMacroGateFixture,
                 "a run stopped at position 0 leaves nothing for the next run to complete on",
                 "[print][collector][start_macro]") {
    start(/*injects=*/true, /*position=*/0);
    report_position(0);
    REQUIRE(seen_zero());
    stop();

    // The next run resumes a saved print, so its position is past 0 from the start.
    start(/*injects=*/true, /*position=*/4096);
    CHECK_FALSE(seen_zero());
    report_position(4096);
    CHECK(phase() != PrintStartPhase::COMPLETE);
}
