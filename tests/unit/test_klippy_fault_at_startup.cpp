// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A printer whose Klipper is already broken when the screen starts: a config
// error at boot leaves Klippy in `error` before HelixScreen ever connects.
// Discovery's gate stops at that state, so nothing subscribed carries Klippy's
// reason, and the recovery dialog has no edge to fire on. These pin the whole
// path from the gate to the dialog, including the setup wizard in between.

#include "ui_emergency_stop.h"
#include "ui_wizard.h"
#include "ui_wizard_connection.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "klippy_state.h"
#include "moonraker_client_mock.h"
#include "power_device_state.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Klipper's own reason, verbatim from a Snapmaker U1 that refused to start.
constexpr const char* kConfigError =
    "Option 'preload_length' in section 'filament_feed left' must have maximum of 1500.0";

/// Runs the REAL discovery sequence; MoonrakerClientMock overrides
/// discover_printer() and would skip the gate.
class GateClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;
    void discover_printer_real() {
        MoonrakerClient::discover_printer([] {}, [](const std::string&) {});
    }
};

KlippyState klippy_state(PrinterState& ps) {
    return static_cast<KlippyState>(
        lv_subject_get_int(ps.network_state().get_klippy_state_subject()));
}

lv_obj_t* recovery_card() {
    return lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
}

std::string recovery_label(const char* name) {
    lv_obj_t* card = recovery_card();
    REQUIRE(card != nullptr);
    lv_obj_t* label = lv_obj_find_by_name(card, name);
    REQUIRE(label != nullptr);
    return lv_label_get_text(label);
}

/// The overlay wired the way PrinterSession wires it, on a fresh PrinterState
/// that has heard nothing from Moonraker yet.
class StartupFaultFixture : public LVGLUITestFixture {
  public:
    StartupFaultFixture() {
        // The overlay is a process-wide singleton: a fresh subject cycle drops any
        // dialog an earlier test left it holding.
        auto& estop = EmergencyStopOverlay::instance();
        estop.deinit_subjects();
        estop.init_subjects();
        set_wizard_active(false);
        estop.init(state(), api());
        estop.create();
        process_lvgl(50);
    }
    ~StartupFaultFixture() override {
        set_wizard_active(false);
        EmergencyStopOverlay::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    void report(const char* klippy, const std::string& message) {
        state().apply_klippy_status(json{{"state", klippy}, {"state_message", message}});
        process_lvgl(100);
    }
};

} // namespace

TEST_CASE("Discovery onto an errored Klippy fetches its state and reason",
          "[klippy_startup_fault][discovery]") {
    LVGLTestFixture fixture;
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    ps.network_state().reset_klippy_state_freshness();

    GateClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::ERROR);
    client.set_klippy_error_message(kConfigError);

    client.discover_printer_real();
    helix::ui::UpdateQueue::instance().drain();

    // The gate stops discovery before any subscription, and it is the only
    // thing that knew Klippy's state: without the fetch this stays at the
    // SHUTDOWN placeholder with no reason at all.
    CHECK(ps.network_state().klippy_state_reported());
    CHECK(klippy_state(ps) == KlippyState::ERROR);
    CHECK(ps.network_state().get_klippy_fault_message() == kConfigError);
}

TEST_CASE("Klipper config errors are told apart from other faults",
          "[klippy_startup_fault][config_error]") {
    CHECK(is_klippy_config_error(kConfigError));
    CHECK(is_klippy_config_error("Option 'max_temp' in section 'extruder' must be specified"));
    CHECK(is_klippy_config_error("Section 'fan_generic foo' is not a valid config section"));
    CHECK(is_klippy_config_error("Unable to open config file /home/pi/printer_data/printer.cfg"));
    CHECK(is_klippy_config_error("Unknown pin chip name 'toolboard'"));

    CHECK_FALSE(is_klippy_config_error(""));
    CHECK_FALSE(is_klippy_config_error("MCU 'mcu' shutdown: Timer too close"));
    CHECK_FALSE(is_klippy_config_error("Shutdown due to M112 command"));
    CHECK_FALSE(is_klippy_config_error("mcu 'mcu': Unable to connect"));
}

TEST_CASE_METHOD(StartupFaultFixture, "Nothing shows before Klippy reports a state",
                 "[klippy_startup_fault][recovery]") {
    // The placeholder is SHUTDOWN, and a dialog for it flashes on every launch.
    REQUIRE_FALSE(state().network_state().klippy_state_reported());
    process_lvgl(200);
    CHECK(recovery_card() == nullptr);
}

TEST_CASE_METHOD(StartupFaultFixture, "A printer in error at connect shows Klipper's reason",
                 "[klippy_startup_fault][recovery]") {
    report("error", "MCU 'mcu' shutdown: Timer too close");

    REQUIRE(recovery_card() != nullptr);
    CHECK(recovery_label("recovery_message").find("Timer too close") != std::string::npos);
    CHECK(recovery_label("recovery_title") == lv_tr("Printer Error"));
}

TEST_CASE_METHOD(StartupFaultFixture, "A printer already shut down at connect shows the dialog",
                 "[klippy_startup_fault][recovery]") {
    // Same value as the placeholder, so the subject has no change to announce.
    report("shutdown", "Shutdown due to M112 command");

    REQUIRE(recovery_card() != nullptr);
    CHECK(recovery_label("recovery_message").find("M112") != std::string::npos);
}

TEST_CASE_METHOD(StartupFaultFixture, "A fault during the setup wizard shows once it completes",
                 "[klippy_startup_fault][recovery][wizard]") {
    set_wizard_active(true);
    process_lvgl(50);

    report("error", kConfigError);
    process_lvgl(200);
    REQUIRE(recovery_card() == nullptr);

    set_wizard_active(false);
    process_lvgl(100);

    REQUIRE(recovery_card() != nullptr);
    CHECK(recovery_label("recovery_message").find("preload_length") != std::string::npos);
}

TEST_CASE_METHOD(StartupFaultFixture, "Closing the wizard over a healthy printer shows nothing",
                 "[klippy_startup_fault][recovery][wizard]") {
    set_wizard_active(true);
    process_lvgl(50);
    report("ready", "Printer is ready");

    set_wizard_active(false);
    process_lvgl(200);
    CHECK(recovery_card() == nullptr);
}

TEST_CASE_METHOD(StartupFaultFixture,
                 "A config error says the config needs fixing and keeps both restarts",
                 "[klippy_startup_fault][recovery][config_error]") {
    report("error", kConfigError);
    REQUIRE(recovery_card() != nullptr);

    CHECK(recovery_label("recovery_title") == lv_tr("Klipper Config Error"));
    const std::string message = recovery_label("recovery_message");
    CHECK(message.find(lv_tr("Klipper could not load its configuration. Fix the setting below "
                             "in the printer's config files, then restart Klipper.")) == 0);
    CHECK(message.find(kConfigError) != std::string::npos);

    // Firmware Restart is what reloads the fixed file, so the row stays.
    lv_obj_t* actions = lv_obj_find_by_name(recovery_card(), "recovery_restart_actions");
    REQUIRE(actions != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(actions));
}

TEST_CASE_METHOD(StartupFaultFixture, "A mainboard powered off at connect shows no dialog",
                 "[klippy_startup_fault][power]") {
    constexpr const char* kUnreachable = "mcu 'mcu': Unable to connect";
    struct PowerReset {
        ~PowerReset() {
            PowerDeviceState::instance().deinit_subjects();
        }
    } power_reset;

    SECTION("the printer's power device is off") {
        PowerDeviceState::instance().set_devices({PowerDevice{"printer", "gpio", "off", false}});
        report("error", kUnreachable);
        CHECK(recovery_card() == nullptr);
    }
    SECTION("the power device is on, so the MCU really is unreachable") {
        PowerDeviceState::instance().set_devices({PowerDevice{"printer", "gpio", "on", false}});
        report("error", kUnreachable);
        REQUIRE(recovery_card() != nullptr);
        CHECK(recovery_label("recovery_message").find(kUnreachable) != std::string::npos);
    }
    SECTION("a config error still shows while the power is off") {
        PowerDeviceState::instance().set_devices({PowerDevice{"printer", "gpio", "off", false}});
        report("error", kConfigError);
        CHECK(recovery_card() != nullptr);
    }
}

TEST_CASE_METHOD(StartupFaultFixture, "A power-off that stops Klipper shows no dialog",
                 "[klippy_startup_fault][power][recovery]") {
    struct PowerReset {
        ~PowerReset() {
            PowerDeviceState::instance().deinit_subjects();
        }
    } power_reset;
    auto& power = PowerDeviceState::instance();
    power.set_devices(
        {PowerDevice{"printer", "gpio", "off", false}, PowerDevice{"light", "gpio", "on", false}});
    report("ready", "");
    REQUIRE(recovery_card() == nullptr);

    // Moonraker's notify_klippy_disconnected carries no message.
    const auto klipper_stops = [&] {
        state().set_klippy_state(KlippyState::SHUTDOWN);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(100);
    };

    SECTION("the device bound to Klipper is off") {
        power.set_klipper_bound_devices({"printer"});
        klipper_stops();
        CHECK(recovery_card() == nullptr);
    }
    SECTION("a bound device that is on leaves the stop unexplained") {
        power.set_klipper_bound_devices({"light"});
        klipper_stops();
        CHECK(recovery_card() != nullptr);
    }
    SECTION("an unrelated device being off explains nothing") {
        power.set_klipper_bound_devices({});
        klipper_stops();
        CHECK(recovery_card() != nullptr);
    }
}

TEST_CASE("Discovery learns which power devices stop Klipper",
          "[klippy_startup_fault][power][discovery]") {
    LVGLTestFixture fixture;
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    struct PowerReset {
        ~PowerReset() {
            PowerDeviceState::instance().deinit_subjects();
        }
    } power_reset;

    GateClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::ERROR);
    client.set_klippy_error_message(kConfigError);
    client.discover_printer_real();
    helix::ui::UpdateQueue::instance().drain();

    auto& power = PowerDeviceState::instance();
    REQUIRE_FALSE(power.klipper_bound_device_off());

    power.update_device_status("printer_psu", "off");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(power.klipper_bound_device_off());
}

TEST_CASE("Klipper's MCU connect failures are told apart from other faults",
          "[klippy_startup_fault][power]") {
    CHECK(is_klippy_mcu_connect_error("mcu 'mcu': Unable to connect"));
    CHECK(is_klippy_mcu_connect_error("mcu 'toolboard': Unable to connect\nOnce the underlying "
                                      "issue is corrected, use the \"FIRMWARE_RESTART\" command"));
    CHECK(is_klippy_mcu_connect_error("mcu 'mcu': Timeout on connect"));

    CHECK_FALSE(is_klippy_mcu_connect_error(""));
    CHECK_FALSE(is_klippy_mcu_connect_error("MCU 'mcu' shutdown: Timer too close"));
    CHECK_FALSE(is_klippy_mcu_connect_error(kConfigError));
}

TEST_CASE_METHOD(StartupFaultFixture, "Klipper coming up after a startup fault dismisses it",
                 "[klippy_startup_fault][recovery]") {
    report("error", kConfigError);
    REQUIRE(recovery_card() != nullptr);

    report("ready", "Printer is ready");
    process_lvgl(100);
    CHECK(recovery_card() == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "The wizard's Connection step shows Klipper's reason",
                 "[klippy_startup_fault][wizard][connection]") {
    ui_wizard_init_subjects();
    WizardConnectionStep* step = get_wizard_connection_step();
    step->init_subjects();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* status = lv_obj_find_by_name(root, "connection_status_text");
    REQUIRE(status != nullptr);

    // Let the step's own localhost auto-probe run first: it writes the status too.
    process_lvgl(300);

    // Discovery gives up first; the reason lands a round trip later.
    step->allow_continue_without_klipper();
    process_lvgl(50);
    CHECK(std::string(lv_label_get_text(status)).find("preload_length") == std::string::npos);

    state().apply_klippy_status(json{{"state", "error"}, {"state_message", kConfigError}});
    process_lvgl(100);

    CHECK(std::string(lv_label_get_text(status)).find(kConfigError) != std::string::npos);
    step->cleanup();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A printer switch does not carry the old printer's Klipper error into the wizard",
                 "[klippy_startup_fault][wizard][connection]") {
    state().apply_klippy_status(json{{"state", "error"}, {"state_message", kConfigError}});
    process_lvgl(50);
    REQUIRE(state().network_state().get_klippy_fault_message() == kConfigError);

    // Add Printer and a switch tear printer state down and build it again for
    // the next printer, which has reported nothing yet.
    state().deinit_subjects();
    state().init_subjects(true);
    CHECK(state().network_state().get_klippy_fault_message().empty());

    ui_wizard_init_subjects();
    WizardConnectionStep* step = get_wizard_connection_step();
    step->init_subjects();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* status = lv_obj_find_by_name(root, "connection_status_text");
    REQUIRE(status != nullptr);
    process_lvgl(300);

    step->allow_continue_without_klipper();
    process_lvgl(50);
    CHECK(std::string(lv_label_get_text(status)).find("preload_length") == std::string::npos);
    step->cleanup();
}
