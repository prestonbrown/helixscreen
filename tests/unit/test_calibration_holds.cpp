// SPDX-License-Identifier: GPL-3.0-or-later

// A calibration screen shows the E-stop while the app has the machine moving or
// heating, and keeps the display awake while it waits on the user's hands.

#include "ui_calibration_holds.h"
#include "ui_emergency_stop.h"
#include "ui_panel_belt_tension.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_motor_ripple.h"
#include "ui_panel_screws_tilt.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_awake_hold.h"
#include "macro_executor.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using helix::calibration::StageHolds;
using helix::calibration::StageNeeds;

namespace {

int busy_count() {
    return EmergencyStopOverlayTestAccess::machine_busy_count(EmergencyStopOverlay::instance());
}

bool awake() {
    return helix::display_awake_hold().is_held();
}

struct HeldRoutineFixture : LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};

    HeldRoutineFixture() {
        get_printer_state().init_subjects(false);
    }
    ~HeldRoutineFixture() override {
        settle();
    }
    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    static helix::MachineGcode routine() {
        helix::MachineGcode cmd;
        cmd.gcode = cmd.log_name = "CUTTER_CALIBRATE";
        cmd.display_name = "Cutter position";
        return cmd;
    }
};

} // namespace

TEST_CASE("A stage's holds follow the stage and end with the screen", "[calibration_holds]") {
    REQUIRE(busy_count() == 0);
    REQUIRE_FALSE(awake());
    {
        StageHolds holds;
        holds.apply(StageNeeds{/*machine_busy=*/true, /*hands_on=*/false});
        CHECK(busy_count() == 1);
        CHECK_FALSE(awake());

        // A repeated stage change takes nothing twice.
        holds.apply(StageNeeds{true, true});
        CHECK(busy_count() == 1);
        CHECK(awake());

        holds.apply(StageNeeds{false, true});
        CHECK(busy_count() == 0);
        CHECK(awake());

        holds.apply(StageNeeds{true, true});
        holds.release();
        CHECK(busy_count() == 0);
        CHECK_FALSE(awake());

        holds.apply(StageNeeds{true, true});
    }
    // Destroying the screen's holds releases what it still had.
    CHECK(busy_count() == 0);
    CHECK_FALSE(awake());
}

TEST_CASE_METHOD(HeldRoutineFixture, "A firmware routine shows the E-stop until its script ends",
                 "[calibration_holds]") {
    client.defer_next("printer.gcode.script");
    helix::calibration::start_held_calibration_gcode(&api, routine(), /*needs_home=*/false,
                                                     "[HoldsTest]");
    settle();
    CHECK(busy_count() == 1);

    SECTION("it answers") {
        client.fire_deferred("printer.gcode.script");
    }
    SECTION("it fails") {
        client.fire_deferred_error("printer.gcode.script",
                                   MoonrakerError::unknown("mock: routine failed"));
    }
    settle();
    CHECK(busy_count() == 0);
}

TEST_CASE("Each calibration screen holds the E-stop while it moves and the screen while it waits",
          "[calibration_holds]") {
    using Pid = PIDCalibrationPanel::State;
    CHECK(PIDCalibrationPanel::stage_needs(Pid::CALIBRATING).machine_busy);
    CHECK_FALSE(PIDCalibrationPanel::stage_needs(Pid::IDLE).machine_busy);
    CHECK_FALSE(PIDCalibrationPanel::stage_needs(Pid::COMPLETE).machine_busy);

    using Zo = ZOffsetCalibrationPanel::State;
    for (Zo s : {Zo::WARMING, Zo::PROBING}) {
        CHECK(ZOffsetCalibrationPanel::stage_needs(s).machine_busy);
        CHECK_FALSE(ZOffsetCalibrationPanel::stage_needs(s).hands_on);
    }
    // The paper test: the nozzle sits at the bed and the user's hands are under it.
    CHECK(ZOffsetCalibrationPanel::stage_needs(Zo::ADJUSTING).machine_busy);
    CHECK(ZOffsetCalibrationPanel::stage_needs(Zo::ADJUSTING).hands_on);
    for (Zo s : {Zo::IDLE, Zo::SAVING, Zo::COMPLETE, Zo::ERROR}) {
        CHECK_FALSE(ZOffsetCalibrationPanel::stage_needs(s).machine_busy);
        CHECK_FALSE(ZOffsetCalibrationPanel::stage_needs(s).hands_on);
    }

    using Is = InputShaperPanel::State;
    CHECK(InputShaperPanel::stage_needs(Is::MEASURING).machine_busy);
    CHECK_FALSE(InputShaperPanel::stage_needs(Is::RESULTS).machine_busy);

    using St = ScrewsTiltPanel::State;
    CHECK(ScrewsTiltPanel::stage_needs(St::PROBING).machine_busy);
    CHECK_FALSE(ScrewsTiltPanel::stage_needs(St::RESULTS).machine_busy);
    CHECK(ScrewsTiltPanel::stage_needs(St::RESULTS).hands_on);

    using Bt = BeltTensionPanel::ViewState;
    using Tp = BeltTensionPanel::TunePhase;
    CHECK(BeltTensionPanel::stage_needs(Bt::RUNNING, Tp::ADJUST).machine_busy);
    CHECK_FALSE(BeltTensionPanel::stage_needs(Bt::RUNNING, Tp::ADJUST).hands_on);
    CHECK(BeltTensionPanel::stage_needs(Bt::TUNING, Tp::SWEEPING).machine_busy);
    CHECK_FALSE(BeltTensionPanel::stage_needs(Bt::TUNING, Tp::ADJUST).machine_busy);
    for (Tp p : {Tp::ADJUST, Tp::SWEEPING, Tp::MATCHED, Tp::CALIBRATE, Tp::ALREADY_MATCHED}) {
        CHECK(BeltTensionPanel::stage_needs(Bt::TUNING, p).hands_on);
    }
    CHECK_FALSE(BeltTensionPanel::stage_needs(Bt::RESULTS, Tp::ADJUST).machine_busy);
    CHECK_FALSE(BeltTensionPanel::stage_needs(Bt::RESULTS, Tp::ADJUST).hands_on);

    using helix::ui::MotorRipplePanel;
    using Rv = MotorRipplePanel::View;
    CHECK(MotorRipplePanel::stage_needs(Rv::Running).machine_busy);
    CHECK_FALSE(MotorRipplePanel::stage_needs(Rv::Results).machine_busy);
}
