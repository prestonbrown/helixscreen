// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

namespace {

/// Moonraker lists `spoolman` in server.info only when the component is
/// configured. Reporting it on the WIRE — rather than short-circuiting the
/// capability flag after discovery — is what lets the real discovery sequence's
/// spoolman branch be exercised at all.
nlohmann::json mock_server_components(bool with_spoolman) {
    nlohmann::json comps = nlohmann::json::array({"file_manager", "database", "machine", "history",
                                                  "announcements", "job_queue", "update_manager"});
    if (with_spoolman) {
        comps.push_back("spoolman");
    }
    return comps;
}

} // namespace

namespace mock_internal {

void register_server_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    // server.connection.identify - Identify client to Moonraker for notifications
    // https://moonraker.readthedocs.io/en/latest/web_api/#identify-connection
    registry["server.connection.identify"] =
        [](MoonrakerClientMock* /*self*/, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        // Log the identification for debugging
        std::string client_name = params.value("client_name", "unknown");
        std::string version = params.value("version", "unknown");
        std::string type = params.value("type", "unknown");

        spdlog::debug("[MoonrakerClientMock] server.connection.identify: {} v{} ({})", client_name,
                      version, type);

        // Return a successful response with mock connection_id
        // This matches the real Moonraker response format
        static std::atomic<int> connection_counter{1000};
        json response = {{"jsonrpc", "2.0"},
                         {"result", {{"connection_id", connection_counter.fetch_add(1)}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.spoolman.status - Spoolman connectivity, queried by the real
    // discovery sequence whenever the component appears in server.info.
    registry["server.spoolman.status"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        json response = {{"jsonrpc", "2.0"},
                         {"result",
                          {{"spoolman_connected", self->is_mock_spoolman_enabled()},
                           {"pending_reports", json::array()},
                           {"spool_id", nullptr}}}};
        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.info - Get Moonraker server information
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-server-info
    registry["server.info"] = [](MoonrakerClientMock* self, const json& /*params*/,
                                 std::function<void(const json&)> success_cb,
                                 std::function<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        // Map KlippyState enum to string
        std::string klippy_state_str;
        bool klippy_connected = false;
        switch (self->get_klippy_state()) {
        case MoonrakerClientMock::KlippyState::READY:
            klippy_state_str = "ready";
            klippy_connected = true;
            break;
        case MoonrakerClientMock::KlippyState::STARTUP:
            klippy_state_str = "startup";
            klippy_connected = false;
            break;
        case MoonrakerClientMock::KlippyState::SHUTDOWN:
            klippy_state_str = "shutdown";
            klippy_connected = true;
            break;
        case MoonrakerClientMock::KlippyState::ERROR:
            klippy_state_str = "error";
            klippy_connected = true;
            break;
        }

        spdlog::debug("[MoonrakerClientMock] server.info: klippy_state={}, connected={}",
                      klippy_state_str, klippy_connected);

        json response = {{"jsonrpc", "2.0"},
                         {"result",
                          {{"klippy_connected", klippy_connected},
                           {"klippy_state", klippy_state_str},
                           {"moonraker_version", "v0.8.0-mock"},
                           {"api_version", json::array({1, 5, 0})},
                           {"api_version_string", "1.5.0"},
                           // Spoolman is reported on the WIRE, the way Moonraker reports it,
                           // rather than by short-circuiting the capability flag after
                           // discovery. The short-circuit is why the real sequence's
                           // spoolman branch went untested and shipped a flag flap.
                           {"components", mock_server_components(self->is_mock_spoolman_enabled())},
                           {"failed_components", json::array()},
                           {"registered_directories", json::array({"gcodes", "config", "logs"})},
                           {"warnings", json::array()},
                           {"websocket_count", 1}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // printer.info - Get Klipper printer information
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-printer-info
    registry["printer.info"] = [](MoonrakerClientMock* self, const json& /*params*/,
                                  std::function<void(const json&)> success_cb,
                                  std::function<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        // Map KlippyState enum to string and state message
        std::string state_str;
        std::string state_message;
        switch (self->get_klippy_state()) {
        case MoonrakerClientMock::KlippyState::READY:
            state_str = "ready";
            state_message = "Printer is ready";
            break;
        case MoonrakerClientMock::KlippyState::STARTUP:
            state_str = "startup";
            state_message = "Printer is starting up";
            break;
        case MoonrakerClientMock::KlippyState::SHUTDOWN:
            state_str = "shutdown";
            state_message = "Printer has been shut down";
            break;
        case MoonrakerClientMock::KlippyState::ERROR:
            state_str = "error";
            state_message = "Printer is in error state";
            break;
        }

        spdlog::debug("[MoonrakerClientMock] printer.info: state={}", state_str);

        // Detect HELIX_MOCK_KALICO env var for Kalico firmware simulation
        const char* kalico_env = std::getenv("HELIX_MOCK_KALICO");
        bool mock_kalico = kalico_env && std::string(kalico_env) == "1";
        std::string app_name = mock_kalico ? "Kalico" : "Klipper";

        // Printer-type-specific hostname so PrinterDetector's hostname heuristic
        // resolves the mock to the matching printer_database.json entry. The
        // generic "mock-printer" matches no fingerprint, leaving the printer
        // type empty (and thus get_pre_print_option_set() empty). AD5M needs an
        // "ad5m" hostname to hit its 90%-confidence heuristic so its pre-print
        // options (incl. the bed_mesh adaptive_param) load under --test.
        const char* hostname = "mock-printer";
        switch (self->get_printer_type()) {
        case MoonrakerClientMock::PrinterType::FLASHFORGE_AD5M:
            hostname = "ad5m-mock";
            break;
        default:
            break;
        }

        json response = {{"jsonrpc", "2.0"},
                         {"result",
                          {{"state", state_str},
                           {"state_message", state_message},
                           {"hostname", hostname},
                           {"app", app_name},
                           {"software_version", "v0.12.0-mock"},
                           {"klipper_path", "/home/pi/klipper"},
                           {"python_path", "/home/pi/klippy-env/bin/python"},
                           {"log_file", "/home/pi/printer_data/logs/klippy.log"}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // machine.system_info - Get OS/system information
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-system-info
    registry["machine.system_info"] =
        [](MoonrakerClientMock* /*self*/, const json& /*params*/,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        spdlog::debug("[MoonrakerClientMock] machine.system_info");

        json response = {
            {"jsonrpc", "2.0"},
            {"result",
             {{"system_info",
               {{"cpu_info",
                 {{"cpu_count", 4},
                  {"total_memory", 3906644},
                  {"memory_units", "kB"},
                  {"processor", "ARMv7 Processor rev 5 (v7l)"}}},
                {"distribution",
                 {{"name", "Ubuntu 22.04 LTS (mock)"},
                  {"id", "ubuntu"},
                  {"version", "22.04"},
                  {"version_parts", {{"major", "22"}, {"minor", "04"}, {"build_number", ""}}},
                  {"like", "debian"},
                  {"codename", "jammy"}}}}}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.restart - Restart Moonraker service
    // https://moonraker.readthedocs.io/en/latest/web_api/#restart-server
    registry["server.restart"] =
        [](MoonrakerClientMock* /*self*/, const json& /*params*/,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        spdlog::info("[MoonrakerClientMock] server.restart (mock — no-op)");

        json response = {{"jsonrpc", "2.0"}, {"result", "ok"}};
        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.temperature_store - Cached per-sensor temperature history
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-cached-temperature-data
    // Mock returns a realistic heat/hold/cool curve so the connect-time seed
    // (#944) populates the graphs in mock mode for visual verification.
    registry["server.temperature_store"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        TemperatureStore store = self->build_historical_temperature_store();

        json result = json::object();
        for (const auto& [key, series] : store) {
            json entry = json::object();
            entry["temperatures"] = series.temperatures;
            if (!series.targets.empty()) {
                entry["targets"] = series.targets;
            }
            if (!series.powers.empty()) {
                entry["powers"] = series.powers;
            }
            result[key] = std::move(entry);
        }
        spdlog::debug("[MoonrakerClientMock] server.temperature_store (mock — {} keys)",
                      result.size());

        json response = {{"jsonrpc", "2.0"}, {"result", std::move(result)}};
        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    spdlog::debug("[MoonrakerClientMock] Registered {} server method handlers", 6);
}

} // namespace mock_internal
