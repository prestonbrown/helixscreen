// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_led_silent_sends.cpp
 * @brief The LED sends that are NOT user commands must not claim the busy toast.
 *
 * The busy gate itself — a silent discretionary send queues fire-and-forget
 * without claiming the once-per-episode toast — is pinned in
 * test_moonraker_api_busy_guard.cpp against execute_gcode() directly. These
 * cases pin the CALLER chain above it: LedAutoState's theme apply on a
 * print-state transition, every branch of LedController::set_power() it
 * reaches (native turn-off, output-pin turn-off, LED-effect stop), and the
 * discovery-triggered startup-preference apply. All of them fire during print
 * start, inside the gate's window, so a dropped `silent` anywhere in the chain
 * claims the toast and the claim below fails.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "print_start_phase.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

struct SilentSendFixture : public LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;

    SilentSendFixture() {
        // Detach any LedAutoState observers an earlier test left pointed at
        // another PrinterState.
        helix::led::LedAutoState::instance().deinit();

        state.init_subjects(false);
        state.set_klippy_state_sync(helix::KlippyState::READY);
        lv_subject_set_int(state.print_state().get_print_state_enum_subject(),
                           static_cast<int>(helix::PrintJobState::STANDBY));
        lv_subject_set_int(state.temperature_state().get_active_extruder_target_subject(), 0);

        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPIMock>(client, state);

        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit(); // fresh startup-preference latch for every case
        ctrl.init(api.get(), &client);

        helix::led::LedStripInfo chamber;
        chamber.id = "neopixel chamber_light";
        chamber.name = "neopixel chamber_light";
        chamber.backend = helix::led::LedBackendType::NATIVE;
        chamber.supports_color = true;
        chamber.supports_white = true;
        ctrl.native().add_strip(chamber);

        helix::led::LedStripInfo pin;
        pin.id = "output_pin cabinet";
        pin.name = "output_pin cabinet";
        pin.backend = helix::led::LedBackendType::OUTPUT_PIN;
        pin.is_pwm = true;
        ctrl.output_pin().add_pin(pin);

        // set_power(false) stops effects targeting the strips being turned
        // off, so one must exist for that branch to run.
        helix::led::LedEffectInfo breathing;
        breathing.name = "led_effect breathing";
        breathing.display_name = "Breathing";
        breathing.target_leds = {"neopixel chamber_light"};
        ctrl.effects().add_effect(breathing);
    }

    ~SilentSendFixture() override {
        drain();
        helix::led::LedAutoState::instance().deinit();
        helix::led::LedController::instance().deinit();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Arm the print-start window the busy gate reads. The phase subject is
    /// not one LedAutoState observes, so arming it triggers no LED send.
    void arm_print_start() {
        lv_subject_set_int(state.print_state().get_print_start_phase_subject(),
                           static_cast<int>(helix::PrintStartPhase::HOMING));
        REQUIRE(state.print_state().is_in_print_start());
    }

    bool sent_gcode_containing(const char* needle) const {
        const auto& history = client.gcode_script_history();
        return std::any_of(history.begin(), history.end(), [needle](const std::string& g) {
            return g.find(needle) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(SilentSendFixture,
                 "LedAutoState: a print-start theme change sends without claiming the busy toast",
                 "[led][silent_send]") {
    auto& as = helix::led::LedAutoState::instance();
    as.init(state);
    as.set_mapping("printing", {"color", 0x00FF00, 100, "", 0, ""});
    as.set_strips({"neopixel chamber_light"});
    as.set_enabled(true); // evaluate() applies the idle default outside the gate
    drain();
    client.clear_gcode_script_history();

    arm_print_start();
    lv_subject_set_int(state.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    drain();

    REQUIRE(sent_gcode_containing("SET_LED"));                 // the theme send reached Klipper
    CHECK(state.calibration_state().claim_busy_queue_toast()); // silent chain claimed nothing
}

TEST_CASE_METHOD(SilentSendFixture,
                 "LedAutoState: an off action during print start stops effects and pins silently",
                 "[led][silent_send]") {
    auto& as = helix::led::LedAutoState::instance();
    as.init(state);
    as.set_mapping("printing", {"off", 0xFFFFFF, 100, "", 0, ""});
    as.set_strips({"neopixel chamber_light", "output_pin cabinet"});
    as.set_enabled(true);
    drain();
    client.clear_gcode_script_history();

    arm_print_start();
    lv_subject_set_int(state.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    drain();

    // Every set_power(false) branch fired: effect stop, native turn-off,
    // output-pin turn-off. All discretionary, all inside the window.
    REQUIRE(sent_gcode_containing("SET_LED_EFFECT"));
    REQUIRE(sent_gcode_containing("SET_LED"));
    REQUIRE(sent_gcode_containing("SET_PIN"));
    CHECK(state.calibration_state().claim_busy_queue_toast());
}

TEST_CASE_METHOD(SilentSendFixture,
                 "LedController: startup preference during print start claims no busy toast",
                 "[led][silent_send]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.set_led_on_at_start(true);
    ctrl.set_startup_brightness(80);

    arm_print_start();
    lv_subject_set_int(state.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::PRINTING));
    ctrl.apply_startup_preference({"neopixel chamber_light"});
    drain();

    REQUIRE(sent_gcode_containing("SET_LED"));
    CHECK(state.calibration_state().claim_busy_queue_toast());
}
