// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_machine_limits.h
 * @brief Machine Limits overlay - adjusts printer velocity and acceleration limits
 *
 * This overlay allows users to adjust runtime motion limits via SET_VELOCITY_LIMIT:
 * - Max Velocity (mm/s)
 * - Max Acceleration (mm/s²)
 * - Acceleration to Deceleration (mm/s²)
 * - Square Corner Velocity (mm/s)
 *
 * Z-axis limits (max_z_velocity, max_z_accel) are displayed read-only since they
 * require config file changes and cannot be set via SET_VELOCITY_LIMIT.
 *
 * @pattern Overlay (two-phase init: init_subjects -> create -> callbacks)
 * @threading Main thread only
 *
 * @see IMoonrakerAPI::set_machine_limits for gcode generation
 * @see MachineLimits struct in calibration_types.h
 */

#pragma once

#include "calibration_types.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

// Forward declarations
class IMoonrakerAPI;

namespace helix::settings {

/**
 * @class MachineLimitsOverlay
 * @brief Overlay for adjusting printer velocity and acceleration limits
 *
 * This overlay provides sliders for adjusting four motion parameters:
 * - max_velocity: Maximum travel speed
 * - max_accel: Maximum acceleration
 * - max_accel_to_decel: Acceleration to deceleration transition
 * - square_corner_velocity: Speed when traversing square corners
 *
 * ## State Management:
 *
 * The overlay tracks two copies of MachineLimits:
 * - current_limits_: Live values reflecting slider positions
 * - original_limits_: Snapshot when overlay opened, for reset functionality
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_machine_limits_overlay();
 * overlay.set_api(api);
 * overlay.show(parent_screen);  // Queries current limits, then shows overlay
 * @endcode
 */
class MachineLimitsOverlay : public OverlayBase {
  public:
    /**
     * @brief Default constructor
     */
    MachineLimitsOverlay();

    /**
     * @brief Destructor - cleans up subjects
     */
    ~MachineLimitsOverlay() override;

    //
    // === Configuration ===
    //

    /**
     * @brief Set the API for querying/setting limits
     *
     * @param api Pointer to IMoonrakerAPI (may be nullptr)
     */
    void set_api(IMoonrakerAPI* api);

    //
    // === Initialization ===
    //

    /**
     * @brief Initialize LVGL subjects for XML data binding
     *
     * Creates subjects for:
     * - max_velocity_display: "500 mm/s"
     * - max_accel_display: "3000 mm/s²"
     * - accel_to_decel_display: "1500 mm/s²"
     * - square_corner_velocity_display: "5 mm/s"
     *
     * Must be called BEFORE create() to ensure bindings work.
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks for:
     * - on_max_velocity_changed
     * - on_max_accel_changed
     * - on_accel_to_decel_changed
     * - on_square_corner_velocity_changed
     * - on_limits_reset
     * - on_limits_apply
     */
    void register_callbacks() override;

    //
    // === UI Creation ===
    //

    /**
     * @brief Create the overlay UI (called lazily)
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Show the overlay (queries current limits first)
     *
     * This method:
     * 1. Ensures overlay is created
     * 2. Queries API for current machine limits
     * 3. Updates sliders and displays
     * 4. Pushes overlay onto navigation stack
     *
     * @param parent_screen The parent screen for overlay creation
     */
    void show(lv_obj_t* parent_screen);

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Get human-readable overlay name
     * @return "Machine Limits"
     */
    const char* get_name() const override {
        return "Machine Limits";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Refreshes machine limits data from printer.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     */
    void on_deactivate() override;

    //
    // === Accessors ===
    //

    /**
     * @brief Check if overlay has been created
     * @return true if create() was called successfully
     */
    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    //
    // === Event Handlers (public for static callbacks) ===
    //

    /**
     * @brief Handle max velocity slider change
     * @param value New velocity value from slider
     */
    void handle_velocity_changed(int value);

    /**
     * @brief Handle max acceleration slider change
     * @param value New acceleration value from slider
     */
    void handle_accel_changed(int value);

    /**
     * @brief Handle accel-to-decel slider change
     * @param value New accel-to-decel value from slider
     */
    void handle_a2d_changed(int value);

    /**
     * @brief Handle square corner velocity slider change
     * @param value New SCV value from slider
     */
    void handle_scv_changed(int value);

    /**
     * @brief Handle reset button - restores original limits
     */
    void handle_reset();

    /**
     * @brief Handle extrude speed slider change
     * @param value New speed value in mm/s
     */
    void handle_extrude_speed_changed(int value);

  private:
    //
    // === Internal Methods ===
    //

    /**
     * @brief Update display subjects from current_limits_
     */
    void update_display();

    /**
     * @brief Update slider positions from current_limits_
     */
    void update_sliders();

    /**
     * @brief Apply current limits to printer immediately
     *
     * Called on slider release to send SET_VELOCITY_LIMIT command.
     * Does not show success toast (only errors).
     */
    void apply_limits();

    /**
     * @brief Schedule a debounced apply_limits() call
     *
     * Creates or resets a 250ms one-shot timer. During slider drags this
     * prevents spamming G-code commands to the printer on every pixel of
     * slider movement.
     */
    void schedule_apply_limits();

    /**
     * @brief Query API for limits and show overlay
     * @param parent_screen Parent screen for overlay creation
     */
    void query_and_show(lv_obj_t* parent_screen);

    /**
     * @brief Deinitialize subjects for clean shutdown
     */
    void deinit_subjects();

    //
    // === Dependencies ===
    //

    IMoonrakerAPI* api_{nullptr};

    //
    // === State Tracking ===
    //

    MachineLimits current_limits_;     ///< Live values from sliders
    MachineLimits original_limits_;    ///< Values when overlay opened (for reset)
    lv_timer_t* apply_timer_{nullptr}; ///< Debounce timer for apply_limits (250ms)

    //
    // === Subject Management ===
    //

    SubjectManager subjects_;

    // Display subjects for XML binding
    lv_subject_t max_velocity_display_subject_{};
    lv_subject_t max_accel_display_subject_{};
    lv_subject_t accel_to_decel_display_subject_{};
    lv_subject_t square_corner_velocity_display_subject_{};
    lv_subject_t extrude_speed_display_subject_{};

    // String buffers for subject values
    char velocity_buf_[16]{};
    char accel_buf_[16]{};
    char a2d_buf_[16]{};
    char scv_buf_[16]{};
    char extrude_speed_buf_[16]{};

    //
    // === Static Callbacks ===
    //

    static void on_velocity_changed(lv_event_t* e);
    static void on_accel_changed(lv_event_t* e);
    static void on_a2d_changed(lv_event_t* e);
    static void on_scv_changed(lv_event_t* e);
    static void on_reset(lv_event_t* e);
    static void on_extrude_speed_changed(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton MachineLimitsOverlay
 */
MachineLimitsOverlay& get_machine_limits_overlay();

/**
 * @brief Initialize the global overlay with API
 *
 * Convenience function to initialize and configure the overlay.
 *
 * @param api Pointer to IMoonrakerAPI
 */
void init_machine_limits_overlay(IMoonrakerAPI* api);

} // namespace helix::settings
