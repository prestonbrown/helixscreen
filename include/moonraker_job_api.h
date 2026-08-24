// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_job_api.h
 * @brief Job control operations via Moonraker
 *
 * Extracted from MoonrakerAPI to encapsulate all print job control functionality
 * in a dedicated class. Uses MoonrakerClient for JSON-RPC transport.
 */

#pragma once

#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "moonraker_types.h"

#include <functional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class IMoonrakerClient;
} // namespace helix

/**
 * @brief Print Job Control API operations via Moonraker
 *
 * Provides high-level operations for starting, pausing, resuming, and canceling
 * prints through Moonraker's printer.print.* endpoints. Also includes helix_print
 * plugin operations for modified print workflows.
 *
 * All methods are asynchronous with callbacks.
 *
 * Usage:
 *   MoonrakerJobAPI job(client);
 *   job.start_print("test.gcode",
 *       []() { ... },
 *       [](const auto& err) { ... });
 */
class MoonrakerJobAPI : public IJobAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using BoolCallback = std::function<void(bool)>;
    using ModifiedPrintCallback = std::function<void(const ModifiedPrintResult&)>;

    static constexpr uint32_t CANCEL_TIMEOUT_MS = 300000; // 5 min — CANCEL_PRINT macros can be slow

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     */
    explicit MoonrakerJobAPI(helix::IMoonrakerClient& client);
    virtual ~MoonrakerJobAPI() = default;

    // ========================================================================
    // Job Control Operations
    // ========================================================================

    /**
     * @brief Start printing a file
     *
     * @param filename Full path to G-code file
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void start_print(const std::string& filename, SuccessCallback on_success,
                     ErrorCallback on_error) override;

    /**
     * @brief Start printing modified G-code via helix_print plugin (v2.0 API)
     *
     * The modified file must already be uploaded to the printer. This method
     * tells the helix_print plugin where to find it and starts the print.
     *
     * Plugin workflow:
     * - Validates temp file exists
     * - Creates a symlink with the original filename (for print_stats)
     * - Starts the print via the symlink
     * - Patches history to record the original filename
     *
     * Use PrinterState::service_has_helix_plugin() to check availability.
     *
     * @param original_filename Path to the original G-code file (for history)
     * @param temp_file_path Path to already-uploaded modified file (e.g., ".helix_temp/foo.gcode")
     * @param modifications List of modification identifiers (e.g., "bed_leveling_disabled")
     * @param on_success Callback with print result
     * @param on_error Error callback
     */
    void start_modified_print(const std::string& original_filename,
                              const std::string& temp_file_path,
                              const std::vector<std::string>& modifications,
                              ModifiedPrintCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Check if helix_print plugin is available
     *
     * Queries /server/helix/status to detect plugin availability.
     * Call this before using start_modified_print() to decide on flow.
     *
     * @param on_result Callback with availability (true if plugin detected)
     * @param on_error Error callback (also means plugin not available)
     */
    void check_helix_plugin(BoolCallback on_result, ErrorCallback on_error) override;

    /**
     * @brief Pause the current print
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void pause_print(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Resume a paused print
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void resume_print(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Cancel the current print
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void cancel_print(SuccessCallback on_success, ErrorCallback on_error) override;

  protected:
    helix::IMoonrakerClient& client_;
};
