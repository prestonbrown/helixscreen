// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_queue_api.h
 * @brief Job queue operations via Moonraker
 *
 * A MoonrakerAPI sub-API that encapsulates all job queue functionality
 * in a dedicated class. Uses MoonrakerClient for JSON-RPC transport.
 */

#pragma once

#include "helix_shared_callback.h"
#include "i_moonraker_sub_apis.h" // for JobQueueEntry, JobQueueStatus, IQueueAPI
#include "moonraker_error.h"

#include <functional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class IMoonrakerClient;
class PrinterState;
} // namespace helix

/**
 * @brief Job Queue API operations via Moonraker
 *
 * Provides high-level operations for managing the Moonraker job queue:
 * querying status, starting/pausing the queue, and adding/removing jobs.
 *
 * All methods are asynchronous with callbacks.
 *
 * Usage:
 *   MoonrakerQueueAPI queue(client);
 *   queue.get_queue_status(
 *       [](const JobQueueStatus& status) { ... },
 *       [](const auto& err) { ... });
 */
class MoonrakerQueueAPI : public helix::IQueueAPI {
  public:
    using StatusCallback = std::function<void(const helix::JobQueueStatus&)>;
    using SuccessCallback = helix::SharedCallback<void()>;
    using ErrorCallback = helix::SharedCallback<void(const MoonrakerError&)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     */
    explicit MoonrakerQueueAPI(helix::IMoonrakerClient& client,
                               const helix::PrinterState* state = nullptr);
    virtual ~MoonrakerQueueAPI() = default;

    /**
     * @brief Get current job queue status and contents
     *
     * @param on_success Callback with queue status
     * @param on_error Error callback
     */
    void get_queue_status(StatusCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Start processing the job queue
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void start_queue(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Pause the job queue
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void pause_queue(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Add a job to the queue
     *
     * Success carries the resulting queue (Moonraker returns `queued_jobs`
     * and `queue_state` from post_job), so the caller can find the new job's
     * id: it is the one absent from the queue before the call.
     *
     * @param filename G-code filename to enqueue
     * @param on_success Callback with the resulting queue status
     * @param on_error Error callback
     */
    void add_job(const std::string& filename, StatusCallback on_success,
                 ErrorCallback on_error) override;

    /**
     * @brief Remove jobs from the queue by ID
     *
     * @param job_ids List of job IDs to remove
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void remove_jobs(const std::vector<std::string>& job_ids, SuccessCallback on_success,
                     ErrorCallback on_error) override;

  protected:
    helix::IMoonrakerClient& client_;
    const helix::PrinterState* state_ = nullptr;
};
