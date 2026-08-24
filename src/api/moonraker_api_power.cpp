// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Split out of moonraker_api_controls.cpp: these are the only two MoonrakerAPI
// control methods that talk HTTP (Moonraker's device_power endpoints have no
// JSON-RPC equivalent) and therefore the only ones needing hv/requests.h.
// Isolating them keeps moonraker_api_controls.cpp libhv-free so it compiles for
// the ESP32 firmware, where the rest of the controls (E-STOP, temperature, fan,
// restart) are safety-critical. Same motivation as the validation split noted in
// moonraker_api_controls.cpp.

#include "http_executor.h"
#include "hv/requests.h"
#include "json_utils.h"
#include "moonraker_api.h"
#include "moonraker_api_internal.h"
#include "spdlog/spdlog.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

using namespace moonraker_internal;

// ============================================================================
// Power Device Control Operations
// ============================================================================

void MoonrakerAPI::get_power_devices(PowerDevicesCallback on_success, ErrorCallback on_error) {
    if (http_base_url_.empty()) {
        spdlog::error("[Moonraker API] HTTP base URL not configured for power devices");
        if (on_error) {
            MoonrakerError err =
                MoonrakerError::connection_lost("get_power_devices", "Not connected to Moonraker");
            on_error(err);
        }
        return;
    }

    std::string url = http_base_url_ + "/machine/device_power/devices";
    spdlog::debug("[Moonraker API] Fetching power devices from: {}", url);

    helix::http::HttpExecutor::fast().submit([url, on_success, on_error]() {
        auto resp = requests::get(url.c_str());

        if (!resp) {
            spdlog::error("[Moonraker API] HTTP request failed for power devices");
            if (on_error) {
                MoonrakerError err =
                    MoonrakerError::connection_lost("get_power_devices", "HTTP request failed");
                on_error(err);
            }
            return;
        }

        if (resp->status_code != 200) {
            spdlog::error("[Moonraker API] Power devices request failed: HTTP {}",
                          static_cast<int>(resp->status_code));
            if (on_error) {
                MoonrakerError err = MoonrakerError::http_status_error(
                    "get_power_devices", static_cast<int>(resp->status_code));
                on_error(err);
            }
            return;
        }

        // Parse JSON response. Only the parse belongs in the try: with
        // on_success() inside it, a throw from the caller's own handler would be
        // reported back through on_error as if Moonraker had sent us garbage,
        // firing on_error after on_success already ran.
        std::vector<PowerDevice> devices;
        try {
            json j = json::parse(resp->body);

            if (j.contains("result") && j["result"].contains("devices") &&
                j["result"]["devices"].is_array()) {
                for (const auto& info : j["result"]["devices"]) {
                    if (!info.is_object()) {
                        continue;
                    }
                    // json_util::safe_* rather than .value(): nlohmann's .value()
                    // throws type_error.302 on a JSON null, and one null field
                    // would drop the whole device list.
                    PowerDevice dev;
                    dev.device = helix::json_util::safe_string(info, "device");
                    dev.type = helix::json_util::safe_string(info, "type", "unknown");
                    dev.status = helix::json_util::safe_string(info, "status", "off");
                    dev.locked_while_printing =
                        helix::json_util::safe_bool(info, "locked_while_printing", false);
                    if (!dev.device.empty()) {
                        devices.push_back(dev);
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker API] Failed to parse power devices: {}", e.what());
            if (on_error) {
                MoonrakerError err = MoonrakerError::unknown(e.what(), "get_power_devices");
                on_error(err);
            }
            return;
        }

        spdlog::info("[Moonraker API] Found {} power devices", devices.size());
        if (on_success) {
            on_success(devices);
        }
    });
}

void MoonrakerAPI::set_device_power(const std::string& device, const std::string& action,
                                    SuccessCallback on_success, ErrorCallback on_error) {
    // Validate device name. Power devices are named by their Moonraker config section
    // (`[power -Power-]`, `[power Printer PSU]`), so the identifier allowlist is far too
    // narrow here — it rejected every hyphenated name (prestonbrown/helixscreen#1241).
    // The name is percent-encoded into the query string below and never reaches G-code,
    // so only control characters need rejecting.
    if (!is_safe_url_param(device)) {
        // Escape before logging: the rejected class is exactly the bytes that would
        // otherwise forge extra log lines.
        std::string escaped;
        for (char c : device) {
            auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F) {
                char buf[5];
                std::snprintf(buf, sizeof(buf), "\\x%02X", byte);
                escaped += buf;
            } else {
                escaped += c;
            }
        }
        // No NOTIFY_ERROR here: every caller already surfaces the failure through
        // on_error (a toast in PowerDeviceWidget, a status line in PowerPanel), and
        // toasting from the API layer too would double-report the same rejection.
        spdlog::warn("[Moonraker API] Rejected power device name '{}' — contains control "
                     "characters",
                     escaped);
        if (on_error) {
            MoonrakerError err =
                MoonrakerError::validation_error("set_device_power", "Invalid device name");
            on_error(err);
        }
        return;
    }

    // Validate action
    if (action != "on" && action != "off" && action != "toggle") {
        spdlog::error("[Moonraker API] Invalid power action: {}", action);
        if (on_error) {
            MoonrakerError err = MoonrakerError::validation_error(
                "set_device_power", "Invalid action (must be on, off, or toggle)");
            on_error(err);
        }
        return;
    }

    if (http_base_url_.empty()) {
        spdlog::error("[Moonraker API] HTTP base URL not configured for power device control");
        if (on_error) {
            MoonrakerError err =
                MoonrakerError::connection_lost("set_device_power", "Not connected to Moonraker");
            on_error(err);
        }
        return;
    }

    // URL-encode device name (spaces, special chars) for safe query param
    std::string encoded_device;
    for (char c : device) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
            encoded_device += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            encoded_device += buf;
        }
    }

    // Build URL with query params
    std::string url = http_base_url_ + "/machine/device_power/device?device=" + encoded_device +
                      "&action=" + action;

    spdlog::info("[Moonraker API] Setting power device '{}' to '{}'", device, action);

    helix::http::HttpExecutor::fast().submit([url, device, action, on_success, on_error]() {
        auto resp = requests::post(url.c_str(), "");

        if (!resp) {
            spdlog::error("[Moonraker API] HTTP request failed for power device");
            if (on_error) {
                MoonrakerError err =
                    MoonrakerError::connection_lost("set_device_power", "HTTP request failed");
                on_error(err);
            }
            return;
        }

        if (resp->status_code != 200) {
            spdlog::error("[Moonraker API] Power device command failed: HTTP {}",
                          static_cast<int>(resp->status_code));
            if (on_error) {
                MoonrakerError err = MoonrakerError::http_status_error(
                    "set_device_power", static_cast<int>(resp->status_code));
                on_error(err);
            }
            return;
        }

        spdlog::info("[Moonraker API] Power device '{}' set to '{}' successfully", device, action);
        if (on_success) {
            on_success();
        }
    });
}
