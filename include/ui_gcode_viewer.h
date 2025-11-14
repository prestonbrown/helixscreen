// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 HelixScreen Contributors
 *
 * This file is part of HelixScreen, which is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * See <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_gcode_viewer.h
 * @brief Custom LVGL widget for 3D G-code visualization
 *
 * Provides an interactive 3D viewer widget for G-code files. Integrates
 * GCodeParser, GCodeCamera, and GCodeRenderer for complete visualization.
 *
 * Features:
 * - 3D wireframe rendering of toolpaths
 * - Interactive camera control (rotate, pan, zoom)
 * - Layer filtering and LOD support
 * - Object highlighting for Klipper exclusion
 * - Touch gesture handling
 *
 * Usage:
 * @code
 *   lv_obj_t* viewer = ui_gcode_viewer_create(parent);
 *   ui_gcode_viewer_load_file(viewer, "/path/to/file.gcode");
 *   // or:
 *   ui_gcode_viewer_set_gcode_data(viewer, parsed_data);
 * @endcode
 *
 * @see docs/GCODE_VISUALIZATION.md for complete design
 */

/**
 * @brief Loading state for async file parsing
 */
typedef enum {
    GCODE_VIEWER_STATE_EMPTY,   ///< No file loaded
    GCODE_VIEWER_STATE_LOADING, ///< File is being parsed
    GCODE_VIEWER_STATE_LOADED,  ///< File loaded and ready to render
    GCODE_VIEWER_STATE_ERROR    ///< Error during loading
} gcode_viewer_state_enum_t;

/**
 * @brief Camera preset views
 */
typedef enum {
    GCODE_VIEWER_VIEW_ISOMETRIC, ///< Default isometric view (45°, 30°)
    GCODE_VIEWER_VIEW_TOP,       ///< Top-down view
    GCODE_VIEWER_VIEW_FRONT,     ///< Front view
    GCODE_VIEWER_VIEW_SIDE       ///< Side view (right)
} gcode_viewer_preset_view_t;

/**
 * @brief Create G-code viewer widget
 * @param parent Parent LVGL object
 * @return Widget object or NULL on failure
 *
 * Creates a custom widget with transparent background and custom drawing.
 * Widget handles its own rendering via draw event callbacks.
 */
lv_obj_t* ui_gcode_viewer_create(lv_obj_t* parent);

/**
 * @brief Load G-code file from path
 * @param obj Viewer widget
 * @param file_path Path to G-code file
 *
 * Asynchronously parses the file in background. Use state callback
 * to be notified when loading completes.
 *
 * Note: Async parsing not implemented in Phase 1 - parses synchronously.
 */
void ui_gcode_viewer_load_file(lv_obj_t* obj, const char* file_path);

/**
 * @brief Set G-code data directly (already parsed)
 * @param obj Viewer widget
 * @param gcode_data Parsed G-code file (widget takes ownership)
 *
 * Use this when you've already parsed the file elsewhere.
 * Widget will NOT free the data - caller retains ownership.
 */
void ui_gcode_viewer_set_gcode_data(lv_obj_t* obj, void* gcode_data);

/**
 * @brief Clear loaded G-code
 * @param obj Viewer widget
 *
 * Frees internal G-code data and resets to empty state.
 */
void ui_gcode_viewer_clear(lv_obj_t* obj);

/**
 * @brief Get current loading state
 * @param obj Viewer widget
 * @return Current state
 */
gcode_viewer_state_enum_t ui_gcode_viewer_get_state(lv_obj_t* obj);

// ==============================================
// Camera Controls
// ==============================================

/**
 * @brief Rotate camera view
 * @param obj Viewer widget
 * @param delta_azimuth Horizontal rotation in degrees
 * @param delta_elevation Vertical rotation in degrees
 */
void ui_gcode_viewer_rotate(lv_obj_t* obj, float delta_azimuth, float delta_elevation);

/**
 * @brief Pan camera view
 * @param obj Viewer widget
 * @param delta_x Horizontal pan in world units
 * @param delta_y Vertical pan in world units
 */
void ui_gcode_viewer_pan(lv_obj_t* obj, float delta_x, float delta_y);

/**
 * @brief Zoom camera
 * @param obj Viewer widget
 * @param factor Zoom factor (>1.0 = zoom in, <1.0 = zoom out)
 */
void ui_gcode_viewer_zoom(lv_obj_t* obj, float factor);

/**
 * @brief Reset camera to default view
 * @param obj Viewer widget
 */
void ui_gcode_viewer_reset_camera(lv_obj_t* obj);

/**
 * @brief Set camera to preset view
 * @param obj Viewer widget
 * @param preset Preset view type
 */
void ui_gcode_viewer_set_view(lv_obj_t* obj, gcode_viewer_preset_view_t preset);

// ==============================================
// Rendering Options
// ==============================================

/**
 * @brief Show/hide travel moves
 * @param obj Viewer widget
 * @param show true to show, false to hide
 */
void ui_gcode_viewer_set_show_travels(lv_obj_t* obj, bool show);

/**
 * @brief Show/hide extrusion moves
 * @param obj Viewer widget
 * @param show true to show, false to hide
 */
void ui_gcode_viewer_set_show_extrusions(lv_obj_t* obj, bool show);

/**
 * @brief Set visible layer range
 * @param obj Viewer widget
 * @param start_layer First layer (0-based, inclusive)
 * @param end_layer Last layer (-1 for all remaining)
 */
void ui_gcode_viewer_set_layer_range(lv_obj_t* obj, int start_layer, int end_layer);

/**
 * @brief Set highlighted object
 * @param obj Viewer widget
 * @param object_name Object name to highlight (NULL to clear)
 */
void ui_gcode_viewer_set_highlighted_object(lv_obj_t* obj, const char* object_name);

// ==============================================
// Object Picking (for exclusion UI)
// ==============================================

/**
 * @brief Pick object at screen coordinates
 * @param obj Viewer widget
 * @param x Screen X coordinate
 * @param y Screen Y coordinate
 * @return Object name or NULL if no object picked
 *
 * Result is only valid until next call to this function.
 */
const char* ui_gcode_viewer_pick_object(lv_obj_t* obj, int x, int y);

// ==============================================
// Statistics
// ==============================================

/**
 * @brief Get number of layers in loaded file
 * @param obj Viewer widget
 * @return Layer count or 0 if no file loaded
 */
int ui_gcode_viewer_get_layer_count(lv_obj_t* obj);

/**
 * @brief Get number of segments rendered in last frame
 * @param obj Viewer widget
 * @return Segment count
 */
int ui_gcode_viewer_get_segments_rendered(lv_obj_t* obj);

// ==============================================
// LVGL XML Component Registration
// ==============================================

/**
 * @brief Register gcode_viewer widget with LVGL XML system
 *
 * Must be called during application initialization before loading any XML
 * that uses the <gcode_viewer> tag. Typically called from main() or ui_init().
 *
 * After registration, the widget can be used in XML like:
 * @code{.xml}
 *   <gcode_viewer name="my_viewer" width="100%" height="100%"/>
 * @endcode
 */
void ui_gcode_viewer_register(void);

#ifdef __cplusplus
}
#endif
