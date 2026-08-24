// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_job_api.h"

#include "moonraker_api_internal.h"
#include "moonraker_client.h"
#include "spdlog/spdlog.h"

using namespace moonraker_internal;

// ============================================================================
// MoonrakerJobAPI Implementation
// ============================================================================

MoonrakerJobAPI::MoonrakerJobAPI(helix::IMoonrakerClient& client) : client_(client) {}

// ============================================================================
// Job Control Operations
// ============================================================================

void MoonrakerJobAPI::start_print(const std::string& filename, SuccessCallback on_success,
                                  ErrorCallback on_error) {
    // Validate filename path
    if (reject_invalid_path(filename, "start_print", on_error))
        return;

    json params = {{"filename", filename}};

    spdlog::debug("[Moonraker API] Starting print: {}", filename);

    client_.send_jsonrpc(
        "printer.print.start", params,
        [on_success](json) {
            spdlog::debug("[Moonraker API] Print started successfully");
            on_success();
        },
        on_error);
}

void MoonrakerJobAPI::pause_print(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Pausing print");

    client_.send_jsonrpc(
        "printer.print.pause", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Print paused successfully");
            on_success();
        },
        on_error);
}

void MoonrakerJobAPI::resume_print(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Resuming print");

    client_.send_jsonrpc(
        "printer.print.resume", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Print resumed successfully");
            on_success();
        },
        on_error);
}

void MoonrakerJobAPI::cancel_print(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Canceling print");

    client_.send_jsonrpc(
        "printer.print.cancel", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Print canceled successfully");
            on_success();
        },
        on_error, CANCEL_TIMEOUT_MS);
}

// ============================================================================
// HelixPrint Plugin Operations
// ============================================================================

void MoonrakerJobAPI::check_helix_plugin(BoolCallback on_result, ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Checking for helix_print plugin");

    client_.send_jsonrpc(
        "server.helix.status", json::object(),
        [on_result](json response) {
            // Plugin is available
            bool enabled = true;
            std::string version = "unknown";
            if (response.contains("result")) {
                const auto& r = response["result"];
                enabled = r.value("enabled", true);
                version = r.value("version", "1.0.0"); // Old plugins lack version
            }
            spdlog::info("[Moonraker API] helix_print plugin v{} detected (enabled={})", version,
                         enabled);
            on_result(enabled);
        },
        [on_result, on_error](const MoonrakerError& err) {
            // Plugin not available (404 or method not found)
            spdlog::debug("[Moonraker API] helix_print plugin not available: {}", err.message);
            // Don't treat this as an error - just means plugin isn't installed
            on_result(false);
        },
        0,     // timeout_ms: use default
        true); // silent: suppress RPC_ERROR events/toasts
}

void MoonrakerJobAPI::start_modified_print(const std::string& original_filename,
                                           const std::string& temp_file_path,
                                           const std::vector<std::string>& modifications,
                                           ModifiedPrintCallback on_success,
                                           ErrorCallback on_error) {
    // Validate filename paths
    if (reject_invalid_path(original_filename, "start_modified_print", on_error))
        return;
    if (reject_invalid_path(temp_file_path, "start_modified_print", on_error))
        return;

    // Build modifications array
    json mods_array = json::array();
    for (const auto& mod : modifications) {
        mods_array.push_back(mod);
    }

    // v2.0 API: Send path to already-uploaded file, not content
    json params = {{"original_filename", original_filename},
                   {"temp_file_path", temp_file_path},
                   {"modifications", mods_array},
                   {"copy_metadata", true}};

    spdlog::info("[Moonraker API] Starting modified print via helix_print plugin: {} (temp: {})",
                 original_filename, temp_file_path);

    client_.send_jsonrpc(
        "server.helix.print_modified", params,
        [on_success, original_filename](json response) {
            ModifiedPrintResult result;
            if (response.contains("result")) {
                const auto& r = response["result"];
                result.original_filename = r.value("original_filename", original_filename);
                result.print_filename = r.value("print_filename", "");
                result.temp_filename = r.value("temp_filename", "");
                result.status = r.value("status", "unknown");
            } else {
                result.original_filename = original_filename;
                result.status = "printing";
            }
            spdlog::info("[Moonraker API] Modified print started: {} -> {}",
                         result.original_filename, result.print_filename);
            on_success(result);
        },
        // Resilience fallback: the helix_print plugin's print_modified endpoint is
        // broken in shipped plugin v1.0.0 (fails for every field), so an already-
        // deployed plugin leaves the user unable to start ANY print until they
        // update it. When the plugin call fails, fall back to the stock
        // printer.print.start on the already-uploaded temp file so the user can
        // still print. `this` is safe to capture: MoonrakerJobAPI is a long-lived
        // API wrapper owned by MoonrakerAPI (same lifetime as the client), matching
        // how ui_resume_dispatch nests API calls through the long-lived api pointer.
        // The callback runs on the WebSocket background thread and only touches
        // spdlog + client_ (via start_print) — no LVGL/subject work here; the
        // caller's on_success/on_error handle their own thread bouncing.
        [this, original_filename, temp_file_path, on_success, on_error](const MoonrakerError& err) {
            spdlog::warn("[Moonraker API] helix_print plugin print_modified failed ({}); falling "
                         "back to direct printer.print.start on temp file '{}'. Job history will "
                         "show the temp filename (known degraded mode — update the helix_print "
                         "plugin to restore original-filename history).",
                         err.message, temp_file_path);

            // temp_file_path is already gcodes-root-relative (e.g.
            // ".helix_temp/modified_<ts>_<name>.gcode"), which is exactly the form
            // printer.print.start expects as "filename" — the non-plugin path in
            // ui_print_preparation_manager passes this same value straight to
            // start_print(). No stripping required.
            start_print(
                temp_file_path,
                [on_success, original_filename, temp_file_path]() {
                    ModifiedPrintResult result;
                    result.original_filename = original_filename;
                    result.print_filename = temp_file_path;
                    result.temp_filename = temp_file_path;
                    result.status = "printing";
                    spdlog::info("[Moonraker API] Fallback direct print started: {} (temp: {})",
                                 original_filename, temp_file_path);
                    on_success(result);
                },
                // Fallback ALSO failed — only now surface the error to the caller.
                on_error);
        });
}
