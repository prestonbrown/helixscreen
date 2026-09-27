// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_history_api.h
 * @brief Print history operations via Moonraker
 *
 * Extracted from MoonrakerAPI to encapsulate all print history functionality
 * in a dedicated class. Uses MoonrakerClient for JSON-RPC transport.
 */

#pragma once

#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "print_history_data.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class IMoonrakerClient;
} // namespace helix

/**
 * @brief Print History API operations via Moonraker
 *
 * Provides high-level operations for querying and managing print history
 * through Moonraker's server.history.* endpoints. All methods are asynchronous
 * with callbacks.
 *
 * Usage:
 *   MoonrakerHistoryAPI history(client);
 *   history.get_history_list(50, 0, 0.0, 0.0,
 *       [](const auto& jobs, uint64_t total) { ... },
 *       [](const auto& err) { ... });
 */
class MoonrakerHistoryAPI : public IHistoryAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using HistoryListCallback =
        std::function<void(const std::vector<PrintHistoryJob>&, uint64_t total_count)>;
    using HistoryTotalsCallback = std::function<void(const PrintHistoryTotals&)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     */
    explicit MoonrakerHistoryAPI(helix::IMoonrakerClient& client);
    virtual ~MoonrakerHistoryAPI() = default;

    // ========================================================================
    // Print History Operations
    // ========================================================================

    /**
     * @brief Get paginated list of print history jobs
     *
     * Calls server.history.list Moonraker endpoint.
     *
     * @param limit Maximum number of jobs to return (default 50)
     * @param start Offset for pagination (0-based)
     * @param since Unix timestamp - only include jobs after this time (0 = no filter)
     * @param before Unix timestamp - only include jobs before this time (0 = no filter)
     * @param on_success Callback with parsed job list and total count
     * @param on_error Error callback
     */
    void get_history_list(int limit, int start, double since, double before,
                          HistoryListCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Get aggregated history totals/statistics
     *
     * Calls server.history.totals Moonraker endpoint.
     *
     * @param on_success Callback with totals struct
     * @param on_error Error callback
     */
    void get_history_totals(HistoryTotalsCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Delete a job from history by its unique ID
     *
     * Calls server.history.delete_job Moonraker endpoint.
     *
     * @param job_id Unique job identifier from PrintHistoryJob::job_id
     * @param on_success Success callback (job deleted)
     * @param on_error Error callback
     */
    void delete_history_job(const std::string& job_id, SuccessCallback on_success,
                            ErrorCallback on_error) override;

  protected:
    helix::IMoonrakerClient& client_;
};
