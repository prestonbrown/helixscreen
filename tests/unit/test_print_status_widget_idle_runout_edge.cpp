// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_idle_runout_edge.cpp
 * @brief Who removed the filament decides whether the home card asks about it.
 *
 * Run with: ./build/bin/helix-tests "[print_status_widget][runout][1497]"
 *
 * Nothing extrudes while the printer is idle, so a runout sensor that goes from
 * detected to not-detected in that state was emptied by a person standing at the
 * machine. Telling them so under a warning icon reading "Filament Runout" reports
 * a fault that cannot have happened. Finding the sensor already empty on arrival
 * is the opposite case: the operator may not know, and offering Load is the point
 * of the dialog.
 *
 * The distinction has to hold on a printer with a plain filament_switch_sensor
 * and no filament system, because every other suppression in the guard chain
 * reads AmsState::ams_action_, which only an AMS backend ever writes. This
 * fixture is that printer: one RUNOUT-roled sensor, no backend, idle.
 *
 * Driven through the observer on an attached widget rather than through
 * check_and_show_idle_runout_modal() directly, because the arrival-vs-edge
 * distinction is only expressible in the sequence of sensor readings.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
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

class IdleRunoutEdgeFixture : public LVGLTestFixture {
  public:
    IdleRunoutEdgeFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
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

        auto& fsm = FilamentSensorManager::instance();
        // Without this, update_subjects() returns at its subjects_initialized_
        // guard and any_runout never leaves its initial 0, so the observer under
        // test would never be called at all.
        fsm.init_subjects();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        fsm.discover_sensors({SENSOR});
        fsm.set_sensor_role(SENSOR, FilamentSensorRole::RUNOUT);
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);

        set_idle();
        REQUIRE(get_runtime_config()->should_show_runout_modal());
    }

    ~IdleRunoutEdgeFixture() override {
        PrintStatusWidget::destroy_formatter_for_test();
        set_moonraker_api(previous_api_);
        AmsState::instance().clear_backends();
        settle();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    /// One drain is not enough: a handler running during a drain queues more,
    /// and the runout observer defers its body through the queue.
    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    static void set_idle() {
        helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        settle();
    }

    /// Publish a sensor reading the way Moonraker does, then let the observer run.
    ///
    /// The grace is cleared after draining rather than once in the constructor:
    /// anything still queued from the mock connection can call discover_sensors(),
    /// which re-anchors the grace to that moment, and update_subjects() pins
    /// any_runout to 0 for as long as it is armed.
    static void set_filament(bool detected) {
        auto& fsm = FilamentSensorManager::instance();
        settle();
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
        fsm.update_from_status(
            nlohmann::json{{SENSOR, {{"filament_detected", detected}, {"enabled", true}}}});
        settle();
        // The widget observes this subject and nothing else, so a reading that
        // failed to reach it would leave every assertion below asserting an
        // absence that no code path could have produced.
        REQUIRE(lv_subject_get_int(fsm.get_any_runout_subject()) == (detected ? 0 : 1));
    }

    /// Only the names attach() looks up; the assertions read widget state, not
    /// these objects.
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

    static constexpr const char* SENSOR = "filament_switch_sensor runout_sensor";

    MoonrakerClientMock mock_client;
    std::unique_ptr<MoonrakerAPI> api;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(IdleRunoutEdgeFixture,
                 "Filament pulled out while idle raises no runout dialog (1497)",
                 "[print_status_widget][runout][1497]") {
    set_filament(true);

    PrintStatusWidget widget;
    widget.attach(create_mock_tree(), test_screen());
    settle();
    REQUIRE_FALSE(PrintStatusWidgetTestAccess::runout_modal_shown(widget));

    // The operator pulls the filament out to change spools.
    set_filament(false);
    REQUIRE(FilamentSensorManager::instance().has_real_runout());

    CHECK_FALSE(PrintStatusWidgetTestAccess::runout_modal_shown(widget));
}

TEST_CASE_METHOD(IdleRunoutEdgeFixture, "An empty sensor found on arrival still offers Load",
                 "[print_status_widget][runout][1497]") {
    // Non-vacuity: the case above asserts an absence, and would pass just as well
    // against a widget that can never raise the dialog at all. This is the same
    // fixture, the same sensor and the same idle state, differing only in whether
    // the widget watched the filament leave.
    set_filament(false);
    REQUIRE(get_printer_state().get_print_lifecycle() == PrintState::Idle);

    PrintStatusWidget widget;
    widget.attach(create_mock_tree(), test_screen());
    settle();

    CHECK(PrintStatusWidgetTestAccess::runout_modal_shown(widget));
}

TEST_CASE_METHOD(IdleRunoutEdgeFixture, "Reloading and pulling again stays quiet",
                 "[print_status_widget][runout][1497]") {
    // The reproduction in the report: a loose piece of filament pushed through
    // the sensor and drawn back out, repeatedly. Each removal is its own edge and
    // clears the one-shot that would otherwise mask the second one.
    set_filament(true);

    PrintStatusWidget widget;
    widget.attach(create_mock_tree(), test_screen());
    settle();

    for (int cycle = 0; cycle < 3; ++cycle) {
        set_filament(false);
        CHECK_FALSE(PrintStatusWidgetTestAccess::runout_modal_shown(widget));
        set_filament(true);
    }
}
