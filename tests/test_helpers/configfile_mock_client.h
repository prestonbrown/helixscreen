// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_shared_callback.h"
#include "json_fwd.h"
#include "moonraker_client_mock.h"

#include <functional>
#include <optional>
#include <string>

/// The stock mock answers printer.objects.query configfile from a hard-coded
/// config; this one answers that query from test-controlled sections and
/// delegates everything else (gcode recording, discovery) to the stock mock.
class ConfigfileMockClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    /// Sections returned for a configfile query. Empty object = silent
    /// configfile (no max_temp anywhere).
    nlohmann::json config_sections = nlohmann::json::object();
    /// Answer the configfile query with an error instead.
    bool fail_configfile = false;

    helix::RequestId send_jsonrpc(
        const std::string& method, const nlohmann::json& params,
        helix::SharedCallback<void(const nlohmann::json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "printer.objects.query" && params.contains("objects") &&
            params["objects"].contains("configfile")) {
            if (fail_configfile) {
                if (error_cb) {
                    MoonrakerError err;
                    err.type = MoonrakerErrorType::JSON_RPC_ERROR;
                    err.message = "configfile unavailable";
                    error_cb(err);
                }
                return 0;
            }
            if (success_cb) {
                success_cb(
                    {{"result", {{"status", {{"configfile", {{"config", config_sections}}}}}}}});
            }
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }
};
