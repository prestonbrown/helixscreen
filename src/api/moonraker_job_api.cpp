// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_job_api.h"

#include "gcode_edit_list.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "moonraker_api_internal.h"
#include "moonraker_gcode_guards.h"
#include "spdlog/spdlog.h"
#include "standing_print_options.h"
#include "storage_report.h"

using namespace moonraker_internal;

static_assert(helix::gcode::kPrintEditedStartMarginMs >= MoonrakerJobAPI::kPrintStartTimeoutMs,
              "print_edited must wait for the start as long as a direct start does");

// ============================================================================
// MoonrakerJobAPI Implementation
// ============================================================================

MoonrakerJobAPI::MoonrakerJobAPI(helix::IMoonrakerClient& client, const helix::PrinterState* state)
    : client_(client), state_(state) {}

bool MoonrakerJobAPI::refused_by_spool_latch(const char* method,
                                             const ErrorCallback& on_error) const {
    return helix::api::reject_job_while_spools_on_bed(state_, method, on_error);
}

// ============================================================================
// Job Control Operations
// ============================================================================

void MoonrakerJobAPI::start_print(const std::string& filename, SuccessCallback on_success,
                                  ErrorCallback on_error,
                                  const helix::PrintStartParams& extra_params) {
    // Validate filename path
    if (reject_invalid_path(filename, "start_print", on_error))
        return;
    if (refused_by_spool_latch("printer.print.start", on_error))
        return;

    json params = {{"filename", filename}};
    for (const auto& [name, value] : extra_params) {
        params[name] = value;
    }

    spdlog::debug("[Moonraker API] Starting print: {}", params.dump());

    // This start sends its own options; the job history adds next is ours.
    auto& ledger = helix::screen_start_ledger();
    ledger.note(filename);
    const bool announced = ledger.announce();
    if (announced) {
        send_screen_start(filename, /*withdraw=*/false);
    }

    client_.send_jsonrpc(
        "printer.print.start", params,
        [on_success](json) {
            spdlog::debug("[Moonraker API] Print started successfully");
            on_success();
        },
        [this, filename, announced, on_error](const MoonrakerError& err) {
            // A refused start takes its notes back, or the next start of the
            // same file elsewhere would read as this screen's. A lost answer
            // says nothing: the print may be running.
            if (!err.is_transport_loss()) {
                helix::screen_start_ledger().withdraw(filename);
                if (announced) {
                    send_screen_start(filename, /*withdraw=*/true);
                }
            }
            if (on_error) {
                on_error(err);
            }
        },
        kPrintStartTimeoutMs);
}

void MoonrakerJobAPI::send_screen_start(const std::string& filename, bool withdraw) {
    json params = {{"filename", filename}};
    if (withdraw) {
        params["withdraw"] = true;
    }
    client_.send_jsonrpc(
        helix::kScreenStartRpc, params, [](json) {},
        [withdraw](const MoonrakerError& err) {
            spdlog::debug("[Moonraker API] print_hooks screen_start{} not taken: {}",
                          withdraw ? " withdraw" : "", err.message);
        },
        0, /*silent=*/true);
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
    if (refused_by_spool_latch("printer.print.resume", on_error))
        return;
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
        on_error, kCancelTimeoutMs);
}

// ============================================================================
// HelixPrint Plugin Operations
// ============================================================================

void MoonrakerJobAPI::check_helix_plugin(PluginInfoCallback on_result, ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Checking for helix_print plugin");

    client_.send_jsonrpc(
        "server.helix.status", json::object(),
        [on_result](const json& response) {
            // Plugin is available
            helix::HelixPluginInfo info;
            info.present = true;
            info.available = true;
            if (response.contains("result")) {
                const auto& r = response["result"];
                info.available = helix::json_util::safe_bool(r, "enabled", true);
                // Empty when the plugin does not say: an unknown version is no
                // mismatch to restart Moonraker for.
                info.version = helix::json_util::safe_string(r, "version", "");
                info.symlink_dir = helix::json_util::safe_string(r, "symlink_dir", "");
                info.features = helix::parse_plugin_features(r);
            }
            spdlog::info("[Moonraker API] helix_print plugin '{}' detected (enabled={}, links in "
                         "'{}', {} features)",
                         info.version, info.available, info.symlink_dir, info.features.size());
            on_result(info);
        },
        [on_result, on_error](const MoonrakerError& err) {
            // Plugin not available (404 or method not found)
            spdlog::debug("[Moonraker API] helix_print plugin not available: {}", err.message);
            // Don't treat this as an error - just means plugin isn't installed
            on_result(helix::HelixPluginInfo{});
        },
        0,     // timeout_ms: use default
        true); // silent: suppress RPC_ERROR events/toasts
}

namespace {

/// The plugin's answer to print_modified or print_edited.
ModifiedPrintResult linked_print_result(const json& response,
                                        const std::string& original_filename) {
    ModifiedPrintResult result;
    if (response.contains("result")) {
        const auto& r = response["result"];
        result.original_filename =
            helix::json_util::safe_string(r, "original_filename", original_filename);
        result.print_filename = helix::json_util::safe_string(r, "print_filename", "");
        result.temp_filename = helix::json_util::safe_string(r, "temp_filename", "");
        result.status = helix::json_util::safe_string(r, "status", "unknown");
    } else {
        result.original_filename = original_filename;
        result.status = "printing";
    }
    return result;
}

} // namespace

void MoonrakerJobAPI::start_modified_print(const std::string& original_filename,
                                           const std::string& temp_file_path,
                                           const std::vector<std::string>& modifications,
                                           ModifiedPrintCallback on_success, ErrorCallback on_error,
                                           const helix::PrintStartParams& extra_params) {
    // Validate filename paths
    if (reject_invalid_path(original_filename, "start_modified_print", on_error))
        return;
    if (reject_invalid_path(temp_file_path, "start_modified_print", on_error))
        return;
    if (refused_by_spool_latch("server.helix.print_modified", on_error))
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
    for (const auto& [name, value] : extra_params) {
        params[name] = value;
    }

    spdlog::info("[Moonraker API] Starting modified print via helix_print plugin: {} (temp: {})",
                 original_filename, temp_file_path);
    // The plugin starts it through a link and marks it as its own start.
    helix::screen_start_ledger().note_any();

    client_.send_jsonrpc(
        "server.helix.print_modified", params,
        [on_success, original_filename](const json& response) {
            const ModifiedPrintResult result = linked_print_result(response, original_filename);
            spdlog::info("[Moonraker API] Modified print started: {} -> {}",
                         result.original_filename, result.print_filename);
            on_success(result);
        },
        // Resilience fallback: the helix_print plugin's print_modified endpoint is
        // broken in shipped plugin v1.0.0 (fails for every field), so an already-
        // deployed plugin leaves the user unable to start ANY print until they
        // update it. When the plugin answers and refuses, fall back to the stock
        // printer.print.start on the already-uploaded temp file so the user can
        // still print. `this` is safe to capture: MoonrakerJobAPI is a long-lived
        // API wrapper owned by MoonrakerAPI (same lifetime as the client), matching
        // how ui_resume_dispatch nests API calls through the long-lived api pointer.
        // The callback runs on the WebSocket background thread and only touches
        // spdlog + client_ (via start_print) — no LVGL/subject work here; the
        // caller's on_success/on_error handle their own thread bouncing.
        [this, original_filename, temp_file_path, on_success, on_error,
         extra_params](const MoonrakerError& err) {
            // Only an answer from the plugin justifies a second start. The endpoint
            // starts the print before it replies, so an error carrying no reply
            // leaves the print's fate unknown and it may already be running. Firing
            // printer.print.start at a live machine is worse than reporting the
            // failure: the caller's error path deletes the temp file that print is
            // reading through the plugin's symlink.
            if (err.type == MoonrakerErrorType::TIMEOUT ||
                err.type == MoonrakerErrorType::CONNECTION_LOST) {
                spdlog::warn("[Moonraker API] helix_print plugin print_modified gave no answer "
                             "({}); NOT falling back to printer.print.start - the plugin starts "
                             "the print before it replies, so '{}' may already be printing.",
                             err.message, temp_file_path);
                on_error(err);
                return;
            }
            // Out of space is the plugin's answer, not its failure: the copy is
            // deleted and no start may replace it.
            if (helix::storage::is_no_space_refusal(err.code)) {
                spdlog::warn("[Moonraker API] helix_print refused '{}' for space: {}",
                             temp_file_path, err.message);
                helix::screen_start_ledger().withdraw_any();
                on_error(err);
                return;
            }
            // A 400 refuses the request itself (a path or tool map the plugin
            // rejected): printer.print.start with the same values is no better.
            if (err.code == 400) {
                spdlog::warn("[Moonraker API] helix_print refused '{}' as invalid: {}",
                             temp_file_path, err.message);
                helix::screen_start_ledger().withdraw_any();
                on_error(err);
                return;
            }

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
                on_error, extra_params);
        },
        kPrintModifiedTimeoutMs);
}

void MoonrakerJobAPI::start_edited_print(const std::string& original_filename,
                                         const std::string& temp_file_path, const json& edits,
                                         const std::vector<std::string>& modifications,
                                         uint64_t file_size, ModifiedPrintCallback on_success,
                                         ErrorCallback on_error,
                                         const helix::PrintStartParams& extra_params) {
    if (reject_invalid_path(original_filename, "start_edited_print", on_error))
        return;
    if (reject_invalid_path(temp_file_path, "start_edited_print", on_error))
        return;
    if (refused_by_spool_latch("server.helix.print_edited", on_error))
        return;

    json params = {{"original_filename", original_filename},
                   {"temp_file_path", temp_file_path},
                   {"edits", edits},
                   {"modifications", modifications}};
    for (const auto& [name, value] : extra_params) {
        params[name] = value;
    }

    spdlog::info("[Moonraker API] Starting edited print via helix_print plugin: {} ({} edits, "
                 "copy: {})",
                 original_filename, edits.size(), temp_file_path);
    // The plugin starts it through a link and marks it as its own start, once
    // its rewrite is done: the note waits as long as this call may.
    const uint32_t timeout_ms = helix::gcode::print_edited_timeout_ms(file_size);
    helix::screen_start_ledger().note_any(helix::ScreenStartLedger::Clock::now(),
                                          std::chrono::milliseconds(timeout_ms));

    client_.send_jsonrpc(
        "server.helix.print_edited", params,
        [on_success, original_filename](const json& response) {
            const ModifiedPrintResult result = linked_print_result(response, original_filename);
            spdlog::info("[Moonraker API] Edited print started: {} -> {}", result.original_filename,
                         result.print_filename);
            if (on_success) {
                on_success(result);
            }
        },
        // No fallback here: after a timeout or a lost connection the plugin may still
        // start the print (gcode::classify_print_edited_failure).
        [on_error](const MoonrakerError& err) {
            spdlog::error("[Moonraker API] helix_print plugin print_edited failed: {} ({})",
                          err.message, err.get_type_string());
            // Nothing will start for a definite answer, so its note goes.
            if (helix::gcode::classify_print_edited_failure(err) !=
                helix::gcode::EditedPrintFailure::Unanswered) {
                helix::screen_start_ledger().withdraw_any();
            }
            if (on_error) {
                on_error(err);
            }
        },
        timeout_ms);
}
