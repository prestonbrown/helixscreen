// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_discovery.h"

#include "ui_update_queue.h"

#include "ams_state.h"
#include "app_globals.h"
#include "audio_settings_manager.h"
#include "i_moonraker_client.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "moonraker_api.h"
#include "printer_name_sync.h"
#include "runtime_config.h"
#include "sensor_manager.h"
#include "spdlog/spdlog.h"
#include "standard_macros.h"
#include "tool_state.h"
#include "wizard_config_paths.h"

#include <vector>

namespace helix {

PrinterDiscovery::PrinterDiscovery() = default;
PrinterDiscovery::PrinterDiscovery(const PrinterDiscovery&) = default;
PrinterDiscovery::PrinterDiscovery(PrinterDiscovery&&) noexcept = default;
PrinterDiscovery& PrinterDiscovery::operator=(const PrinterDiscovery&) = default;
PrinterDiscovery& PrinterDiscovery::operator=(PrinterDiscovery&&) noexcept = default;
PrinterDiscovery::~PrinterDiscovery() = default;

void remember_printer_hostname(const std::string& hostname) {
    if (hostname.empty() || hostname == "unknown") {
        return;
    }
    Config* config = Config::get_instance();
    const std::string path = config->df() + helix::wizard::kHostname;
    if (config->get<std::string>(path, "") != hostname) {
        config->set<std::string>(path, hostname);
        config->save();
    }
}

void reload_settings_after_preset() {
    for (auto* m : helix::sensors::sensor_managers()) {
        m->load_settings();
    }
    AudioSettingsManager::instance().reload_from_config();
}

void init_subsystems_from_hardware(const PrinterDiscovery& hardware, IMoonrakerAPI* api,
                                   IMoonrakerClient* client) {
    spdlog::debug("[PrinterDiscovery] Initializing subsystems from hardware discovery");

    // Initialize AMS backend (AFC, Happy Hare, ACE, Tool Changer)
    AmsState::instance().init_backend_from_hardware(hardware, api, client);

    // Sensor discovery is unconditional: an empty list is what clears the
    // previous printer's sensors on a switch.

    for (auto* m : helix::sensors::sensor_managers()) {
        m->discover(hardware);
        m->load_settings();
    }

    // Initialize multi-extruder temperature tracking
    auto& printer_state = get_printer_state();
    printer_state.temperature_state().init_extruders(hardware.heaters());

    // Initialize tool changer state from discovered hardware
    helix::ToolState::instance().init_tools(hardware);
    // init_tools() mounts T0 and drops any backend topology. A backend started
    // above, or still running from the last discovery, already said which head
    // is mounted and sends nothing more until its state changes, so its answer
    // goes back to ToolState here.
    AmsState::instance().sync_from_backend();

    // Seed the per-tool offsets. Must follow init_tools(): the initial status
    // snapshot arrived before tools_ existed, so it was dropped, and Moonraker
    // republishes only what changes afterwards.
    helix::ToolState::instance().query_tool_offsets(client, hardware);

    // Restore persisted spool assignments (Moonraker DB primary, local JSON fallback)
    helix::ToolState::instance().load_spool_assignments(api);

    // Sync printer name from Mainsail/Fluidd DB (seeds local config on first connect)
    helix::PrinterNameSync::resolve(api, hardware.hostname());

    remember_printer_hostname(hardware.hostname());

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
