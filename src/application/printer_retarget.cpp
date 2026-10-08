// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_retarget.h"

#include "ui_emergency_stop.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "i_moonraker_client.h"
#include "lap_log.h"
#include "moonraker_manager.h"
#include "print_history_manager.h"
#include "printer_state.h"
#include "sound_manager.h"

#include <spdlog/spdlog.h>

#include <string>

#include "hv/json.hpp"

namespace helix {

namespace {

/// The disconnect looks exactly like an unexpected drop; suppress the recovery dialog so an
/// intentional one doesn't raise it.
IMoonrakerClient* disconnect_for_retarget() {
    IMoonrakerClient* client = get_moonraker_client();
    if (!client || !get_moonraker_manager()) {
        spdlog::error("[PrinterRetarget] Cannot reconnect - client or manager unavailable");
        return nullptr;
    }
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::SHORT);
    client->disconnect();
    return client;
}

/// The active printer's Moonraker as "host:port".
std::string active_printer_host_port(const std::string& default_host) {
    Config* config = Config::get_instance();
    return config->get<std::string>(config->df() + "moonraker_host", default_host) + ":" +
           std::to_string(config->get<int>(config->df() + "moonraker_port", 7125));
}

/// The new printer's first status replaces the live values; an unreachable one never sends
/// any, so the previous printer's job, message, temperatures and history are cleared here
/// rather than shown as the new printer's.
void forget_previous_printer() {
    PrinterState& ps = get_printer_state();
    nlohmann::json neutral = {
        {"print_stats", {{"state", "standby"}, {"filename", ""}, {"message", ""}}},
        {"display_status", {{"message", nullptr}, {"progress", 0.0}}},
        {"heater_bed", {{"temperature", 0.0}, {"target", 0.0}}}};
    for (const auto& [name, info] : ps.temperature_state().extruders()) {
        neutral[name] = {{"temperature", 0.0}, {"target", 0.0}};
    }
    ps.update_from_status(neutral);

    if (PrintHistoryManager* history = get_print_history_manager()) {
        history->forget_printer();
    }
}

} // namespace

std::string active_printer_ws_url(const std::string& default_host) {
    return "ws://" + active_printer_host_port(default_host) + "/websocket";
}

std::string active_printer_http_url(const std::string& default_host) {
    return "http://" + active_printer_host_port(default_host);
}

bool connect_printer(MoonrakerManager& manager, const std::string& ws_url,
                     const std::string& http_url) {
    // Tracks PRINT_START progress for this printer, with its own print-start profile.
    // Created before the connect: the observers it installs on the API are then in place
    // before the WebSocket task can call them.
    manager.init_print_start_collector();
    spdlog::info("[PrinterRetarget] Connecting to {}", ws_url);
    if (manager.connect(ws_url, http_url) != 0) {
        spdlog::error("[PrinterRetarget] Connecting to {} could not start", ws_url);
        manager.release_print_start_collector();
        return false;
    }
    return true;
}

bool connect_active_printer() {
    MoonrakerManager* manager = get_moonraker_manager();
    if (!manager) {
        spdlog::error("[PrinterRetarget] Cannot connect - no manager");
        return false;
    }
    return connect_printer(*manager, active_printer_ws_url(), active_printer_http_url());
}

bool reconnect_active_printer() {
    if (!disconnect_for_retarget()) {
        return false;
    }
    return connect_active_printer();
}

bool retarget_printer_connection() {
    IMoonrakerClient* client = disconnect_for_retarget();
    if (!client) {
        return false;
    }

    // The old printer's queued frames apply now rather than on top of the new printer.
    LapLog laps("PrinterRetarget");
    get_moonraker_manager()->process_notifications();
    laps.lap("drain notifications");

    AmsState::instance().clear_backends();
    laps.lap("clear filament backends");
    forget_previous_printer();
    laps.lap("forget previous printer");
    // An M300 beeper belongs to the previous printer, and its sequencer thread holds an
    // internal-RAM stack the next WebSocket task may need. The next printer's discovery
    // installs one again only if that printer has a beeper.
    SoundManager::instance().set_moonraker_client(nullptr, /*host_recovery=*/true);
    SoundManager::instance().set_moonraker_client(client);
    laps.lap("drop printer sound");
    get_printer_state().set_active_printer_name(Config::get_instance()->get_active_printer_name());

    return connect_active_printer();
}

} // namespace helix
