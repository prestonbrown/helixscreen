// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_queue_api.h"

#include "i_moonraker_client.h"
#include "json_utils.h"
#include "moonraker_gcode_guards.h"
#include "spdlog/spdlog.h"

// MoonrakerQueueAPI Implementation

namespace {

/// Shared parser for every response that carries the queue (status and
/// post_job both return `queue_state` + `queued_jobs` in result).
helix::JobQueueStatus parse_queue_status(const json& response) {
    helix::JobQueueStatus status;
    const json* result = helix::json_util::find_member(response, "result");
    if (!result) {
        return status;
    }
    status.queue_state = helix::json_util::safe_string(*result, "queue_state", "ready");
    const json* queued_jobs = helix::json_util::find_member(*result, "queued_jobs");
    if (queued_jobs && queued_jobs->is_array()) {
        for (const auto& job : *queued_jobs) {
            helix::JobQueueEntry entry;
            entry.job_id = helix::json_util::safe_string(job, "job_id", "");
            entry.filename = helix::json_util::safe_string(job, "filename", "");
            entry.time_added = helix::json_util::safe_double(job, "time_added", 0.0);
            entry.time_in_queue = helix::json_util::safe_double(job, "time_in_queue", 0.0);
            status.queued_jobs.push_back(std::move(entry));
        }
    }
    return status;
}

} // namespace

MoonrakerQueueAPI::MoonrakerQueueAPI(helix::IMoonrakerClient& client,
                                     const helix::PrinterState* state)
    : client_(client), state_(state) {}

// Queue Operations

void MoonrakerQueueAPI::get_queue_status(StatusCallback on_success, ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Querying job queue status");

    client_.send_jsonrpc(
        "server.job_queue.status", json::object(),
        [on_success](const json& response) {
            helix::JobQueueStatus status = parse_queue_status(response);
            spdlog::debug("[Moonraker API] Job queue: state={}, {} jobs", status.queue_state,
                          status.queued_jobs.size());
            on_success(status);
        },
        on_error);
}

void MoonrakerQueueAPI::start_queue(SuccessCallback on_success, ErrorCallback on_error) {
    if (helix::api::reject_job_while_spools_on_bed(state_, "server.job_queue.start", on_error)) {
        return;
    }
    spdlog::info("[Moonraker API] Starting job queue");

    client_.send_jsonrpc(
        "server.job_queue.start", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Job queue started");
            on_success();
        },
        on_error);
}

void MoonrakerQueueAPI::pause_queue(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Pausing job queue");

    client_.send_jsonrpc(
        "server.job_queue.pause", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Job queue paused");
            on_success();
        },
        on_error);
}

void MoonrakerQueueAPI::add_job(const std::string& filename, StatusCallback on_success,
                                ErrorCallback on_error) {
    json params;
    params["filenames"] = json::array({filename});

    spdlog::info("[Moonraker API] Adding job to queue: {}", filename);

    client_.send_jsonrpc(
        "server.job_queue.post_job", params,
        [on_success, filename](const json& response) {
            helix::JobQueueStatus status = parse_queue_status(response);
            spdlog::info("[Moonraker API] Job added to queue: {} ({} jobs now)", filename,
                         status.queued_jobs.size());
            on_success(status);
        },
        on_error);
}

void MoonrakerQueueAPI::remove_jobs(const std::vector<std::string>& job_ids,
                                    SuccessCallback on_success, ErrorCallback on_error) {
    json params;
    params["job_ids"] = job_ids;

    spdlog::info("[Moonraker API] Removing {} jobs from queue", job_ids.size());

    client_.send_jsonrpc(
        "server.job_queue.delete_job", params,
        [on_success](json) {
            spdlog::info("[Moonraker API] Jobs removed from queue");
            on_success();
        },
        on_error);
}
