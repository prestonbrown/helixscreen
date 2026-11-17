// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_api.h"
#include "moonraker_api_internal.h"

#include <spdlog/spdlog.h>

using namespace moonraker_internal;

// ============================================================================
// Configuration Query Operations
// ============================================================================

void MoonrakerAPI::query_configfile(JsonCallback on_success, ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Querying configfile object");

    // Query the configfile object, requesting the "config" key which contains
    // raw string values (as opposed to "settings" which has parsed types).
    // This is needed because macro gcode is stored as raw strings.
    json params = {{"objects", json::object({{"configfile", json::array({"config"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success, on_error](const json& response) {
            // Navigate to result.status.configfile.config
            const json* configfile =
                helix::json_util::find_path(response, "result", "status", "configfile");
            if (!configfile) {
                spdlog::warn("[Moonraker API] Configfile not available in response");
                if (on_error) {
                    MoonrakerError err;
                    err.type = MoonrakerErrorType::PARSE_ERROR;
                    err.message = "Configfile not available in printer response";
                    on_error(err);
                }
                return;
            }

            const json* config = helix::json_util::find_member(*configfile, "config");
            if (!config) {
                spdlog::warn("[Moonraker API] Config section not available in configfile");
                if (on_error) {
                    MoonrakerError err;
                    err.type = MoonrakerErrorType::PARSE_ERROR;
                    err.message = "Config section not available";
                    on_error(err);
                }
                return;
            }

            spdlog::debug("[Moonraker API] Configfile query successful, {} sections",
                          config->size());

            if (on_success) {
                on_success(*config);
            }
        },
        on_error);
}
