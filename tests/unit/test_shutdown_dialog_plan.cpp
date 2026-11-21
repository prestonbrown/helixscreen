// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Which shutdown dialog opens, and which scope its split buttons start on.
 *
 * A screen on the printer's own host gets one row of buttons that go through
 * Moonraker, or straight to local power when Moonraker is down or the host
 * does not run systemd (Moonraker's own power path needs it). A screen on a different
 * host gets the Both / Printer / Screen split buttons, starting on Screen: the
 * user is standing at the screen, and taking a printer down from another
 * machine is the choice that has to be made on purpose.
 *
 * A screen whose service user has no polkit grant for logind power-off and
 * reboot gets a toast that says so, since restarting the service installs it.
 */

#include "ui_split_button.h"

#include "../../src/ui/panel_widgets/shutdown_widget.h"
#include "../test_fixtures.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "platform_info.h"
#include "printer_state.h"
#include "system_power.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("Shutdown dialog plan: remote screen defaults to Screen", "[shutdown][power]") {
    SECTION("connected, live verdict remote") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/false,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }

    SECTION("connected: the live verdict outranks a stale configured host") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/true,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }

    SECTION("disconnected: the configured host decides") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/false,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/false,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }
}

TEST_CASE("Shutdown dialog plan: a remote printer splits whatever this host runs",
          "[shutdown][power]") {
    for (const bool connected : {true, false}) {
        const auto plan = plan_shutdown_dialog(connected,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/false,
                                               /*this_host_runs_systemd=*/false);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }
}

TEST_CASE("Shutdown dialog plan: same host stays single-scope", "[shutdown][power]") {
    SECTION("connected goes through Moonraker") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/false,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaMoonraker);
    }

    SECTION("connected on a non-systemd host goes straight to local power") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/true,
                                               /*this_host_runs_systemd=*/false);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaLocalPower);
    }

    SECTION("disconnected on a non-systemd host uses local power too") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/false,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/true,
                                               /*this_host_runs_systemd=*/false);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaLocalPower);
    }

    SECTION("disconnected falls back to local power") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/false,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/true,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaLocalPower);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "Dual-scope shutdown modal opens on the requested scope",
                 "[shutdown][power]") {
    REQUIRE(register_component("shutdown_modal"));
    ui_split_button_init();

    ShutdownModal modal;
    auto noop = []() {};
    modal.set_dual_callbacks(noop, noop, noop, noop, noop, noop, ShutdownScope::Screen);
    REQUIRE(modal.show(test_screen()));
    process_lvgl(10);

    lv_obj_t* restart = lv_obj_find_by_name(modal.dialog(), "split_restart");
    lv_obj_t* shutdown = lv_obj_find_by_name(modal.dialog(), "split_shutdown");
    REQUIRE(restart != nullptr);
    REQUIRE(shutdown != nullptr);
    CHECK(ui_split_button_get_selected(restart) == static_cast<uint32_t>(ShutdownScope::Screen));
    CHECK(ui_split_button_get_selected(shutdown) == static_cast<uint32_t>(ShutdownScope::Screen));

    SECTION("a narrowed selection does not survive a reopen") {
        ui_split_button_set_selected(restart, static_cast<uint32_t>(ShutdownScope::Printer));
        ui_split_button_set_selected(shutdown, static_cast<uint32_t>(ShutdownScope::Both));
        modal.hide();
        process_lvgl(10);

        modal.set_dual_callbacks(noop, noop, noop, noop, noop, noop, ShutdownScope::Screen);
        REQUIRE(modal.show(test_screen()));
        process_lvgl(10);
        restart = lv_obj_find_by_name(modal.dialog(), "split_restart");
        shutdown = lv_obj_find_by_name(modal.dialog(), "split_shutdown");
        REQUIRE(restart != nullptr);
        REQUIRE(shutdown != nullptr);
        CHECK(ui_split_button_get_selected(restart) ==
              static_cast<uint32_t>(ShutdownScope::Screen));
        CHECK(ui_split_button_get_selected(shutdown) ==
              static_cast<uint32_t>(ShutdownScope::Screen));
    }

    modal.hide();
    process_lvgl(10);
}

TEST_CASE_METHOD(XMLTestFixture, "Dual-scope shutdown modal applies a non-XML default scope",
                 "[shutdown][power]") {
    // The XML parses with Screen selected, so only on_show() can put the
    // buttons on any other scope.
    REQUIRE(register_component("shutdown_modal"));
    ui_split_button_init();

    for (const ShutdownScope scope : {ShutdownScope::Printer, ShutdownScope::Both}) {
        ShutdownModal modal;
        auto noop = []() {};
        modal.set_dual_callbacks(noop, noop, noop, noop, noop, noop, scope);
        REQUIRE(modal.show(test_screen()));
        process_lvgl(10);

        lv_obj_t* restart = lv_obj_find_by_name(modal.dialog(), "split_restart");
        lv_obj_t* shutdown = lv_obj_find_by_name(modal.dialog(), "split_shutdown");
        REQUIRE(restart != nullptr);
        REQUIRE(shutdown != nullptr);
        CHECK(ui_split_button_get_selected(restart) == static_cast<uint32_t>(scope));
        CHECK(ui_split_button_get_selected(shutdown) == static_cast<uint32_t>(scope));

        modal.hide();
        process_lvgl(10);
    }
}

TEST_CASE("logind authorization errors are recognised", "[shutdown][power]") {
    CHECK(is_authorization_error("org.freedesktop.DBus.Error.InteractiveAuthorizationRequired"));
    CHECK(is_authorization_error("org.freedesktop.DBus.Error.AccessDenied"));
    CHECK_FALSE(is_authorization_error("org.freedesktop.DBus.Error.NoReply"));
    CHECK_FALSE(is_authorization_error("org.freedesktop.login1.OperationInProgress"));
    CHECK_FALSE(is_authorization_error(""));
    CHECK_FALSE(is_authorization_error(nullptr));
}

TEST_CASE("Screen power failure toast names a missing permission", "[shutdown][power]") {
    CHECK(screen_power_failure_message(PowerResult::Dispatched, false) == nullptr);
    CHECK(screen_power_failure_message(PowerResult::Dispatched, true) == nullptr);

    const std::string denied = screen_power_failure_message(PowerResult::NotAuthorized, false);
    CHECK(denied.find("not permitted") != std::string::npos);
    CHECK(denied.find("Restart HelixScreen") != std::string::npos);
    CHECK(std::string(screen_power_failure_message(PowerResult::NotAuthorized, true)) == denied);

    CHECK(std::string(screen_power_failure_message(PowerResult::Failed, false)) ==
          "Screen shutdown failed");
    CHECK(std::string(screen_power_failure_message(PowerResult::Failed, true)) ==
          "Screen reboot failed");
}

TEST_CASE("Local power holds black only after the request dispatched", "[shutdown][power]") {
    std::vector<std::string> steps;
    auto power_returning = [&steps](PowerResult r) {
        return [&steps, r]() {
            steps.push_back("power");
            return r;
        };
    };
    auto stop_drawing = [&steps]() { steps.push_back("stop_drawing"); };
    auto clear = [&steps]() { steps.push_back("clear"); };

    SECTION("dispatched: power, then stop drawing, then clear") {
        CHECK(power_then_hold_black(power_returning(PowerResult::Dispatched), stop_drawing,
                                    clear) == PowerResult::Dispatched);
        CHECK(steps == std::vector<std::string>{"power", "stop_drawing", "clear"});
    }

    SECTION("failed or refused: the screen is left alone") {
        for (const PowerResult r : {PowerResult::Failed, PowerResult::NotAuthorized}) {
            steps.clear();
            CHECK(power_then_hold_black(power_returning(r), stop_drawing, clear) == r);
            CHECK(steps == std::vector<std::string>{"power"});
        }
    }
}

TEST_CASE("A failed local power retries through a connected Moonraker", "[shutdown][power]") {
    CHECK(host_power_follow_up(PowerResult::Dispatched, true) == HostPowerFollowUp::Done);
    CHECK(host_power_follow_up(PowerResult::Dispatched, false) == HostPowerFollowUp::Done);
    CHECK(host_power_follow_up(PowerResult::Failed, true) == HostPowerFollowUp::RetryViaMoonraker);
    CHECK(host_power_follow_up(PowerResult::NotAuthorized, true) ==
          HostPowerFollowUp::RetryViaMoonraker);
    CHECK(host_power_follow_up(PowerResult::Failed, false) == HostPowerFollowUp::Toast);
    CHECK(host_power_follow_up(PowerResult::NotAuthorized, false) == HostPowerFollowUp::Toast);
}

TEST_CASE("A power request runs at once only with its scope settled and nothing asking",
          "[shutdown][power][safety]") {
    using M = ShutdownDialogMode;
    using S = ShutdownScope;
    using T = PowerTarget;
    const ShutdownDialogPlan single{M::SingleViaMoonraker, S::Screen};
    const ShutdownDialogPlan local{M::SingleViaLocalPower, S::Screen};
    const ShutdownDialogPlan dual{M::Dual, S::Screen};
    constexpr bool kConfirm = true;
    constexpr bool kNoConfirm = false;
    constexpr auto kJob = PowerJobState::Active;
    constexpr auto kIdle = PowerJobState::Idle;
    constexpr auto kUnknown = PowerJobState::Unknown;
    const auto run = [](PowerDecision d) { return d.run_now; };

    SECTION("single host: confirmation alone decides, the target does not matter") {
        CHECK(run(decide_power_request(PowerIntent::Shutdown, kNoConfirm, single, T::Ask, kIdle)));
        CHECK(run(decide_power_request(PowerIntent::Reboot, kNoConfirm, local, T::Printer, kIdle)));
        CHECK_FALSE(
            run(decide_power_request(PowerIntent::Reboot, kConfirm, single, T::Printer, kIdle)));
    }

    SECTION("two hosts: a chosen target with confirmation off acts on it at once") {
        const auto printer =
            decide_power_request(PowerIntent::Reboot, kNoConfirm, dual, T::Printer, kIdle);
        CHECK(printer.run_now);
        CHECK(printer.scope == S::Printer);
        const auto screen =
            decide_power_request(PowerIntent::Shutdown, kNoConfirm, dual, T::Screen, kIdle);
        CHECK(screen.run_now);
        CHECK(screen.scope == S::Screen);
    }

    SECTION("two hosts: Ask keeps the dialog on the plan's default scope") {
        const auto d = decide_power_request(PowerIntent::Shutdown, kNoConfirm, dual, T::Ask, kIdle);
        CHECK_FALSE(d.run_now);
        CHECK(d.scope == S::Screen);
    }

    SECTION("two hosts: confirmation on opens the dialog on the chosen target") {
        const auto d =
            decide_power_request(PowerIntent::Shutdown, kConfirm, dual, T::Printer, kIdle);
        CHECK_FALSE(d.run_now);
        CHECK(d.scope == S::Printer);
        // The Home power button names no action, but its dialog starts there too.
        const auto home =
            decide_power_request(PowerIntent::Choose, kNoConfirm, dual, T::Printer, kIdle);
        CHECK_FALSE(home.run_now);
        CHECK(home.scope == S::Printer);
    }

    SECTION("the Home power button and a running job always ask") {
        CHECK_FALSE(
            run(decide_power_request(PowerIntent::Choose, kNoConfirm, single, T::Ask, kIdle)));
        CHECK_FALSE(
            run(decide_power_request(PowerIntent::Shutdown, kNoConfirm, single, T::Ask, kJob)));
        CHECK_FALSE(
            run(decide_power_request(PowerIntent::Reboot, kNoConfirm, dual, T::Printer, kJob)));
    }

    SECTION("an unknown job state asks, with the warning, instead of acting or refusing") {
        const auto single_reboot =
            decide_power_request(PowerIntent::Reboot, kNoConfirm, local, T::Ask, kUnknown);
        CHECK_FALSE(single_reboot.run_now);
        CHECK(single_reboot.warn_status_unknown);
        const auto dual_shutdown =
            decide_power_request(PowerIntent::Shutdown, kNoConfirm, dual, T::Printer, kUnknown);
        CHECK_FALSE(dual_shutdown.run_now);
        CHECK(dual_shutdown.scope == S::Printer);
        CHECK(dual_shutdown.warn_status_unknown);
        // The Home power button's dialog carries the warning too.
        CHECK(decide_power_request(PowerIntent::Choose, kConfirm, single, T::Ask, kUnknown)
                  .warn_status_unknown);
    }

    SECTION("a known job state carries no warning") {
        CHECK_FALSE(decide_power_request(PowerIntent::Reboot, kNoConfirm, single, T::Ask, kIdle)
                        .warn_status_unknown);
        CHECK_FALSE(decide_power_request(PowerIntent::Reboot, kNoConfirm, single, T::Ask, kJob)
                        .warn_status_unknown);
    }
}

TEST_CASE("The job state is unknown until it is known, whatever the job flag reads",
          "[shutdown][power][safety]") {
    CHECK(classify_power_job(/*known=*/true, /*active=*/false) == PowerJobState::Idle);
    CHECK(classify_power_job(/*known=*/true, /*active=*/true) == PowerJobState::Active);
    CHECK(classify_power_job(/*known=*/false, /*active=*/false) == PowerJobState::Unknown);
    CHECK(classify_power_job(/*known=*/false, /*active=*/true) == PowerJobState::Unknown);
}

namespace {

/// The global PrinterState as a fresh start leaves it: nothing from Moonraker yet.
class FreshPrinterStateFixture : public XMLTestFixture {
  public:
    FreshPrinterStateFixture() {
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        set_platform_override(0);
    }
    ~FreshPrinterStateFixture() override {
        set_platform_override(-1);
        PrinterStateTestAccess::reset(get_printer_state());
    }

    static void connect_ready() {
        get_printer_state().set_printer_connection_state(
            static_cast<int>(ConnectionState::CONNECTED), nullptr);
        helix::ui::UpdateQueue::instance().drain();
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
    }
    static void apply_print_stats(const char* state) {
        get_printer_state().update_from_status({{"print_stats", {{"state", state}}}});
    }
};

} // namespace

TEST_CASE_METHOD(FreshPrinterStateFixture,
                 "The job state is known only once Klipper is ready and print_stats arrived",
                 "[shutdown][power][safety]") {
    CHECK_FALSE(power_job_state_known());

    connect_ready();
    REQUIRE(lv_subject_get_int(
                get_printer_state().network_state().get_printer_connection_state_subject()) ==
            static_cast<int>(ConnectionState::CONNECTED));
    // Connected and ready, but the job subjects still hold their STANDBY defaults.
    CHECK_FALSE(power_job_state_known());

    apply_print_stats("printing");
    CHECK(power_job_state_known());
    CHECK(power_job_active());

    get_printer_state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    CHECK_FALSE(power_job_state_known());
}

TEST_CASE_METHOD(FreshPrinterStateFixture,
                 "The power dialog warns while the printer's status is not known",
                 "[shutdown][power][safety]") {
    // Registering the component makes a fresh scope; the modal's subjects
    // must register into it, not into the scope an earlier test left behind.
    StaticSubjectRegistry::instance().deinit_one("ShutdownModal");
    REQUIRE(register_component("shutdown_modal"));
    ui_split_button_init();
    const auto note_visible = [](ShutdownModal& modal) {
        lv_obj_t* note = lv_obj_find_by_name(modal.dialog(), "status_unknown_note");
        REQUIRE(note != nullptr);
        return !lv_obj_is_hidden(note);
    };

    // PowerIntent::Choose never acts without a button press, so nothing here
    // powers down the machine running the tests.
    SECTION("before Moonraker reports, the warning shows") {
        ShutdownModal modal;
        helix::AsyncLifetimeGuard lifetime;
        helix::show_shutdown_dialog(&api(), modal, lifetime, test_screen());
        REQUIRE(modal.is_visible());
        CHECK(note_visible(modal));
    }

    SECTION("once the job state is known, it does not") {
        connect_ready();
        apply_print_stats("standby");
        ShutdownModal modal;
        helix::AsyncLifetimeGuard lifetime;
        helix::show_shutdown_dialog(&api(), modal, lifetime, test_screen());
        REQUIRE(modal.is_visible());
        CHECK_FALSE(note_visible(modal));
    }
}
