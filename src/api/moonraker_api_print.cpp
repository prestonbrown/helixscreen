// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_error_reporting.h"
#include "ui_notification.h"

#include "moonraker_api.h"
#include "moonraker_api_internal.h"
#include "plr_backend.h"
#include "spdlog/spdlog.h"

using namespace moonraker_internal;

// ============================================================================
// Motion Control Operations
// ============================================================================

// ============================================================================
// Query Operations
// ============================================================================

void MoonrakerAPI::is_printer_ready(BoolCallback on_result, ErrorCallback on_error) {
    client_.send_jsonrpc(
        "printer.info", json::object(),
        [on_result](json response) {
            bool ready = false;
            if (response.contains("result") && response["result"].contains("state")) {
                std::string state = response["result"]["state"].get<std::string>();
                ready = (state == "ready");
            }
            on_result(ready);
        },
        on_error);
}

void MoonrakerAPI::get_print_state(StringCallback on_result, ErrorCallback on_error) {
    json params = {{"objects", json::object({{"print_stats", nullptr}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_result](json response) {
            std::string state = "unknown";
            if (response.contains("result") && response["result"].contains("status") &&
                response["result"]["status"].contains("print_stats") &&
                response["result"]["status"]["print_stats"].contains("state")) {
                state = response["result"]["status"]["print_stats"]["state"].get<std::string>();
            }
            on_result(state);
        },
        on_error);
}

// ============================================================================
// Power-Loss Recovery — Creality Klipper fork
// ============================================================================

void MoonrakerAPI::check_continue_print_state(
    std::function<void(const helix::PlrDetectResult&)> on_result, ErrorCallback on_error) {
    // Deliberately logged at info: this is a SIDE-EFFECTFUL, at-most-once-per-
    // connection call, so a second line in a session's log is itself a bug
    // report. See the header warning and docs/devel/POWER_LOSS_RECOVERY.md.
    spdlog::info("[Moonraker API] Creality PLR probe -> {}", helix::CREALITY_DETECT_RPC);

    client_.send_jsonrpc(
        helix::CREALITY_DETECT_RPC, json::object(),
        [on_result](json response) {
            helix::PlrDetectResult result;
            if (!helix::plr_parse_check_continue_response(response, result)) {
                // result.completed stays false, which forbids resume downstream.
                spdlog::warn("[Moonraker API] Unusable check_continue_print_state response: {}",
                             response.dump());
            } else {
                spdlog::info("[Moonraker API] Creality PLR probe: file_state={} eeprom_state={}",
                             result.file_state, result.eeprom_state);
            }
            if (on_result) {
                on_result(result);
            }
        },
        on_error);
}

void MoonrakerAPI::cancel_continue_print(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Discarding Creality PLR snapshot -> {}",
                 helix::CREALITY_DISCARD_RPC);
    client_.send_jsonrpc(
        helix::CREALITY_DISCARD_RPC, json::object(),
        [on_success](const json&) {
            if (on_success) {
                on_success();
            }
        },
        on_error);
}
