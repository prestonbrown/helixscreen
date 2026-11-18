// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/error_reporting.h"
#include "json_utils.h"
#include "moonraker_api.h"
#include "moonraker_api_internal.h"
#include "spdlog/spdlog.h"

using namespace moonraker_internal;

// ============================================================================
// Query Operations
// ============================================================================

helix::KlippyStatus helix::parse_klippy_status(const json& info) {
    return {helix::json_util::safe_string(info, "state", ""),
            helix::json_util::safe_string(info, "state_message", "")};
}

void MoonrakerAPI::get_klippy_status(KlippyStatusCallback on_result, ErrorCallback on_error) {
    client_.send_jsonrpc(
        "printer.info", json::object(),
        [on_result](const json& response) {
            const json* result = helix::json_util::find_member(response, "result");
            on_result(result ? helix::parse_klippy_status(*result) : helix::KlippyStatus{});
        },
        on_error);
}

void MoonrakerAPI::get_print_state(StringCallback on_result, ErrorCallback on_error) {
    json params = {{"objects", json::object({{"print_stats", nullptr}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_result](const json& response) {
            std::string state = "unknown";
            if (const json* stats =
                    helix::json_util::find_path(response, "result", "status", "print_stats")) {
                state = helix::json_util::read_field<std::string>(*stats, "state").value_or(state);
            }
            on_result(state);
        },
        on_error);
}
