// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_resume_refusal_report.cpp
 * @brief A refused RESUME reaches the user as exactly one message.
 *
 * Klipper reports a refused RESUME on two channels: the gcode.script RPC error
 * (dispatch_prepared_resume's error callback, which toasts "Failed to resume:
 * <firmware text>") and the `!! <firmware text>` broadcast (GcodeErrorRouter).
 * When an AMS backend's classifier owns the refusal, its explanation is the one
 * message the user gets: no raw toast beside it, and none instead of it. A
 * refusal no classifier owns keeps the caller's toast.
 *
 * Runs the real dispatch, StandardMacros, mock client, router and Snapmaker
 * backend; the mock answers RESUME with Klipper's two copies of the refusal.
 */

#include "ui_resume_dispatch.h"

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "../test_helpers/recovery_modal_presenter_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "fault_surface_correlation.h"
#include "gcode_error_router.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "rpc_error_correlation.h"
#include "standard_macros.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr const char* kRefusal = "e1 not edit filament";
constexpr const char* kRefusalLine = "!! e1 not edit filament";
/// The Snapmaker classifier's explanation for e1, the machine's slot 2.
constexpr const char* kExplanation = "Slot 2 has no filament type set";

class ResumeRefusalFixture : public LVGLUITestFixture {
  public:
    ResumeRefusalFixture() : mock(MoonrakerClientMock::PrinterType::VORON_24) {
        rpc_error_correlation::clear_for_test();
        fault_surface_correlation::clear_for_test();

        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock, state);

        PrinterDiscovery hardware;
        hardware.parse_objects(nlohmann::json{"extruder", "gcode_macro RESUME"});
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::Resume).is_empty());

        // The classifiers read the process-wide printer state: the print is paused.
        get_printer_state().update_from_status({{"pause_resume", {{"is_paused", true}}}});

        AmsState::instance().set_backend(std::make_unique<AmsBackendSnapmaker>(nullptr, nullptr));

        ui::set_test_notification_error_hook([this](const std::string& m) { toasts.push_back(m); });
        ui::set_test_toast_hook([this](ToastSeverity, const std::string& m, uint32_t) {
            if (m.rfind("Recovery failed", 0) == 0) {
                recovery_toasts.push_back(m);
            }
        });
    }

    ~ResumeRefusalFixture() override {
        ui::set_test_notification_error_hook(nullptr);
        ui::set_test_toast_hook(nullptr);
        router.reset();
        presenter.reset();
        AmsState::instance().set_backend(nullptr);
        get_printer_state().update_from_status({{"pause_resume", {{"is_paused", false}}}});
        StandardMacros::instance().reset();
        ui::UpdateQueue::instance().drain();
        mock.stop_temperature_simulation();
        mock.disconnect();
        api.reset();
        rpc_error_correlation::clear_for_test();
        fault_surface_correlation::clear_for_test();
    }

    void attach_router() {
        presenter = std::make_unique<ui::RecoveryModalPresenter>(api.get());
        router = std::make_unique<GcodeErrorRouter>(api.get(), &mock, *presenter);
    }

    /// The next RESUME fails over RPC with @p message; when @p echo, the `!!`
    /// copy reaches the console first, the order Klipper sends them in.
    void refuse_next_resume(const std::string& message, bool echo) {
        if (echo) {
            mock.force_next_gcode_console_reply("RESUME", "!! " + message);
        }
        mock.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, message, "RESUME");
    }

    void press_resume() {
        ui::dispatch_prepared_resume(api.get(), "[test]", [this]() { ++failures; });
    }

    /// Main-thread hops, then past the router's 150ms held-presentation window.
    void settle() {
        ui::UpdateQueue::instance().drain();
        process_lvgl(300);
        ui::UpdateQueue::instance().drain();
    }

    /// Past the window in which a claimed report waits for the explanation.
    void settle_past_claim() {
        settle();
        process_lvgl(1500);
        ui::UpdateQueue::instance().drain();
    }

    /// The router put the classifier's explanation of the refusal on screen.
    [[nodiscard]] bool explanation_on_screen() const {
        return presenter && presenter->is_visible() &&
               RecoveryModalPresenterTestAccess::shown_detail(*presenter).find(kExplanation) !=
                   std::string::npos;
    }

    MoonrakerClientMock mock;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<ui::RecoveryModalPresenter> presenter;
    std::unique_ptr<GcodeErrorRouter> router;
    std::vector<std::string> toasts;
    std::vector<std::string> recovery_toasts;
    int failures = 0;
};

} // namespace

TEST_CASE_METHOD(ResumeRefusalFixture,
                 "a classified resume refusal shows its explanation and no raw toast",
                 "[resume][error-router][snapmaker]") {
    attach_router();
    refuse_next_resume(kRefusal, /*echo=*/true);

    press_resume();
    settle_past_claim();

    INFO("toasts: " << toasts.size());
    CHECK(toasts.empty());
    REQUIRE(explanation_on_screen());
    CHECK(failures == 1);
}

TEST_CASE_METHOD(ResumeRefusalFixture,
                 "the explanation shows when the broadcast copy lands after the RPC error",
                 "[resume][error-router][snapmaker]") {
    attach_router();
    refuse_next_resume(kRefusal, /*echo=*/false);

    press_resume();
    settle();
    CHECK(toasts.empty());

    // Klipper's `!!` copy, reaching the router after the caller already ran.
    GcodeErrorRouterTestAccess::process_line(*router, kRefusalLine);
    settle_past_claim();

    CHECK(toasts.empty());
    REQUIRE(explanation_on_screen());
}

TEST_CASE_METHOD(ResumeRefusalFixture,
                 "a second refusal after the user dismissed the first explains again",
                 "[resume][error-router][snapmaker]") {
    attach_router();
    refuse_next_resume(kRefusal, /*echo=*/true);
    press_resume();
    settle();
    REQUIRE(explanation_on_screen());

    RecoveryModalPresenterTestAccess::user_dismiss(*presenter);
    settle();
    REQUIRE_FALSE(explanation_on_screen());

    // Past the RPC correlation window, so the second press is its own report.
    rpc_error_correlation::clear_for_test();
    fault_surface_correlation::clear_for_test();

    refuse_next_resume(kRefusal, /*echo=*/true);
    press_resume();
    settle_past_claim();

    CHECK(toasts.empty());
    REQUIRE(explanation_on_screen());
}

TEST_CASE_METHOD(ResumeRefusalFixture,
                 "a resume that turns into a restart question ends its pending state at once",
                 "[resume][restart]") {
    // A dirty-bed pause cannot resume; the user is asked about a restart.
    get_printer_state().update_from_status(
        {{"print_stats", {{"state", "paused"}, {"message", "detected dirty bed"}}},
         {"virtual_sdcard", {{"is_active", false}}}});
    press_resume();
    settle();
    // The resume's spinner and its backstop end before the user answers, so a
    // restart taking longer cannot be reported as a resume that timed out.
    CHECK(failures == 1);
    get_printer_state().update_from_status({{"print_stats", {{"state", "standby"}}}});
}

TEST_CASE_METHOD(ResumeRefusalFixture, "an unclassified resume error keeps the caller's toast",
                 "[resume][error-router]") {
    attach_router();
    refuse_next_resume("resume macro failed", /*echo=*/true);

    press_resume();
    settle();

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].rfind("Failed to resume: ", 0) == 0);
    CHECK(toasts[0].find("resume macro failed") != std::string::npos);
    CHECK_FALSE(explanation_on_screen());
    CHECK(failures == 1);
}

TEST_CASE_METHOD(ResumeRefusalFixture,
                 "with no error router attached a classified refusal still toasts",
                 "[resume][error-router][snapmaker]") {
    // A build with no router (the ESP32 slice) has nobody to explain the `!!`
    // copy, so the caller's toast is the only report there is.
    refuse_next_resume(kRefusal, /*echo=*/false);

    press_resume();
    settle();

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].find(kRefusal) != std::string::npos);
}

TEST_CASE_METHOD(ResumeRefusalFixture,
                 "a refused Resume recovery tap shows the explanation, not a failure toast",
                 "[resume][error-router][snapmaker]") {
    attach_router();

    // The runout modal a paused print raises, offering Resume.
    ErrorEvent runout;
    runout.severity = ErrorSeverity::CRITICAL;
    runout.detail = "Filament runout";
    runout.recovery_actions.push_back({"Resume", "RESUME", "test::resume", "primary", false});
    presenter->present(runout);
    REQUIRE(presenter->is_visible());

    refuse_next_resume(kRefusal, /*echo=*/true);
    RecoveryModalPresenterTestAccess::tap(*presenter, "RESUME");
    settle_past_claim();

    CHECK(recovery_toasts.empty());
    CHECK(toasts.empty());
    REQUIRE(explanation_on_screen());
}

TEST_CASE_METHOD(ResumeRefusalFixture, "a refusal whose console copy is lost still toasts",
                 "[resume][error-router][snapmaker]") {
    attach_router();
    // The RPC error arrives; the `!!` copy never does.
    refuse_next_resume(kRefusal, /*echo=*/false);

    press_resume();
    settle();
    // Inside the window the explanation may still be on its way.
    CHECK(toasts.empty());

    settle_past_claim();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].find(kRefusal) != std::string::npos);
    CHECK_FALSE(explanation_on_screen());
}

TEST_CASE_METHOD(ResumeRefusalFixture, "a re-press inside the correlation window explains again",
                 "[resume][error-router][snapmaker]") {
    attach_router();
    refuse_next_resume(kRefusal, /*echo=*/true);
    press_resume();
    settle();
    REQUIRE(explanation_on_screen());

    RecoveryModalPresenterTestAccess::user_dismiss(*presenter);
    settle();
    REQUIRE_FALSE(presenter->is_visible());

    // Both correlation buffers still hold the first refusal.
    refuse_next_resume(kRefusal, /*echo=*/true);
    press_resume();
    settle_past_claim();

    CHECK(toasts.empty());
    REQUIRE(explanation_on_screen());
    CHECK(failures == 2);
}

TEST_CASE_METHOD(ResumeRefusalFixture, "a refusal on a printer that is not paused keeps the toast",
                 "[resume][error-router][snapmaker]") {
    // The classifier only owns the refusal against a job being continued.
    get_printer_state().update_from_status({{"pause_resume", {{"is_paused", false}}}});
    attach_router();
    refuse_next_resume(kRefusal, /*echo=*/true);

    press_resume();
    settle_past_claim();

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].rfind("Failed to resume: ", 0) == 0);
    CHECK_FALSE(explanation_on_screen());
}
