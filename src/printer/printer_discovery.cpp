// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_discovery.h"

#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "led/led_controller.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "spdlog/spdlog.h"
#include "standard_macros.h"
#include "temperature_sensor_manager.h"
#include "tool_state.h"

#include <sstream>
#include <vector>

namespace helix {

std::string PrinterDiscovery::summary() const {
    std::ostringstream ss;
    ss << "Capabilities: ";

    std::vector<std::string> caps;

    if (has_qgl_)
        caps.push_back("QGL");
    if (has_z_tilt_)
        caps.push_back("Z-tilt");
    if (has_bed_mesh_)
        caps.push_back("bed_mesh");
    if (has_chamber_heater_)
        caps.push_back("chamber_heater");
    if (has_chamber_sensor_)
        caps.push_back("chamber_sensor");
    if (has_exclude_object_)
        caps.push_back("exclude_object");
    if (has_probe_)
        caps.push_back("probe");
    if (has_heater_bed_)
        caps.push_back("heater_bed");
    if (has_led_)
        caps.push_back("LED");
    if (has_accelerometer_)
        caps.push_back("accelerometer");
    if (has_screws_tilt_)
        caps.push_back("screws_tilt");
    if (has_klippain_shaketune_)
        caps.push_back("Klippain");
    if (has_speaker_)
        caps.push_back("speaker");
    if (has_firmware_retraction_)
        caps.push_back("firmware_retraction");
    if (has_mmu_)
        caps.push_back(mmu_type_ == AmsType::HAPPY_HARE ? "Happy Hare" : "AFC");
    if (has_tool_changer_) {
        std::string tc_str = "Tool Changer";
        if (!tool_names_.empty()) {
            tc_str += " (" + std::to_string(tool_names_.size()) + " tools)";
        }
        caps.push_back(tc_str);
    }
    if (has_timelapse_)
        caps.push_back("timelapse");
    if (!filament_sensor_names_.empty())
        caps.push_back("filament_sensors(" + std::to_string(filament_sensor_names_.size()) + ")");

    if (caps.empty()) {
        ss << "none";
    } else {
        for (size_t i = 0; i < caps.size(); ++i) {
            if (i > 0)
                ss << ", ";
            ss << caps[i];
        }
    }

    ss << " | " << macros_.size() << " macros";
    if (!helix_macros_.empty()) {
        ss << " (" << helix_macros_.size() << " HELIX_*)";
    }

    return ss.str();
}

void init_subsystems_from_hardware(const PrinterDiscovery& hardware, MoonrakerAPI* api,
                                   MoonrakerClient* client) {
    spdlog::debug("[PrinterDiscovery] Initializing subsystems from hardware discovery");

    // Initialize AMS backend (AFC, Happy Hare, ValgACE, Tool Changer)
    AmsState::instance().init_backend_from_hardware(hardware, api, client);

    // Initialize filament sensor manager
    if (hardware.has_filament_sensors()) {
        auto& fsm = FilamentSensorManager::instance();
        fsm.discover_sensors(hardware.filament_sensor_names());
        fsm.load_config_from_file();
        spdlog::debug("[PrinterDiscovery] Discovered {} filament sensors",
                      hardware.filament_sensor_names().size());
    }

    // Initialize temperature sensor manager
    // hardware.sensors() returns temperature_sensor and temperature_fan objects
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    tsm.discover(hardware.sensors());

    // Initialize multi-extruder temperature tracking
    auto& printer_state = get_printer_state();
    printer_state.init_extruders(hardware.heaters());

    // Initialize tool changer state from discovered hardware
    helix::ToolState::instance().init_tools(hardware);

    // Initialize standard macros
    StandardMacros::instance().init(hardware);

    // Initialize LED controller and discover LED backends
    auto& led_ctrl = helix::led::LedController::instance();
    if (!led_ctrl.is_initialized()) {
        led_ctrl.init(api, client);
    }
    led_ctrl.discover_from_hardware(hardware);
    led_ctrl.discover_wled_strips();

    // Set tracked LED early so the subscription response populates subjects correctly.
    // LedWidget::bind_led() will also call this (idempotent) when it attaches later.
    const auto& strips = led_ctrl.selected_strips();
    if (!strips.empty()) {
        printer_state.set_tracked_led(strips.front());
    }

    spdlog::info("[PrinterDiscovery] Subsystem initialization complete");
}

} // namespace helix
