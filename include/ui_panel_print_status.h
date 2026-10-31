// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_exclude_mode_controller.h"
#include "ui_filament_runout_handler.h"
#include "ui_heater_icon_binder.h"
#include "ui_modal.h"
#include "ui_observer_guard.h"
#include "ui_print_exclude_object_manager.h"
#include "ui_print_layer_pause.h"
#include "ui_print_light_timelapse.h"
#include "ui_print_tune_overlay.h"
#include "ui_widget_ref.h"

#include "camera_view.h"
#include "overlay_base.h"
#include "print_control_buttons.h"
#include "print_lifecycle_state.h"
#include "print_status_preview_decision.h"
#include "printer_state.h"
#include "subject_managed_panel.h"
#include "ui/temperature_observer_bundle.h"

// Forward declaration
namespace helix {
class IMoonrakerAPI;
namespace ui {
class PrintStartController;
} // namespace ui
struct MemoryInfo;
} // namespace helix

#include "ui_print_status_more_menu.h"
#include "ui_print_status_tiles.h"

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
    PrintStatusPanel(helix::PrinterState& printer_state, helix::IMoonrakerAPI* api);

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
     * @brief Push the print status overlay and open the exclude-object map over its preview
     *
     * The home exclude_map tile's entry. The map opens once the panel is active, so its
     * canvas is sized against a laid-out preview card. Opening only shows the map: the
     * exclusion itself still goes through the confirmation modal and undo window.
     *
     * @return true if the overlay was pushed
     */
    static bool push_overlay_with_exclude_map(lv_obj_t* parent_screen);

    /**
     * @brief Get the cached overlay widget pointer, if created.
     *
     * Exposes the panel's root to callers that need to check nav-stack
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
    void set_api(helix::IMoonrakerAPI* api) {
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
    helix::IMoonrakerAPI* api_;

    //
    // === Subjects (owned by this panel) ===
    // Note: Display filename uses shared print_display_filename from helix::PrinterState
    //       (populated by ActivePrintMediaManager)
    //

    SubjectManager subjects_; ///< RAII manager for automatic subject cleanup

    lv_subject_t
        view_toggle_icon_subject_{}; ///< MDI codepoint for btn_view_toggle_icon (cube/layers)

    // Preparing state subjects
    lv_subject_t preparing_visible_subject_{};  // int: 1 if preparing, 0 otherwise
    lv_subject_t preparing_progress_subject_{}; // int: 0-100 progress percentage

    // Viewer mode subject (0=thumbnail mode, 1=gcode viewer mode)
    lv_subject_t gcode_viewer_mode_subject_{};

    // 1 while the exclude-object overhead map overlay covers the thumbnail
    // section; drives XML bindings that hide print_thumbnail/gradient underneath.
    lv_subject_t exclude_map_active_subject_{};

    // print_status_camera_available: 1 when this screen may show the printer's
    // camera (a webcam, a remote screen, platform extras). Shared by the
    // preview card's camera button and the More menu's Camera row.
    lv_subject_t camera_available_subject_{};
    // print_status_camera_showing: 1 while the preview card shows the camera
    // in place of the preview (available, chosen, exclude map closed).
    lv_subject_t camera_showing_subject_{};
    // print_status_camera_has_frame: 1 once the running stream's first frame
    // is on screen; hides the connecting indicator.
    lv_subject_t camera_has_frame_subject_{};

    // 1 once the user taps the print end overlay to dismiss it. Reset to 0
    // on new-print transitions so the next outcome's overlay appears normally.
    lv_subject_t end_overlay_dismissed_subject_{};

    // The tile arrangement PrintStatusLayoutFitter measures (ps_arrangement,
    // ps_strip_stacked, ps_checklist_compact).
    lv_subject_t arrangement_subject_{};
    lv_subject_t strip_stacked_subject_{};
    lv_subject_t checklist_compact_subject_{};

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
    char objects_text_buf_[32] = "";        ///< "X of Y obj" buffer
    char view_toggle_icon_buf_[8] = "";     ///< View toggle icon codepoint (cube/layers)
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
    helix::ui::WidgetRef gcode_viewer_;
    lv_obj_t* print_thumbnail_ = nullptr;
    lv_obj_t* gradient_background_ = nullptr;
    helix::ui::WidgetRef preview_camera_image_;

    /// What the thumbnail and G-code viewer show for the running print.
    helix::ui::PrintPreviewController preview_;

    /// The progress card's text subjects and their formatting.
    helix::ui::PrintProgressText progress_text_;

    /// Measured arrangement of the job tile, printer tile and strip.
    helix::ui::PrintStatusLayoutFitter layout_fitter_;

    /// The printer tile's chips and the job tile's checklist.
    helix::ui::PrintStatusTiles tiles_;

    helix::ui::PrintStatusMoreMenu more_menu_;

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
    static constexpr uint32_t kPreparingShowDelayMs = 750;

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
    bool exclude_map_requested_ = false; ///< Open the exclude map on the next activation

    // Control buttons (stored for enable/disable on state changes)
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
    helix::ui::PrintLayerPauseControls layer_pause_controls_;

    // Resize callback registration flag
    bool resize_registered_ = false;

    //
    // === Private Helpers ===
    //

    void bind_fan_observers(); ///< Reclassify + rebind on fans_version
    void rebind_single_fan(ObserverGuard& guard, SubjectLifetime& lt,
                           const std::string& object_name);

    void update_all_displays();
    void show_gcode_viewer(bool show);
    /// Pause the G-code viewer unless it is the visible layer: shown by
    /// gcode_viewer_mode and not covered by the camera.
    void apply_gcode_viewer_pause();
    /// Recompute the camera subjects from their inputs, then run the camera
    /// stream exactly while it shows on an active panel with the display awake.
    void update_preview_camera();
    void start_preview_camera();
    void stop_preview_camera();
    void handle_camera_toggle();
    void update_button_states(); ///< Enable/disable buttons based on current print state

    /// The two per-job resets, shared by the job-state edge and the
    /// exit-from-Preparing edge. A print started in-app only ever reaches the
    /// second one: the panel is Preparing before Moonraker reports printing, so
    /// the job-state handler derives no transition and returns early.
    void apply_new_print_resets(bool reset_progress_bar, bool clear_excluded_objects);
    void update_objects_text(); ///< Update "X of Y obj" display from exclude state
    /// Publish whether the objects button shows: [exclude_object] and 2+ defined objects.
    void refresh_exclude_objects_available();
    void animate_badge_pop_in(lv_obj_t* badge, const char* label); ///< Pop-in animation for badges
    void animate_print_complete();  ///< Celebratory animation when print finishes
    void animate_print_cancelled(); ///< Warning animation when print is cancelled
    void animate_print_error();     ///< Error animation when print fails
    void show_exclude_map_view();   ///< Show overhead map view of print objects
    void hide_exclude_map_view();   ///< Destroy map view and restore thumbnail/gradient
    /// Open the map a push_overlay_with_exclude_map() asked for, once the panel is active.
    void open_pending_exclude_map();

    //
    // === Instance Handlers ===
    //

    void handle_temp_card_click();
    void recompute_end_overlay_visibility();
    void recompute_paused_overlay_visibility();
    void handle_tune_button();
    void handle_reprint_button(); ///< Reprint the cancelled file
    /// The file Reprint starts: the original, never a rewritten temp copy.
    [[nodiscard]] std::string reprint_filename() const;
    /// reprint_filename() with the archive plate to extract, for a .3mf printed that way.
    [[nodiscard]] helix::gcode::ReprintTarget reprint_target() const;
    /// Reprints @p target through @p controller, which owns the pre-print step a
    /// firmware needs (the U1's used heads) and the preparing window.
    void reprint_through(helix::ui::PrintStartController& controller,
                         const helix::gcode::ReprintTarget& target);

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

    // SIZE_CHANGED on overlay_content — triggers the tile layout recompute.
    // Direct lv_obj_add_event_cb registration is correct here: SIZE_CHANGED
    // has no XML binding equivalent (only click/value events go through XML).
    static void on_content_size_changed(lv_event_t* e);
    static void on_printer_area_size_changed(lv_event_t* e);
    void relayout();
    void handle_more_clicked(lv_obj_t* anchor);
    void handle_more_action(int action);
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
    ObserverGuard chamber_temp_observer_;   ///< Chamber chip reading
    ObserverGuard chamber_target_observer_; ///< Effective setpoint moved (fan-parked M141 sets)
    ObserverGuard chamber_mode_observer_;   ///< Control mode moved (Heating/Maintaining/Resting)
    ObserverGuard print_identity_observer_; ///< Reconciles when the print's identity changes
    ObserverGuard print_thumbnail_path_observer_; ///< Updates print_thumbnail_ from shared subject
#if defined(HELIX_PLATFORM_ESP32)
    ObserverGuard print_psram_thumb_observer_; ///< Ditto, via the PSRAM generation counter
#endif
    ObserverGuard gcode_render_mode_observer_; ///< Watches settings changes to update viewer mode
    /// Inputs of print_status_camera_available / _showing
    ObserverGuard camera_webcam_observer_;
    ObserverGuard camera_remote_observer_;
    ObserverGuard camera_choice_observer_;
    ObserverGuard camera_exclude_map_observer_;
    bool display_asleep_ = false;          ///< Last display sleep edge the camera view reported
    ObserverGuard print_outcome_observer_; ///< Drives show_{complete,cancelled,error}_overlay
    ObserverGuard end_overlay_dismissed_observer_; ///< Ditto; second input to the same recompute
    ObserverGuard print_stop_reason_observer_; ///< Drives pause reason text from print_stop_reason
    ObserverGuard pending_action_observer_; ///< observes PrintControlButtons' print_pending_action
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
    ObserverGuard breakpoint_observer_;
    ObserverGuard job_queue_count_observer_;
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

#if HELIX_HAS_CAMERA
    /// The preview card's live camera; runs only while print_status_camera_showing.
    helix::CameraView camera_view_;
#endif
};

// Global instance accessor (needed by main.cpp)
PrintStatusPanel& get_global_print_status_panel();
