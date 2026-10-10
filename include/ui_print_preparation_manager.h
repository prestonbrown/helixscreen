// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "capability_matrix.h"
#include "gcode_file_modifier.h"
#include "gcode_ops_detector.h"
#include "gcode_rewrite_block.h"
#include "i_moonraker_api.h"
#include "operation_timeout_guard.h"
#include "preprint_predictor.h"
#include "print_start_analyzer.h"
#include "print_start_checks.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "thermal_rate_model.h"

#include <functional>
#include <lvgl.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PrintPreparationManagerTestAccess;

namespace helix::ui {

/**
 * @file ui_print_preparation_manager.h
 * @brief Manages pre-print operations and G-code modification
 *
 * Handles the print preparation workflow including:
 * - Scanning G-code files for embedded operations (bed leveling, QGL, etc.)
 * - Collecting user-selected pre-print options from LVGL subjects
 * - Building and executing pre-print operation sequences
 * - Modifying G-code to disable embedded operations when requested
 *
 * ## Usage:
 * ```cpp
 * PrintPreparationManager prep_manager;
 * prep_manager.set_dependencies(api, printer_state);
 * prep_manager.set_option_state_provider([&](const std::string& id) {
 *     return renderer.get_state_for(id); // 0/1/-1 from active panel rows
 * });
 *
 * // When detail view opens:
 * prep_manager.scan_file_for_operations(filename, current_path);
 *
 * // When print button clicked:
 * prep_manager.start_print(filename, current_path, on_navigate_to_status);
 * ```
 */

/**
 * @brief Tri-state result for visibility + checked logic
 *
 * Single source of truth for determining the user's intent for a pre-print option:
 * - ENABLED: visible + checked (user wants this operation)
 * - DISABLED: visible + unchecked (user explicitly skipped this operation)
 * - NOT_APPLICABLE: hidden or no subject (not relevant to this printer)
 */
enum class PrePrintOptionState { ENABLED, DISABLED, NOT_APPLICABLE };

/**
 * @brief Pre-print options read from UI subjects
 */
struct PrePrintOptions {
    // File-level operations (from checkboxes in detail view)
    bool bed_mesh = false;
    bool qgl = false;
    bool z_tilt = false;
    bool nozzle_clean = false;
    bool purge_line = false;
    bool timelapse = false;

    // Macro-level skip flags (passed to PRINT_START as parameters)
    // These are only used when the macro supports the corresponding skip param
    bool skip_macro_bed_mesh = false;
    bool skip_macro_qgl = false;
    bool skip_macro_z_tilt = false;
    bool skip_macro_nozzle_clean = false;
    bool skip_macro_purge_line = false;
};

/**
 * @brief Callback for navigating to print status panel
 */
using NavigateToStatusCallback = std::function<void()>;

/**
 * @brief Callback for print completion (success or failure)
 */
using PrintCompletionCallback = std::function<void(bool success, const std::string& error)>;

/**
 * @brief Callback when PRINT_START macro analysis completes
 *
 * @param analysis The analysis result (check .found for validity)
 */
using MacroAnalysisCallback = std::function<void(const helix::PrintStartAnalysis& analysis)>;

/**
 * @brief Manages print preparation workflow
 */
class PrintPreparationManager {
  public:
    PrintPreparationManager() = default;
    ~PrintPreparationManager();

    // Non-copyable, non-movable (AsyncLifetimeGuard is non-movable)
    PrintPreparationManager(const PrintPreparationManager&) = delete;
    PrintPreparationManager& operator=(const PrintPreparationManager&) = delete;
    PrintPreparationManager(PrintPreparationManager&&) = delete;
    PrintPreparationManager& operator=(PrintPreparationManager&&) = delete;

    // === Setup ===

    /**
     * @brief Set API and printer state dependencies
     */
    void set_dependencies(IMoonrakerAPI* api, PrinterState* printer_state);

    /**
     * @brief Set callback for when PRINT_START macro analysis completes
     */
    void set_macro_analysis_callback(MacroAnalysisCallback callback) {
        on_macro_analysis_complete_ = std::move(callback);
    }

    /**
     * @brief Set a provider that returns the current toggle state for an
     *        option id ("bed_mesh", "qgl", ...).
     *
     * Used by `collect_macro_skip_params()` and friends to read user intent:
     * the print-detail panel sets this provider so the manager can query
     * dynamic per-option subjects without hardcoding their names.
     *
     * The provider returns 1 when the user has the option toggled ON, 0 when
     * OFF. Returning -1 means "not currently bound to a UI row" — the manager
     * then falls back to the option's `default_enabled` value from the
     * cached `PrePrintOptionSet`.
     *
     * Setting nullptr clears the provider; the manager falls back unconditionally
     * to `default_enabled` from the cached option set for every id (the legacy
     * built-in subject-pointer path was removed in Phase 3.5).
     */
    using OptionStateProvider = std::function<int(const std::string& id)>;
    void set_option_state_provider(OptionStateProvider provider) {
        option_state_provider_ = std::move(provider);
    }

    /**
     * @brief Read the user-intent state for a pre-print option by id.
     *
     * Resolution order:
     *   1. If an `OptionStateProvider` is set and returns 0/1, use that.
     *      The detail panel registers a provider that reads from the
     *      per-option dynamic subjects on the active row widgets.
     *   2. Otherwise, return ENABLED iff the option's `default_enabled` is
     *      true, DISABLED if the option is in the cached set with default
     *      false, or NOT_APPLICABLE if the option is unknown to the cached
     *      set (e.g. headless macro analysis querying about an option that
     *      isn't in this printer's database entry).
     *
     * @param id Option id (e.g. "bed_mesh", "ai_detect")
     */
    [[nodiscard]] PrePrintOptionState get_option_state(const std::string& id) const;

    // === PRINT_START Macro Analysis ===

    /**
     * @brief Analyze the printer's PRINT_START macro (async)
     *
     * Fetches macro definition from printer config and detects operations
     * like bed mesh, QGL, etc. Result is cached and reused until a reconnect
     * or a Klipper restart, either of which re-reads the macro.
     *
     * Call this once when connecting to the printer or when the detail
     * view needs to show macro-level operations.
     */
    void analyze_print_start_macro();

    /**
     * @brief Check if PRINT_START analysis is available
     */
    [[nodiscard]] bool has_macro_analysis() const {
        return macro_analysis_.has_value() && macro_analysis_->found;
    }

    /**
     * @brief Check if macro analysis is currently in progress
     *
     * Used to disable Print button until analysis completes, preventing
     * race conditions where print starts before skip params are known.
     */
    [[nodiscard]] bool is_macro_analysis_in_progress() const {
        return macro_analysis_in_progress_;
    }

    /**
     * @brief Get cached PRINT_START analysis result
     */
    [[nodiscard]] const std::optional<helix::PrintStartAnalysis>& get_macro_analysis() const {
        return macro_analysis_;
    }

    /**
     * @brief The options the detail view shows as rows
     *
     * The printer's option set. When the printer database declares no options
     * for this printer, one MacroParam row is added per operation the PRINT_START
     * analysis found controllable; collect_macro_skip_params() sends exactly
     * those rows' params.
     */
    [[nodiscard]] PrePrintOptionSet displayed_options() const;

    // === CapabilityMatrix Integration ===

    /**
     * @brief Builds a CapabilityMatrix from all available sources
     *
     * Layers capabilities with priority: DATABASE > MACRO_ANALYSIS > FILE_SCAN
     * @return CapabilityMatrix populated with all known capabilities
     */
    [[nodiscard]] CapabilityMatrix build_capability_matrix() const;

    // === Test Helpers ===

    /**
     * @brief Set macro analysis data (for testing)
     *
     * Allows injecting mock macro analysis data without async API calls.
     * @param analysis The analysis result to set
     */
    void set_macro_analysis(const helix::PrintStartAnalysis& analysis);

    /**
     * @brief Set cached scan result (for testing)
     *
     * Allows injecting mock scan data without async file downloads.
     * @param scan The scan result to cache
     * @param filename The filename to associate with this scan
     */
    void set_cached_scan_result(const gcode::ScanResult& scan, const std::string& filename);

    // === G-code Scanning ===

    /**
     * @brief Scan a G-code file for embedded operations (async)
     *
     * Downloads file content and scans for operations like bed leveling, QGL, etc.
     * Result is cached until a different file is scanned.
     *
     * @param filename File name (relative to gcodes root)
     * @param current_path Current directory path (empty = root)
     * @param local_path The file on this host (a USB stick) when Moonraker has
     *        no copy; read directly instead of downloaded.
     */
    void scan_file_for_operations(const std::string& filename, const std::string& current_path,
                                  const std::string& local_path = {});

    /**
     * @brief Clear cached scan result
     */
    void clear_scan_cache();

    /**
     * @brief What the last scan of @p filename found about printer-stopping commands
     *
     * NotRun until a scan of that file answers, or when its download failed or the
     * printer's macros had not been read; not_run_reason says which.
     */
    [[nodiscard]] helix::PrinterStopCheck printer_stop_check_for(const std::string& filename) const;

    /// Whether a scan of @p filename has answered, whatever the answer.
    [[nodiscard]] bool has_printer_stop_answer_for(const std::string& filename) const;

    /// Called on the main thread each time a scan answers, success or failure.
    void set_on_scan_answered(std::function<void()> cb);

    /**
     * @brief Check if scan result is available for a file
     */
    [[nodiscard]] bool has_scan_result_for(const std::string& filename) const;

    /**
     * @brief The PRINT_START/START_PRINT call the cached scan found in `filename`
     *
     * @return nullptr when the cache holds another file, or no call was found.
     */
    [[nodiscard]] const gcode::PrintStartCallInfo*
    print_start_for(const std::string& filename) const;

    /**
     * @brief Get cached scan result (if available)
     */
    [[nodiscard]] const std::optional<gcode::ScanResult>& get_scan_result() const {
        return cached_scan_result_;
    }

    // === Resource Safety ===

    /**
     * @brief Set the cached file size from Moonraker metadata
     *
     * Called when detail view fetches file metadata, allowing safety checks
     * to estimate memory/disk requirements for modification.
     *
     * @param size File size in bytes
     */
    void set_cached_file_size(size_t size);

    /**
     * @brief May we print a rewritten copy of the selected G-code file?
     *
     * The HelixPrint plugin is what makes a rewrite survivable as a product
     * decision rather than a technical one: without it the rewritten copy is
     * what lands in print history, so finished jobs are filed under names like
     * modified_1730824_benchy.gcode. Size is not part of the question - the
     * rewrite streams a line at a time, so a 2GB file costs what a 2KB one does.
     * It streams through a local copy, though, so a transport that cannot keep
     * one (ITransfersAPI::supports_local_copies) declines too.
     */
    [[nodiscard]] bool can_modify_gcode() const;

    /// Why a rewrite cannot run right now, or None: the one answer behind
    /// can_modify_gcode(), the pre-print option rows and the remap pick.
    [[nodiscard]] GcodeRewriteBlock gcode_rewrite_block() const;

    /// The same answer for a caller with no manager to hand.
    [[nodiscard]] static GcodeRewriteBlock gcode_rewrite_block_for(PrinterState* printer_state,
                                                                   IMoonrakerAPI* api);

    /**
     * @brief Get the temp directory path for streaming operations
     *
     * Uses same logic as ThumbnailCache: XDG → ~/.cache → TMPDIR → /tmp
     *
     * @return Path to usable temp directory, or empty string if none available
     */
    [[nodiscard]] std::string get_temp_directory() const;

    // === Print Execution ===

    /**
     * @brief Read pre-print options from subject states (LT2)
     *
     * Reads the current state of pre-print options from subjects instead
     * of directly querying widget states. This decouples the state from
     * the UI widgets and enables subject-based reactive patterns.
     *
     * Logic for each option:
     * 1. If visibility subject is set and value is 0, treat as hidden (return false)
     * 2. Otherwise, check the state subject - return true if value is 1
     *
     * @return PrePrintOptions with current selections
     */
    [[nodiscard]] PrePrintOptions read_options_from_subjects() const;

    /**
     * @brief Start print with optional pre-print operations
     *
     * Handles the full workflow:
     * 1. Read checkbox states for pre-print options
     * 2. Check if user disabled operations embedded in G-code
     * 3. If so, modify file (add skip params or comment out embedded ops) and print
     * 4. Otherwise, start print directly
     *
     * Print is started by calling Moonraker's print API. The PRINT_START macro
     * handles all pre-print operations (homing, heating, bed mesh, etc.) internally.
     *
     * @param filename File to print
     * @param current_path Current directory path
     * @param on_navigate_to_status Callback to navigate to print status panel
     * @param on_completion Optional callback for print completion
     */
    void start_print(const std::string& filename, const std::string& current_path,
                     NavigateToStatusCallback on_navigate_to_status,
                     PrintCompletionCallback on_completion = nullptr);

    /**
     * @brief Rewrite tool commands in a G-code file and print the modified copy.
     *
     * For RemapStrategy::GcodeRewrite backends - a tool changer driving its
     * swaps with its own T<n> macros rather than klipper-toolchanger - that
     * own no internal tool table: the only way to redirect a logical tool to a
     * different physical head is to rewrite the Tx / ACTIVATE_EXTRUDER /
     * SET_GCODE_VARIABLE lines in the file itself.
     *
     * Flow, streaming end to end - no stage ever holds the file:
     * 1. Download the original to a temp file (streaming).
     * 2. GcodeToolRemapper::apply_to_stream() rewrites it to a second temp file
     *    a line at a time, so peak memory is one line whatever the job's size.
     * 3. Upload the modified copy and start it via the HelixPrint plugin's
     *    start_modified_print() so print history stays under the ORIGINAL
     *    filename. (This path requires the plugin; callers must guard.)
     *
     * If `remap` produces no line changes (identity remap), the original file is
     * printed directly with no temp copy.
     *
     * @param file_path Full path to the original file relative to gcodes root
     *                  (e.g. "usb/multicolor.gcode")
     * @param remap Logical tool index -> physical head index
     * @param on_navigate_to_status Callback to navigate to the print status panel
     */
    void modify_and_print_with_remap(const std::string& file_path, const std::map<int, int>& remap,
                                     NavigateToStatusCallback on_navigate_to_status);

    /**
     * @brief Would DISABLING this pre-print option require the HelixPrint plugin?
     *
     * The print-detail view uses this to HIDE a toggle when the plugin is
     * absent: without the plugin, disabling such an option can't be honored —
     * start_print() reaches can_modify_gcode() and drops the
     * modification with a "Requires HelixPrint plugin" warning.
     *
     * Returns true only when NO pre-start short-circuit in start_print() would
     * fire before that capability check. Two ways to require the plugin:
     *   (a) a file-embeddable op (bed_mesh/qgl/z_tilt/nozzle_clean) is embedded
     *       in the currently-scanned file, OR
     *   (b) the option is a MacroParam whose skip must be rewritten into the
     *       START_PRINT call.
     * False whenever disabling the option has an effect that needs no plugin,
     * so the row stays useful: a PreStartGcode option always emits its line,
     * and a MacroParam skip is carried by a pre-start block (`setup_gcode`, or
     * any PreStartGcode line). This is why the K2 Plus PREPARE bed_mesh toggle
     * stays visible. Stripping an embedded op needs the plugin on every start
     * path, so it alone hides the row.
     *
     * @param opt The option whose disable-cost is being evaluated
     */
    [[nodiscard]] bool disabling_option_requires_plugin(const PrePrintOption& opt) const;

    /**
     * @brief Can the adaptive-mesh params actually reach the printer?
     *
     * The inverse-facing sibling of disabling_option_requires_plugin(): that one
     * asks whether DISABLING an option needs the plugin (and hides the toggle if
     * so), this one asks whether the params an ENABLED adaptive bed_mesh emits
     * can be delivered at all. True when the plugin can rewrite the PRINT_START
     * call, or when a pre-start mechanism (printer `setup_gcode` / per-option
     * PreStartGcode lines) carries the intent instead.
     *
     * False means collect_macro_skip_params() must not emit them: start_print()
     * would collect, fail the capability check, drop them, and warn the user
     * about a modification they never asked for.
     */
    [[nodiscard]] bool adaptive_emit_is_deliverable() const;

    /**
     * @brief Translated, comma-joined names of the features a dropped
     *        modification would have carried.
     *
     * Feeds the "needs the HelixPrint plugin" warning so it names what it is
     * dropping. Empty when nothing identifiable contributed, in which case the
     * caller falls back to the generic message.
     *
     * @param ops_to_disable Embedded ops this print was going to strip out
     */
    [[nodiscard]] std::string
    describe_dropped_modifications(const std::vector<gcode::OperationType>& ops_to_disable) const;

    /// Log and toast that this print's modifications are being dropped, naming
    /// the affected features and why: the HelixPrint plugin is absent, or the
    /// transport keeps no local copy to rewrite. Every start path that declines
    /// a modification reports it through here.
    void warn_modifications_dropped(const std::vector<gcode::OperationType>& ops_to_disable) const;

    /// Whether the transport can write the local copy a rewrite streams through.
    [[nodiscard]] bool transport_keeps_local_copies() const;
    [[nodiscard]] static bool keeps_local_copies(IMoonrakerAPI* api);
    [[nodiscard]] static GcodeRewriteBlock rewrite_block_with(PrinterState* printer_state,
                                                              bool local_copies);

    /**
     * @brief Get the pre-print time estimate subject (seconds)
     *
     * Updated by recalculate_estimate() on open and whenever checkbox toggles
     * change. Value is total estimated seconds for all enabled pre-print
     * operations.
     */
    lv_subject_t* get_preprint_estimate_subject();

    /**
     * @brief Recalculate the pre-print time estimate
     *
     * Reads current temperatures, checkbox states, and historical timing data
     * to produce an estimated prep time in seconds. Updates preprint_estimate_subject_.
     */
    void recalculate_estimate();

    /**
     * @brief Invalidate the cached PreprintPredictor
     *
     * Forces the next recalculate_estimate() call to reload entries from config.
     * Call this after a print completes so timing data is re-read.
     */
    void invalidate_predictor_cache();

    /**
     * @brief Check if a print is currently being started
     *
     * Delegates to PrinterState::is_print_in_progress(). Returns true from
     * when start_print() is called until the print actually starts or fails.
     * Used to prevent double-tap issues.
     */
    [[nodiscard]] bool is_print_in_progress() const;

  private:
    /// Tell PrinterState how many of the rows the macro analysis adds to
    /// displayed_options() will render: rewrite-gated rows hide while a rewrite
    /// cannot run.
    void publish_macro_option_count();

    friend class ::PrintPreparationManagerTestAccess;

    // === Dependencies ===
    IMoonrakerAPI* api_ = nullptr;
    PrinterState* printer_state_ = nullptr;

    // === Checkbox State Subjects (LT2 - from PrintSelectDetailView) ===
    // === Pre-print estimate ===
    lv_subject_t preprint_estimate_subject_{};
    bool estimate_subject_initialized_ = false;

    // === Predictor Cache ===
    // Avoids reparsing config JSON on every checkbox toggle; invalidated after print completes
    helix::PreprintPredictor cached_predictor_;
    bool predictor_cached_ = false;

    // === Scan Cache ===
    std::optional<gcode::ScanResult> cached_scan_result_;
    std::string cached_scan_filename_;
    /// Full identity of the file cached_scan_result_ came from (the stick path
    /// for a USB file, else current_path/filename), so a same-named file
    /// elsewhere never reuses it.
    std::string cached_scan_key_;
    /// Key of the newest scan requested; an older scan's late answer is dropped.
    std::string requested_scan_key_;
    helix::PrinterStopCheck printer_stop_check_;
    std::string printer_stop_check_filename_; ///< The file printer_stop_check_ answers for
    std::function<void()> on_scan_answered_;

    /// Record the scan's printer-stopping command answer for @p filename and report it.
    void answer_printer_stop_check(const std::string& filename, helix::PrinterStopCheck check);
    std::optional<size_t> cached_file_size_; ///< File size from Moonraker metadata
    /// Job temps for {bed_temp}/{extruder_temp} come from the scan cache's
    /// print_start call (the file's own START_PRINT line) — see
    /// collect_pre_start_gcode_lines().

    /**
     * @brief Get the cached pre-print option set from PrinterState
     *
     * Delegates to PrinterState which owns the cache. PrinterState refreshes
     * the option set when the printer type changes.
     *
     * @return Option set for current printer type, or empty if PrinterState not set
     */
    [[nodiscard]] const PrePrintOptionSet& get_cached_options() const;

    // === Callbacks ===
    MacroAnalysisCallback on_macro_analysis_complete_;

    // === Option-state provider (LT3) ===
    // When set, drives get_option_state() — used by the print-detail panel
    // to surface dynamic per-option subjects without the manager having to
    // know about their LVGL pointers.
    OptionStateProvider option_state_provider_;

    // === PRINT_START Analysis Cache ===
    std::optional<helix::PrintStartAnalysis> macro_analysis_;
    bool macro_analysis_in_progress_ = false;
    // The config may have changed under the analysis in flight; run again when it lands.
    bool macro_analysis_stale_ = false;
    // Last value seen, so a re-subscription is told apart from a real transition.
    int last_connection_state_ = -1;
    // Klippy left READY while connected; its return to READY is a restart.
    bool klippy_restarting_ = false;

    // Retry logic for macro analysis
    int macro_analysis_retry_count_ = 0;
    static constexpr int MAX_MACRO_ANALYSIS_RETRIES = 2; // 3 total attempts

    // === Lifetime Guard for Async Callbacks ===
    helix::AsyncLifetimeGuard lifetime_;

    // === Connection Observers ===
    // Analyze on connect; re-read the macro on a reconnect or a Klipper restart.
    ObserverGuard connection_observer_;
    ObserverGuard klippy_observer_;
    /// A leveling skip still set while no job holds the machine is reset: on
    /// the job's release, and on the status frame that reports it pending, so
    /// a frame landing after the release or a reconnect still clears it and
    /// the next print, started from anywhere, runs every step.
    ObserverGuard job_holds_observer_;
    ObserverGuard plugin_observer_;
    /// keeps_local_copies(api_), answered by set_dependencies().
    bool keeps_local_copies_ = true;
    bool skip_reset_sent_ = false; ///< One reset per pending skip, until it reads clear
    void reset_pending_skips();

    // === Pre-start completion wait ===
    // When the pre-start gcode RPC times out but Klipper still reports
    // idle_timeout "Printing", the macro is still executing on the printer
    // (execute_gcode blocks until it finishes, and a long macro — e.g.
    // Creality's BED_MESH_CALIBRATE_START_PRINT chain — can outlive the RPC
    // ceiling). Instead of failing the print start, wait for the busy->idle
    // edge and start the print then. The guard is the backstop: if the
    // printer never goes idle within another ceiling, the start fails.
    bool pre_start_wait_active_ = false;
    OperationTimeoutGuard pre_start_wait_guard_;
    ObserverGuard pre_start_wait_observer_;

    /**
     * @brief Continue the print start after the pre-start block is done
     *
     * Shared continuation of the pre-start success callback and the
     * completion wait: modifies the file when operations must be stripped,
     * otherwise starts the print directly.
     */
    /// Whether a preparing job was live when this start began. Only then can
    /// its disappearance mean "the user cancelled" rather than "this caller
    /// never armed one".
    bool armed_at_start_ = false;

    /// When the pre-start gcode RPC was sent. The preparing-job guard above
    /// cannot catch a late response to a cancelled print: the job can still
    /// be armed when klippy finally flushes a backed-up request (K1C capture
    /// 2026-08-20: the ack arrived 370s later, at cancel time, and relaunched
    /// the print). A pre-start block takes seconds; one that old is stale.
    std::chrono::steady_clock::time_point pre_start_sent_at_{};

    /// The preparing-job epoch as it stood when the pre-start block was sent.
    ///
    /// This, not elapsed time, is what says whether the intent behind a
    /// pre-start is still live. PrinterPrintState bumps the epoch on
    /// begin_preparing() and zeroes it on retire_preparing() — so a cancel, a
    /// failure, or a different print taking over all move it, and a comparison
    /// at ack time catches every one of them without guessing from a clock.
    /// 0 means "no preparing job was armed when we sent", in which case there
    /// is no intent signal to check and only the time backstop applies.
    int pre_start_epoch_ = 0;

    void continue_print_start(const std::string& filename,
                              const std::vector<gcode::OperationType>& ops_to_disable,
                              NavigateToStatusCallback on_navigate_to_status,
                              PrintCompletionCallback on_completion);

    /**
     * @brief Pre-start RPC failed — decide failure vs. wait-for-idle
     *
     * Main thread only (called via token.defer). A TIMEOUT error while the
     * printer still reports idle_timeout "Printing" enters the completion
     * wait; anything else fails the print start.
     */
    void handle_pre_start_gcode_error(const MoonrakerError& error, const std::string& filename,
                                      const std::vector<gcode::OperationType>& ops_to_disable,
                                      NavigateToStatusCallback on_navigate_to_status,
                                      PrintCompletionCallback on_completion);

    /// Arm the busy->idle observer + backstop. Main thread only.
    void begin_pre_start_completion_wait(const MoonrakerError& timeout_error,
                                         const std::string& filename,
                                         const std::vector<gcode::OperationType>& ops_to_disable,
                                         NavigateToStatusCallback on_navigate_to_status,
                                         PrintCompletionCallback on_completion);

    /// Tear the wait down (observer + backstop). Idempotent.
    void finish_pre_start_wait();

    // === Internal Methods ===

    /**
     * @brief Collect operations that user wants to disable
     *
     * Compares checkbox states against cached scan result to identify
     * operations that are embedded in the file but disabled by user. An option
     * whose value a self-storing firmware holds strips nothing: that firmware
     * gates the file's own command on its setting.
     */
    [[nodiscard]] std::vector<gcode::OperationType> collect_ops_to_disable() const;

    /**
     * @brief Download, modify, and print a G-code file
     *
     * Used when user disabled an operation that's embedded in the G-code
     * or when macro skip parameters need to be added to PRINT_START.
     *
     * @param file_path Full path to file relative to gcodes root
     * @param ops_to_disable Operations to comment out in the file
     * @param macro_skip_params Skip params to append to PRINT_START call
     * @param on_navigate_to_status Callback to navigate to print status panel
     */
    void modify_and_print(const std::string& file_path,
                          const std::vector<gcode::OperationType>& ops_to_disable,
                          const std::vector<std::pair<std::string, std::string>>& macro_skip_params,
                          NavigateToStatusCallback on_navigate_to_status);

    /**
     * @brief Unified streaming modification and print flow
     *
     * Downloads file to disk, applies streaming modification (file-to-file),
     * then uploads from disk. This is the single path for all G-code modifications,
     * avoiding memory spikes that cause TTC errors on constrained devices.
     *
     * If use_plugin is true and helix_print plugin is available, the plugin's
     * path-based API is used after upload for symlink creation and history patching.
     *
     * @param file_path Full path to original file relative to gcodes root
     * @param display_filename Filename for display purposes
     * @param ops_to_disable Operations to comment out in the file
     * @param macro_skip_params Skip params to append to PRINT_START call
     * @param mod_names Modification identifiers for tracking
     * @param on_navigate_to_status Callback to navigate to print status panel
     * @param use_plugin Whether to use helix_print plugin for print start
     */
    void modify_and_print_streaming(
        const std::string& file_path, const std::string& display_filename,
        const std::vector<gcode::OperationType>& ops_to_disable,
        const std::vector<std::pair<std::string, std::string>>& macro_skip_params,
        const std::vector<std::string>& mod_names, NavigateToStatusCallback on_navigate_to_status,
        bool use_plugin);

    /**
     * @brief Start print directly (no pre-print operations)
     */
    void start_print_directly(const std::string& filename,
                              NavigateToStatusCallback on_navigate_to_status,
                              PrintCompletionCallback on_completion);

    /**
     * @brief Retire the preparing job after a start attempt dies inside here
     *
     * The modify and remap routes take no completion callback -
     * continue_print_start() calls modify_and_print() and returns - so
     * PrintStartController's own retire_preparing(Failed) is unreachable from
     * them. Whatever ends the attempt here has to retire the job.
     *
     * Getting this wrong is not a cosmetic leak: `print_in_progress` is now
     * PUBLISHED from the preparing job rather than set by hand, so a job left
     * armed keeps the flag true until the 1800s watchdog fires. In that window
     * can_start_new_print() refuses every later print, `job_holds_machine` stays
     * 1 (greying jog, levelling, macros and the bypass tile, refusing filament
     * ops, inhibiting display sleep), and the status panel sits on "Preparing".
     *
     * Main thread only - it writes subjects. Every call site below is already
     * inside a token.defer() or on a synchronous path.
     *
     * @param where Short tag naming the exit, for the log line.
     */
    void abandon_start(const char* where);

    /**
     * @brief Lazy-initialize the prep-time estimate subject.
     *
     * Called from get_preprint_estimate_subject() and
     * recalculate_estimate(); ensures the subject is ready before any
     * observer wires up.
     */
    void ensure_estimate_subject_initialized();

    /**
     * @brief Internal implementation of macro analysis (for retries)
     *
     * Called by analyze_print_start_macro() and by retry timer callbacks.
     * Does not reset retry counter.
     */
    void analyze_print_start_macro_internal();

    void on_connection_state(int state);
    void on_klippy_state(int state);

    /// Drop the cached analysis and run a fresh one, or mark the one in flight stale.
    void refresh_macro_analysis();

    /// When the analysis that just landed is stale, start another and return true.
    bool restart_stale_macro_analysis();

    /**
     * @brief Collect macro skip parameters based on user checkboxes and macro analysis
     *
     * Checks which macro operations the user disabled (checkbox unchecked) and
     * are controllable (have skip parameters). Returns the params to add to PRINT_START.
     *
     * @return Vector of (param_name, value) pairs like {"SKIP_BED_MESH", "1"}
     */
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    collect_macro_skip_params() const;

    /**
     * @brief Collect rendered gcode lines for every PreStartGcode option in the
     *        active printer's set.
     *
     * Walks the cached `PrePrintOptionSet`. For each option whose strategy is
     * `PreStartGcode`, calls `render_pre_start_gcode(opt, enabled)` where
     * `enabled` is `true` when `get_option_state(id) == ENABLED` and `false`
     * otherwise (DISABLED). Options resolving to `NOT_APPLICABLE` are skipped
     * entirely — they don't represent capabilities of this printer.
     *
     * Lines are returned in the same (category, order) order the option set is
     * sorted in, so callers can fire them sequentially before START_PRINT.
     *
     * @param with_skip_toggles False leaves out the leveling-skip toggles
     *        (skip_wrappers::option_for()): those set a flag and carry no
     *        START_PRINT skip parameter, so they are no delivery path for one.
     * @return Vector of rendered gcode lines, e.g. {"LOAD_AI_RUN SWITCH=1"}.
     */
    [[nodiscard]] std::vector<std::string>
    collect_pre_start_gcode_lines(const std::string& filename = {},
                                  bool with_skip_toggles = true) const;

    /**
     * @brief Build the combined pre-start gcode block executed before START_PRINT.
     *
     * Concatenates `setup_gcode` (when `emit_setup` is true and `setup_gcode`
     * is non-empty) and `pre_start_lines` with `\n` separators. Returns "" when
     * nothing should be emitted.
     *
     * Pure static function — exists as a separate symbol so the join contract
     * has a single test entry point (`PrintPreparationManagerTestAccess::
     * build_pre_start_gcode_block`). Re-implementing the join in test code
     * would let production drift silently.
     *
     * @param setup_gcode Printer-wide preamble (e.g. "PRINT_PREPARED")
     * @param pre_start_lines Per-option PreStartGcode lines
     * @param emit_setup Whether to include setup_gcode. start_print() gates on
     *        `!macro_skip_params.empty()` — when no skip params are passed,
     *        START_PRINT runs without modification and setup_gcode is suppressed.
     */
    [[nodiscard]] static std::string
    build_pre_start_gcode_block(const std::string& setup_gcode,
                                const std::vector<std::string>& pre_start_lines, bool emit_setup);
};

} // namespace helix::ui
