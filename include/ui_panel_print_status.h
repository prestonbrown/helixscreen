// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_exclude_mode_controller.h"
#include "ui_filament_runout_handler.h"
#include "ui_heater_icon_binder.h"
#include "ui_modal.h"
#include "ui_observer_guard.h"
#include "ui_print_exclude_object_manager.h"
#include "ui_print_light_timelapse.h"
#include "ui_print_tune_overlay.h"
#include "ui_temperature_utils.h" // HEATER_STATUS_BUF_BYTES

#include "overlay_base.h"
#include "print_control_buttons.h"
#include "print_lifecycle_state.h"
#include "print_status_preview_decision.h"
#include "printer_state.h"
#include "subject_managed_panel.h"
#include "ui/temperature_observer_bundle.h"

// Forward declaration
class IMoonrakerAPI;
namespace helix {
struct MemoryInfo;
} // namespace helix

#include "filament_mapper.h" // helix::GcodeToolInfo
#include "print_preview_controller.h"
#include "print_progress_text.h"
#include "print_status_layout_fitter.h"

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
    void on_deactivating(DeactivateReason reason) override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    /**
     * @brief Push the print status overlay, creating its widget tree if there is none
     *
     * All call sites should use this instead of manually pushing the overlay.
     * Handles lazy creation, NavigationManager registration, and the close
     * callback. That callback asks print_status_destroy_on_close() when the
     * close happens whether to destroy the widget tree (~400-800KB) or keep it;
     * subjects survive either way.
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

    /**
     * @brief Destroy the cached widget tree, if there is one, and log why
     *
     * Every path that drops the cached tree goes through here, so each
     * destruction leaves one log line naming its cause. Subjects and observers
     * survive; the next push_overlay() creates a fresh tree.
     *
     * @param cause Why: named in the log line, and read when the next tree is
     *              created to decide how loudly that creation logs
     */
    static void destroy_cached_overlay(helix::ui::PrintStatusTreeDestroyCause cause);

  protected:
    /**
     * @brief Called after destroy_overlay_ui() deletes the widget tree
     *
     * Nulls all widget pointers, resets widget-dependent state (exclude manager,
     * resize registration), and cancels any in-flight animations. Does NOT
     * destroy subjects or observers on live PrinterState subjects.
     */
    void on_ui_destroyed() override;

    /**
     * @brief LV_EVENT_DELETE hook on the panel root — the only notice this
     *        panel gets when the tree is deleted by anything other than
     *        destroy_overlay_ui()
     *
     * A raw lv_obj_delete() (screen teardown, tests) fires no panel call, but
     * the queued observe<int> handlers still run on the next drain and
     * dereference the cached child pointers. Drops them via
     * forget_cached_widgets(). Same contract as PowerPanel's hook (#776
     * family).
     */
    static void on_root_deleted(lv_event_t* e);

    /**
     * @brief Drop every cached raw widget pointer, including overlay_root_
     *
     * Idempotent. Called from on_ui_destroyed() (explicit teardown) and
     * on_root_deleted() (the tree died some other way). Does NOT tear down
     * the owned sub-objects (side list, map view, exclude manager) — those
     * need a live tree to tear down and must never run from inside LVGL's
     * delete event; the exclude manager only forgets its viewer pointer. Also does not touch
     * delete_hook_root_: that member tracks where the delete hook is installed and is cleared only
     * by the hook firing, the explicit teardown, or the destructor.
     */
    void forget_cached_widgets();

    /**
     * @brief The widget on_ui_destroyed()/~PrintStatusPanel must uninstall the
     *        delete hook from
     *
     * Not overlay_root_: destroy_overlay_ui() hands that to
     * safe_delete_deferred(), which nulls it immediately — before the deferred
     * deletion runs and while the hook is still installed on the detached
     * tree. This copy stays set until the hook fires on it, the explicit
     * teardown removes it, or the destructor does, whichever comes first.
     */
    lv_obj_t* delete_hook_root_ = nullptr;

  public:
    //
    // === Legacy Compatibility ===

    /**
     * @brief Death signal for the subjects this PrintStatusPanel owns.
     *
     * Pass to observe_*() from anything that can outlive this object's
     * deinit_subjects(): that path frees every observer node without bumping
     * the ObserverGuard invalidation epoch, so a guard without the token
     * dereferences a freed observer on its next reset().
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }
    //

    const char* xml_component() const override {
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
        preview_.set_api(api);
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

    /// The close callback push_overlay() registers. Asks
    /// print_status_destroy_on_close() when the close lands whether the tree
    /// goes; a tree it keeps is deactivated and watched for the job ending.
    static void on_overlay_closed();

    /// Queue the release of a tree a close kept, once its job has let go of the
    /// machine. Every condition is checked again when the release lands.
    static void release_kept_tree_after_job();

    /// Where the close-time decision and the creation log read system memory.
    /// A function pointer so a test can stand in a low-memory host.
    static helix::MemoryInfo (*memory_info_source_)();

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

    lv_subject_t nozzle_status_subject_{};        ///< duty text ("" = none)
    lv_subject_t bed_status_subject_{};           ///< duty text ("" = none)
    lv_subject_t chamber_status_subject_{};       ///< duty text ("" = none)
    lv_subject_t nozzle_status_state_subject_{};  ///< HeaterStatusState int
    lv_subject_t bed_status_state_subject_{};     ///< HeaterStatusState int
    lv_subject_t chamber_status_state_subject_{}; ///< HeaterStatusState int
    lv_subject_t
        view_toggle_icon_subject_{}; ///< MDI codepoint for btn_view_toggle_icon (cube/layers)
    lv_subject_t
        camera_button_label_subject_{}; ///< "Cam"/"Camera" — short form at Medium and below

    // Preparing state subjects
    lv_subject_t preparing_visible_subject_{};  // int: 1 if preparing, 0 otherwise
    lv_subject_t preparing_progress_subject_{}; // int: 0-100 progress percentage

    // Viewer mode subject (0=thumbnail mode, 1=gcode viewer mode)
    lv_subject_t gcode_viewer_mode_subject_{};

    // 1 while the exclude-object overhead map overlay covers the thumbnail
    // section; drives XML bindings that hide print_thumbnail/gradient underneath.
    lv_subject_t exclude_map_active_subject_{};

    // 1 once the user taps the print end overlay to dismiss it. Reset to 0
    // on new-print transitions so the next outcome's overlay appears normally.
    lv_subject_t end_overlay_dismissed_subject_{};

    // Fan row adaptive-fit subject (1=row fits in the column, 0=hidden).
    // Set by recompute_fans_fit() after every breakpoint/layout change.
    lv_subject_t fans_fit_subject_{};

    // Temperature mini-graph fit subject (1=the portrait slack band is tall
    // enough to hold a readable graph, 0=hidden). Set by recompute_graph_fits()
    // from the slack the preview aspect cap just computed. 0 in landscape and at
    // every size where the cap does not bind.
    lv_subject_t graph_fits_subject_{};

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
    lv_subject_t show_complete_overlay_subject_{};
    lv_subject_t show_cancelled_overlay_subject_{};
    lv_subject_t show_error_overlay_subject_{};

    // Pause overlay: 1 iff print_state_enum == PAUSED. Not gated on a
    // dismiss flag — paused is a transient runtime state, not a terminal
    // outcome, so the overlay auto-clears when the print resumes/ends.
    lv_subject_t show_paused_overlay_subject_{};
    // Optional reason text shown as a second label *inside* the bubble below the
    // title (print_stats.message from Klipper, or "Filament Runout" derived from
    // a tripped sensor). The visible flag drives the reason label's hidden flag.
    lv_subject_t print_pause_reason_subject_{};
    lv_subject_t print_pause_reason_visible_subject_{};

    lv_subject_t exclude_objects_available_subject_{}; ///< Int: 1 if multi-object print
    lv_subject_t objects_text_subject_{};              ///< String: "X of Y obj" display text

    // Button enable subjects — XML bind_state_if_eq drives LV_STATE_DISABLED
    // declaratively based on lifecycle state and macro-slot availability.
    lv_subject_t print_controls_enabled_subject_{}; ///< 1 when lifecycle.is_active()

    // Subject storage buffers
    char nozzle_status_buf_[helix::ui::temperature::HEATER_STATUS_BUF_BYTES] = "Off";
    char bed_status_buf_[helix::ui::temperature::HEATER_STATUS_BUF_BYTES] = "Off";
    char chamber_status_buf_[helix::ui::temperature::HEATER_STATUS_BUF_BYTES] = "";
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

    std::string current_print_filename_; ///< Full path to current print file (for metadata fetch)

    // Child widgets
    lv_obj_t* progress_bar_ = nullptr;
    lv_obj_t* preparing_progress_bar_ = nullptr;
    lv_obj_t* gcode_viewer_ = nullptr;
    lv_obj_t* print_thumbnail_ = nullptr;
    lv_obj_t* gradient_background_ = nullptr;

    /// What the thumbnail and G-code viewer show for the running print.
    helix::ui::PrintPreviewController preview_;

    /// The progress card's text subjects and their formatting.
    helix::ui::PrintProgressText progress_text_;

    /// Measured layout: fan row fit, preview height cap, temperature mini-graph.
    helix::ui::PrintStatusLayoutFitter layout_fitter_;

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

    void update_all_displays();
    void update_heater_status_rows();
    void show_gcode_viewer(bool show);
    void update_button_states(); ///< Enable/disable buttons based on current print state

    /// "Cam"/"Camera" per ui_breakpoint — full word only where Row 2 has room
    void update_camera_button_label(int breakpoint_value);

    /// The two per-job resets, shared by the job-state edge and the
    /// exit-from-Preparing edge. A print started in-app only ever reaches the
    /// second one: the panel is Preparing before Moonraker reports printing, so
    /// the job-state handler derives no transition and returns early.
    void apply_new_print_resets(bool reset_progress_bar, bool clear_excluded_objects);
    void update_objects_text(); ///< Update "X of Y obj" display from exclude state
    /// Publish whether the objects button shows: [exclude_object] and 2+ defined objects.
    void refresh_exclude_objects_available();
    void
    update_view_toggle_position(bool objects_visible); ///< Shift view toggle when objects btn shown
    void animate_badge_pop_in(lv_obj_t* badge, const char* label); ///< Pop-in animation for badges
    void animate_print_complete();  ///< Celebratory animation when print finishes
    void animate_print_cancelled(); ///< Warning animation when print is cancelled
    void animate_print_error();     ///< Error animation when print fails
    void show_exclude_map_view();   ///< Show overhead map view of print objects
    void hide_exclude_map_view();   ///< Destroy map view and restore thumbnail/gradient

    //
    // === Instance Handlers ===
    //

    void handle_temp_card_click();
    void update_chamber_status();
    void recompute_end_overlay_visibility();
    void recompute_paused_overlay_visibility();
    void handle_tune_button();
    void handle_reprint_button(); ///< Reprint the cancelled file
    /// The file Reprint starts: the original, never a rewritten temp copy.
    [[nodiscard]] std::string reprint_filename() const;
    void handle_files_click(); ///< Open print select while this overlay holds the screen
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

    // SIZE_CHANGED on controls_section — triggers density + fit recompute.
    // Direct lv_obj_add_event_cb registration is correct here: SIZE_CHANGED
    // has no XML binding equivalent (only click/value events go through XML).
    static void on_controls_size_changed(lv_event_t* e);
    void handle_fans_click();
    void handle_objects_toggle();
    void handle_view_toggle();

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
    /// Re-renders the Speed/Flow row in the units the speed/flow preference picks.
    void update_speed_flow_text();
    void on_gcode_z_offset_changed(int microns);
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
    /// Armed while a tree a close kept is hidden; fires when the job lets go of
    /// the machine. push_overlay() and on_ui_destroyed() disarm it.
    ObserverGuard kept_tree_job_observer_;
    ObserverGuard print_filename_observer_;
    ObserverGuard speed_factor_observer_;
    ObserverGuard flow_factor_observer_;
    ObserverGuard live_velocity_observer_;
    ObserverGuard extruder_velocity_observer_;
    ObserverGuard physical_units_observer_;
    ObserverGuard gcode_z_offset_observer_;
    ObserverGuard print_layer_observer_;
    ObserverGuard z_position_observer_;
    ObserverGuard print_duration_observer_;
    ObserverGuard print_time_left_observer_;
    ObserverGuard print_start_phase_observer_;
    ObserverGuard print_start_progress_observer_;
    ObserverGuard preprint_remaining_observer_;
    ObserverGuard preprint_elapsed_observer_;
    ObserverGuard exclude_objects_observer_;
    ObserverGuard exclude_object_capability_observer_;
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
    /// ...and on AMS lane change (slots_version), which also re-pushes the
    /// viewer's per-tool colors: ams_color_observer_ only sees the ACTIVE lane
    /// and tool_map_version_observer_ only an explicit remap, so a color edit on
    /// any other lane reaches the live preview through here alone.
    ObserverGuard scoped_runout_slots_observer_;

    //
    // === Exclude Object Manager ===
    //

    /// Manages exclude object feature (extracted from PrintStatusPanel)
    std::unique_ptr<helix::ui::PrintExcludeObjectManager> exclude_manager_;

    /// Exclude mode over the preview card (list + map or render badges).
    helix::ui::ExcludeModeController exclude_mode_;

    //
    // === Filament Runout Handler ===
    //

    /// Manages filament runout guidance (extracted from PrintStatusPanel)
    std::unique_ptr<helix::ui::FilamentRunoutHandler> runout_handler_;
};

// Global instance accessor (needed by main.cpp)
PrintStatusPanel& get_global_print_status_panel();
