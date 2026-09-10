// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_calibration_transport_loss.cpp
 * @brief A dropped WebSocket (or an RPC timeout a slow calibration outlived)
 *        is the transport vanishing, not the printer's opinion of the macro
 *        (prestonbrown/helixscreen#1543).
 *
 * Every calibration collector must absorb TIMEOUT/CONNECTION_LOST, keep its
 * notify_gcode_response handler registered, and let the result lines (or the
 * busy->idle follow-up) complete the run. Anything carrying Klipper's own
 * complaint is still terminal.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "calibration_types.h"
#include "i_moonraker_sub_apis.h"
#include "moonraker_advanced_api.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "operation_timeout_guard.h"
// timer_can_still_fire() reads lv_timer_t::timer_cb/repeat_count, which the
// public header keeps opaque.
#include "lib/lvgl/src/misc/lv_timer_private.h"
#include "printer_state.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

struct TransportLossFixture : public LVGLUITestFixture {
    TransportLossFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state().init_subjects(false);
        // The drivers arm the follow-up against the GLOBAL printer state's
        // idle subject — initialize it too, or the arm silently no-ops there.
        get_printer_state().init_subjects(false);
        state().set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state());
    }

    ~TransportLossFixture() override {
        api_.reset();
        UpdateQueue::instance().drain();
    }

    /// Drain everything the driver queued, including work that queues more work.
    /// UpdateQueue::process_pending() drains a snapshot, so each hop costs one
    /// tick: the arm is one, an observer report is another, and handing the
    /// owner its terminal answer is a third. Bounded, so a callback that
    /// re-queues forever fails the case on its own assertion instead of
    /// spinning here. The real sleep is what lets the mock's WebSocket thread
    /// deliver in the first place.
    void settle() {
        for (int tick = 0; tick < 6; ++tick) {
            UpdateQueue::instance().drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    /// Run the armed idle-fallback backstop now. Its real budget is 5-20
    /// minutes of virtual time; OperationTimeoutGuard::pending_timer() is the
    /// seam for running the handler the driver installed without waiting it out.
    /// @return false when nothing is armed, so a case cannot assert against a
    ///         timer that was never there.
    bool fire_backstop() {
        return fire_guard(helix::calibration::armed_idle_backstop());
    }

    /// Run the armed grace window now. Advancing the clock instead would race
    /// the mock printer's own PID simulation, which emits its result at virtual
    /// t=3000ms — exactly the grace budget.
    bool fire_grace() {
        return fire_guard(helix::calibration::armed_idle_grace());
    }

    bool fire_guard(OperationTimeoutGuard* guard) {
        if (!guard) {
            return false;
        }
        lv_timer_t* timer = guard->pending_timer();
        if (!timer) {
            return false;
        }
        lv_timer_set_period(timer, 1);
        lv_timer_ready(timer);
        process_lvgl(20);
        settle();
        return true;
    }

    MoonrakerClientMock mock_client_;
    std::unique_ptr<MoonrakerAPI> api_;

    std::atomic<bool> success_{false};
    std::atomic<bool> error_{false};
    std::string captured_error_;
};

/// The follow-up arms against the GLOBAL printer state's idle subject — the
/// one the driver reads (get_printer_state()), not the per-fixture state.
lv_subject_t* global_idle_subject() {
    return get_printer_state().get_idle_timeout_printing_subject();
}

/// True when @p timer is still in LVGL's timer list AND still able to fire.
/// Compares pointers only, never dereferencing a timer it did not find linked.
/// A cancel that neuters instead of unlinking (lv_timer_cancel_safe, used
/// widely elsewhere) leaves a listed entry with a null callback, so liveness is
/// the callback and the repeat count, not mere presence in the list.
bool timer_can_still_fire(lv_timer_t* timer) {
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (t == timer) {
            return t->timer_cb != nullptr && t->repeat_count != 0;
        }
    }
    return false;
}

} // namespace

TEST_CASE_METHOD(TransportLossFixture, "PID absorbs CONNECTION_LOST and still delivers the result",
                 "[calibration][transport][1543]") {
    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "heater_bed", 60, [this](float, float, float) { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    // The socket dropped: not a failure, and the collector must still be there.
    REQUIRE_FALSE(error_.load());

    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=73.517 pid_Ki=1.132 pid_Kd=1194.093");
    settle();

    REQUIRE(success_.load());
    REQUIRE_FALSE(error_.load());
}

TEST_CASE_METHOD(TransportLossFixture, "MPC absorbs CONNECTION_LOST and still delivers the result",
                 "[calibration][transport][1543]") {
    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "MPC_CALIBRATE");

    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 0,
        [this](const MoonrakerAdvancedAPI::MPCResult&) { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    REQUIRE_FALSE(error_.load());

    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    mock_client_.dispatch_gcode_response("block_heat_capacity=18.5432 [J/K]");
    mock_client_.dispatch_gcode_response("sensor_responsiveness=0.123456 [K/s/K]");
    mock_client_.dispatch_gcode_response("ambient_transfer=0.078901 [W/K]");
    settle();

    REQUIRE(success_.load());
    REQUIRE_FALSE(error_.load());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "Screws tilt absorbs CONNECTION_LOST and completes on the busy->idle edge",
                 "[calibration][transport][1543]") {
    get_printer_state().init_subjects(false);
    lv_subject_t* idle = global_idle_subject();
    REQUIRE(idle != nullptr);
    lv_subject_set_int(idle, 1); // the probe run holds the printer busy

    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "SCREWS_TILT_CALCULATE");

    api_->advanced().calculate_screws_tilt(
        [this](const std::vector<ScrewTiltResult>& results) {
            CAPTURE(results.size());
            REQUIRE_FALSE(results.empty());
            success_.store(true);
        },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    REQUIRE_FALSE(error_.load());

    // Result lines arriving while the socket was down are what the collector
    // has; the idle edge is the definitive completion signal for this driver.
    mock_client_.dispatch_gcode_response("// front_left (base) : x=-5.0, y=30.0, z=2.48750");
    settle();
    lv_subject_set_int(idle, 0); // macro finished, socket restored
    settle();

    REQUIRE(success_.load());
    REQUIRE_FALSE(error_.load());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "Bed mesh absorbs CONNECTION_LOST and completes from its markers",
                 "[calibration][transport][1543]") {
    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "BED_MESH_CALIBRATE");

    api_->advanced().start_bed_mesh_calibrate(
        IAdvancedAPI::BedMeshCommand{/*script=*/"BED_MESH_CALIBRATE", /*self_prepares=*/true},
        [](int, int) {}, [this]() { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        },
        /*expected_probes=*/0, /*probe_samples=*/1);

    settle();
    REQUIRE_FALSE(error_.load());

    mock_client_.dispatch_gcode_response("Mesh Bed Leveling Complete");
    settle();

    REQUIRE(success_.load());
    REQUIRE_FALSE(error_.load());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "Input shaper absorbs CONNECTION_LOST instead of blaming the wiring",
                 "[calibration][transport][1543]") {
    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "SHAPER_CALIBRATE");

    api_->advanced().start_resonance_test(
        'X', [](int, ShaperCalibrationPhase) {},
        [this](const InputShaperResult&) { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    REQUIRE_FALSE(error_.load());
    REQUIRE(captured_error_.empty());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "Noise check absorbs CONNECTION_LOST instead of failing fast",
                 "[calibration][transport][1543]") {
    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "MEASURE_AXES_NOISE");

    api_->advanced().measure_axes_noise([this](float) { success_.store(true); },
                                        [this](const MoonrakerError& err) {
                                            captured_error_ = err.message;
                                            error_.store(true);
                                        });

    settle();
    REQUIRE_FALSE(error_.load());
    REQUIRE(captured_error_.empty());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "A JSON-RPC error is still terminal after the transport policy",
                 "[calibration][transport][1543]") {
    // Klipper's own complaint must keep failing fast — the absorb path exists
    // for a vanished transport, not for a rejection.
    mock_client_.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                        "Heater extruder not configured", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "extruder", 200, [this](float, float, float) { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    REQUIRE(error_.load());
    REQUIRE_FALSE(success_.load());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "Line-driven collectors fail honestly after the edge grace window",
                 "[calibration][transport][1543]") {
    lv_subject_t* idle = global_idle_subject();
    REQUIRE(idle != nullptr);
    lv_subject_set_int(idle, 1); // in-flight macro holds the printer busy

    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "heater_bed", 60, [this](float, float, float) { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    REQUIRE_FALSE(error_.load());

    // The socket comes back with no result lines behind it: idle edge, and the
    // collector waits out the grace window before concluding the results were
    // lost.
    lv_subject_set_int(idle, 0);
    settle();
    REQUIRE_FALSE(error_.load()); // grace still running

    REQUIRE(fire_grace());

    REQUIRE(error_.load());
    REQUIRE_FALSE(success_.load());
    REQUIRE(captured_error_.find("result unavailable") != std::string::npos);
}

TEST_CASE_METHOD(TransportLossFixture,
                 "A result line inside the grace window beats the unavailable answer",
                 "[calibration][transport][1543]") {
    lv_subject_t* idle = global_idle_subject();
    REQUIRE(idle != nullptr);
    lv_subject_set_int(idle, 1);

    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "heater_bed", 60, [this](float, float, float) { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    lv_subject_set_int(idle, 0); // the edge opens the grace window
    settle();
    REQUIRE(helix::calibration::armed_idle_grace() != nullptr);

    // The grace window exists so result lines trailing the edge still land. A
    // line arriving inside it is the printer's own answer and outranks the
    // "result unavailable" the window would otherwise conclude with.
    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=73.517 pid_Ki=1.132 pid_Kd=1194.093");
    settle();

    REQUIRE(success_.load());
    REQUIRE_FALSE(error_.load());

    // Completion retires the window, so it cannot fire a second answer later.
    REQUIRE_FALSE(fire_grace());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "A result line landing inside the grace window's own callback still wins",
                 "[calibration][transport][1543]") {
    lv_subject_t* idle = global_idle_subject();
    REQUIRE(idle != nullptr);
    lv_subject_set_int(idle, 1);

    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "heater_bed", 60, [this](float, float, float) { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    lv_subject_set_int(idle, 0); // the edge opens the grace window
    settle();

    OperationTimeoutGuard* grace = helix::calibration::armed_idle_grace();
    REQUIRE(grace != nullptr);
    lv_timer_t* grace_timer = grace->pending_timer();
    REQUIRE(grace_timer != nullptr);

    {
        // The freeze stands in for the main thread not having reached its next
        // drain: work the expiry queues is held, work it performs inline is not.
        auto held = helix::ui::UpdateQueue::instance().scoped_freeze("grace-window-tie");

        lv_timer_set_period(grace_timer, 1);
        lv_timer_ready(grace_timer);
        process_lvgl(20);
        REQUIRE_FALSE(error_.load());

        // The result line lands in that window, which is the tie: the printer's
        // own answer outranks the "result unavailable" the expiry is holding.
        mock_client_.dispatch_gcode_response(
            "PID parameters: pid_Kp=73.517 pid_Ki=1.132 pid_Kd=1194.093");
    }
    settle();

    REQUIRE(success_.load());
    REQUIRE_FALSE(error_.load());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "The backstop calls a still-busy printer unrecovered rather than finished",
                 "[calibration][transport][1543]") {
    lv_subject_t* idle = global_idle_subject();
    REQUIRE(idle != nullptr);
    lv_subject_set_int(idle, 1); // the macro is still holding the printer busy

    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "heater_bed", 60, [this](float, float, float) { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        });

    settle();
    REQUIRE_FALSE(error_.load());

    // No idle edge ever arrives: the ceiling is the only thing left to answer.
    REQUIRE(fire_backstop());

    REQUIRE(error_.load());
    REQUIRE_FALSE(success_.load());
    REQUIRE(captured_error_.find("still busy") != std::string::npos);
}

TEST_CASE_METHOD(TransportLossFixture, "The backstop completes a run whose idle edge was missed",
                 "[calibration][transport][1543]") {
    lv_subject_t* idle = global_idle_subject();
    REQUIRE(idle != nullptr);
    lv_subject_set_int(idle, 1); // probing, so the follow-up sees the run start

    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "BED_MESH_CALIBRATE");

    api_->advanced().start_bed_mesh_calibrate(
        IAdvancedAPI::BedMeshCommand{/*script=*/"BED_MESH_CALIBRATE", /*self_prepares=*/true},
        [](int, int) {}, [this]() { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        },
        /*expected_probes=*/0, /*probe_samples=*/1);

    settle();
    REQUIRE_FALSE(error_.load());

    // The busy->idle transition the outage swallowed: the value moves with no
    // notification behind it, which is the missed edge the backstop re-read
    // exists for.
    idle->value.num = 0;

    REQUIRE(fire_backstop());

    REQUIRE(success_.load());
    REQUIRE_FALSE(error_.load());
}

TEST_CASE_METHOD(TransportLossFixture,
                 "The backstop refuses to complete a mesh the printer was never seen probing",
                 "[calibration][transport][1543]") {
    lv_subject_t* idle = global_idle_subject();
    REQUIRE(idle != nullptr);
    lv_subject_set_int(idle, 0); // never busy: the socket was already down at send

    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "BED_MESH_CALIBRATE");

    api_->advanced().start_bed_mesh_calibrate(
        IAdvancedAPI::BedMeshCommand{/*script=*/"BED_MESH_CALIBRATE", /*self_prepares=*/true},
        [](int, int) {}, [this]() { success_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_.store(true);
        },
        /*expected_probes=*/0, /*probe_samples=*/1);

    settle();
    REQUIRE_FALSE(error_.load());

    REQUIRE(fire_backstop());

    // Completing here would offer to save a mesh that was never probed.
    REQUIRE_FALSE(success_.load());
    REQUIRE(error_.load());
    REQUIRE(captured_error_.find("may not have run") != std::string::npos);
}

TEST_CASE_METHOD(TransportLossFixture,
                 "An armed collector dies with the client instead of outliving it",
                 "[calibration][transport][1543]") {
    lv_subject_t* idle = global_idle_subject();
    REQUIRE(idle != nullptr);
    lv_subject_set_int(idle, 1);

    lv_timer_t* backstop_timer = nullptr;

    // A client scoped to this block stands in for the one
    // Application::tear_down_printer_state() step 18 releases on a printer
    // switch, an add-printer, or a cancelled add-printer wizard. The fixture
    // cannot run the whole 20-step teardown, so this exercises the step that
    // frees the object the armed backstop calls back into.
    {
        MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
        MoonrakerAPI api(client, state());
        client.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                      "Connection to printer lost", "PID_CALIBRATE");

        api.advanced().start_pid_calibrate(
            "heater_bed", 60, [this](float, float, float) { success_.store(true); },
            [this](const MoonrakerError& err) {
                captured_error_ = err.message;
                error_.store(true);
            });

        settle();
        // Proves the fallback really armed, so the assertions below are about
        // teardown and not about a backstop that never existed.
        OperationTimeoutGuard* backstop = helix::calibration::armed_idle_backstop();
        REQUIRE(backstop != nullptr);
        backstop_timer = backstop->pending_timer();
        REQUIRE(backstop_timer != nullptr);
        REQUIRE(timer_can_still_fire(backstop_timer));
    }

    settle();
    // The collector's only strong owner is the client's handler map, so
    // releasing the client destroys the collector, and its member guards take
    // the backstop timer and the idle observer with them.
    REQUIRE(helix::calibration::armed_idle_backstop() == nullptr);
    REQUIRE_FALSE(timer_can_still_fire(backstop_timer));

    // Nothing is left to come due, so the wait the backstop was covering ends
    // silently instead of reporting a calibration to a printer that is gone.
    process_lvgl(50);
    settle();
    REQUIRE_FALSE(error_.load());
    REQUIRE_FALSE(success_.load());
}
