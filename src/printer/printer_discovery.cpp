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
        caps.push_back(ams_type_to_string(mmu_type_));
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

void init_subsystems_from_hardware(const PrinterDiscovery& hardware, IMoonrakerAPI* api,
                                   IMoonrakerClient* client) {
    spdlog::debug("[PrinterDiscovery] Initializing subsystems from hardware discovery");

    // Initialize AMS backend (AFC, Happy Hare, ACE, Tool Changer)
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
    if (hardware.has_width_sensors()) {
        auto& wsm = helix::sensors::WidthSensorManager::instance();
        wsm.discover(hardware.width_sensor_objects());
        wsm.load_config_from_file();
        spdlog::debug("[PrinterDiscovery] Discovered {} width sensors",
                      hardware.width_sensor_objects().size());
    }

    // Initialize multi-extruder temperature tracking
    auto& printer_state = get_printer_state();
    printer_state.init_extruders(hardware.heaters());

    // Initialize tool changer state from discovered hardware
    helix::ToolState::instance().init_tools(hardware);

    // Restore persisted spool assignments (Moonraker DB primary, local JSON fallback)
    helix::ToolState::instance().load_spool_assignments(api);

    // Sync printer name from Mainsail/Fluidd DB (seeds local config on first connect)
    helix::PrinterNameSync::resolve(api, hardware.hostname());

    // Initialize standard macros
    StandardMacros::instance().init(hardware);

    // Initialize LED controller and discover LED backends.
    // Application::init_core_subjects ran init(nullptr, nullptr) earlier so the
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
    // invoked inside a queue_update() drain), so the observe_int_sync subscriptions
    // are main-thread-safe.
    helix::led::LedAutoState::instance().init(printer_state);
    led_ctrl.discover_wled_strips();

    // Set tracked LED early so the subscription response populates subjects correctly.
    // LedWidget::bind_led() will also call this (idempotent) when it attaches later.
    const auto& strips = led_ctrl.selected_strips();
    if (!strips.empty()) {
        printer_state.set_tracked_led(strips.front());

        // Query the tracked LED's current state explicitly.
        // The subscription response may return empty data for LEDs whose state was
        // set before we subscribed (e.g., LED effects enabled by Klipper macros at
        // startup).  A direct query always returns the current values.
        std::string tracked = strips.front();
        nlohmann::json query_objects = nlohmann::json::object();
        query_objects[tracked] = nullptr;
        client->send_jsonrpc("printer.objects.query", {{"objects", query_objects}},
                             [tracked](nlohmann::json response) {
                                 if (!response.contains("result") ||
                                     !response["result"].contains("status")) {
                                     return;
                                 }
                                 const auto& status = response["result"]["status"];
                                 if (!status.contains(tracked)) {
                                     return;
                                 }
                                 // Feed into PrinterState on the UI thread
                                 helix::ui::queue_update([status]() {
                                     get_printer_state().update_from_status(status);
                                 });
                             });
    }

    spdlog::info("[PrinterDiscovery] Subsystem initialization complete");
}

} // namespace helix
