// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_queue_api.h
 * @brief Job queue operations via Moonraker
 *
 * Extracted from MoonrakerAPI to encapsulate all job queue functionality
 * in a dedicated class. Uses MoonrakerClient for JSON-RPC transport.
 */

#pragma once

#include "i_moonraker_sub_apis.h" // for JobQueueEntry, JobQueueStatus, IQueueAPI
#include "moonraker_error.h"

#include <functional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class IMoonrakerClient;
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
class MoonrakerQueueAPI : public IQueueAPI {
  public:
    using StatusCallback = std::function<void(const JobQueueStatus&)>;
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     */
    explicit MoonrakerQueueAPI(helix::IMoonrakerClient& client);
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
     * @param filename G-code filename to enqueue
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void add_job(const std::string& filename, SuccessCallback on_success,
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
};
