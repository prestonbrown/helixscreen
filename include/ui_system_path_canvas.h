// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_system_path_canvas.h
 * @brief System path visualization widget for multi-unit AMS overview panel
 *
 * Draws a schematic "system path" showing unit outputs converging through
 * a combiner/hub to a single toolhead. Each unit is represented by an
 * entry point at the top, with lines converging to a central combiner box,
 * then a single output line to the nozzle.
 *
 * Visual layout (vertical, top to bottom):
 *   - Entry points at top (one per unit, at unit_x_positions[])
 *   - Lines from each unit converging toward center_x
 *   - Hub box labeled "Hub"
 *   - Single output line down from hub
 *   - Simplified nozzle icon at bottom
 *
 * Visual states:
 *   - Inactive unit: Thin gray dashed line
 *   - Active unit: Thick line in filament color
 *   - Filament loaded: Color path from active unit through hub to nozzle
 *
 * XML usage:
 * @code{.xml}
 * <system_path_canvas name="sys_path"
 *                     width="100%" height="150"/>
 * @endcode
 */

/**
 * @brief Register the system_path_canvas widget with LVGL's XML system
 *
 * Must be called BEFORE any XML files using <system_path_canvas> are registered.
 */
void ui_system_path_canvas_register(void);

/**
 * @brief Create a system path canvas widget programmatically
 *
 * @param parent Parent LVGL object
 * @return Created widget or NULL on failure
 */
lv_obj_t* ui_system_path_canvas_create(lv_obj_t* parent);

/**
 * @brief Set the number of units
 *
 * @param obj The system_path_canvas widget
 * @param count Number of units (1-8)
 */
void ui_system_path_canvas_set_unit_count(lv_obj_t* obj, int count);

/**
 * @brief Set the X center position for a unit's entry point
 *
 * @param obj The system_path_canvas widget
 * @param unit_index Unit index (0-7)
 * @param center_x X pixel position of the unit card center
 */
void ui_system_path_canvas_set_unit_x(lv_obj_t* obj, int unit_index, int32_t center_x);

/**
 * @brief Set the active unit (whose path is highlighted)
 *
 * @param obj The system_path_canvas widget
 * @param unit_index Unit index (0+), or -1 for none
 */
void ui_system_path_canvas_set_active_unit(lv_obj_t* obj, int unit_index);

/**
 * @brief Set the active filament color
 *
 * @param obj The system_path_canvas widget
 * @param color RGB color (0xRRGGBB)
 */
void ui_system_path_canvas_set_active_color(lv_obj_t* obj, uint32_t color);

/**
 * @brief Set whether filament is loaded through to the nozzle
 *
 * When true, the active unit's path is colored all the way through
 * the combiner hub to the nozzle.
 *
 * @param obj The system_path_canvas widget
 * @param loaded true if filament reaches nozzle
 */
void ui_system_path_canvas_set_filament_loaded(lv_obj_t* obj, bool loaded);

/**
 * @brief Set the status text drawn to the left of the nozzle
 *
 * @param obj The system_path_canvas widget
 * @param text Status text (e.g., "Idle", "Loading T0"), or NULL to clear
 */
void ui_system_path_canvas_set_status_text(lv_obj_t* obj, const char* text);

/**
 * @brief Set bypass path state
 *
 * Configures the bypass filament path (direct feed to toolhead, bypassing AMS units).
 * Bypass is drawn inside the canvas to the right of the hub area (no external card).
 *
 * @param obj The system_path_canvas widget
 * @param has_bypass Whether to show the bypass path at all
 * @param bypass_active Whether bypass is the active path (current_slot == -2)
 * @param bypass_color RGB color when bypass is active (0xRRGGBB)
 */
void ui_system_path_canvas_set_bypass(lv_obj_t* obj, bool has_bypass, bool bypass_active,
                                      uint32_t bypass_color);

/// Set whether an external spool is assigned to bypass
void ui_system_path_canvas_set_bypass_has_spool(lv_obj_t* obj, bool has_spool);

/// Callback type for bypass spool clicks
typedef void (*system_path_bypass_cb_t)(void* user_data);

/// Set callback for when the bypass spool box is clicked
void ui_system_path_canvas_set_bypass_callback(lv_obj_t* obj, system_path_bypass_cb_t cb,
                                               void* user_data);

/**
 * @brief Set per-unit hub sensor state
 *
 * Each unit can have its own hub sensor, drawn on that unit's line near
 * the merge area. The system-level hub sensor concept is replaced by
 * per-unit hub sensors.
 *
 * @param obj The system_path_canvas widget
 * @param unit_index Unit index (0-7)
 * @param has_sensor Whether this unit has a hub sensor
 * @param triggered Whether filament is detected at this unit's hub sensor
 */
void ui_system_path_canvas_set_unit_hub_sensor(lv_obj_t* obj, int unit_index, bool has_sensor,
                                               bool triggered);

/**
 * @brief Set toolhead sensor state
 *
 * The toolhead sensor sits on the output line between hub and nozzle.
 *
 * @param obj The system_path_canvas widget
 * @param has_toolhead_sensor Whether the system has a toolhead entry sensor
 * @param toolhead_sensor_triggered Whether filament is detected at the toolhead sensor
 */
void ui_system_path_canvas_set_toolhead_sensor(lv_obj_t* obj, bool has_toolhead_sensor,
                                               bool toolhead_sensor_triggered);

/**
 * @brief Set per-unit tool routing info
 *
 * For mixed topology systems, each unit can route to different tools:
 * - PARALLEL units (Box Turtle): 4 lanes fan out to T0-T3 (4 separate tools)
 * - HUB units (OpenAMS): all lanes converge to 1 tool (e.g., T4)
 *
 * @param obj The system_path_canvas widget
 * @param unit_index Unit index (0-7)
 * @param tool_count Number of tools this unit feeds (BT=4, OpenAMS=1)
 * @param first_tool First tool index for this unit (e.g., 0 for BT, 4 for OpenAMS)
 */
void ui_system_path_canvas_set_unit_tools(lv_obj_t* obj, int unit_index, int tool_count,
                                          int first_tool);

/**
 * @brief Set per-unit topology
 *
 * @param obj The system_path_canvas widget
 * @param unit_index Unit index (0-7)
 * @param topology 0=LINEAR, 1=HUB, 2=PARALLEL
 */
void ui_system_path_canvas_set_unit_topology(lv_obj_t* obj, int unit_index, int topology);

/**
 * @brief Set total tool count across all units
 *
 * @param obj The system_path_canvas widget
 * @param total_tools Total number of tools (e.g., 6 for BT(4) + OpenAMS(1) + OpenAMS(1))
 */
void ui_system_path_canvas_set_total_tools(lv_obj_t* obj, int total_tools);

/**
 * @brief Set the currently active tool
 *
 * @param obj The system_path_canvas widget
 * @param tool_index Active tool index (0+), or -1 for none
 */
void ui_system_path_canvas_set_active_tool(lv_obj_t* obj, int tool_index);

/**
 * @brief Set the virtual tool number (slot-based) for single-nozzle label
 *
 * When multiple slots feed a single toolhead, this shows the virtual tool
 * number (e.g. "T3" when slot 3 is loaded) next to the nozzle.
 *
 * @param obj The system_path_canvas widget
 * @param tool_index Virtual tool index, or -1 for none
 */
void ui_system_path_canvas_set_current_tool(lv_obj_t* obj, int tool_index);

/**
 * @brief Set virtual tool numbers for badge labels
 *
 * Maps physical nozzle positions to AFC virtual tool numbers for display.
 * When set, tool badges show the virtual number (e.g., "T4") instead of
 * the physical position index (e.g., "T1").
 *
 * @param obj The system_path_canvas widget
 * @param numbers Array of virtual tool numbers (one per physical tool)
 * @param count Number of entries in the array
 */
void ui_system_path_canvas_set_tool_virtual_numbers(lv_obj_t* obj, const int* numbers, int count);

/**
 * @brief Force redraw of the path visualization
 *
 * @param obj The system_path_canvas widget
 */
void ui_system_path_canvas_refresh(lv_obj_t* obj);

#ifdef __cplusplus
}
#endif
