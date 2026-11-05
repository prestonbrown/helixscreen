// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include "ams_types.h"
#include "async_lifetime_guard.h"
#include "filament_mapper.h"
#include "helix_shared_callback.h"
#include "print_start_checks.h"
#include "print_start_inputs.h"

#include <functional>
#include <lvgl.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Forward declarations
namespace helix {
class AmsBackend;
class PrinterState;
class IMoonrakerAPI;
} // namespace helix
struct PrintFileData;
class PrintStartControllerTestAccess; // test-only friend (global scope)

namespace helix::ui {

class PrintSelectDetailView;
class PrintPreparationManager;

/**
 * @brief Controller for print initiation workflow
 *
 * Handles the print start process including:
 * - Ordered pre-print gate pipeline (insufficient spool weight, filament
 *   presence, color/material matching) driven by run_gates_from()
 * - Actual print start via PrintPreparationManager
 *
 * This controller does NOT own the file selection state or the detail view.
 * It receives file information via set_file() and delegates the actual
 * print start to PrintPreparationManager: the detail view's, or for a start
 * with no detail view, the one set_prep_manager() names, with the file half of
 * the gates from set_file_facts(). Every way initiate() ends without a print
 * calls the cancelled callback.
 *
 * @pattern Extracted controller pattern - separates print initiation workflow
 *          from the larger PrintSelectPanel.
 */
class PrintStartController {
  public:
    using PrintStartedCallback = helix::SharedCallback<void()>;
    using PrintCancelledCallback = helix::SharedCallback<void()>;
    using UpdatePrintButtonCallback = helix::SharedCallback<void()>;
    using HideDetailViewCallback = helix::SharedCallback<void()>;
    using ShowDetailViewCallback = helix::SharedCallback<void()>;
    using NavigateToPrintStatusCallback = helix::SharedCallback<void()>;

    /**
     * @brief Construct controller with required dependencies
     *
     * @param printer_state Reference to PrinterState for capability queries
     * @param api Pointer to IMoonrakerAPI (may be nullptr initially)
     */
    PrintStartController(PrinterState& printer_state, IMoonrakerAPI* api);
    ~PrintStartController();

    // Non-copyable
    PrintStartController(const PrintStartController&) = delete;
    PrintStartController& operator=(const PrintStartController&) = delete;

    /**
     * @brief Set the API (can be null initially, set later)
     */
    void set_api(IMoonrakerAPI* api);

    /**
     * @brief Set the detail view for prep manager access
     *
     * The detail view owns the PrintPreparationManager which is needed
     * for the actual print start sequence.
     */
    void set_detail_view(PrintSelectDetailView* detail_view);

    /// The prep manager a start with no detail view uses. Ignored while a
    /// detail view is set: the view owns the manager its rows feed.
    void set_prep_manager(PrintPreparationManager* prep);

    /// The file half of the gates for a start with no detail view. Ignored
    /// while a detail view is set.
    void set_file_facts(helix::FileStartFacts facts);

    /**
     * @brief Set the file to print
     *
     * @param filename Raw filename (e.g., "benchy.gcode")
     * @param path Directory path relative to gcodes root
     * @param filament_colors Hex colors per tool for AMS matching
     * @param thumbnail_path Extracted thumbnail path (for USB/embedded thumbnails)
     */
    void set_file(const std::string& filename, const std::string& path,
                  const std::vector<std::string>& filament_colors,
                  const std::string& thumbnail_path = "");

    /// The key the prep manager keeps this file's scan under, when it is not
    /// the file's own path (a stick file printed from its copy). set_file()
    /// clears it.
    void set_scan_key(std::string key) {
        scan_key_ = std::move(key);
    }

    /// Runs when a start with no detail view meets a printer-stopping command
    /// check that did not run: the start ends there (then the cancelled
    /// callback), since nothing on screen can stand in for that check.
    void set_on_file_unchecked(helix::SharedCallback<void()> cb) {
        on_file_unchecked_ = std::move(cb);
    }

    /// Objects to skip in the next start, captured at the Print tap. Consumed
    /// by that start; a reprint never reads them.
    void set_exclude_picks(std::vector<std::string> picks) {
        exclude_picks_ = std::move(picks);
    }

    /**
     * @brief Initiate print workflow
     *
     * Entry point for starting a print: startup grace-period guard, button
     * park, and the can_start_new_print() state guard, then the ordered gate
     * pipeline (run_gates_from). Each gate that warns shows ONE dialog; the
     * dialog drives continuation (proceed resumes at the next gate).
     */
    void initiate();

    /**
     * @brief Reprint a file already on the printer.
     *
     * Shares the U1 native pre-print send with the normal start path, but uses
     * the lightweight job().start_print (no upload/prep — the file is already on
     * the printer). @p tools_used drives the SET_PRINT_USED_EXTRUDERS
     * computation (empty → no native send, just starts). @p on_started /
     * @p on_error are invoked on the MAIN thread.
     *
     * There is no detail view here and no reprint picker, so the routing comes
     * from IPrintRouting::last_print_tool_mapping() — the routing the printer ran
     * the job with — via FilamentMapper::reprint_remap(). When that is not known
     * the map is SKIPPED rather than defaulted: an empty remap makes
     * build_preprint_gcode write every used tool to its firmware-default head,
     * which erases the crossover the print actually used. A backend whose
     * firmware default is that identity (default_print_routing_is_identity())
     * gets the empty remap instead, so its map for the file's tools goes out.
     *
     * @param filename   Raw filename to reprint (already on the printer).
     * @param path       Directory path relative to gcodes root (unused by the
     *                   lightweight start; kept for symmetry with set_file()).
     * @param tools_used Set of tool indices used by the file (U1 native send).
     * @param on_started Called on success (main thread).
     * @param on_error   Called on failure (main thread).
     */
    /// The tools @p filename uses, read through FileStartFactsLoader and
    /// file_tools_used(); empty when the file answers nothing. Answers once, on
    /// the main thread; a later read supersedes an unanswered one.
    /// @p read bounds the file read: a caller that cannot wait on a download
    /// passes FileRead::Footer.
    void read_file_tools(const std::string& filename,
                         helix::SharedCallback<void(std::set<int>)> on_tools,
                         helix::FileRead read = helix::FileRead::Whole);

    void initiate_reprint(const std::string& filename, const std::string& path,
                          const std::set<int>& tools_used, helix::SharedCallback<void()> on_started,
                          helix::SharedCallback<void()> on_error, int archive_plate = 0);

    /**
     * @brief initiate_reprint() with a routing the caller knows.
     *
     * For a file whose tool-to-head map is fixed by whoever generated it, so
     * the pre-print send writes @p routing instead of the last job's.
     */
    void initiate_reprint_with_routing(const std::string& filename, const std::set<int>& tools_used,
                                       const std::map<int, int>& routing,
                                       helix::SharedCallback<void()> on_started,
                                       helix::SharedCallback<void()> on_error);

    /**
     * @brief Check if controller is ready to start a print
     *
     * @return true if a filename is set and a detail view or a prep manager is available
     */
    [[nodiscard]] bool is_ready() const;

    // === Callbacks ===

    void set_on_print_started(PrintStartedCallback cb) {
        on_print_started_ = std::move(cb);
    }
    void set_on_print_cancelled(PrintCancelledCallback cb) {
        on_print_cancelled_ = std::move(cb);
    }
    void set_update_print_button(UpdatePrintButtonCallback cb) {
        update_print_button_ = std::move(cb);
    }
    void set_hide_detail_view(HideDetailViewCallback cb) {
        hide_detail_view_ = std::move(cb);
    }
    void set_show_detail_view(ShowDetailViewCallback cb) {
        show_detail_view_ = std::move(cb);
    }
    void set_navigate_to_print_status(NavigateToPrintStatusCallback cb) {
        navigate_to_print_status_ = std::move(cb);
    }

    /**
     * @brief Set the subject that controls print button enabled state
     *
     * The controller sets this to 0 when print is initiated and relies
     * on update_print_button_ callback for re-enabling on cancel/failure.
     */
    void set_can_print_subject(lv_subject_t* subject) {
        can_print_subject_ = subject;
    }

  private:
    /// Reads a reprinted file's tools when its caller has none; created on that use.
    std::unique_ptr<helix::FileStartFactsLoader> reprint_facts_;

    // Test-only access to the gate-pipeline runner (toy-gate injection and
    // dialog introspection).
    friend class ::PrintStartControllerTestAccess;

    /// With spools latched on the bed, opens the drying banner's prompt instead
    /// of a start the API layer would refuse. True when the start must stop.
    bool divert_to_spool_removal();
    /// A filament operation this screen sequences is running
    /// (AmsState::any_operation_running_here()): say so instead of starting a
    /// print it would keep sending moves into.
    /// True when the start must stop.
    bool refuse_while_filament_op_runs();

    /**
     * @brief Execute the actual print start
     *
     * Called when the gate pipeline runs off the end (all gates passed or the
     * user confirmed every warning). Delegates to PrintPreparationManager for
     * file operations and Moonraker API calls.
     */
    void execute_print_start();

    /**
     * @brief Send the backend's firmware-native pre-print config gcode, then continue.
     *
     * When the active AMS backend's RemapStrategy is PrePrintSend, the U1
     * firmware requires SET_PRINT_USED_EXTRUDERS / SET_PRINT_EXTRUDER_MAP to be
     * emitted BEFORE PRINT_START (they error mid-print). Builds that gcode from
     * @p tools_used and @p remap (supplied by the caller), sends it, and only invokes
     * @p on_done (which starts the print) after the send succeeds. On send error it
     * surfaces a user-visible failure, calls @p on_abort, and does NOT start the print.
     * A remap the backend refuses (check_preprint_remap()) ends the same way, with
     * nothing sent.
     * If there is nothing to send (empty tools or empty gcode), @p on_done runs immediately.
     *
     * Callbacks land on the WebSocket background thread; the body is deferred to
     * the main thread via the lifetime token.
     *
     * @param tools_used  Set of tool indices used by the file (from detail view or reprint).
     * @param remap       Tool-to-slot remap map (from detail view or reprint).
     * @param on_done     Called on success — proceeds with the actual print start.
     * @param on_abort    Called on error — caller is responsible for re-enabling UI state.
     */
    void reprint_impl(const std::string& filename, const std::string& path,
                      const std::set<int>& tools_used,
                      const std::optional<std::map<int, int>>& routing,
                      helix::SharedCallback<void()> on_started,
                      helix::SharedCallback<void()> on_error, int archive_plate);

    /// @p routed false sends the backend's unrouted step for @p tools_used
    /// (build_unrouted_preprint_gcode()) instead of a map.
    void send_backend_preprint_then(const std::set<int>& tools_used,
                                    const std::map<int, int>& remap,
                                    helix::SharedCallback<void()> on_done,
                                    helix::SharedCallback<void()> on_abort, bool routed = true);

    /// The "Print setup failed" modal for a pre-print send that did not
    /// happen: @p reason when one is known, else the printer-refusal text.
    static void report_preprint_failure(const std::string& reason);

    /// Ordered gate pipeline. Runs gates[index..], shows ONE modal at the
    /// first non-Pass verdict; proceed resumes at the next index with a
    /// freshly gathered context. All-Pass falls through to
    /// execute_print_start().
    void run_gates_from(size_t index);

    /// Snapshot every input a gate may read (single source of truth — gates
    /// never fetch singletons themselves). Gathered per pipeline entry so a
    /// resume re-reads live state, matching the old chain's fresh-fetch
    /// continuations.
    [[nodiscard]] helix::PrintStartContext gather_print_start_context() const;

    void on_gate_proceed();
    void on_gate_cancel();

    /// The detail view's prep manager when a view is set, else set_prep_manager()'s.
    [[nodiscard]] PrintPreparationManager* active_prep_manager() const;

    /// A start ended without a print: the button comes back and the owner hears.
    void end_without_print();

    /// The key this file's scan is kept under: set_scan_key()'s, else its path.
    [[nodiscard]] std::string file_scan_key() const;

    // === Dependencies ===
    PrinterState& printer_state_;
    IMoonrakerAPI* api_ = nullptr;
    PrintSelectDetailView* detail_view_ = nullptr;
    PrintPreparationManager* prep_manager_ = nullptr;
    std::optional<helix::FileStartFacts> file_facts_;
    lv_subject_t* can_print_subject_ = nullptr;

    // Guards background-thread API callbacks (Snapmaker U1 pre-print send) so a
    // dismissed controller doesn't UAF. From bg threads use lifetime_.token()
    // then tok.defer(...). See async_lifetime_guard.h.
    helix::AsyncLifetimeGuard lifetime_;

    // === File State ===
    std::string filename_;
    std::string path_;
    std::vector<std::string> filament_colors_;
    std::vector<std::string> exclude_picks_;
    std::string thumbnail_path_; ///< Pre-extracted thumbnail for USB/embedded files
    std::string scan_key_;
    helix::SharedCallback<void()> on_file_unchecked_;

    // === Gate Pipeline State ===
    std::vector<helix::PrintStartGate> gate_list_; ///< ctor-seeded; overridable in tests
    size_t gate_resume_index_ = 0;                 ///< gate the open modal belongs to
    helix::ui::WidgetRef print_gate_modal_;        ///< the ONE gate dialog handle

    // === Callbacks ===
    PrintStartedCallback on_print_started_;
    PrintCancelledCallback on_print_cancelled_;
    UpdatePrintButtonCallback update_print_button_;
    HideDetailViewCallback hide_detail_view_;
    ShowDetailViewCallback show_detail_view_;
    NavigateToPrintStatusCallback navigate_to_print_status_;

    // === Filament Remap State ===
    // The firmware mapping a remap displaced lives in
    // AmsState::pending_tool_map_restore(), shared by every controller: the
    // next print start keeps it, and the seed reads it as the printer's own.

    // The record epoch the klippy and retry observers were armed under. A print
    // start since then owns the record, and those observers stand down.
    uint64_t restore_epoch_ = 0;

    // Observer for print state changes (to restore mapping on print end)
    ObserverGuard print_state_observer_;

    // True once the restore observer has seen THIS print live (Preparing/
    // Printing/Paused). Registration can land while the wire still reports the
    // PREVIOUS job's terminal state, so terminal values are ignored until the
    // latch arms (prestonbrown/helixscreen#1386).
    bool print_active_since_remap_ = false;

    // Observer for klippy state, armed only while a restore is deferred waiting
    // for Klipper to come back (see observe_klippy_state_for_restore).
    ObserverGuard klippy_state_observer_;

    // Armed while a record the backend refused is retained; retries once the
    // backend would accept every refused entry (see observe_backend_for_retry).
    ObserverGuard backend_retry_observer_;

    // Armed only between sending a restore and the firmware confirming it.
    ObserverGuard ams_data_observer_;
    bool awaiting_restore_confirmation_ = false;
    uint64_t restore_generation_at_send_ = 0;

    // === Filament Remap Methods ===
    /// Snapshot current firmware mapping, send remap commands, return true if remaps were sent
    bool apply_filament_remaps();

    /**
     * @brief Whether to warn the user that their explicit remap can't be honored.
     *
     * Pure decision helper for apply_filament_remaps(). A backend that applies
     * the remap via its firmware-native pre-print path (Snapmaker U1) honors the
     * user's choice through build_preprint_gcode() even though it writes no
     * persistent table — so the "remap not supported" toast is a STALE false
     * alarm and must be suppressed. Only warn when the backend has no route at
     * all, or has one it cannot use yet.
     *
     * Takes the backend rather than a pair of extracted flags: the two-value form
     * let a caller pass a capability from one backend and a pre-send flag from
     * another, and it is how the question came to have four spellings.
     *
     * @param backend the active AMS backend
     * @return true if the unsupported-remap warning toast should be shown
     */
    [[nodiscard]] static bool should_warn_remap_unsupported(const AmsBackend& backend);

    /// Restore original firmware mapping (called on print end)
    void restore_filament_mapping();

    /// Set up observer for print state to auto-restore mapping
    void observe_lifecycle_for_restore();

    /**
     * @brief Wait for Klipper to become READY, then retry a deferred restore.
     *
     * restore_filament_mapping() refuses to spend the snapshot on a halted
     * Klipper because the backends cannot report the refusal (#1270). This is
     * what makes that deferral resolve on its own — without it the snapshot
     * would sit until the next app start, the only other thing that replays
     * pending_remap.json. Idempotent: a second deferral does not stack observers.
     */
    void observe_klippy_state_for_restore();

    /**
     * @brief Retry a restore the backend refused, once it would accept it.
     *
     * Watches AmsState's data revision and retries when every entry that still
     * differs passes AmsBackend::can_set_tool_mapping(), never while a job holds
     * the machine (prestonbrown/helixscreen#1684). Idempotent.
     */
    void observe_backend_for_retry();

    /// For a record a print has run under: an entry whose lane the backend
    /// still does not report takes the tool's live lane in @p current.
    void drop_unreported_entries(const AmsBackend& backend, const std::vector<int>& current);

    /// True while the pending record is still the one this controller's retry
    /// observers were armed for.
    [[nodiscard]] bool still_owns_restore() const;

    /// True when this controller has an observer armed for the pending record.
    [[nodiscard]] bool owns_restore_observers() const;

    /// True when the backend has reported its lanes and would send every saved
    /// entry that differs from its current mapping.
    [[nodiscard]] bool retained_restore_sendable() const;

    /**
     * @brief Clear the restore snapshot and the on-disk recovery record.
     *
     * The single place both the confirmed path and the can't-confirm fallback
     * converge on, so the snapshot and pending_remap.json can never be cleared
     * by one and not the other.
     */
    void finish_restore();

    /// Observe AmsState's data-revision tick while awaiting firmware confirmation.
    void observe_ams_data_for_confirmation();

    /**
     * @brief Re-check whether the firmware has confirmed the restore.
     *
     * Cheap and idempotent — the revision tick it hangs off is deliberately
     * coarse and fires for our own optimistic writes too.
     */
    void check_restore_confirmed();

    // === Crash Recovery Persistence ===
    /// Save the active printer's pending record so it survives app restart.
    static void persist_remap_state();

    /// Delete the active printer's persisted record; other printers' stay.
    static void clear_persisted_remap_state();

    friend void adopt_hand_tool_mapping(int backend_index, int tool, int slot);

  public:
    /// Check for and restore any pending remap from a previous crashed session.
    /// Call once after AMS backends are initialized.
    void recover_pending_remap();
};

/**
 * @brief The user mapped @p tool to @p slot by hand on backend @p backend_index.
 *
 * Outside a print that is the printer's own map now: a pending restore takes it
 * as the tool's entry, so no replay reverts it, and ends once nothing is left
 * to restore. During a print the edit re-routes that print only, and the
 * restore at its end still puts the printer's map back.
 */
void adopt_hand_tool_mapping(int backend_index, int tool, int slot);

} // namespace helix::ui
