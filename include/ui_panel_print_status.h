// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_exclude_object_map_view.h"
#include "ui_exclude_object_side_list.h"
#include "ui_filament_runout_handler.h"
#include "ui_heater_icon_binder.h"
#include "ui_modal.h"
#include "ui_observer_guard.h"
#include "ui_print_exclude_object_manager.h"
#include "ui_print_light_timelapse.h"
#include "ui_print_tune_overlay.h"
#include "ui_save_z_offset_modal.h"

#include "overlay_base.h"
#include "print_control_buttons.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "subject_managed_panel.h"
#include "ui/temperature_observer_bundle.h"

// Forward declaration
class IMoonrakerAPI;
namespace helix {
class TempGraphController;
}

#include "filament_mapper.h" // helix::GcodeToolInfo

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Forward declarations
class TemperatureService;
class PrintStatusPanel;
struct FileMetadata;

/**
 * @brief Print status panel - shows active print progress and controls
 *
 * Displays filename, thumbnail, progress, layers, times, temperatures,
 * speed/flow, and provides pause/tune/cancel buttons.
 */

// PrintState enum is now in print_lifecycle_state.h

class PrintStatusPanel : public OverlayBase {
  public:
    /**
     * @brief Construct PrintStatusPanel with injected dependencies
     *
     * @param printer_state Reference to helix::PrinterState
     * @param api Pointer to IMoonrakerAPI (for pause/cancel commands)
     */
    PrintStatusPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    ~PrintStatusPanel() override;

    //
    // === OverlayBase Implementation ===
    //

    /**
     * @brief Initialize subjects for XML binding
     *
     * Registers all 10 subjects for reactive data binding.
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Calls lv_subject_deinit() on all local lv_subject_t members.
     */
    void deinit_subjects();

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Print Status"
     */
    const char* get_name() const override {
        return "Print Status";
    }

    /// The longest-dwell screen in the app — users watch it for hours. A
    /// "you will be leaving shortly" gap against the nav dock is wrong here, so
    /// it renders full width and its drill-downs (fan control, temp graph,
    /// exclude object, gcode viewer) inherit that. #1178
    [[nodiscard]] bool is_destination() const override {
        return true;
    }

    /**
     * @brief Called when panel becomes visible
     *
     * Resumes G-code viewer rendering if viewer mode is active.
     */
    void on_activate() override;

    /**
     * @brief Called when panel is hidden
     *
     * Pauses G-code viewer rendering to save CPU cycles.
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    /**
     * @brief Push the print status overlay with lazy creation and destroy-on-close
     *
     * All call sites should use this instead of manually pushing the overlay.
     * Handles lazy creation, NavigationManager registration, and destroy-on-close
     * callback registration. The widget tree is destroyed when the overlay closes
     * to free memory (~400-800KB); subjects survive for re-creation.
     *
     * @param parent_screen Parent screen for overlay creation
     * @return true if overlay was pushed successfully
     */
    static bool push_overlay(lv_obj_t* parent_screen);

    /**
     * @brief Get the cached overlay widget pointer, if created.
     *
     * Exposes `s_cached_panel` to callers that need to check nav-stack
     * membership before triggering auto-navigation (e.g. the print-start
     * observer skipping push when the user is already viewing print status).
     * Returns nullptr if the widget tree hasn't been created yet or was
     * destroyed via destroy-on-close.
     *
     * @return cached overlay root, or nullptr
     */
    static lv_obj_t* get_cached_overlay();

  protected:
    /**
     * @brief Called after destroy_overlay_ui() deletes the widget tree
     *
     * Nulls all widget pointers, resets widget-dependent state (exclude manager,
     * resize registration), and cancels any in-flight animations. Does NOT
     * destroy subjects or observers on live PrinterState subjects.
     */
    void on_ui_destroyed() override;

  public:
    //
    // === Legacy Compatibility ===
    //

    /**
     * @brief Get XML component name for lv_xml_create()
     * @return "print_status_panel"
     */
    const char* get_xml_component_name() const {
        return "print_status_panel";
    }

    /**
     * @brief Get root panel object (alias for get_root())
     * @return Panel object, or nullptr if not yet created
     */
    lv_obj_t* get_panel() const {
        return overlay_root_;
    }

    /**
     * @brief Update IMoonrakerAPI pointer
     * @param api New API pointer (may be nullptr)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
        if (exclude_manager_) {
            exclude_manager_->set_api(api);
        }
        if (runout_handler_) {
            runout_handler_->set_api(api);
        }
    }

    //
    // === Public API - Print State Updates ===
    //

    /**
     * @brief Set the current print filename
     * @param filename Print file name to display
     */
    void set_filename(const char* filename);

    /**
     * @brief Set print state
     * @param state New print state
     */

    /**
     * @brief Get current print state
     * @return Current PrintState
     */
    PrintState get_state() const {
        return lifecycle_.state();
    }

    //
    // === Pre-Print Preparation State ===
    //

    /**
     * @brief Clear preparing state and transition to Idle or Printing
     *
     * Call this when the print start API call completes or fails.
     *
     * @param success If true, transitions to Printing; if false, transitions to Idle
     */

    /**
     * @brief Get current progress percentage
     * @return Progress 0-100
     */
    int get_progress() const {
        return lifecycle_.progress();
    }

    /**
     * @brief Set reference to TemperatureService for temperature overlays
     *
     * Must be called before temp card click handlers can work.
     * @param temp_panel Pointer to shared TemperatureService instance
     */
    void set_temp_control_panel(TemperatureService* temp_panel);

    // Tune panel handlers delegated to PrintTuneOverlay (tune_overlay_ member)

  private:
    friend class PrintStatusPanelTestAccess;

    //
    // === Injected Dependencies ===
    //

    helix::PrinterState& printer_state_;
    IMoonrakerAPI* api_;
    lv_obj_t* parent_screen_ = nullptr;

    //
    // === Subjects (owned by this panel) ===
    // Note: Display filename uses shared print_display_filename from helix::PrinterState
    //       (populated by ActivePrintMediaManager)
    //

    SubjectManager subjects_; ///< RAII manager for automatic subject cleanup

    lv_subject_t layer_text_subject_;
    lv_subject_t filament_used_text_subject_;
    lv_subject_t elapsed_subject_;
    lv_subject_t remaining_subject_;
    lv_subject_t eta_subject_;
    lv_subject_t nozzle_status_subject_;
    lv_subject_t bed_status_subject_;
    lv_subject_t chamber_status_subject_;
    lv_subject_t speed_subject_;
    lv_subject_t flow_subject_;
    lv_subject_t
        view_toggle_icon_subject_; ///< MDI codepoint for btn_view_toggle_icon (cube/layers)
    lv_subject_t camera_button_label_subject_; ///< "Cam"/"Camera" — short form at Medium and below

    // Preparing state subjects
    lv_subject_t preparing_visible_subject_;  // int: 1 if preparing, 0 otherwise
    lv_subject_t preparing_progress_subject_; // int: 0-100 progress percentage

    // Viewer mode subject (0=thumbnail mode, 1=gcode viewer mode)
    lv_subject_t gcode_viewer_mode_subject_;

    // 1 while the exclude-object overhead map overlay covers the thumbnail
    // section; drives XML bindings that hide print_thumbnail/gradient underneath.
    lv_subject_t exclude_map_active_subject_;

    // 1 once the user taps the print end overlay to dismiss it. Reset to 0
    // on new-print transitions so the next outcome's overlay appears normally.
    lv_subject_t end_overlay_dismissed_subject_;

    // Fan row adaptive-fit subject (1=row fits in the column, 0=hidden).
    // Set by recompute_fans_fit() after every breakpoint/layout change.
    lv_subject_t fans_fit_subject_{};

    // Temperature mini-graph fit subject (1=the portrait slack band is tall
    // enough to hold a readable graph, 0=hidden). Set by recompute_graph_fits()
    // from the slack the preview aspect cap just computed. 0 in landscape and at
    // every size where the cap does not bind.
    lv_subject_t graph_fits_subject_{};

    // Height apply_preview_height_cap() last parked in the preview_slack
    // absorber, in px. The single input to recompute_graph_fits(), cached rather
    // than re-measured so the fit decision cannot disagree with the layout that
    // produced it.
    int32_t preview_slack_h_ = 0;
    // Aux fan present subject (1=aux cluster visible, 0=hidden).
    // Set by bind_fan_speeds() when an aux fan is discovered.
    lv_subject_t aux_fan_present_subject_{};

    // Fan row content density (0=full: icon+label+val, 1=medium: label+val,
    // 2=compact: single-letter+val). Set by recompute_fans_density().
    lv_subject_t fan_row_density_subject_{};

    // Composite visibility subjects for the aux cluster.  Each combines
    // aux_fan_present AND the current density tier so each aux widget only
    // needs ONE bind_flag (satisfying [L042]).
    lv_subject_t aux_icon_visible_subject_{};  // aux_present && density==0
    lv_subject_t aux_full_visible_subject_{};  // aux_present && density!=2
    lv_subject_t aux_short_visible_subject_{}; // aux_present && density==2

    // Cached natural height of the fan row (measured at attach while
    // forced-visible). Used by recompute_fans_fit() as the `needed` value.
    int fan_row_natural_height_ = 0;

    // Cached natural widths per density tier (measured at attach).
    // Index 0=full, 1=medium, 2=compact. 0 means not yet measured.
    int fan_row_natural_width_[3] = {0, 0, 0};

    bool animations_enabled_ = false; ///< Cached from DisplaySettingsManager

    // Resolved fan object names (refreshed when fans_version ticks).
    std::string part_fan_name_;
    std::string hotend_fan_name_;
    std::string aux_fan_name_;

    // Derived visibility for the three end-of-print overlays. Each is 1 iff
    // print_outcome matches AND end_overlay_dismissed == 0. Stacking two
    // independent XML bind_flag observers on the same hidden flag raced at
    // startup (issue L042) — the second observer unhid the overlay even when
    // outcome was NONE. Computed in recompute_end_overlay_visibility().
    lv_subject_t show_complete_overlay_subject_;
    lv_subject_t show_cancelled_overlay_subject_;
    lv_subject_t show_error_overlay_subject_;

    // Pause overlay: 1 iff print_state_enum == PAUSED. Not gated on a
    // dismiss flag — paused is a transient runtime state, not a terminal
    // outcome, so the overlay auto-clears when the print resumes/ends.
    lv_subject_t show_paused_overlay_subject_;
    // Optional reason text shown as a second label *inside* the bubble below the
    // title (print_stats.message from Klipper, or "Filament Runout" derived from
    // a tripped sensor). The visible flag drives the reason label's hidden flag.
    lv_subject_t print_pause_reason_subject_;
    lv_subject_t print_pause_reason_visible_subject_;

    lv_subject_t exclude_objects_available_subject_; ///< Int: 1 if multi-object print
    lv_subject_t objects_text_subject_;              ///< String: "X of Y obj" display text

    // Button enable subjects — XML bind_state_if_eq drives LV_STATE_DISABLED
    // declaratively based on lifecycle state and macro-slot availability.
    lv_subject_t print_controls_enabled_subject_; ///< 1 when lifecycle.is_active()

    // Subject storage buffers
    char layer_text_buf_[80] = "Layer 0 / 0";
    char filament_used_text_buf_[32] = "";
    char elapsed_buf_[32] = "0h 00m";
    char remaining_buf_[32] = "0h 00m";
    char eta_buf_[32] = "";
    char nozzle_status_buf_[16] = "Off";
    char bed_status_buf_[16] = "Off";
    char chamber_status_buf_[16] = "";
    char speed_buf_[32] = "100%";
    char flow_buf_[32] = "100%";
    char objects_text_buf_[32] = "";        ///< "X of Y obj" buffer
    char view_toggle_icon_buf_[8] = "";     ///< View toggle icon codepoint (cube/layers)
    char camera_button_label_buf_[16] = ""; ///< Short/long camera label per ui_breakpoint
    char print_pause_reason_buf_[256] = ""; ///< Reason line shown under "Print Paused"

    //
    // === Instance State ===
    //

    // Async callback safety provided by OverlayBase::lifetime_

    /// Pure-logic state machine (no LVGL deps) — owns all print state variables
    PrintLifecycleState lifecycle_;

    // Thumbnail loading state
    std::string current_print_filename_; ///< Full path to current print file (for metadata fetch)
    /// Path most recently accepted from the shared thumbnail subject. Kept so
    /// on_activate() can re-apply it without a refetch.
    std::string cached_thumbnail_path_;

#if defined(HELIX_PLATFORM_ESP32)
    /// PSRAM-resident thumbnail currently shown in print_thumbnail_. There is
    /// no cache file on this platform, so cached_thumbnail_path_ stays empty
    /// and this shared_ptr is what keeps the image src's buffer alive.
    /// Main-thread only (its destructor drops the LVGL image cache entry).
    std::shared_ptr<helix::ui::EspPsramThumbnail> esp_thumbnail_;
#endif

    // Child widgets
    lv_obj_t* progress_bar_ = nullptr;
    lv_obj_t* preparing_progress_bar_ = nullptr;
    lv_obj_t* gcode_viewer_ = nullptr;
    lv_obj_t* print_thumbnail_ = nullptr;
    lv_obj_t* gradient_background_ = nullptr;

    // Per-asset "what is on screen" markers. The thumbnail (fallback image) and
    // the gcode viewer (3D/2D geometry) load on independent paths with very
    // different latencies — the thumbnail subject observer can advance its marker
    // even while the panel is hidden, while the gcode load is deferred and only
    // scheduled when active. A SINGLE shared marker let the thumbnail mask a
    // stale gcode render from the previous print (metadata+thumbnail correct, 3D
    // render still the old model), so the two are tracked separately and
    // reconciled independently in ensure_preview_current(). Empty when that
    // widget shows nothing; cleared in lockstep with widget destruction
    // (on_ui_destroyed) and geometry clearing (clear callback) so the
    // reconciliation never trusts a stale "showing X" claim against a blank
    // widget.
    std::string displayed_file_;       // file whose image is in the thumbnail
    std::string gcode_displayed_file_; // file whose geometry is in the viewer

    // Deferred G-code loading: filename to load when panel becomes visible
    // Set in set_filename(), consumed in on_activate() - avoids downloading
    // large files unless user actually navigates to print status panel
    std::string pending_gcode_filename_;

    // One-shot timer for deferred G-code loading (5s delay after print start)
    // Prevents memory spike during homing/heating phase
    lv_timer_t* gcode_load_timer_ = nullptr;

    /**
     * @brief Withholds the preparing overlay until preparation is worth showing
     *
     * A print with no host-side pre-start block can pass through Preparing in
     * well under a second, and flashing the overlay for that is worse than not
     * showing it. Debouncing on ELAPSED time rather than a predicted duration is
     * deliberate: a prediction can fail closed - predict "fast", reality is a
     * ten-minute mesh, and the overlay never appears at all.
     */
    lv_timer_t* preparing_show_timer_ = nullptr;

    /// Shared by cleanup() and the destructor - see CLAUDE.md threading rule 5.
    void cancel_preparing_show_timer();

    /// How long Preparing must persist before the overlay is shown.
    static constexpr uint32_t PREPARING_SHOW_DELAY_MS = 750;
    void schedule_deferred_gcode_load();

    // Reconcile the preview widgets against the current print state. Reads the
    // ACTUAL widget state (thumbnail image source, gcode viewer geometry) and
    // (re)loads only what is missing or stale. Safe and idempotent to call any
    // time; called unconditionally on every on_activate() so re-entry after a
    // destroy-on-close / memory-reclaim cycle is self-healing.
    void ensure_preview_current();

    bool complete_view_mode_ = false;

    // Tracks whether the panel was already in the preparing (pre-print) state on
    // the previous phase change. The pre-print phase number changes many times
    // during a single preparation (homing → heating → mesh → ...), and on some
    // firmwares (Snapmaker U1) it legitimately oscillates between phase enums as
    // the firmware interleaves operations. The heavy reset side-effects (zeroing
    // the progress bar / elapsed / remaining) must fire only once, on the
    // Idle→Preparing edge — not on every sub-phase — or the progress display
    // flickers back to 0% repeatedly. Message/progress observers keep the
    // display live during preparation.
    bool was_preparing_ = false;

    // Track whether panel is currently active (visible and receiving updates)
    // Used to load gcode immediately if already active when print starts
    bool is_active_ = false;

    // Path to temp G-code file downloaded for viewing (cleaned up on print end)
    std::string temp_gcode_path_;

    // Control buttons (stored for enable/disable on state changes)
    lv_obj_t* btn_timelapse_ = nullptr;
    lv_obj_t* btn_tune_ = nullptr;
    lv_obj_t* btn_cancel_ = nullptr;

    // Print completion celebration badge (animated on print complete)
    lv_obj_t* success_badge_ = nullptr;

    // Print cancelled badge (animated on print cancel)
    lv_obj_t* cancel_badge_ = nullptr;

    // Print error badge (animated on print error)
    lv_obj_t* error_badge_ = nullptr;

    //
    // === Temperature & Tuning Overlays ===
    //

    TemperatureService* temp_control_panel_ = nullptr;

    // Light/timelapse controls (extracted Phase 2 functionality)
    PrintLightTimelapseControls light_timelapse_controls_;

    // Resize callback registration flag
    bool resize_registered_ = false;

    //
    // === Private Helpers ===
    //

    void bind_fan_observers(); ///< Reclassify + rebind on fans_version
    void rebind_single_fan(ObserverGuard& guard, SubjectLifetime& lt,
                           const std::string& object_name, const char* speed_label_widget_name,
                           const char* icon_widget_name);
    void update_fan_speed_display(const char* label_name, const char* icon_name, int speed);
    void refresh_fan_animations();
    /// Portrait: cap thumbnail_section's aspect and park the leftover in the
    /// preview_slack absorber between the card and the controls. No-op in
    /// landscape and at every size where the cap does not bind.
    void apply_preview_height_cap();
    /// Record the absorber height the cap just applied and re-decide whether the
    /// temperature mini-graph fits in it. Called from every exit path of
    /// apply_preview_height_cap(), including the ones that leave the layout alone.
    void note_preview_slack(int32_t slack_h);
    void recompute_graph_fits(); ///< Slack-based graph visibility (graph_fits_subject_)
    /// Build the mini-graph controller into temp_graph_container if it is not
    /// already live. Idempotent; no-op when the widget tree is gone.
    void ensure_temp_graph();
    /// Detach the mini-graph's observers synchronously, then release the
    /// controller. Must run BEFORE the container is freed.
    /// @param defer_delete Hand the deallocation to lv_async_call instead of
    ///        running it here. True on the on_ui_destroyed() path, which is a
    ///        close callback and may be inside an UpdateQueue batch (#696).
    ///        False from the destructor, where nothing will ever drain the async
    ///        queue again and a deferred delete would leak the observers along
    ///        with the object.
    void destroy_temp_graph(bool defer_delete = true);
    void recompute_fans_fit();       ///< Height-based row visibility (fans_fit_subject_)
    void recompute_fans_density();   ///< Width-based content tier (fan_row_density_subject_)
    void recompute_aux_composites(); ///< Compute 3 aux_*_visible from aux_present + density
    void recompute_aux_composites_for_measurement(int density,
                                                  bool aux_present); ///< Measurement helper

    /// Render print_layer_text from the lifecycle's layer counters.
    void update_layer_text();

    /// Render print_filament_used_text from the current filament_used subject.
    void update_filament_used_text();

    void update_all_displays();
    void show_gcode_viewer(bool show);
    void load_gcode_file(const char* file_path);
#if defined(HELIX_PLATFORM_ESP32)
    /// Pull the current PSRAM thumbnail from PrinterState, hold a reference,
    /// and point print_thumbnail_ at its descriptor. Main thread only; no-op
    /// when the widget is absent or no thumbnail has been fetched yet.
    void apply_esp_psram_thumbnail();
#endif
    void
    load_gcode_for_viewing(const std::string& filename); ///< Download and load G-code into viewer
    void update_button_states(); ///< Enable/disable buttons based on current print state

    /// "Cam"/"Camera" per ui_breakpoint — full word only where Row 2 has room
    void update_camera_button_label(int breakpoint_value);

    /// The two per-job resets, shared by the job-state edge and the
    /// exit-from-Preparing edge. A print started in-app only ever reaches the
    /// second one: the panel is Preparing before Moonraker reports printing, so
    /// the job-state handler derives no transition and returns early.
    void apply_new_print_resets(bool reset_progress_bar, bool clear_excluded_objects);
    void update_objects_text(); ///< Update "X of Y obj" display from exclude state
    void
    update_view_toggle_position(bool objects_visible); ///< Shift view toggle when objects btn shown
    void animate_badge_pop_in(lv_obj_t* badge, const char* label); ///< Pop-in animation for badges
    void animate_print_complete();  ///< Celebratory animation when print finishes
    void animate_print_cancelled(); ///< Warning animation when print is cancelled
    void animate_print_error();     ///< Error animation when print fails
    void cleanup_temp_gcode();      ///< Remove temp G-code file downloaded for viewing
    void show_exclude_map_view();   ///< Show overhead map view of print objects
    void hide_exclude_map_view();   ///< Destroy map view and restore thumbnail/gradient
    void apply_filament_color_override(
        uint32_t color_rgb);            ///< Apply AMS/Spoolman filament color to gcode viewer
    bool build_and_apply_tool_colors(); ///< Build per-tool AMS color map and apply to viewer

    /// Per-tool slicer palette from the active file's Moonraker metadata, stored
    /// so the live render can resolve the SAME toggle-aware tool→lane match the
    /// print-select swatches and pre-flight use (instead of coloring every tool
    /// by the identity tool_to_slot_map, which paints the whole model in T0's
    /// filament on true toolchangers like the Snapmaker U1). Populated in the
    /// get_file_metadata callbacks; empty until metadata arrives.
    std::vector<std::string> filament_colors_;    ///< per-tool hex ("#RRGGBB")
    std::vector<std::string> filament_materials_; ///< per-tool material, split from ';' list

    /// Store per-tool colors/materials from file metadata (main-thread only).
    void store_filament_metadata(const FileMetadata& metadata);

    /// Build per-tool GcodeToolInfo for the tools the parsed file actually uses,
    /// from the stored slicer palette. Empty when metadata or the parsed used-set
    /// is unavailable (caller then falls back to apply_ams_tool_colors).
    [[nodiscard]] std::vector<helix::GcodeToolInfo> build_print_tool_info() const;

    /// Whether auto (color+type) matching applies for the active backend.
    /// Delegates to AmsState::effective_auto_match(), which owns the rule and is
    /// shared with PrintSelectDetailView: non-editable-card backends (U1 / ACE)
    /// always auto-match; editable backends honor the user setting.
    [[nodiscard]] bool effective_auto_match() const;

    static void format_time(int seconds, char* buf, size_t buf_size);

    //
    // === Instance Handlers ===
    //

    void handle_temp_card_click();
    void update_chamber_status();
    void recompute_end_overlay_visibility();
    void recompute_paused_overlay_visibility();
    void handle_tune_button();
    void handle_reprint_button(); ///< Reprint the cancelled file
    void handle_resize();

    /// @brief Tool indices used by the currently-loaded G-code (for U1 native pre-send).
    /// Mirrors PrintSelectDetailView::get_tools_used(). Empty if no parsed file.
    std::set<int> get_tools_used() const;

    /// @brief Recompute the print-scoped runout badge value (FIX B).
    /// Scopes FilamentSensorManager's runout state to the active print's used
    /// tools using AMS lane truth and publishes it into filament_runout_scoped,
    /// which the in-print filament_sensor_indicator binds. Runs on the main
    /// thread (observer callbacks + gcode-load paths).
    void recompute_scoped_runout();

    //
    // === Static Trampolines ===
    //

    static void on_temp_card_clicked(lv_event_t* e);
    static void on_temp_graph_clicked(lv_event_t* e);
    static void on_dismiss_overlay_clicked(lv_event_t* e);
    static void on_tune_clicked(lv_event_t* e);
    static void on_print_status_camera(lv_event_t* e);
    static void on_reprint_clicked(lv_event_t* e);
    static void on_objects_clicked(lv_event_t* e);
    static void on_view_toggle_clicked(lv_event_t* e);
    static void on_fans_clicked(lv_event_t* e);
    // SIZE_CHANGED on controls_section — triggers density + fit recompute.
    // Direct lv_obj_add_event_cb registration is correct here: SIZE_CHANGED
    // has no XML binding equivalent (only click/value events go through XML).
    static void on_controls_size_changed(lv_event_t* e);
    void handle_fans_click();

    // Static resize callback (registered with ui_resize_handler)
    static void on_resize_static();

    //
    // === Observer Instance Methods ===
    //

    void on_temperature_changed();
    void on_print_progress_changed(int progress);
    void on_print_state_changed(helix::PrintJobState state);
    void on_print_filename_changed(const char* filename);
    void on_speed_factor_changed(int speed);
    void on_flow_factor_changed(int flow);
    void on_gcode_z_offset_changed(int microns);
    void on_led_state_changed(int state);
    void on_print_layer_changed(int current_layer);
    void on_print_duration_changed(int seconds);
    void on_print_time_left_changed(int seconds);
    void on_print_start_phase_changed(int phase);
    void on_print_start_progress_changed(int progress);
    void on_preprint_remaining_changed(int seconds);
    void on_preprint_elapsed_changed(int seconds);

    // helix::PrinterState observers (ObserverGuard handles cleanup)
    /// @brief Temperature observer bundle (nozzle + bed temps)
    helix::ui::TemperatureObserverBundle<PrintStatusPanel> temp_observers_;
    ObserverGuard print_progress_observer_;
    ObserverGuard print_state_observer_;
    ObserverGuard print_filename_observer_;
    ObserverGuard speed_factor_observer_;
    ObserverGuard flow_factor_observer_;
    ObserverGuard gcode_z_offset_observer_;
    ObserverGuard led_state_observer_;
    ObserverGuard print_layer_observer_;
    ObserverGuard z_position_observer_;
    ObserverGuard print_duration_observer_;
    ObserverGuard print_time_left_observer_;
    ObserverGuard print_start_phase_observer_;
    ObserverGuard print_start_progress_observer_;
    ObserverGuard preprint_remaining_observer_;
    ObserverGuard preprint_elapsed_observer_;
    ObserverGuard exclude_objects_observer_;
    ObserverGuard excluded_objects_version_observer_;
    ObserverGuard ams_color_observer_; ///< Tracks AMS/Spoolman filament color for gcode viewer
    ObserverGuard tool_map_version_observer_; ///< Refreshes gcode viewer colors on tool remap
    ObserverGuard active_tool_observer_;    ///< Refreshes nozzle temp display with tool name prefix
    ObserverGuard chamber_temp_observer_;   ///< Updates chamber status text
    ObserverGuard print_identity_observer_; ///< Reconciles when the print's identity changes
    ObserverGuard print_thumbnail_path_observer_; ///< Updates print_thumbnail_ from shared subject
#if defined(HELIX_PLATFORM_ESP32)
    ObserverGuard print_psram_thumb_observer_; ///< Ditto, via the PSRAM generation counter
#endif
    ObserverGuard gcode_render_mode_observer_; ///< Watches settings changes to update viewer mode
    ObserverGuard print_outcome_observer_;     ///< Drives show_{complete,cancelled,error}_overlay
    ObserverGuard end_overlay_dismissed_observer_; ///< Ditto; second input to the same recompute
    ObserverGuard print_message_observer_;  ///< Drives pause reason text from print_stats.message
    ObserverGuard pending_action_observer_; ///< observes PrintControlButtons' print_pending_action
    ObserverGuard camera_label_observer_;   ///< observes ui_breakpoint → camera label length
                                            ///< for the paused overlay

    // Per-fan speed observers — each watches a DYNAMIC subject, so a paired
    // SubjectLifetime is mandatory (see [L084]: lifetime must outlive observer).
    ObserverGuard part_speed_observer_;
    SubjectLifetime part_speed_lifetime_;
    ObserverGuard hotend_speed_observer_;
    SubjectLifetime hotend_speed_lifetime_;
    ObserverGuard aux_speed_observer_;
    SubjectLifetime aux_speed_lifetime_;

    // Thermal tint for the temp-card heater icons. Bound from overlay_root_ so
    // lv_obj_find_by_name() cannot pick up a same-named icon from another
    // panel. Each binder owns its own temperature observers.
    helix::ui::HeaterIconBinder nozzle_icon_binder_;
    helix::ui::HeaterIconBinder bed_icon_binder_;
    helix::ui::HeaterIconBinder chamber_icon_binder_;

    // Static-subject observers (singleton lifetime — no SubjectLifetime token needed).
    ObserverGuard fans_version_observer_;
    ObserverGuard primary_fans_version_observer_;
    ObserverGuard animations_enabled_observer_;
    ObserverGuard breakpoint_observer_;
    ObserverGuard filament_sensor_count_observer_;
    ObserverGuard ams_slot_count_observer_;
    ObserverGuard toolchange_visible_observer_;
    ObserverGuard scoped_runout_observer_; ///< Recomputes scoped runout badge on sensor edge
    ObserverGuard
        scoped_runout_slots_observer_; ///< ...and on AMS lane-presence change (slots_version)

    // Lazy fan control overlay (created on first click; Task 9 wires the push).
    lv_obj_t* fan_control_panel_ = nullptr;

    //
    // === Exclude Object Manager ===
    //

    /// Manages exclude object feature (extracted from PrintStatusPanel)
    std::unique_ptr<helix::ui::PrintExcludeObjectManager> exclude_manager_;

    /// Overhead map view for exclude objects (shown in thumbnail-only mode)
    std::unique_ptr<helix::ui::ExcludeObjectMapView> map_view_;

    /// Side-panel companion list (shown alongside map_view_).
    std::unique_ptr<helix::ui::ExcludeObjectSideList> side_list_;

    //
    // === Filament Runout Handler ===
    //

    /// Manages filament runout guidance (extracted from PrintStatusPanel)
    std::unique_ptr<helix::ui::FilamentRunoutHandler> runout_handler_;

    //
    // === Portrait Temperature Mini-Graph ===
    //

    /// Owns the graph widget, its series observers and history backfill. Built
    /// on demand once the slack band is big enough, then kept alive across
    /// show/hide — recreating it would discard the backfilled trace.
    std::unique_ptr<helix::TempGraphController> temp_graph_controller_;

    /// The XML container the controller drew into. Nulled by on_ui_destroyed()
    /// so a rebuilt tree is never populated through a stale pointer.
    lv_obj_t* temp_graph_container_ = nullptr;
};

// Global instance accessor (needed by main.cpp)
PrintStatusPanel& get_global_print_status_panel();
