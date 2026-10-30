// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_start_collector_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "moonraker_client_mock.h"
#include "print_start_collector.h"
#include "print_start_profile.h"
#include "printer_state.h"
#include "start_macro_injection.h"

#include "hv/json.hpp"

#include <memory>

#include "../catch_amalgamated.hpp"

using helix::PrinterState;
using helix::PrintStartPhase;
using helix::ui::UpdateQueueTestAccess;
using json = nlohmann::json;

TEST_CASE("a firmware that starts the print before the file is named by its config",
          "[print][collector][start_macro]") {
    CHECK(helix::firmware_runs_start_macro_before_file(
        json{{"virtual_sdcard", {{"auto_creator5_start", true}}}}));
    CHECK_FALSE(helix::firmware_runs_start_macro_before_file(
        json{{"virtual_sdcard", {{"auto_creator5_start", false}}}}));
    CHECK_FALSE(
        helix::firmware_runs_start_macro_before_file(json{{"virtual_sdcard", json::object()}}));
    CHECK_FALSE(helix::firmware_runs_start_macro_before_file(json::object()));
    CHECK_FALSE(helix::firmware_runs_start_macro_before_file(
        json{{"virtual_sdcard", {{"auto_creator5_start", "True"}}}}));
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

    void start(bool injects, uint64_t position) {
        client_->set_config_settings_section("virtual_sdcard",
                                             {{"auto_creator5_start", injects}});
        client_->set_object_status("virtual_sdcard", {{"file_position", position}});
        collector_->start();
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

    bool armed() {
        return PrintStartCollectorTestAccess::start_macro_gate_armed(*collector_);
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
    REQUIRE(wait_until([this] { return armed(); }));

    // The macro, KAMP's purge line included, is still running at position 0.
    report_position(0);
    CHECK(phase() != PrintStartPhase::COMPLETE);

    report_position(1872);
    CHECK(phase() == PrintStartPhase::COMPLETE);
    CHECK_FALSE(armed());
}

TEST_CASE_METHOD(StartMacroGateFixture,
                 "a streaming file says nothing about pre-print without the firmware's start macro",
                 "[print][collector][start_macro]") {
    start(/*injects=*/false, /*position=*/0);
    CHECK_FALSE(wait_until([this] { return armed(); }, 200));

    report_position(1872);
    CHECK(phase() != PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(StartMacroGateFixture,
                 "a file already streaming at start does not arm the start-macro gate",
                 "[print][collector][start_macro]") {
    // The file calls the macro itself, so the position stands on that line.
    start(/*injects=*/true, /*position=*/4096);
    CHECK_FALSE(wait_until([this] { return armed(); }, 200));

    report_position(5120);
    CHECK(phase() != PrintStartPhase::COMPLETE);
}
