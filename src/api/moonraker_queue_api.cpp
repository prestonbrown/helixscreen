// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_queue_api.h"

#include "moonraker_client.h"
#include "spdlog/spdlog.h"

// ============================================================================
// MoonrakerQueueAPI Implementation
// ============================================================================

MoonrakerQueueAPI::MoonrakerQueueAPI(helix::IMoonrakerClient& client) : client_(client) {}

// ============================================================================
// Queue Operations
// ============================================================================

void MoonrakerQueueAPI::get_queue_status(StatusCallback on_success, ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Querying job queue status");

    client_.send_jsonrpc(
        "server.job_queue.status", json::object(),
        [on_success](json response) {
            JobQueueStatus status;
            auto result = response.value("result", json::object());
            status.queue_state = result.value("queue_state", "ready");
            for (const auto& job : result.value("queued_jobs", json::array())) {
                JobQueueEntry entry;
                entry.job_id = job.value("job_id", "");
                entry.filename = job.value("filename", "");
                entry.time_added = job.value("time_added", 0.0);
                entry.time_in_queue = job.value("time_in_queue", 0.0);
                status.queued_jobs.push_back(std::move(entry));
            }
            spdlog::debug("[Moonraker API] Job queue: state={}, {} jobs", status.queue_state,
                          status.queued_jobs.size());
            on_success(status);
        },
        on_error);
}

void MoonrakerQueueAPI::start_queue(SuccessCallback on_success, ErrorCallback on_error) {
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

void MoonrakerQueueAPI::add_job(const std::string& filename, SuccessCallback on_success,
                                ErrorCallback on_error) {
    json params;
    params["filenames"] = json::array({filename});

    spdlog::info("[Moonraker API] Adding job to queue: {}", filename);

    client_.send_jsonrpc(
        "server.job_queue.post_job", params,
        [on_success, filename](json) {
            spdlog::info("[Moonraker API] Job added to queue: {}", filename);
            on_success();
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
