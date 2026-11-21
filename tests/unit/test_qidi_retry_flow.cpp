// SPDX-License-Identifier: GPL-3.0-or-later
//
// QIDI's Retry (TRY_MOVE_AGAIN) on a live Box fault, in the order the printer
// delivers it: the console line goes out at once, and save_variables'
// retry_step and slot state reach us on the next status push, about 250 ms
// later.

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/recovery_modal_presenter_test_access.h"
#include "ams_backend_qidi.h"
#include "ams_error_bridge.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "fault_surface_correlation.h"
#include "gcode_error_router.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/qidi_box_test_access.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsBackendQidi;
using helix::fire_vars;

namespace {

constexpr const char* kSlot2LoadFailure =
    "!!Code:QDE_004_001; Message:Slot loading failure, please check the trigger, please reload "
    "1B.";

bool has_gcode_action(const std::vector<helix::RecoveryAction>& actions, const std::string& gcode) {
    return std::any_of(actions.begin(), actions.end(),
                       [&gcode](const helix::RecoveryAction& a) { return a.gcode == gcode; });
}

bool offers_retry(const std::optional<helix::ErrorEvent>& ev) {
    return ev && has_gcode_action(ev->recovery_actions, "TRY_MOVE_AGAIN");
}

void settle(LVGLUITestFixture& f, int ms) {
    helix::ui::UpdateQueue::instance().drain();
    f.process_lvgl(ms);
    helix::ui::UpdateQueue::instance().drain();
}

/// A QIDI Box registered with AmsState, a recovery presenter that the router
/// and the bridge share, as production wires them, and a paused print.
class QidiRetryFixture : public LVGLUITestFixture {
  public:
    QidiRetryFixture() {
        helix::fault_surface_correlation::clear_for_test();
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().set_action(helix::AmsAction::IDLE);
        state().update_from_status(json{{"pause_resume", {{"is_paused", true}}}});
    }
    ~QidiRetryFixture() override {
        state().update_from_status(json{{"pause_resume", {{"is_paused", false}}}});
        helix::test::set_wire_state(state(), helix::PrintJobState::STANDBY);
        state().print_state().set_print_stop_reason("");
        helix::AmsState::instance().set_action(helix::AmsAction::IDLE);
        helix::ui::UpdateQueue::instance().drain();
        helix::fault_surface_correlation::clear_for_test();
    }

    /// A save_variables push, delivered as the backend's STATE_CHANGED event
    /// is: AmsState syncs, then its data revision ticks.
    void status_push(const json& vars) {
        fire_vars(backend(), vars);
        helix::AmsState::instance().sync_from_backend();
        helix::AmsState::instance().bump_data_revision();
        settle(*this, 20);
    }

    helix::test::WiredBackend<AmsBackendQidi>& backend() {
        return *harness_;
    }

    [[nodiscard]] bool dialog_offers_retry() const {
        return has_gcode_action(RecoveryModalPresenterTestAccess::active_actions(presenter),
                                "TRY_MOVE_AGAIN");
    }

    helix::test::RegisteredWired<AmsBackendQidi> harness_;
    helix::ui::RecoveryModalPresenter presenter{nullptr};
    helix::AmsErrorBridge bridge{presenter};
    helix::GcodeErrorRouter router{nullptr, nullptr, presenter};
};

} // namespace

TEST_CASE_METHOD(QidiRetryFixture,
                 "QIDI Retry reaches the shown fault when retry_step lands after the line",
                 "[qidi][qidi_box][error-center]") {
    status_push(json{
        {"enable_box", 1}, {"box_count", 1}, {"slot0", 1}, {"slot1", 1}, {"retry_step", "None"}});
    bridge.start();
    settle(*this, 20);

    // The console line first: retry_step still names nothing.
    GcodeErrorRouterTestAccess::process_line(router, kSlot2LoadFailure);
    settle(*this, 20);
    REQUIRE(presenter.is_visible());
    const std::string shown = RecoveryModalPresenterTestAccess::shown_detail(presenter);
    REQUIRE(shown.find("Slot 2") != std::string::npos);
    REQUIRE_FALSE(dialog_offers_retry());

    // The status push a quarter second later: slot 2 blocked, the step
    // recorded for TRY_MOVE_AGAIN.
    settle(*this, 250);
    status_push(json{{"slot1", -1}, {"retry_step", "QDE_004_001_2"}});
    REQUIRE(lv_subject_get_int(helix::AmsState::instance().get_ams_action_subject()) ==
            static_cast<int>(helix::AmsAction::ERROR));
    CHECK(presenter.is_visible());
    CHECK(RecoveryModalPresenterTestAccess::shown_detail(presenter) == shown);
    CHECK(dialog_offers_retry());
}

TEST_CASE_METHOD(QidiRetryFixture, "QIDI Retry reaches a fault that was already standing",
                 "[qidi][qidi_box][error-center]") {
    // Slot 2 already reads blocked (AmsAction is ERROR) before the console line
    // and retry_step arrive, so no rising edge carries the upgrade.
    status_push(json{{"enable_box", 1}, {"box_count", 1}, {"slot1", -1}, {"retry_step", "None"}});
    bridge.start();
    settle(*this, 20);
    REQUIRE(lv_subject_get_int(helix::AmsState::instance().get_ams_action_subject()) ==
            static_cast<int>(helix::AmsAction::ERROR));

    GcodeErrorRouterTestAccess::process_line(router, kSlot2LoadFailure);
    settle(*this, 20);
    REQUIRE(presenter.is_visible());
    REQUIRE_FALSE(dialog_offers_retry());
    const std::string shown = RecoveryModalPresenterTestAccess::shown_detail(presenter);

    SECTION("the dialog on screen gains Retry") {
        status_push(json{{"retry_step", "QDE_004_001_2"}});
        CHECK(presenter.is_visible());
        CHECK(RecoveryModalPresenterTestAccess::shown_detail(presenter) == shown);
        CHECK(dialog_offers_retry());
    }
    SECTION("a dialog the user closed stays closed") {
        RecoveryModalPresenterTestAccess::user_dismiss(presenter);
        settle(*this, 20);
        REQUIRE_FALSE(presenter.is_visible());
        status_push(json{{"retry_step", "QDE_004_001_2"}});
        CHECK_FALSE(presenter.is_visible());
    }
}

TEST_CASE_METHOD(QidiRetryFixture, "QIDI Retry never sits under a cause about another slot",
                 "[qidi][qidi_box][error-center]") {
    // Slot 2 stands blocked with a step recorded for it; the print pauses on a
    // fault whose reason names slot 3. The dialog reads the reason.
    status_push(
        json{{"enable_box", 1}, {"box_count", 1}, {"slot1", -1}, {"retry_step", "QDE_004_001_2"}});
    REQUIRE(offers_retry(backend().current_error()));
    helix::test::set_wire_state(state(), helix::PrintJobState::PAUSED);

    SECTION("a cause naming another slot drops Retry") {
        state().print_state().set_print_stop_reason(
            "Code:QDE_004_001; Message:Slot loading failure, please check the trigger, please "
            "reload 1C.");
        helix::AmsState::instance().refresh_fault_cause();
        const auto ev = helix::AmsState::instance().standing_fault();
        REQUIRE(ev.has_value());
        REQUIRE(ev->detail.find("1C") != std::string::npos);
        CHECK_FALSE(offers_retry(ev));
    }
    SECTION("a cause naming no slot drops Retry") {
        state().print_state().set_print_stop_reason("Extruder loading failure.");
        helix::AmsState::instance().refresh_fault_cause();
        CHECK_FALSE(offers_retry(helix::AmsState::instance().standing_fault()));
    }
    SECTION("a cause naming the same slot keeps it") {
        state().print_state().set_print_stop_reason(
            "Code:QDE_004_001; Message:Slot loading failure, please check the trigger, please "
            "reload 1B.");
        helix::AmsState::instance().refresh_fault_cause();
        CHECK(offers_retry(helix::AmsState::instance().standing_fault()));
    }
}
