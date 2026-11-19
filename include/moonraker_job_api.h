// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_job_api.h
 * @brief Job control operations via Moonraker
 *
 * A MoonrakerAPI sub-API that encapsulates all print job control functionality
 * in a dedicated class. Uses MoonrakerClient for JSON-RPC transport.
 */

#pragma once

#include "helix_shared_callback.h"
#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "moonraker_types.h"

#include <functional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class IMoonrakerClient;
class PrinterState;
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
class MoonrakerJobAPI : public helix::IJobAPI {
  public:
    using SuccessCallback = helix::SharedCallback<void()>;
    using ErrorCallback = helix::SharedCallback<void(const MoonrakerError&)>;
    using BoolCallback = std::function<void(bool)>;
    using ModifiedPrintCallback = std::function<void(const ModifiedPrintResult&)>;

    // Moonraker confirms printer.print.start only once Klipper acknowledges
    // SDCARD_PRINT_FILE. A print-start macro that heats synchronously holds that
    // ack for the whole heat-up, so a cold machine legitimately takes minutes to
    // answer while the print is already running. 10 min covers a cold heat-up; a
    // genuinely lost request still surfaces, just later.
    // (prestonbrown/helixscreen#1451)
    static constexpr uint32_t kPrintStartTimeoutMs = 600000; // 10 min

    // The helix_print plugin answers print_modified only after awaiting that same
    // SDCARD_PRINT_FILE ack, and it gets there after copying metadata and creating
    // its symlink. Strictly more work for the same wait, so this endpoint can never
    // deserve the shorter ceiling of the two.
    static constexpr uint32_t kPrintModifiedTimeoutMs = kPrintStartTimeoutMs;

    static constexpr uint32_t kCancelTimeoutMs = 300000; // 5 min — CANCEL_PRINT macros can be slow

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     * @param state Printer state the spools-on-the-bed latch is read from; null
     *        refuses nothing
     */
    explicit MoonrakerJobAPI(helix::IMoonrakerClient& client,
                             const helix::PrinterState* state = nullptr);
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
     * @param extra_params Sent beside `filename` (a firmware's per-print tool map)
     */
    void start_print(const std::string& filename, SuccessCallback on_success,
                     ErrorCallback on_error,
                     const helix::PrintStartParams& extra_params = {}) override;

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
     * @param extra_params Sent beside the copy (the tool map), and with the
     *        fallback printer.print.start when the plugin refuses
     */
    void start_modified_print(const std::string& original_filename,
                              const std::string& temp_file_path,
                              const std::vector<std::string>& modifications,
                              ModifiedPrintCallback on_success, ErrorCallback on_error,
                              const helix::PrintStartParams& extra_params = {}) override;

    /**
     * @brief Have the helix_print plugin apply @p edits to the original and start the result
     *
     * server.helix.print_edited: the plugin rewrites @p original_filename into
     * @p temp_file_path beside it (gcode_edit_list.h has the edit grammar) and
     * starts that copy as start_modified_print() would. Nothing is uploaded.
     * Gate on the plugin's `server_edits` feature (gcode::choose_edit_transport).
     *
     * @p file_size (0 when unknown) sizes the wait (gcode::print_edited_timeout_ms):
     * the plugin answers only once the print has started. What an error means for
     * the start is gcode::classify_print_edited_failure's to say.
     */
    void start_edited_print(const std::string& original_filename, const std::string& temp_file_path,
                            const json& edits, const std::vector<std::string>& modifications,
                            uint64_t file_size, ModifiedPrintCallback on_success,
                            ErrorCallback on_error,
                            const helix::PrintStartParams& extra_params = {}) override;

    /**
     * @brief Check if helix_print plugin is available
     *
     * Queries /server/helix/status to detect plugin availability.
     * Call this before using start_modified_print() to decide on flow.
     *
     * @param on_result Callback with availability (true if plugin detected)
     * @param on_error Error callback (also means plugin not available)
     */
    void check_helix_plugin(PluginInfoCallback on_result, ErrorCallback on_error) override;

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
    /// True, with @p on_error called, while spools lie on the bed.
    /// Announce a start of @p filename to the plugin's print_hooks, or take
    /// back the announcement of one that failed.
    void send_screen_start(const std::string& filename, bool withdraw);

    bool refused_by_spool_latch(const char* method, const ErrorCallback& on_error) const;

    helix::IMoonrakerClient& client_;
    const helix::PrinterState* state_ = nullptr;
};
