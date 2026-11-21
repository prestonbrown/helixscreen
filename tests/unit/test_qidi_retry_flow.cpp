// SPDX-License-Identifier: GPL-3.0-or-later
//
// QIDI's Retry (TRY_MOVE_AGAIN) on a live Box fault, in the order the printer
// delivers it: the console line goes out at once, and save_variables'
// retry_step and slot state reach us on the next status push, about 250 ms
// later.

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "../test_helpers/recovery_modal_presenter_test_access.h"
#include "ams_backend_qidi.h"
#include "ams_error_bridge.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "fault_surface_correlation.h"
#include "gcode_error_router.h"
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

void settle(LVGLUITestFixture& f, int ms) {
    helix::ui::UpdateQueue::instance().drain();
    f.process_lvgl(ms);
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "QIDI Retry reaches the shown fault when retry_step lands after the line",
                 "[qidi][qidi_box][error-center]") {
    helix::fault_surface_correlation::clear_for_test();
    helix::AmsState::instance().init_subjects(true);
    helix::AmsState::instance().set_action(helix::AmsAction::IDLE);
    helix::test::RegisteredWired<AmsBackendQidi> harness;
    auto& backend = *harness;
    fire_vars(backend, json{{"enable_box", 1},
                            {"box_count", 1},
                            {"slot0", 1},
                            {"slot1", 1},
                            {"retry_step", "None"}});

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::AmsErrorBridge bridge(presenter);
    bridge.start();
    {
        helix::GcodeErrorRouter router(nullptr, nullptr, presenter);

        // The console line first: retry_step still names nothing.
        GcodeErrorRouterTestAccess::process_line(router, kSlot2LoadFailure);
        settle(*this, 20);
        REQUIRE(presenter.is_visible());
        const std::string shown = RecoveryModalPresenterTestAccess::shown_detail(presenter);
        REQUIRE(shown.find("Slot 2") != std::string::npos);
        REQUIRE_FALSE(has_gcode_action(RecoveryModalPresenterTestAccess::active_actions(presenter),
                                       "TRY_MOVE_AGAIN"));

        // The status push a quarter second later: slot 2 blocked, the step
        // recorded for TRY_MOVE_AGAIN.
        settle(*this, 250);
        fire_vars(backend, json{{"slot1", -1}, {"retry_step", "QDE_004_001_2"}});
        // The backend's STATE_CHANGED delivery, which brings AmsState to ERROR.
        helix::AmsState::instance().sync_from_backend();
        settle(*this, 20);
        REQUIRE(lv_subject_get_int(helix::AmsState::instance().get_ams_action_subject()) ==
                static_cast<int>(helix::AmsAction::ERROR));
        CHECK(presenter.is_visible());
        CHECK(RecoveryModalPresenterTestAccess::shown_detail(presenter) == shown);
        CHECK(has_gcode_action(RecoveryModalPresenterTestAccess::active_actions(presenter),
                               "TRY_MOVE_AGAIN"));
    }
    helix::AmsState::instance().set_action(helix::AmsAction::IDLE);
    helix::fault_surface_correlation::clear_for_test();
}
