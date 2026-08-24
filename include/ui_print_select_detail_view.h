// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_filament_mapping_card.h"
#include "ui_observer_guard.h"
#include "ui_pre_print_options_renderer.h"
#include "ui_print_preparation_manager.h"

#include "moonraker_types.h"
#include "overlay_base.h"
#include "preflight_validator.h"
#include "print_file_data.h" // For FileHistoryStatus
#include "subject_managed_panel.h"
#include "tools_used_cache.h"

#include <functional>
#include <lvgl.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

// Forward declarations
class IMoonrakerAPI;
namespace helix {
class PrinterState;
}

namespace helix::ui {

/**
 * @file ui_print_select_detail_view.h
 * @brief Detail view overlay manager for print selection panel
 *
 * Handles the file detail overlay that appears when a file is selected,
 * including:
 * - Creating and positioning the detail view widget
 * - Showing/hiding with nav system integration
 * - Delete confirmation modal management
 * - Filament type dropdown synchronization
 *
 * ## Usage:
 * @code
 * PrintSelectDetailView detail_view;
 * detail_view.create(parent_screen);
 * detail_view.set_prep_manager(prep_manager);
 * detail_view.set_on_delete_confirmed([this]() { delete_file(); });
 *
 * // When file selected:
 * detail_view.show(filename, current_path, filament_type);
 *
 * // When back button clicked:
 * detail_view.hide();
 * @endcode
 */

/**
 * @brief Callback when delete is confirmed
 */
using DeleteConfirmedCallback = std::function<void()>;

/**
 * @brief Detail view overlay manager
 *
 * Inherits from OverlayBase for lifecycle management (on_activate/on_deactivate).
 * The NavigationManager calls these hooks automatically when the overlay is
 * pushed/popped from the stack.
 */
class PrintSelectDetailView : public OverlayBase {
  public:
    PrintSelectDetailView() = default;
    ~PrintSelectDetailView() override;

    // Non-copyable, non-movable (owns LVGL widgets with external references)
    PrintSelectDetailView(const PrintSelectDetailView&) = delete;
    PrintSelectDetailView& operator=(const PrintSelectDetailView&) = delete;
    PrintSelectDetailView(PrintSelectDetailView&&) = delete;
    PrintSelectDetailView& operator=(PrintSelectDetailView&&) = delete;

    // === OverlayBase Interface ===

    /**
     * @brief Initialize subjects for pre-print option switches
     *
     * Creates and registers subjects that control switch default states.
     * Skip switches (bed_mesh, qgl, z_tilt, nozzle_clean) default to ON.
     * Add-on switches (timelapse) default to OFF.
     *
     * MUST be called BEFORE create() so bindings can find subjects.
     */
    void init_subjects() override;

    /**
     * @brief Create the detail view widget
     *
     * Creates the print_file_detail XML component and configures it.
     * Must be called before show().
     *
     * @param parent_screen Screen to create detail view on
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent_screen) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Print File Details"
     */
    const char* get_name() const override {
        return "Print File Details";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Resets pre-print subjects to defaults and starts async file scanning.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Closes any open modals, pauses gcode viewer.
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     *
     * Invalidates lifetime tokens so async callbacks bail out.
     * Unregisters from NavigationManager and deinitializes subjects.
     */
    void cleanup() override;

    // === Setup ===

    /**
     * @brief Set dependencies for print preparation
     *
     * @param api IMoonrakerAPI for file operations
     * @param printer_state PrinterState for capability detection
     */
    void set_dependencies(IMoonrakerAPI* api, PrinterState* printer_state);

    /**
     * @brief Set callback for delete confirmation
     */
    void set_on_delete_confirmed(DeleteConfirmedCallback callback) {
        on_delete_confirmed_ = std::move(callback);
    }

    /**
     * @brief Set callback fired when the color-requirements swatch card is tapped.
     *
     * Mirrors the AFC/CFS mapping-card UX: the panel wires this to open the
     * native (Snapmaker) remap modal. The click is only made active for backends
     * whose remap strategy is SnapmakerNative (see create()).
     */
    void set_on_remap_requested(std::function<void()> callback) {
        on_remap_requested_ = std::move(callback);
    }

    /**
     * @brief Handle a tap on the color-requirements swatch card.
     *
     * Gates on the active backend's remap strategy (SnapmakerNative only) and,
     * when applicable, fires on_remap_requested_. Public so the LVGL click
     * trampoline in create() can dispatch to it. No-op on non-Snapmaker backends.
     */
    void on_color_card_clicked();

    /**
     * @brief Set the visible subject for XML binding
     *
     * The subject should be initialized to 0 (hidden).
     */
    void set_visible_subject(lv_subject_t* subject) {
        visible_subject_ = subject;
    }

    // === Visibility ===

    /**
     * @brief Show the detail view overlay
     *
     * Pushes overlay via nav system and triggers G-code scanning.
     *
     * @param filename Selected filename (for G-code scanning)
     * @param current_path Current directory path
     * @param filament_type Filament type from metadata (for dropdown default)
     * @param filament_colors Optional tool colors for multi-color prints
     * @param filament_materials Optional per-tool materials from metadata
     * @param file_size_bytes File size from Moonraker metadata (for safety checks)
     * @param modified_timestamp File mtime (tools-used cache validation)
     * @param gcode_end_byte Offset where the G-code body ends (Moonraker
     *        metadata); 0 when unknown. Sizes the footer read.
     */
    void show(const std::string& filename, const std::string& current_path,
              const std::string& filament_type,
              const std::vector<std::string>& filament_colors = {},
              const std::vector<std::string>& filament_materials = {}, size_t file_size_bytes = 0,
              time_t modified_timestamp = 0, uint64_t gcode_end_byte = 0);

    /**
     * @brief Hide the detail view overlay
     *
     * Uses nav system to properly hide with backdrop management.
     */
    void hide();

    /// Toggle the preview between slicer-intended colors (true) and actual loaded
    /// AMS slot colors (false). Session-local; the panel's toggle callback calls
    /// this with the switch's checked state.
    void set_prefer_sliced_colors(bool prefer_sliced);

    /**
     * @brief Whether the current file's gcode has finished parsing.
     *
     * Set true in the gcode-viewer load callback AFTER the pre-flight
     * validator has run, so a true return guarantees preflight_result() is
     * fresh for the loaded file. The print-start gate waits on this to close
     * the race where Print is tapped before parse completes.
     */
    [[nodiscard]] bool is_gcode_loaded() const {
        return gcode_loaded_;
    }

    /**
     * @brief Whether pre-flight inputs are ready for the print-start gate.
     *
     * True once EITHER the visual gcode viewer has parsed (full platforms) OR
     * the headless tools_used scan has completed (runs on ALL platforms,
     * including 2D-only where the viewer skips parsing). On either path
     * recompute_preflight() has run, so preflight_result() is fresh.
     *
     * This is the signal the print-start gate must wait on — NOT is_gcode_loaded(),
     * which never becomes true on 2D-only platforms and would hang the print.
     */
    [[nodiscard]] bool is_preflight_ready() const {
        return gcode_loaded_ || headless_scan_done_;
    }

    /**
     * @brief Run @p cb once the gcode parse + pre-flight validation complete.
     *
     * If the gcode is already loaded, @p cb is invoked synchronously (main
     * thread). Otherwise it is stored and fired exactly once from the load
     * callback after preflight_result() is populated. Only one pending
     * callback is tracked; a later call overwrites an earlier pending one.
     * Cleared on show() so a stale attempt from a previous file can't fire.
     */
    void run_when_loaded(std::function<void()> cb);

    /**
     * @brief Run @p cb once pre-flight inputs are ready (is_preflight_ready()).
     *
     * Unlike run_when_loaded() this fires when EITHER the viewer parse OR the
     * headless tools_used scan completes — so it never hangs on 2D-only
     * platforms. If already ready, runs @p cb synchronously. A safety timeout
     * (see PREFLIGHT_READY_TIMEOUT_MS) fires @p cb anyway if neither signal
     * arrives, so a stuck/failed scan can never wedge the print: the print
     * proceeds without Part A's optimization rather than never starting.
     */
    void run_when_preflight_ready(std::function<void()> cb);

    // Note: is_visible() inherited from OverlayBase

    // === Delete Confirmation ===

    /**
     * @brief Show delete confirmation dialog
     *
     * @param filename Filename to display in confirmation message
     */
    void show_delete_confirmation(const std::string& filename);

    /**
     * @brief Hide delete confirmation dialog
     */
    void hide_delete_confirmation();

    // === Widget Access ===

    /**
     * @brief Get the detail view widget
     * @note Returns overlay_root_ from OverlayBase
     */
    [[nodiscard]] lv_obj_t* get_widget() const {
        return overlay_root_;
    }

    /**
     * @brief Get the print button (for enable/disable state)
     */
    [[nodiscard]] lv_obj_t* get_print_button() const {
        return print_button_;
    }

    /**
     * @brief Get the print preparation manager
     */
    [[nodiscard]] PrintPreparationManager* get_prep_manager() const {
        return prep_manager_.get();
    }

    /**
     * @brief Get current filament mappings from the mapping card
     */
    [[nodiscard]] std::vector<helix::ToolMapping> get_filament_mappings() const {
        return filament_mapping_card_.get_mappings();
    }

    /**
     * @brief Get per-tool gcode info from the mapping card
     */
    [[nodiscard]] std::vector<helix::GcodeToolInfo> get_filament_tool_info() const {
        return filament_mapping_card_.get_tool_info();
    }

    /**
     * @brief Per-tool gcode info for the tools THIS file actually uses.
     *
     * Single source of truth for "the per-tool color/material of the used
     * tools". Builds the full slicer palette via
     * FilamentMappingCard::build_tool_info(current_filament_colors_,
     * current_filament_materials_) — the same Moonraker-metadata data the color
     * swatches use, populated on ALL platforms — then keeps only the entries
     * whose tool_index is in tools_used_effective() (original tool_index
     * preserved). Decouples preflight/remap from the mapping card INSTANCE's
     * tool_info_, which is empty on the U1/headless path.
     */
    [[nodiscard]] std::vector<helix::GcodeToolInfo> get_used_tool_info() const;

    /**
     * @brief The mapping the print will actually use — display + gate source.
     *
     * Editable backends (AFC / Happy Hare / CFS / AD5X-IFS / toolchanger): the
     * card seeds and owns mappings_, and user edits win, so this returns
     * get_mappings() unchanged. Non-editable backends (Snapmaker U1 / ACE): the
     * card is hidden and get_mappings() is empty, so we compute the effective
     * mapping via FilamentMapper::effective_mappings() using
     * effective_auto_match(). This is the single helper both the color swatches
     * and the pre-flight gate consult so they resolve identically.
     */
    [[nodiscard]] std::vector<helix::ToolMapping> effective_mappings() const;

    /**
     * @brief Whether auto (color+type) matching applies for this backend.
     *
     * Non-editable-card backends (U1 / ACE) have no UI to flip the persisted
     * auto-color preference, so they always auto-match. Editable backends honor
     * SettingsManager::get_auto_color_map().
     */
    [[nodiscard]] bool effective_auto_match() const;

    /**
     * @brief Logical tools the parsed gcode body actually uses.
     *
     * Returns ParsedGCodeFile::tools_used_indices from the gcode viewer's
     * parsed file (empty if no file is parsed). Read the same way
     * recompute_preflight() does. Consumed by the print-start gate to build
     * the Snapmaker U1 native print_task_config command sequence.
     */
    [[nodiscard]] std::set<int> get_tools_used() const;

    /**
     * @brief Effective tool→slot remap the print will actually use.
     *
     * Built from the mapping card's current mappings. Contains ONLY real
     * remaps: an entry tool_index → mapped_slot is included only when
     * mapped_slot >= 0 AND mapped_slot != default_head(tool_index), where
     * default_head(t) = (t in [0,3]) ? t : 0. Tools mapped to their identity
     * head are omitted (the firmware default already routes them).
     *
     * On Snapmaker U1 today the mapping card is hidden so get_mappings() is
     * empty and this returns empty (identity / Part A). It becomes non-empty
     * once the U1 remap modal lands (Batch 2) — kept forward-compatible.
     */
    [[nodiscard]] std::map<int, int> get_effective_remap() const;

    /**
     * @brief Push a chosen tool→slot mapping into the card's store.
     *
     * Used by the U1 native-remap flow: the inline card widget is hidden, but
     * its mappings_ store still feeds get_effective_remap() and
     * recompute_preflight(). Storing here makes the chosen map flow to BOTH the
     * pre-flight gate and the print-start SET_PRINT_EXTRUDER_MAP send without a
     * visible card. Fires the card's on_mappings_changed_ → recompute_preflight().
     */
    void set_filament_mappings(std::vector<helix::ToolMapping> mappings);

    /**
     * @brief Get per-tool filament materials from gcode metadata
     *
     * Available even when AMS is not present (unlike get_filament_tool_info
     * which relies on the mapping card).
     */
    [[nodiscard]] const std::vector<std::string>& get_filament_materials() const {
        return current_filament_materials_;
    }

    /**
     * @brief Get available AMS slots from the mapping card
     */
    [[nodiscard]] const std::vector<helix::AvailableSlot>& get_available_slots() const {
        return filament_mapping_card_.get_available_slots();
    }

    /**
     * @brief Get the cached pre-flight validation result for the current file.
     *
     * Computed in try_extract_gcode_colors() after the gcode is parsed, for
     * ALL AMS backends (including those whose mapping card is hidden, e.g.
     * Snapmaker U1 / ACE). Drives the filament_mismatch_ and
     * empty_tools_warning_ subjects, and is read by the print-start gate and
     * the enriched pre-print modal. Empty (no checks) until a file is parsed.
     */
    [[nodiscard]] const helix::PreflightResult& preflight_result() const {
        return preflight_result_;
    }

    /**
     * @brief Re-run the backend-agnostic pre-flight validator and republish.
     *
     * Rebuilds the per-tool intent (filtered to the precise tools_used set from
     * the parsed gcode), re-reads available slots from AmsState, computes the
     * default mapping, runs PreflightValidator::validate, caches the result into
     * preflight_result_, and republishes the filament_mismatch_ /
     * empty_tools_warning_ subjects.
     *
     * Called once after parse (from try_extract_gcode_colors()) and again after
     * a native tool→slot remap so the print-start gate reflects the new mapping.
     * No-op until the gcode is loaded (gcode_viewer_ has a parsed file).
     */
    void recompute_preflight();

    /**
     * @brief Open the filament mapping modal (tool→slot reassignment).
     *
     * Thin passthrough to the embedded FilamentMappingCard's modal. Used by the
     * pre-flight gate's "Remap…" button for native-routing backends (AFC, Happy
     * Hare, CFS, AD5X-IFS, toolchanger). Applied mappings flow through the card's
     * existing on_mappings_changed path, which recomputes pre-flight.
     */
    void open_filament_mapping_modal();

    /**
     * @brief Get cached file metadata from the most recent async fetch
     *
     * Populated after the metadata fetch completes. Returns nullopt if the
     * user opened the detail view before the fetch finished.
     */
    [[nodiscard]] std::optional<FileMetadata> get_file_metadata() const {
        return cached_file_metadata_;
    }

    // === Checkbox Access (for prep manager setup) ===

    [[nodiscard]] lv_obj_t* get_bed_mesh_checkbox() const {
        return bed_mesh_checkbox_;
    }
    [[nodiscard]] lv_obj_t* get_qgl_checkbox() const {
        return qgl_checkbox_;
    }
    [[nodiscard]] lv_obj_t* get_z_tilt_checkbox() const {
        return z_tilt_checkbox_;
    }
    [[nodiscard]] lv_obj_t* get_nozzle_clean_checkbox() const {
        return nozzle_clean_checkbox_;
    }
    [[nodiscard]] lv_obj_t* get_timelapse_checkbox() const {
        return timelapse_checkbox_;
    }

    // === Subject Access ===

    [[nodiscard]] lv_subject_t* get_prep_time_estimate_subject() {
        return &prep_time_estimate_subject_;
    }

    // === Resize Handling ===

    /**
     * @brief Handle resize event - update responsive padding
     *
     * @param parent_screen Parent screen for height calculation
     */
    void handle_resize(lv_obj_t* parent_screen);

    /**
     * @brief Update the print history status display
     *
     * @param status The history status (NEVER_PRINTED, CURRENTLY_PRINTING, COMPLETED, FAILED)
     * @param success_count Number of successful prints (used when status is COMPLETED)
     */
    void update_history_status(FileHistoryStatus status, int success_count);

  protected:
    /**
     * @brief Called after widget tree is destroyed by destroy_overlay_ui()
     *
     * Nulls all child widget pointers so that create() works correctly
     * when re-invoked on next open. Also invalidates lifetime tokens
     * so in-flight async callbacks bail out (they may reference stale
     * widget pointers like gcode_viewer_).
     */
    void on_ui_destroyed() override;

  private:
    // === Dependencies ===
    IMoonrakerAPI* api_ = nullptr;
    PrinterState* printer_state_ = nullptr;
    lv_subject_t* visible_subject_ = nullptr;

    // === Widget References ===
    // Note: overlay_root_ inherited from OverlayBase holds the main widget
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* confirmation_dialog_widget_ = nullptr;
    lv_obj_t* print_button_ = nullptr;
    lv_obj_t* gcode_viewer_ = nullptr;

    // Pre-print option checkboxes
    lv_obj_t* bed_mesh_checkbox_ = nullptr;
    lv_obj_t* qgl_checkbox_ = nullptr;
    lv_obj_t* z_tilt_checkbox_ = nullptr;
    lv_obj_t* nozzle_clean_checkbox_ = nullptr;
    lv_obj_t* purge_line_checkbox_ = nullptr;
    lv_obj_t* timelapse_checkbox_ = nullptr;

    // Color swatches container (parent card visibility driven by the
    // color_swatches_visible subject — bound in print_file_detail.xml).
    lv_obj_t* color_swatches_row_ = nullptr;
    // The legacy color-requirements card (parent of color_swatches_row_). Made
    // tappable on Snapmaker (SnapmakerNative remap strategy) to open the remap
    // modal, mirroring the AFC/CFS FilamentMappingCard whole-card click.
    lv_obj_t* color_requirements_card_ = nullptr;

    // History status display
    lv_obj_t* history_status_row_ = nullptr;
    lv_obj_t* history_status_icon_ = nullptr;
    lv_obj_t* history_status_label_ = nullptr;

    // G-code viewer visibility mode (0=thumbnail, 1=3D, 2=2D)
    lv_subject_t detail_gcode_viewer_mode_{};
    // G-code loading indicator (0=hidden, 1=visible)
    lv_subject_t detail_gcode_loading_{};
    // 1 = the gcode viewer has rendered its first real frame. The thumbnail
    // (which sits on top of the viewer in z-order) stays visible until this
    // flips, covering the gray viewer during the load-to-render gap.
    lv_subject_t detail_viewer_first_frame_{};
    // 1 = show slicer-intended colors instead of loaded AMS slot colors.
    // View-local, resets to 0 (actual) on every show().
    lv_subject_t detail_prefer_sliced_colors_{};
    // 0 = mapping/swatch chips not authoritative yet (XML shows skeletons),
    // 1 = authoritative chip state rendered. Mirrors is_preflight_ready() —
    // the same readiness the print-start gate waits on. Flips 0→1 at show()
    // on a tools-used cache hit, else when the headless scan or the viewer
    // parse completes; back to 0 in on_deactivate(). Published ONLY via
    // publish_mapping_ready().
    lv_subject_t detail_mapping_ready_{};
    std::string temp_gcode_path_; // Cached downloaded gcode file path
    bool gcode_loaded_ = false;   // Whether gcode file has been loaded into viewer
    // Pending print-attempt (or other) callback registered via run_when_loaded()
    // while a parse was still in flight. Fired once from the load callback after
    // preflight_result_ is fresh, then cleared. Reset on show().
    std::function<void()> on_loaded_cb_;

    // --- Shared gcode download (ONE file + ONE download per open) ---
    // The headless tools scan and the viewer preview share a single canonical
    // file (canonical_gcode_path()) and a single in-flight transfer;
    // concurrent callers queue in gcode_download_waiters_ and are fanned out
    // (main thread) when the transfer resolves.
    bool gcode_download_in_flight_ = false;
    std::vector<std::function<void(bool, std::string)>> gcode_download_waiters_;

    // Absolute path of Moonraker's `gcodes` root, but ONLY when Moonraker runs
    // on this machine — "" whenever the file must come over HTTP. Resolved once
    // per view (server.files.roots), because on the printer itself the file we
    // were copying was already on local disk: a 13 MB G-code took 26 s to stream
    // flash -> loopback -> flash before anything could be parsed out of it.
    std::string local_gcodes_root_;
    // Set once the resolve has been ATTEMPTED, so a printer with no
    // server.files.roots (older forks) is asked once, not on every open.
    bool local_gcodes_root_resolved_ = false;

    // --- Headless tools_used scan (works on ALL platforms incl. 2D-only) ---
    // The visual gcode viewer does NOT parse on 2D-only platforms (Snapmaker U1,
    // AD5M, small screens), so tools_used and the pre-flight "ready" signal must
    // come from a separate, memory-safe streaming scan. Kicked off in
    // on_activate(); completion sets headless_scan_done_ and runs
    // recompute_preflight() so the gate has a fresh result even when the viewer
    // never parsed. See kick_off_headless_tools_scan().
    std::optional<std::set<int>> headless_tools_used_; // result of the streaming scan
    bool headless_scan_done_ = false;                  // scan finished (success/empty/fail/timeout)
    // True ONLY where the scan actually SETTLED: finish_scan's deferred body,
    // the show() cache-hit seed, or the no-API early-out. Deliberately NOT
    // set by the preflight safety timeout, which flips headless_scan_done_
    // while a download/scan may still be in flight. Gates oversize-reject
    // removal of the canonical gcode file — removing it under a still-running
    // scan would make the scanner see "no file" ≡ "no tools" and persist an
    // authoritative-empty tools set (the Finding-1 poison).
    bool headless_scan_settled_ = false;
    // Pending callback registered via run_when_preflight_ready() while neither the
    // viewer parse nor the headless scan had completed. Fired once when readiness
    // arrives (or on the safety timeout), then cleared. Reset on show().
    std::function<void()> on_preflight_ready_cb_;
    lv_timer_t* preflight_ready_timeout_timer_ = nullptr; // safety fallback; never wedge a print

    // Safety fallback: if neither the viewer parse nor the headless scan reports
    // readiness within this window, fire the deferred print attempt anyway
    // (graceful degradation — print without Part A's optimization, never hang).
    static constexpr uint32_t PREFLIGHT_READY_TIMEOUT_MS = 8000;

    // Pre-print option toggle state lives in `option_rows_renderer_` (one
    // heap-allocated subject per option) — the legacy fixed six subjects
    // (preprint_bed_mesh_, preprint_qgl_, etc.) were retired in Phase 3.5.
    lv_subject_t filament_mismatch_{};        // 1 = material mismatch warning visible
    lv_subject_t filament_mapping_visible_{}; // 1 = filament mapping card visible (AMS+tools)
    lv_subject_t color_swatches_visible_{};   // 1 = legacy color swatches card visible
    lv_subject_t empty_tools_warning_{};      // 1 = at least one used tool's slot is empty
    // Cached backend-agnostic pre-flight validation result for the current file.
    // Computed in try_extract_gcode_colors() once the gcode is parsed; the single
    // source of truth driving filament_mismatch_ + empty_tools_warning_ (works
    // even when the mapping card is hidden, e.g. Snapmaker U1 / ACE). Read by the
    // print-start gate and the enriched pre-print modal. Empty until a file parses.
    helix::PreflightResult preflight_result_{};
    lv_subject_t prep_time_estimate_subject_{}; // formatted prep time string for bind_text
    char prep_time_estimate_buf_[64]{};         // buffer backing the string subject
    SubjectManager subjects_;                   // RAII manager for subject cleanup
    // Note: subjects_initialized_ inherited from OverlayBase

    // Live re-color when AMS slot colors/presence change while this view is open.
    // Observes the STATIC AmsState::slots_version subject — no SubjectLifetime
    // token (singleton, not a per-slot dynamic subject).
    ObserverGuard slots_version_observer_;

    // Print preparation manager (owns it)
    std::unique_ptr<PrintPreparationManager> prep_manager_;

    // Filament mapping card (replaces color swatches when AMS available)
    FilamentMappingCard filament_mapping_card_;

    // Dynamically-built option toggle rows for the active printer's
    // PrePrintOptionSet. Populated in on_activate() and rebuilt when the
    // printer type changes. Owns per-option state subjects.
    PrePrintOptionsRenderer option_rows_renderer_;
    lv_obj_t* pre_print_options_container_ = nullptr;
    std::string last_rendered_printer_type_;

    // === Cached show() parameters (used by on_activate) ===
    std::string current_filename_;
    std::string current_path_;
    std::string current_filament_type_;
    std::vector<std::string> current_filament_colors_;
    std::vector<std::string> current_filament_materials_;
    size_t current_file_size_bytes_ = 0;
    time_t current_file_modified_ = 0; // mtime of the shown file (cache key part)
    // Offset where the G-code body ends (Moonraker metadata). Everything after
    // it is the slicer's footer, so it sizes the footer read exactly. 0 = the
    // server didn't report it; the tail read falls back to a fixed window.
    uint64_t current_gcode_end_byte_ = 0;

    // Persistent tools-used cache (path/size/mtime keyed, JSON in the helix
    // cache dir). Seeded at show() so re-prints render the final chip state
    // in one paint; written through when a scan/parse makes the set final.
    // Main-thread use only (documented on helix::ToolsUsedCache).
    helix::ToolsUsedCache tools_used_cache_;

    // Cached metadata from the async fetch (nullopt until fetch completes or when file changes)
    std::optional<FileMetadata> cached_file_metadata_;

    // === Callbacks ===
    DeleteConfirmedCallback on_delete_confirmed_;

    // Fired when the user taps the (Snapmaker) color-requirements swatch card.
    // Set by PrintSelectPanel to open the native remap modal — the same entry
    // point the preflight-check modal's "Remap…" button uses. Empty on backends
    // where the swatch card is purely informational.
    std::function<void()> on_remap_requested_;

    // === Internal Methods ===

    /**
     * @brief Load gcode file for 3D/2D preview
     *
     * Downloads gcode via Moonraker API and loads into gcode_viewer widget.
     * Shows all layers with no ghost (progress = -1). Falls back to thumbnail
     * on failure, disabled config, or oversized files.
     */
    void load_gcode_for_preview();

    /**
     * @brief Canonical shared download path for the current file's gcode
     *
     * `<cache>/gcode_temp/detail_<hash(full relative path)>.gcode` — hashed on
     * the FULL path (dir + filename), so same-name files in different
     * directories never collide. The headless tools scan and the viewer
     * preview load from this ONE file.
     */
    [[nodiscard]] std::string canonical_gcode_path() const;

    /**
     * @brief Moonraker-relative gcode path of the shown file
     *
     * `current_path_ + "/" + current_filename_` (or bare filename at the root).
     * The tools-used cache key and every server-side file request use this
     * same expression — one helper, no forked twins.
     */
    /**
     * @brief Delete a G-code file this view downloaded; refuse anything else.
     *
     * Every reclaim in the view funnels here, so "is this ours to delete?" has
     * exactly one home (is_reclaimable_download(), gcode_temp_reclaim.h). The
     * seven call sites were each safe only while every path they held came from
     * our own cache; a same-host open can hand them Moonraker's real print file.
     */
    /**
     * @brief Absolute path of this file on local disk, or "" if unreachable.
     *
     * Non-empty only when Moonraker runs on this machine AND the file is there
     * at the size the metadata promised. The returned path is Moonraker's own
     * print file — it is deliberately never adopted into temp_gcode_path_, and
     * reclaim_download() refuses it on top of that.
     */
    [[nodiscard]] std::string local_gcode_source() const;

    /**
     * @brief Kick the one-time resolve of Moonraker's gcodes root.
     *
     * Fire-and-forget by design. Nothing awaits it: server.files.roots is absent
     * on older forks and unanswered by our mock client, and a load that waited
     * for a reply that never comes would hang instead of falling back to HTTP.
     * Until it lands, local_gcode_source() simply answers "".
     */
    void resolve_local_gcodes_root();

    void reclaim_download(const std::string& path);

    [[nodiscard]] std::string current_file_key() const;

    /**
     * @brief Invoke @p cb (main thread) once the current file's gcode is on disk
     *
     * Single shared download: the in-flight transfer is joined (waiters) before
     * the disk probe — the mid-write file must not be mistaken for a complete
     * copy. A disk hit fires cb synchronously; otherwise the first caller
     * starts ONE transfer and concurrent callers are fanned out when it
     * resolves (main thread). ok=false on download error / no API / no cache
     * dir.
     */
    void ensure_gcode_downloaded(std::function<void(bool ok, const std::string& path)> cb);

    /**
     * @brief Point the viewer at @p path and start loading it
     *
     * Installs the (single) load callback and kicks off the parse. The
     * callback body — progress reset, preview colors, color extraction,
     * preflight readiness, reveal — was identical in the former cached-file
     * and post-download paths; it lives here once now.
     */
    void begin_viewer_load(const std::string& path);

    /**
     * @brief Apply a finished headless tools-scan result (callable from any thread)
     *
     * Marshals via tok.defer — `this` is only touched inside the deferred
     * body (main thread), where the result is stored, the color swatches are
     * rendered (2D-only platforms), pre-flight is recomputed, the mapping
     * card is restricted, and any deferred preflight-ready attempt is
     * released. When @p authoritative is true (the scan actually read the
     * downloaded file) the result is also written through to the tools-used
     * cache — including a legitimate empty set (single-extruder file). A
     * degraded finish (download failed, authoritative=false) must NOT
     * persist: its empty set carries no information and, on 2D-only
     * platforms where no viewer parse ever repairs it, would freeze "no
     * tools" for the file permanently.
     */
    void finish_scan(LifetimeToken tok, std::set<int> tools, bool authoritative);

    /**
     * @brief Main-thread body of finish_scan() (see there for the contract).
     *
     * Split out so the footer-read path can adopt the file's palette and
     * publish the used-tool set in ONE deferred tick, instead of chaining a
     * second defer behind the colour update.
     */
    void apply_scan_result(std::set<int> tools, bool authoritative);

    /**
     * @brief Fast path: answer tools_used (and colors) from the file's footer.
     *
     * Slicers write `filament used [g]` and `filament_colour` after the last
     * move, so one HTTP suffix range over the last few tens of KB answers both
     * — no whole-file download, no geometry parse (~16s for an 870 KB file on
     * a K2). Sized from gcode_end_byte when Moonraker reported it.
     *
     * Parsing happens on the HTTP thread (pure, no `this`); the result is
     * marshaled by apply_scan_result() via tok.defer. ANY failure — transport
     * error, a slicer that writes neither key, an all-zero usage vector —
     * falls through to start_full_tools_scan(), so the footer read can only
     * ever be faster than today, never worse.
     */
    void start_tail_summary_scan(LifetimeToken tok, std::set<int> stop_set);

    /**
     * @brief Fallback: download the whole file and scan it for Tn commands.
     *
     * The pre-footer-read behavior, unchanged. Shares the ONE canonical
     * download with the viewer preview (ensure_gcode_downloaded).
     */
    void start_full_tools_scan(LifetimeToken tok, std::set<int> stop_set);

    /**
     * @brief Show or hide the gcode viewer
     *
     * Sets detail_gcode_viewer_mode_ subject to control XML visibility bindings.
     * @param show true to show viewer, false to revert to thumbnail
     */
    void show_gcode_viewer(bool show);

    // Resolve the used tools' colors via the shared FilamentMapper color engine
    // (effective_tool_colors) and push them to the viewer. The sliced/actual
    // toggle selects the mappings fed in: effective_mappings() for actual (loaded,
    // lane-matched) vs default/unmapped for sliced (the file's own palette).
    void apply_preview_colors();

    void on_ams_state_changed();                // AMS slots_version observer handler
    void refresh_preview_colors_and_mismatch(); // shared by card-edit + AMS observer

    /**
     * @brief Extract filament colors from parsed gcode when metadata lacks them
     *
     * Called after gcode viewer finishes loading. If current_filament_colors_ is empty,
     * extracts tool_color_palette from the parsed file and updates the mapping card.
     * Fixes Snapmaker (and other printers whose Moonraker doesn't return filament_colors).
     */
    void try_extract_gcode_colors(lv_obj_t* viewer);

    /**
     * @brief Fire and clear any pending run_when_loaded() callback.
     *
     * Invoked from the gcode load callback after try_extract_gcode_colors()
     * has refreshed preflight_result_. Moves the callback out and clears the
     * member before invoking it, so it runs exactly once and a re-entrant
     * run_when_loaded() during the callback registers cleanly for next time.
     */
    void fire_on_loaded();

    /**
     * @brief Start the headless tools_used scan for the current file.
     *
     * Tries the footer read first (start_tail_summary_scan) — a small suffix
     * range that usually answers outright. Only when that cannot answer does
     * it fall back to start_full_tools_scan(), which shares the ONE canonical
     * download with the viewer preview
     * (ensure_gcode_downloaded); once the file is on disk, runs a lightweight
     * Tn-only line scan on the slow HTTP lane (off the main thread,
     * memory-safe — never holds the whole file), early-exiting once every
     * slicer-palette tool has been seen. finish_scan() then marshals the
     * result back to the main thread: sets headless_tools_used_ +
     * headless_scan_done_ and runs recompute_preflight() +
     * fire_on_preflight_ready(). Runs on ALL platforms; it is the readiness
     * signal the gate uses on 2D-only (where the viewer never parses).
     * Degrades gracefully: a download/scan failure still marks the scan done
     * (with an empty set) so the print can proceed.
     */
    void kick_off_headless_tools_scan();

    /**
     * @brief Mark readiness arrived and fire/clear any pending preflight-ready cb.
     *
     * Invoked from the viewer load callback, the headless scan completion, or the
     * safety timeout. Cancels the timeout timer and runs the deferred attempt
     * exactly once.
     */
    void fire_on_preflight_ready();

    /**
     * @brief Publish is_preflight_ready() onto the detail_mapping_ready subject.
     *
     * Called from every site that flips readiness (show() reset + cache seed,
     * scan finish, viewer load callback, on_deactivate). The XML skeleton
     * bindings inside the mapping/swatch cards ride this subject — it must
     * always equal is_preflight_ready() ? 1 : 0.
     */
    void publish_mapping_ready();

    /**
     * @brief Tools the print will use, sourced from whichever scan is available.
     *
     * Prefers the visual viewer's parsed tools_used_indices (full platforms);
     * falls back to the headless scan result (2D-only). Empty if neither is
     * available yet.
     */
    [[nodiscard]] std::set<int> tools_used_effective() const;

    /**
     * @brief Populate the dynamic per-printer option-toggle rows.
     *
     * Reads the active printer's `PrePrintOptionSet` from PrinterState and
     * regenerates the rows inside `pre_print_options_container_`. Idempotent
     * — safe to call repeatedly; only rebuilds when the printer type changes.
     * Wires the resulting state subjects through to the prep manager via
     * `set_option_state_provider()`, and binds a single value-changed
     * callback that updates the prep-time estimate.
     */
    void populate_option_rows();

    /**
     * @brief Static callback for delete confirmation
     */
    static void on_confirm_delete_static(lv_event_t* e);

    /**
     * @brief Static callback for cancel delete
     */
    static void on_cancel_delete_static(lv_event_t* e);

    /**
     * @brief Update color swatches display.
     *
     * Renders one swatch per entry in `tool_indices`, sourcing each swatch's
     * color from the live AMS backend slot when available (slot index = tool
     * index), falling back to `palette_colors[tool]` when no backend exists.
     * Also publishes `empty_tools_warning_` (1 if any used tool's slot is
     * empty, 0 otherwise).
     *
     * @param tool_indices Tool indices to render (from
     *                     ParsedGCodeFile::tools_used_indices)
     * @param palette_colors Slicer-provided palette (fallback when no AMS
     *                       backend; indexed by tool)
     */
    void update_color_swatches(const std::set<int>& tool_indices,
                               const std::vector<std::string>& palette_colors);

    /**
     * @brief Whether the FILAMENTS card should be visible for `tool_count` tools.
     *
     * Multi-tool printers (toolchanger / multi-extruder / multi-slot AMS) show
     * the card whenever at least one tool is referenced — lane identity matters
     * even for single-tool prints. Single-extruder printers only show it for
     * 2+ tools (manual-swap multi-color files). Caller is responsible for
     * AND-ing with `!filament_mapping_card_.should_show()`.
     */
    [[nodiscard]] bool swatches_card_visible_for(size_t tool_count) const;

    /**
     * @brief Render the authoritative chip state for a known used-tool set.
     *
     * The single implementation of the "we now know which tools this file
     * really uses" render, shared by the three sites that learn that set:
     * show()'s tools-used cache seed, the viewer parse
     * (try_extract_gcode_colors) and the headless scan (finish_scan). Decides
     * swatch-card visibility from @p tools_used (never the slicer palette
     * size, which over-counts), renders the swatches, re-runs the pre-flight
     * gate, and restricts the mapping card to @p tools_used.
     *
     * @param tools_used              The precise tool indices the file uses,
     *                                normally tools_used_effective().
     * @param refresh_card_from_palette Also rebuild the mapping card's display
     *                                state from the full palette. Only the
     *                                headless path needs this — see
     *                                finish_scan().
     */
    void render_authoritative_chips(const std::set<int>& tools_used,
                                    bool refresh_card_from_palette = false);
};

} // namespace helix::ui
