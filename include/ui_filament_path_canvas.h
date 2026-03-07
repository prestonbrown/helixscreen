// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_filament_path_canvas.h
 * @brief Filament path visualization widget for AMS panel
 *
 * Draws a schematic view of the filament path from spool storage through
 * hub/selector to the nozzle. Supports both Happy Hare (linear/selector)
 * and AFC (hub/merger) topologies.
 *
 * Visual layout (vertical, top to bottom):
 *   - Entry points at top (one per slot, connecting to ams_slot widgets)
 *   - Prep sensors (AFC) or slot markers
 *   - Lane/slot lines converging to center
 *   - Hub/Selector box
 *   - Output tube
 *   - Toolhead sensor
 *   - Nozzle at bottom
 *
 * Visual states:
 *   - Idle lane: Thin gray dashed line
 *   - Available: Thin gray solid line
 *   - Active/loaded: Thick line in filament color
 *   - Loading: Animated gradient moving downward
 *   - Unloading: Animated gradient moving upward
 *   - Error segment: Thick red pulsing line
 *
 * The widget works alongside existing ams_slot widgets - the slots show
 * individual filament colors/status, while this shows the path routing.
 *
 * XML usage:
 * @code{.xml}
 * <filament_path_canvas name="path_view"
 *                       width="100%" height="200"
 *                       topology="hub"
 *                       slot_count="4"
 *                       active_slot="2"/>
 * @endcode
 *
 * XML attributes:
 *   - topology: "linear" (Happy Hare) or "hub" (AFC) - default "hub"
 *   - slot_count: Number of slots (1-16) - default 4
 *   - active_slot: Currently active slot (-1 = none) - default -1
 *   - filament_segment: Current position (0-7, PathSegment enum)
 *   - error_segment: Error location (0-7, PathSegment enum, 0=none)
 *   - anim_progress: Animation progress 0-100
 *   - filament_color: Active filament color (0xRRGGBB)
 *   - faceted_toolhead: "true" for Stealthburner style, "false" for Bambu style (default)
 *     Also accepts "stealthburner", "a4t", or "default" for explicit style selection
 */

/**
 * @brief Register the filament_path_canvas widget with LVGL's XML system
 *
 * Must be called AFTER AmsState::init_subjects() and BEFORE any XML files
 * using <filament_path_canvas> are registered.
 */
void ui_filament_path_canvas_register(void);

/**
 * @brief Create a filament path canvas widget programmatically
 *
 * @param parent Parent LVGL object
 * @return Created widget or NULL on failure
 */
lv_obj_t* ui_filament_path_canvas_create(lv_obj_t* parent);

/**
 * @brief Set the path topology (LINEAR or HUB)
 *
 * @param obj The filament_path_canvas widget
 * @param topology 0=LINEAR (selector), 1=HUB (merger)
 */
void ui_filament_path_canvas_set_topology(lv_obj_t* obj, int topology);

/**
 * @brief Set the number of slots
 *
 * @param obj The filament_path_canvas widget
 * @param count Number of slots (1-16)
 */
void ui_filament_path_canvas_set_slot_count(lv_obj_t* obj, int count);

/**
 * @brief Set the slot overlap amount for lane X position calculation
 *
 * When slots use negative column padding to overlap visually (for 5+ slots),
 * the lane entry points need to match the actual slot center positions.
 *
 * @param obj The filament_path_canvas widget
 * @param overlap Overlap in pixels (same as negative pad_column value)
 */
void ui_filament_path_canvas_set_slot_overlap(lv_obj_t* obj, int32_t overlap);

/**
 * @brief Set the slot width for lane X position calculation
 *
 * Dynamic slot width is calculated by AmsPanel based on available space.
 * This must match the actual slot widget width for lanes to align.
 *
 * @param obj The filament_path_canvas widget
 * @param width Slot width in pixels
 */
void ui_filament_path_canvas_set_slot_width(lv_obj_t* obj, int32_t width);

/**
 * @brief Set the slot grid widget for live position measurement
 *
 * When set, the draw callback measures slot positions directly from the grid's
 * child widgets, ensuring pixel-perfect alignment at any screen size.
 *
 * @param obj The filament_path_canvas widget
 * @param slot_grid The slot_grid lv_obj containing slot child widgets
 */
void ui_filament_path_canvas_set_slot_grid(lv_obj_t* obj, lv_obj_t* slot_grid);

/**
 * @brief Set the active slot (whose path is highlighted)
 *
 * @param obj The filament_path_canvas widget
 * @param slot Slot index (0+), or -1 for none
 */
void ui_filament_path_canvas_set_active_slot(lv_obj_t* obj, int slot);

/**
 * @brief Set the current filament segment position
 *
 * @param obj The filament_path_canvas widget
 * @param segment PathSegment enum value (0-7)
 */
void ui_filament_path_canvas_set_filament_segment(lv_obj_t* obj, int segment);

/**
 * @brief Set the error segment (highlighted in red)
 *
 * @param obj The filament_path_canvas widget
 * @param segment PathSegment enum value (0-7), 0=NONE for no error
 */
void ui_filament_path_canvas_set_error_segment(lv_obj_t* obj, int segment);

/**
 * @brief Set animation progress (for load/unload animations)
 *
 * @param obj The filament_path_canvas widget
 * @param progress Progress 0-100
 */
void ui_filament_path_canvas_set_anim_progress(lv_obj_t* obj, int progress);

/**
 * @brief Set the active filament color
 *
 * @param obj The filament_path_canvas widget
 * @param color RGB color (0xRRGGBB)
 */
void ui_filament_path_canvas_set_filament_color(lv_obj_t* obj, uint32_t color);

/**
 * @brief Force redraw of the path visualization
 *
 * @param obj The filament_path_canvas widget
 */
void ui_filament_path_canvas_refresh(lv_obj_t* obj);

/**
 * @brief Set click callback for slot selection
 *
 * When user taps on a slot's entry point, this callback is invoked.
 *
 * @param obj The filament_path_canvas widget
 * @param cb Callback function (slot_index, user_data)
 * @param user_data User data passed to callback
 */
typedef void (*filament_path_slot_cb_t)(int slot_index, void* user_data);
void ui_filament_path_canvas_set_slot_callback(lv_obj_t* obj, filament_path_slot_cb_t cb,
                                               void* user_data);

/**
 * @brief Start segment transition animation
 *
 * Animates the filament tip moving from one segment to another.
 * Called automatically when filament_segment changes via set_filament_segment().
 *
 * @param obj The filament_path_canvas widget
 * @param from_segment Starting PathSegment (0-7)
 * @param to_segment Target PathSegment (0-7)
 */
void ui_filament_path_canvas_animate_segment(lv_obj_t* obj, int from_segment, int to_segment);

/**
 * @brief Check if animation is currently active
 *
 * @param obj The filament_path_canvas widget
 * @return true if segment or error animation is running
 */
bool ui_filament_path_canvas_is_animating(lv_obj_t* obj);

/**
 * @brief Stop all animations
 *
 * @param obj The filament_path_canvas widget
 */
void ui_filament_path_canvas_stop_animations(lv_obj_t* obj);

/**
 * @brief Set per-slot filament state for multi-filament visualization
 *
 * Shows filament color from spool to the specified sensor position for slots
 * that have filament installed, even when not the active slot.
 *
 * @param obj The filament_path_canvas widget
 * @param slot_index Slot index (0-15)
 * @param segment PathSegment enum value indicating how far filament extends
 * @param color RGB color (0xRRGGBB) of the filament
 */
void ui_filament_path_canvas_set_slot_filament(lv_obj_t* obj, int slot_index, int segment,
                                               uint32_t color);

/**
 * @brief Set per-slot prep sensor capability flag
 *
 * Controls whether a prep/pre-gate sensor dot is drawn for the given slot.
 * HUB topology (AFC) callers set all slots true; LINEAR topology (Happy Hare)
 * callers set per-slot based on actual pre-gate sensor presence.
 *
 * @param obj The filament_path_canvas widget
 * @param slot Slot index (0-15)
 * @param has_sensor true if this slot has a prep/pre-gate sensor
 */
void ui_filament_path_canvas_set_slot_prep_sensor(lv_obj_t* obj, int slot, bool has_sensor);

/**
 * @brief Clear all per-slot filament states
 *
 * Resets all slots to show as idle (no filament installed).
 *
 * @param obj The filament_path_canvas widget
 */
void ui_filament_path_canvas_clear_slot_filaments(lv_obj_t* obj);

/**
 * @brief Show or hide the bypass path entirely
 *
 * When false, the bypass spool, label, path line, and merge sensor are
 * all suppressed.  Used for tool changers where bypass is not a thing.
 * Default: true (bypass drawn when not in hub_only mode).
 *
 * @param obj The filament_path_canvas widget
 * @param show true to draw bypass elements, false to hide them
 */
void ui_filament_path_canvas_set_show_bypass(lv_obj_t* obj, bool show);

/**
 * @brief Set bypass mode active state
 *
 * When bypass is active, shows an alternate filament path from the bypass
 * entry point directly to the toolhead, skipping the MMU slots and hub.
 * Used for external spool feeding.
 *
 * @param obj The filament_path_canvas widget
 * @param active true if bypass mode is active
 */
void ui_filament_path_canvas_set_bypass_active(lv_obj_t* obj, bool active);

/**
 * @brief Set click callback for bypass entry point
 *
 * When user taps on the bypass entry point, this callback is invoked.
 *
 * @param obj The filament_path_canvas widget
 * @param cb Callback function (user_data)
 * @param user_data User data passed to callback
 */
typedef void (*filament_path_bypass_cb_t)(void* user_data);
void ui_filament_path_canvas_set_bypass_callback(lv_obj_t* obj, filament_path_bypass_cb_t cb,
                                                 void* user_data);

/**
 * @brief Set click callback for buffer coil element
 *
 * When user taps on the buffer coil, this callback is invoked.
 *
 * @param obj The filament_path_canvas widget
 * @param cb Callback function (user_data)
 * @param user_data User data passed to callback
 */
typedef void (*filament_path_buffer_cb_t)(void* user_data);
void ui_filament_path_canvas_set_buffer_callback(lv_obj_t* obj, filament_path_buffer_cb_t cb,
                                                 void* user_data);

/**
 * @brief Set hub-only rendering mode
 *
 * When enabled, only draws slots → prep sensors → hub. Skips everything
 * downstream: bypass, output sensor, toolhead sensor, nozzle. Used in the
 * overview panel's inline detail view where system-level routing is shown
 * separately by the system_path_canvas.
 *
 * @param obj The filament_path_canvas widget
 * @param hub_only true to stop rendering at the hub
 */
void ui_filament_path_canvas_set_hub_only(lv_obj_t* obj, bool hub_only);

/**
 * @brief Set nozzle heat active state
 *
 * When heat is active, draws a pulsing orange/red glow around the nozzle tip
 * to indicate the nozzle is heating. Uses an 800ms pulse cycle.
 *
 * @param obj The filament_path_canvas widget
 * @param active true when nozzle is heating, false otherwise
 */
void ui_filament_path_canvas_set_heat_active(lv_obj_t* obj, bool active);

/**
 * @brief Set buffer fault state for hub tinting
 *
 * Tints the hub box based on buffer health:
 *   0 = no fault (normal filament color tint)
 *   1 = approaching fault (yellow tint)
 *   2 = fault detected (red tint)
 *
 * @param obj The filament_path_canvas widget
 * @param state Buffer fault state (0=healthy, 1=warning, 2=fault)
 */
void ui_filament_path_canvas_set_buffer_fault_state(lv_obj_t* obj, int state);

/**
 * @brief Set buffer element info (presence and state)
 *
 * Shows a small buffer/spring icon on the path between hub output and toolhead.
 * Used for AFC TurtleNeck buffers and Happy Hare sync feedback (eSpooler).
 *
 * @param obj The filament_path_canvas widget
 * @param present true to draw the buffer element
 * @param state 0=neutral (even coil), 1=compressed (tight coil), 2=tension (stretched coil)
 */
void ui_filament_path_canvas_set_buffer_info(lv_obj_t* obj, bool present, int state);

/**
 * @brief Set proportional buffer bias for smooth color interpolation
 *
 * When set to a valid value (> -1.5), the buffer coil color interpolates
 * smoothly from green (neutral) through orange to red based on abs(bias).
 * When unavailable (-2.0), falls back to discrete 3-state color logic.
 *
 * @param obj The filament_path_canvas widget
 * @param bias Bias value [-1.0,1.0], or -2.0 for unavailable (discrete mode)
 */
void ui_filament_path_canvas_set_buffer_bias(lv_obj_t* obj, float bias);

/**
 * @brief Set bypass entry filament color
 * @param obj The filament_path_canvas widget
 * @param color RGB color (0xRRGGBB) for bypass filament
 */
void ui_filament_path_canvas_set_bypass_color(lv_obj_t* obj, uint32_t color);

/**
 * @brief Set whether an external spool is assigned to bypass
 * @param obj The filament_path_canvas widget
 * @param has_spool true if spool assigned (shows filled box), false (shows empty indicator)
 */
void ui_filament_path_canvas_set_bypass_has_spool(lv_obj_t* obj, bool has_spool);

#ifdef __cplusplus
}
#endif
