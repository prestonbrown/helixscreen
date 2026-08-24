// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_preparing_card.cpp
 * @brief The home print-status card must not read idle during a committed start.
 *
 * Run with: ./build/bin/helix-tests "[print_status_widget][print_state]"
 *
 * Seen on the K2 during the Phase 1 hardware run and recorded in the plan as
 * "found, not fixed": the home widget kept showing print_card_idle for the whole
 * of a host-side pre-print block. It observed print_state_enum, which reads
 * standby until the printer accepts the job, so the card claimed nothing was
 * happening while the machine was homing and probing - and tapping it went to
 * the file browser rather than the status overlay.
 *
 * Two consumers in the same widget, both fixed here:
 *   1. is_active_, which picks the view_subject_ variant (idle 0-2 vs active 3-4)
 *   2. the idle runout modal's print-state gate, which would otherwise pop a
 *      "load filament" dialog on top of a start already under way
 *
 * The state is driven through the real inputs - update_from_status() plus
 * set_print_start_state() - never by writing print_state_enum, which no longer
 * reaches the widget.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

class PreparingCardFixture : public LVGLTestFixture {
  public:
    PreparingCardFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        PanelWidgetManager::instance().init_widget_subjects();
        PrintStatusWidget::init_static_subjects();
        PrintStatusWidget::destroy_formatter_for_test();

        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, get_printer_state());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());

        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        AmsState::instance().init_subjects(false);
        AmsState::instance().clear_backends();
        set_lifecycle(PrintJobState::STANDBY, PrintStartPhase::IDLE);
    }

    ~PreparingCardFixture() override {
        auto& ps = get_printer_state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(PreparingExit::Superseded);
        }
        ps.set_print_start_state(PrintStartPhase::IDLE, "", 0);
        settle();
        PrintStatusWidget::destroy_formatter_for_test();
        set_moonraker_api(previous_api_);
        AmsState::instance().clear_backends();
        settle();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    /// One drain is not enough - a handler running during a drain queues more.
    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// PHASE FIRST, then the wire. update_from_status() republishes the lifecycle
    /// using whatever phase is live at that instant, so raising the wire first
    /// publishes a transient Printing on the way to Preparing.
    static void set_lifecycle(PrintJobState wire, PrintStartPhase phase) {
        auto& ps = get_printer_state();
        ps.set_print_start_state(phase, "", 0);
        settle();
        helix::test::set_wire_state(ps, wire);
        settle();
    }

    /// The app has committed to a job the printer has not accepted: the whole of
    /// a host-side pre-start block.
    static void enter_host_side_preparing() {
        auto& ps = get_printer_state();
        ps.begin_preparing(PrintJobRef{"chosen.gcode", "", ""});
        ps.set_print_start_state(PrintStartPhase::HOMING, "", 0);
        settle();
        REQUIRE(ps.get_print_job_state() == PrintJobState::STANDBY);
        REQUIRE(ps.get_print_lifecycle() == PrintState::Preparing);
    }

    /// Minimal tree carrying the names attach() looks up. Same shape as
    /// test_print_status_widget_detailed_visibility.cpp - visibility is asserted
    /// through the print_status_view subject, not through flags on these objects.
    lv_obj_t* create_mock_tree() {
        lv_obj_t* container = lv_obj_create(test_screen());
        auto add = [container](const char* name, bool as_image = false) {
            lv_obj_t* obj = as_image ? lv_image_create(container) : lv_obj_create(container);
            lv_obj_set_name(obj, name);
        };
        add("print_card_idle");
        add("print_card_thumb", /*as_image=*/true);
        add("print_card_idle_compact");
        add("print_card_idle_detailed");
        add("print_card_thumb_compact", /*as_image=*/true);
        add("print_card_printing");
        add("print_card_layout");
        add("print_card_thumb_wrap");
        add("print_card_active_thumb", /*as_image=*/true);
        add("print_card_info");
        add("print_card_preparing_info");
        add("print_card_printing_detailed");
        return container;
    }

    static int view_value() {
        return lv_subject_get_int(PrintStatusWidget::view_subject_for_test());
    }

    /// view_subject_: 0-2 are the idle variants, 3-4 the active ones.
    static bool card_is_active() {
        return view_value() >= 3;
    }

    MoonrakerClientMock mock_client;
    std::unique_ptr<MoonrakerAPI> api;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

// ============================================================================
// The card variant
// ============================================================================

TEST_CASE_METHOD(PreparingCardFixture, "Print card reads idle when nothing is running",
                 "[print_status_widget][print_state]") {
    // Non-vacuity baseline: without it, every "card is active" assertion below
    // would also pass against a widget stuck on the active variant.
    PrintStatusWidget w;
    lv_obj_t* container = create_mock_tree();
    w.attach(container, test_screen());
    settle();
    process_lvgl(50);

    CHECK_FALSE(card_is_active());

    w.detach();
}

TEST_CASE_METHOD(PreparingCardFixture, "Print card goes active once the printer reports the job",
                 "[print_status_widget][print_state]") {
    // Characterization of what already worked, so the migration cannot trade one
    // broken window for another.
    PrintStatusWidget w;
    lv_obj_t* container = create_mock_tree();
    w.attach(container, test_screen());
    settle();
    process_lvgl(50);

    SECTION("printing") {
        set_lifecycle(PrintJobState::PRINTING, PrintStartPhase::IDLE);
    }
    SECTION("paused") {
        set_lifecycle(PrintJobState::PAUSED, PrintStartPhase::IDLE);
    }
    settle();
    process_lvgl(20);

    CHECK(card_is_active());

    w.detach();
}

TEST_CASE_METHOD(PreparingCardFixture, "Print card is active during a host-side pre-print block",
                 "[print_status_widget][print_state]") {
    // THE K2 DEFECT. print_stats reads standby for the whole block, so the card
    // claimed the printer was idle while it homed and probed.
    PrintStatusWidget w;
    lv_obj_t* container = create_mock_tree();
    w.attach(container, test_screen());
    settle();
    process_lvgl(50);
    REQUIRE_FALSE(card_is_active());

    enter_host_side_preparing();
    settle();
    process_lvgl(20);

    CHECK(card_is_active());

    w.detach();
}

TEST_CASE_METHOD(PreparingCardFixture, "Print card is active during a firmware-side PRINT_START",
                 "[print_status_widget][print_state]") {
    // The other half of Preparing: Klipper already reports printing because the
    // pre-print work lives inside PRINT_START. The wire caught this one; it must
    // stay caught once the read moves to the lifecycle.
    PrintStatusWidget w;
    lv_obj_t* container = create_mock_tree();
    w.attach(container, test_screen());
    settle();
    process_lvgl(50);

    set_lifecycle(PrintJobState::PRINTING, PrintStartPhase::HOMING);
    REQUIRE(get_printer_state().get_print_lifecycle() == PrintState::Preparing);
    settle();
    process_lvgl(20);

    CHECK(card_is_active());

    w.detach();
}

// ============================================================================
// The idle runout modal's print-state gate
// ============================================================================

namespace {

/// A basic runout-sensor printer with a real runout standing: the shape that
/// makes every gate ahead of the print-state check pass, so the check itself is
/// what the assertion observes. Mirrors IdleRunoutGraceFixture in
/// test_ams_post_unload_runout_grace.cpp.
class PreparingRunoutFixture : public PreparingCardFixture {
  public:
    static constexpr const char* SENSOR = "filament_switch_sensor runout_sensor";

    PreparingRunoutFixture() {
        auto& fsm = FilamentSensorManager::instance();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        fsm.discover_sensors({SENSOR});
        fsm.set_sensor_role(SENSOR, FilamentSensorRole::RUNOUT);
        fsm.update_from_status(sensor_status(true));
        fsm.update_from_status(sensor_status(false));
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
        settle();
        REQUIRE_FALSE(fsm.is_in_startup_grace_period());
        REQUIRE(fsm.has_real_runout());
        REQUIRE(get_runtime_config()->should_show_runout_modal());
    }

    ~PreparingRunoutFixture() override {
        PostUnloadGraceTestAccess::reset(FilamentSensorManager::instance());
    }

    static nlohmann::json sensor_status(bool detected) {
        return nlohmann::json{{SENSOR, {{"filament_detected", detected}, {"enabled", true}}}};
    }
};

} // namespace

TEST_CASE_METHOD(PreparingRunoutFixture, "Idle runout check runs its gates when truly idle",
                 "[print_status_widget][print_state]") {
    // Non-vacuity baseline. The post-unload grace sits BELOW the print-state gate
    // and is one-shot, so "was it spent?" is a precise answer to "did the call get
    // past that gate?" - without needing the modal to render.
    auto& ams = AmsState::instance();
    AmsStateTestAccess::arm_post_unload_runout_grace(ams);

    PrintStatusWidget w;
    PrintStatusWidgetTestAccess::check_idle_runout(w);

    CHECK_FALSE(ams.consume_post_unload_runout_grace()); // spent: the gate was passed
}

TEST_CASE_METHOD(PreparingRunoutFixture,
                 "Idle runout check bails during a host-side pre-print block",
                 "[print_status_widget][print_state]") {
    // A "load filament" dialog on top of a start the user just committed to. The
    // gate read the wire, which says standby for the whole block.
    auto& ams = AmsState::instance();
    AmsStateTestAccess::arm_post_unload_runout_grace(ams);
    enter_host_side_preparing();

    PrintStatusWidget w;
    PrintStatusWidgetTestAccess::check_idle_runout(w);

    CHECK_FALSE(PrintStatusWidgetTestAccess::runout_modal_shown(w));
    // Still armed: bailing at the print-state gate must not burn the one-shot.
    CHECK(ams.consume_post_unload_runout_grace());
}
