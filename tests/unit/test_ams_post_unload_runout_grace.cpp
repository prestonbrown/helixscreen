// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_ams_post_unload_runout_grace.cpp
 * @brief The one-shot "an unload just finished" runout grace.
 *
 * Run with: ./build/bin/helix-tests "[runout][grace]"
 *
 * An unload ends with the filament deliberately dragged off the toolhead
 * sensor. is_filament_operation_active() only covers the window while the
 * action is still running, and on a K2 Plus the sensor cleared ten seconds
 * after the script completed — so the idle runout modal fired on a deliberate
 * unload. The grace covers that tail.
 *
 * Three properties are pinned here.
 *
 * 1. ARMING tracks the whole operation, not an UNLOADING -> IDLE edge:
 *    apply_synthesized_action_locked() overwrites the action with sub-phases as
 *    physical signals arrive, and the real K2 unload ended CUTTING -> IDLE.
 *
 * 2. The grace is BOUNDED. Retirement at "filament loaded again" never fires
 *    for an unload that leaves nothing loaded, so without a time bound the flag
 *    outlives its operation indefinitely and swallows the next genuine idle
 *    runout — possibly days later, with one spdlog line as the only evidence.
 *    Backend teardown drops it for the same reason the four sibling runout-edge
 *    fields are dropped there.
 *
 * 3. It is consumed LAST in PrintStatusWidget's guard chain. It is one-shot, so
 *    taking it above a gate that returns anyway burns it on a call that could
 *    never have shown a modal, and the deliberate unload it was armed for then
 *    pops the modal unsuppressed.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "runtime_config.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::printer;

namespace {

/// A mock backend whose reported action and toolhead-loaded flag are directly
/// settable, so an operation can be walked through its sub-phases exactly as
/// the firmware reports them. Everything else is AmsBackendMock's behavior.
class GraceProbeBackend : public AmsBackendMock {
  public:
    GraceProbeBackend() : AmsBackendMock(4) {}

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        info.action = reported_action_;
        info.filament_loaded = reported_loaded_;
        return info;
    }

    void report_action(AmsAction a) {
        reported_action_ = a;
    }
    void report_loaded(bool v) {
        reported_loaded_ = v;
    }

  private:
    AmsAction reported_action_ = AmsAction::IDLE;
    bool reported_loaded_ = false;
};

/// Install a probe backend and settle AmsState on a known IDLE baseline, so the
/// first transition a test drives is the one it means to drive.
GraceProbeBackend* install_probe() {
    auto backend = std::make_unique<GraceProbeBackend>();
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));
    raw->report_action(AmsAction::IDLE);
    AmsState::instance().sync_from_backend();
    return raw;
}

/// Walk the backend through one reported action and let AmsState observe it.
void step(GraceProbeBackend* backend, AmsAction action) {
    backend->report_action(action);
    AmsState::instance().sync_from_backend();
}

bool armed() {
    return AmsStateTestAccess::post_unload_runout_grace_armed(AmsState::instance());
}

} // namespace

// ============================================================================
// 1. Arming — across a whole operation, not an edge
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Post-unload grace arms when an operation ends after UNLOADING",
                 "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    // PrinterState first: AmsState observes its print_state_enum subject, and an
    // uninitialized subject swallows lv_subject_set_int without complaint.
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    SECTION("UNLOADING -> IDLE arms") {
        step(backend, AmsAction::UNLOADING);
        CHECK_FALSE(armed()); // not until the operation actually ends
        step(backend, AmsAction::IDLE);
        CHECK(armed());
    }

    SECTION("UNLOADING -> CUTTING -> IDLE arms (the observed K2 Plus shape)") {
        // apply_synthesized_action_locked() rewrites the action as physical
        // signals land, so the unload's final transition is CUTTING -> IDLE. An
        // UNLOADING -> IDLE edge test would miss this entirely.
        step(backend, AmsAction::UNLOADING);
        step(backend, AmsAction::CUTTING);
        step(backend, AmsAction::IDLE);
        CHECK(armed());
    }

    SECTION("an operation that never unloaded does NOT arm") {
        step(backend, AmsAction::HEATING);
        step(backend, AmsAction::LOADING);
        step(backend, AmsAction::IDLE);
        CHECK_FALSE(armed());
    }

    SECTION("a second operation with no unload does not re-arm after consumption") {
        step(backend, AmsAction::UNLOADING);
        step(backend, AmsAction::IDLE);
        REQUIRE(ams.consume_post_unload_runout_grace());

        step(backend, AmsAction::LOADING);
        step(backend, AmsAction::IDLE);
        CHECK_FALSE(armed());
    }

    ams.clear_backends();
}

// ============================================================================
// 2. Consumption — one shot only
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Post-unload grace is one-shot", "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    step(backend, AmsAction::UNLOADING);
    step(backend, AmsAction::IDLE);

    CHECK(ams.consume_post_unload_runout_grace());
    CHECK_FALSE(ams.consume_post_unload_runout_grace());
    CHECK_FALSE(ams.consume_post_unload_runout_grace());

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLTestFixture, "Peeking the post-unload grace does not spend it",
                 "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    step(backend, AmsAction::UNLOADING);
    step(backend, AmsAction::IDLE);

    // The toast path peeks. Repeatedly peeking must leave the single shot intact
    // for the idle runout modal, which is the only consumer — otherwise whichever
    // surface observes the sensor edge first silently disarms the other.
    CHECK(ams.post_unload_runout_grace_armed());
    CHECK(ams.post_unload_runout_grace_armed());
    CHECK(ams.post_unload_runout_grace_armed());

    CHECK(ams.consume_post_unload_runout_grace());
    // ...and once the consumer HAS spent it, the peek agrees it is gone.
    CHECK_FALSE(ams.post_unload_runout_grace_armed());

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLTestFixture, "An expired post-unload grace does not read as armed",
                 "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    step(backend, AmsAction::UNLOADING);
    step(backend, AmsAction::IDLE);
    REQUIRE(ams.post_unload_runout_grace_armed());

    // Same window the consumer honours: past it, an empty sensor is a real
    // runout again and the toast must come back.
    AmsStateTestAccess::age_post_unload_runout_grace(ams, AmsStateTestAccess::grace_window() +
                                                              std::chrono::seconds(1));
    CHECK_FALSE(ams.post_unload_runout_grace_armed());

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLTestFixture, "No unload means nothing for the toast to suppress",
                 "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    // A genuine runout arrives with no unload anywhere in the operation. The
    // peek must stay false, or suppressing the "Filament removed" warning would
    // blind the real event it exists to report.
    step(backend, AmsAction::HEATING);
    step(backend, AmsAction::IDLE);
    CHECK_FALSE(ams.post_unload_runout_grace_armed());

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLTestFixture, "Filament returning retires the post-unload grace",
                 "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    step(backend, AmsAction::UNLOADING);
    step(backend, AmsAction::IDLE);
    REQUIRE(armed());

    // The grace was armed for the removal THIS unload caused. Filament back at
    // the toolhead means anything after it is a new event.
    backend->report_loaded(true);
    ams.sync_from_backend();

    CHECK_FALSE(armed());
    CHECK_FALSE(ams.consume_post_unload_runout_grace());

    ams.clear_backends();
}

// ============================================================================
// 3. Bounds — time and lifetime
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Post-unload grace expires instead of waiting forever",
                 "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    SECTION("inside the window it still applies") {
        step(backend, AmsAction::UNLOADING);
        step(backend, AmsAction::IDLE);
        AmsStateTestAccess::age_post_unload_runout_grace(ams, AmsStateTestAccess::grace_window() -
                                                                  std::chrono::seconds(1));
        CHECK(ams.consume_post_unload_runout_grace());
    }

    SECTION("past the window it does not, and clears itself") {
        // An end-of-print or idle unload leaves nothing loaded, so the
        // filament-back retirement never fires. Without the bound this flag
        // would still be armed days later and would eat a real runout.
        step(backend, AmsAction::UNLOADING);
        step(backend, AmsAction::IDLE);
        AmsStateTestAccess::age_post_unload_runout_grace(ams, AmsStateTestAccess::grace_window() +
                                                                  std::chrono::seconds(1));

        CHECK_FALSE(ams.consume_post_unload_runout_grace());
        CHECK_FALSE(armed()); // expiry clears, it does not just report

        // And a fresh unload after the stale one arms cleanly again.
        step(backend, AmsAction::UNLOADING);
        step(backend, AmsAction::IDLE);
        CHECK(ams.consume_post_unload_runout_grace());
    }

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLTestFixture, "Backend teardown drops the post-unload grace",
                 "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    step(backend, AmsAction::UNLOADING);
    step(backend, AmsAction::IDLE);
    REQUIRE(armed());

    // The grace describes a removal on THIS backend. Nothing the next one
    // reports can be that — same reasoning as the four runout-edge fields
    // cleared alongside it.
    ams.clear_backends();

    CHECK_FALSE(armed());
    CHECK_FALSE(ams.consume_post_unload_runout_grace());
}

TEST_CASE_METHOD(LVGLTestFixture, "Teardown mid-unload leaves nothing to arm later",
                 "[runout][grace][ams]") {
    auto& ams = AmsState::instance();
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install_probe();

    // Backend disappears with an unload still in flight: saw_unload_in_op_ is
    // latched. If teardown left it set, the NEXT backend's first settle to IDLE
    // would arm a grace for an unload that happened on a different machine.
    step(backend, AmsAction::UNLOADING);
    ams.clear_backends();

    auto* next = install_probe();
    step(next, AmsAction::HEATING);
    step(next, AmsAction::IDLE);
    CHECK_FALSE(armed());

    ams.clear_backends();
}

// ============================================================================
// 4. PrintStatusWidget consumes it last
// ============================================================================

namespace {

/// A basic runout-sensor printer: no AMS backend, one RUNOUT-roled sensor.
/// That shape makes has_real_runout() true (no lane to scope against) and
/// should_show_runout_modal() true (no AMS), and leaves the backend suppression
/// gate inert — so the guard chain runs all the way down to the grace.
class IdleRunoutGraceFixture : public LVGLTestFixture {
  public:
    IdleRunoutGraceFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        helix::PanelWidgetManager::instance().init_widget_subjects();
        PrintStatusWidget::init_static_subjects();

        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());

        get_printer_state().init_subjects(false);
        AmsState::instance().init_subjects(false);
        AmsState::instance().clear_backends();

        auto& fsm = FilamentSensorManager::instance();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        fsm.discover_sensors({SENSOR});
        fsm.set_sensor_role(SENSOR, FilamentSensorRole::RUNOUT);
        // Baseline present, then empty — the state a runout leaves behind.
        fsm.update_from_status(sensor_status(true));
        fsm.update_from_status(sensor_status(false));
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE_FALSE(fsm.is_in_startup_grace_period());
        REQUIRE(fsm.has_real_runout());
        REQUIRE(get_runtime_config()->should_show_runout_modal());
    }

    ~IdleRunoutGraceFixture() override {
        set_moonraker_api(previous_api_);
        AmsState::instance().clear_backends();
        helix::ui::UpdateQueue::instance().drain();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    /// Drive the real input. PrintStatusWidget's idle-runout gate reads
    /// print_lifecycle, which is republished from update_from_status(); writing
    /// print_state_enum by hand leaves the lifecycle stale and the gate never
    /// re-evaluates, so the assertion would fail as if the guard were missing.
    static void set_print_state(PrintJobState s) {
        helix::test::set_wire_state(get_printer_state(), s);
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    static constexpr const char* SENSOR = "filament_switch_sensor runout_sensor";

    static nlohmann::json sensor_status(bool detected) {
        return nlohmann::json{{SENSOR, {{"filament_detected", detected}, {"enabled", true}}}};
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(IdleRunoutGraceFixture, "Idle runout check takes the grace when it would show",
                 "[runout][grace][print_status_widget]") {
    auto& ams = AmsState::instance();
    AmsStateTestAccess::arm_post_unload_runout_grace(ams);
    set_print_state(PrintJobState::STANDBY);

    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::check_idle_runout(widget);

    // The grace did its job: no modal, and it is spent.
    CHECK_FALSE(PrintStatusWidgetTestAccess::runout_modal_shown(widget));
    CHECK_FALSE(ams.consume_post_unload_runout_grace());
}

TEST_CASE_METHOD(IdleRunoutGraceFixture, "A runout during an active print does not burn the grace",
                 "[runout][grace][print_status_widget]") {
    auto& ams = AmsState::instance();
    AmsStateTestAccess::arm_post_unload_runout_grace(ams);

    // Mid-print runout is FilamentRunoutHandler's job; this check bails at the
    // print-state gate and shows nothing either way. Consuming the one-shot on
    // the way past means the deliberate unload it was armed for — which happens
    // once the job is over — pops the modal unsuppressed.
    set_print_state(PrintJobState::PRINTING);

    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::check_idle_runout(widget);
    CHECK_FALSE(PrintStatusWidgetTestAccess::runout_modal_shown(widget));

    // Still armed for the unload that comes after the print.
    CHECK(ams.consume_post_unload_runout_grace());
}

TEST_CASE_METHOD(IdleRunoutGraceFixture, "An already-shown modal does not burn the grace",
                 "[runout][grace][print_status_widget]") {
    auto& ams = AmsState::instance();
    AmsStateTestAccess::arm_post_unload_runout_grace(ams);
    set_print_state(PrintJobState::STANDBY);

    PrintStatusWidget widget;
    // The dialog is already on screen; this call has nothing to do. Spending the
    // one-shot here would leave the next deliberate unload unprotected.
    PrintStatusWidgetTestAccess::set_runout_modal_shown(widget, true);
    PrintStatusWidgetTestAccess::check_idle_runout(widget);

    CHECK(ams.consume_post_unload_runout_grace());
}
