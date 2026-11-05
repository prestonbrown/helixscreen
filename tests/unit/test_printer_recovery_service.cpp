// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// PrinterRecoveryService's chain: an accepted printer.firmware_restart is
// verified against printer.info before it counts as recovered, and the local
// helix-recover.sh runs at most once when Klippy fails to come back.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scripted_recovery_api.h"
#include "app_globals.h"
#include "core/http_executor.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_recovery_service.h"
#include "printer_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr RecoveryTiming kFastTiming{/*poll_interval_ms=*/100, /*verify_window_ms=*/1000,
                                     /*chain_deadline_ms=*/3000, /*script_timeout_ms=*/5000};

const KlippyStatus kKey298{"error",
                           "Internal error during connect: !{\"code\": \"key298\", "
                           "\"msg\": \"Can not update MCU rpi config as it is shutdown\"}"};

class RecoveryFixture : public LVGLTestFixture {
  public:
    RecoveryFixture()
        : client_(
              std::make_unique<MoonrakerClientMock>(MoonrakerClientMock::PrinterType::VORON_24)),
          dir_(std::filesystem::temp_directory_path() /
               ("helix_recovery_" + std::to_string(::getpid()))) {
        get_printer_state().init_subjects(false);
        api_ = std::make_unique<ScriptedRecoveryApi>(*client_, get_printer_state());
        std::filesystem::create_directories(dir_);
        detail::local_recovery_script_override_ref() = (dir_ / "absent.sh").string();
    }

    ~RecoveryFixture() override {
        detail::local_recovery_script_override_ref().clear();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// Install a helix-recover.sh that records each run and exits with @p rc.
    /// Install a helix-recover.sh that records each run, then runs @p tail
    /// (its exit, or what it does instead).
    void install_script(int rc, const std::string& tail = "") {
        const auto script = dir_ / "helix-recover.sh";
        std::ofstream(script) << "#!/bin/sh\necho run >> '" << runs_file().string() << "'\n"
                              << (tail.empty() ? "exit " + std::to_string(rc) : tail) << "\n";
        ::chmod(script.c_str(), 0755);
        detail::local_recovery_script_override_ref() = script.string();
    }

    int script_runs() const {
        std::ifstream in(runs_file());
        int n = 0;
        for (std::string line; std::getline(in, line);)
            ++n;
        return n;
    }

    void recover(RecoveryTiming timing = kFastTiming) {
        PrinterRecoveryService(api_.get(), timing)
            .recover([this](RecoveryOutcome o) { outcome_ = o; },
                     [this](const MoonrakerError& e) { error_ = e; });
    }

    bool finished() const {
        return outcome_.has_value() || error_.has_value();
    }

    /// Run the chain to its end. The virtual clock runs ahead while only the
    /// chain's timers are pending, and moves with real time while
    /// helix-recover.sh runs on the slow lane, so a script's real duration still
    /// counts against the chain's virtual deadline.
    void run_to_end() {
        using Clock = std::chrono::steady_clock;
        constexpr int kVirtualStepMs = 20;
        const auto give_up = Clock::now() + std::chrono::seconds(10);
        auto last = Clock::now();
        while (!finished()) {
            REQUIRE(Clock::now() < give_up);
            if (http::HttpExecutor::slow().inflight() > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const auto now = Clock::now();
                process_lvgl(static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count()));
            } else {
                process_lvgl(kVirtualStepMs);
            }
            last = Clock::now();
        }
    }

    int status_queries() const {
        return *api_->status_queries;
    }

    std::unique_ptr<MoonrakerClientMock> client_;
    std::unique_ptr<ScriptedRecoveryApi> api_;
    std::optional<RecoveryOutcome> outcome_;
    std::optional<MoonrakerError> error_;

  private:
    std::filesystem::path runs_file() const {
        return dir_ / "runs";
    }

    std::filesystem::path dir_;
};

} // namespace

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: accepted restart that fails to start runs the local script once",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {{"disconnected", ""}, {"startup", ""}, kKey298};

    recover();
    run_to_end();

    // The script ran, then the re-check saw the same error: no second run.
    REQUIRE(script_runs() == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    REQUIRE_FALSE(error_.has_value());
    REQUIRE(status_queries() == 4);
    REQUIRE(api_->service_restarts == 0);

    // Nothing polls on after that: the chain is over.
    process_lvgl(2000);
    REQUIRE(status_queries() == 4);
    REQUIRE(script_runs() == 1);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: a failing local script after a startup error is an error",
                 "[recovery]") {
    install_script(3);
    api_->statuses = {kKey298};

    recover();
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE_FALSE(outcome_.has_value());
    REQUIRE(error_.has_value());
    REQUIRE(error_->code == 3);
    REQUIRE(api_->service_restarts == 0);
}

TEST_CASE_METHOD(RecoveryFixture, "Recovery: accepted restart that comes back ready is verified",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {{"startup", ""}, {"ready", "Printer is ready"}};

    recover();

    // Accepting the restart is not success on its own.
    process_lvgl(20);
    REQUIRE_FALSE(finished());

    run_to_end();

    REQUIRE(outcome_ == RecoveryOutcome::KLIPPY_READY);
    REQUIRE(script_runs() == 0);
    REQUIRE(api_->service_restarts == 0);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: accepted restart never ready within the window runs the script",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {{"disconnected", ""}};

    recover();
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    // One poll per interval across the 1000ms window and again across the
    // re-check after the script, not a tight loop.
    REQUIRE(status_queries() >= 16);
    REQUIRE(status_queries() <= 22);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: startup error without a local script ends the chain with no service "
                 "restart",
                 "[recovery]") {
    api_->statuses = {kKey298};

    recover();
    run_to_end();

    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    REQUIRE(api_->service_restarts == 0);
    REQUIRE(script_runs() == 0);
}

TEST_CASE_METHOD(RecoveryFixture, "Recovery: 503 from firmware_restart goes straight to the script",
                 "[recovery]") {
    install_script(0);
    api_->firmware_error = MoonrakerError{MoonrakerErrorType::UNKNOWN, 503,
                                          "Klippy Host not connected", "printer.firmware_restart"};

    recover();
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    REQUIRE(status_queries() == 0);
    REQUIRE(api_->service_restarts == 0);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: 503 with no local script falls back to a service restart",
                 "[recovery]") {
    api_->firmware_error = MoonrakerError{MoonrakerErrorType::UNKNOWN, 503,
                                          "Klippy Host not connected", "printer.firmware_restart"};

    recover();
    run_to_end();

    REQUIRE(api_->service_restarts == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: a Klippy still starting up is waited for, never scripted",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {{"startup", "Printer is starting up"}};

    recover();

    // Past the window and still `startup`: a slow boot, so no script.
    process_lvgl(1500);
    REQUIRE_FALSE(finished());
    REQUIRE(script_runs() == 0);

    run_to_end();

    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    REQUIRE(script_runs() == 0);
    REQUIRE(api_->service_restarts == 0);
    REQUIRE(status_queries() >= 25);
}

TEST_CASE_METHOD(RecoveryFixture, "Recovery: a slow boot that comes up ready is verified",
                 "[recovery]") {
    install_script(0);
    std::vector<KlippyStatus> boot(20, KlippyStatus{"startup", ""});
    boot.push_back({"ready", "Printer is ready"});
    api_->statuses = boot;

    recover();
    run_to_end();

    REQUIRE(outcome_ == RecoveryOutcome::KLIPPY_READY);
    REQUIRE(script_runs() == 0);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: after the script Klippy coming back ready reports KLIPPY_READY",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {kKey298, {"startup", ""}, {"ready", "Printer is ready"}};

    recover();
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE(outcome_ == RecoveryOutcome::KLIPPY_READY);
    REQUIRE(status_queries() == 3);
}

TEST_CASE_METHOD(RecoveryFixture, "Recovery: a failed printer.info is retried, not escalated",
                 "[recovery]") {
    install_script(0);
    api_->status_errors = 2;
    api_->statuses = {{"ready", "Printer is ready"}};

    recover();
    run_to_end();

    REQUIRE(outcome_ == RecoveryOutcome::KLIPPY_READY);
    REQUIRE(status_queries() == 3);
    REQUIRE(script_runs() == 0);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: a destroyed client ends the check without a call or a callback",
                 "[recovery]") {
    install_script(0);
    api_->statuses = {{"startup", ""}};
    const std::shared_ptr<int> queries = api_->status_queries;

    recover();
    process_lvgl(250);
    const int before = *queries;
    REQUIRE(before >= 1);

    // A printer switch: the API and its client go while a poll is pending.
    api_.reset();
    client_.reset();
    process_lvgl(2000);

    REQUIRE(*queries == before);
    REQUIRE_FALSE(finished());
    REQUIRE(script_runs() == 0);

    // The abandoned chain no longer holds the single-flight slot.
    client_ = std::make_unique<MoonrakerClientMock>(MoonrakerClientMock::PrinterType::VORON_24);
    api_ = std::make_unique<ScriptedRecoveryApi>(*client_, get_printer_state());
    api_->statuses = {{"ready", ""}};
    recover();
    run_to_end();
    REQUIRE(outcome_ == RecoveryOutcome::KLIPPY_READY);
}

TEST_CASE_METHOD(RecoveryFixture, "Recovery: one chain at a time, and the slot frees when it ends",
                 "[recovery]") {
    install_script(3);
    api_->statuses = {kKey298};
    api_->hold_firmware_restart = true;

    recover();
    REQUIRE(api_->firmware_restarts == 1);

    std::optional<MoonrakerError> refused;
    PrinterRecoveryService(api_.get(), kFastTiming)
        .recover([](RecoveryOutcome) { FAIL("a second chain must not run"); },
                 [&refused](const MoonrakerError& e) { refused = e; });
    REQUIRE(refused.has_value());
    REQUIRE(refused->type == MoonrakerErrorType::NOT_READY);
    REQUIRE(api_->firmware_restarts == 1);

    // The first chain ends on its error path (script exits 3)...
    api_->hold_firmware_restart = false;
    api_->held_firmware_ok();
    run_to_end();
    REQUIRE(error_.has_value());

    // ...and the next recovery is accepted.
    error_.reset();
    api_->statuses = {{"ready", ""}};
    *api_->status_queries = 0;
    recover();
    run_to_end();
    REQUIRE(outcome_ == RecoveryOutcome::KLIPPY_READY);
    REQUIRE(api_->firmware_restarts == 2);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: a script that never exits is killed and ends the chain", "[recovery]") {
    install_script(0, "exec sleep 30");
    api_->statuses = {kKey298};
    RecoveryTiming timing = kFastTiming;
    timing.script_timeout_ms = 300;

    recover(timing);
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE_FALSE(outcome_.has_value());
    REQUIRE(error_.has_value());
    REQUIRE(error_->type == MoonrakerErrorType::TIMEOUT);

    // The chain ended, so the next recovery is accepted.
    error_.reset();
    api_->statuses = {{"ready", ""}};
    *api_->status_queries = 0;
    recover(timing);
    run_to_end();
    REQUIRE(outcome_ == RecoveryOutcome::KLIPPY_READY);
}

TEST_CASE_METHOD(RecoveryFixture,
                 "Recovery: the re-check after the script stops at the chain's deadline",
                 "[recovery]") {
    // The script takes a third of the chain's 1000ms, so a loaded machine slowing
    // its start still leaves it inside the deadline; the re-check gets only the rest.
    install_script(0, "sleep 0.3\nexit 0");
    api_->statuses = {kKey298, {"startup", ""}};
    RecoveryTiming timing = kFastTiming;
    timing.poll_interval_ms = 25;
    timing.chain_deadline_ms = 1000;

    recover(timing);
    run_to_end();

    REQUIRE(script_runs() == 1);
    REQUIRE(outcome_ == RecoveryOutcome::RESTART_ISSUED);
    // About 20 polls in the time the script leaves, fewer when it runs long;
    // a fresh 1000ms re-check would make about 35.
    REQUIRE(status_queries() <= 27);
}
