// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_discovery.h"

#include "ui_update_queue.h"

#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "humidity_sensor_manager.h"
#include "i_moonraker_client.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "load_cell_manager.h"
#include "moonraker_api.h"
#include "printer_name_sync.h"
#include "probe_sensor_manager.h"
#include "runtime_config.h"
#include "spdlog/spdlog.h"
#include "standard_macros.h"
#include "temperature_sensor_manager.h"
#include "tool_state.h"
#include "width_sensor_manager.h"
#include "width_sensor_types.h"
#include "wizard_config_paths.h"

#include <vector>

namespace helix {

std::vector<std::string> temperature_sensor_objects(const PrinterDiscovery& hardware) {
    std::vector<std::string> objects = hardware.sensors();
    for (const auto& heater : hardware.heaters()) {
        if (heater.rfind("heater_generic ", 0) == 0)
            objects.push_back(heater);
    }
    return objects;
}

void init_subsystems_from_hardware(const PrinterDiscovery& hardware, IMoonrakerAPI* api,
                                   IMoonrakerClient* client) {
    spdlog::debug("[PrinterDiscovery] Initializing subsystems from hardware discovery");

    // Initialize AMS backend (AFC, Happy Hare, ACE, Tool Changer)
    AmsState::instance().init_backend_from_hardware(hardware, api, client);

    // Sensor discovery is unconditional: an empty list is what clears the
    // previous printer's sensors on a switch.
    auto& fsm = FilamentSensorManager::instance();
    fsm.discover_sensors(hardware.filament_sensor_names());
    fsm.load_config_from_file();

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    tsm.discover(temperature_sensor_objects(hardware));

    // Initialize load cell manager
    // hardware.load_cells() returns load_cell objects
    auto& lcm = helix::sensors::LoadCellManager::instance();
    lcm.discover(hardware.load_cells());

    // Initialize probe sensor manager
    // Probe sensors (bltouch, cartographer, beacon, etc.) are discovered from the full objects list
    auto& psm = helix::sensors::ProbeSensorManager::instance();
    psm.discover(hardware.printer_objects());
    psm.load_config_from_file();

    // Initialize humidity sensor manager
    // Humidity sensors (bme280, htu21d) are discovered from the full objects list
    auto& hsm = helix::sensors::HumiditySensorManager::instance();
    hsm.discover(hardware.printer_objects());

    // Initialize width sensor manager
    // Width sensors (hall/tsl1401cl filament width) are discovered from Klipper objects
    auto& wsm = helix::sensors::WidthSensorManager::instance();
    wsm.discover(hardware.width_sensor_objects());
    wsm.load_config_from_file();

    // Initialize multi-extruder temperature tracking
    auto& printer_state = get_printer_state();
    printer_state.temperature_state().init_extruders(hardware.heaters());

    // Initialize tool changer state from discovered hardware
    helix::ToolState::instance().init_tools(hardware);

    // Seed the per-tool offsets. Must follow init_tools(): the initial status
    // snapshot arrived before tools_ existed, so it was dropped, and Moonraker
    // republishes only what changes afterwards.
    helix::ToolState::instance().query_tool_offsets(client, hardware);

    // Restore persisted spool assignments (Moonraker DB primary, local JSON fallback)
    helix::ToolState::instance().load_spool_assignments(api);

    // Sync printer name from Mainsail/Fluidd DB (seeds local config on first connect)
    helix::PrinterNameSync::resolve(api, hardware.hostname());

    // Initialize standard macros
    // Type from Config, NOT PrinterState: this callback runs before
    // auto_detect_and_save sets PrinterState's copy, so reading that one hands
    // StandardMacros an empty string and the shipped tier never fills. Same trap
    // and same remedy as the CFS backend's K1/K2 dialect latch.
    StandardMacros::instance().init(hardware, helix::get_saved_printer_type());

    // Initialize LED controller and discover LED backends.
    // PrinterSession::init_core_subjects ran init(nullptr, nullptr) earlier so the
    // led_controllable subject was registered before XML instantiation; this call
    // re-runs init() to rebind the real api/client (init() always overwrites
    // api_/client_ + backend api pointers, and the subject path is idempotent).
    auto& led_ctrl = helix::led::LedController::instance();
    led_ctrl.init(api, client);
    led_ctrl.discover_from_hardware(hardware);
    // Wire up auto-state LED control. init() reloads the per-printer auto-state
    // config and (re)subscribes to PrinterState subjects. Because switch_printer()
    // re-runs this discovery path, this also reloads per-printer auto-state config
    // on printer switch automatically. Runs on the main thread (this function is
    // invoked inside a queue_update() drain), so the observe<int> subscriptions
    // are main-thread-safe.
    helix::led::LedAutoState::instance().init(printer_state);
    led_ctrl.discover_wled_strips();

    spdlog::info("[PrinterDiscovery] Subsystem initialization complete");
}

} // namespace helix
