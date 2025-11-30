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
 * @brief Callback invoked when async file loading completes
 * @param viewer The viewer widget that finished loading
 * @param user_data User data pointer passed during callback registration
 * @param success true if loading succeeded, false on error
 */
typedef void (*gcode_viewer_load_callback_t)(lv_obj_t* viewer, void* user_data, bool success);

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
 * @brief Set callback to be invoked when async file loading completes
 * @param obj Viewer widget
 * @param callback Callback function (NULL to clear)
 * @param user_data User data passed to callback
 *
 * The callback will be invoked from the main LVGL thread after async
 * geometry building completes. Use this to update UI elements that
 * depend on the loaded file data.
 */
void ui_gcode_viewer_set_load_callback(lv_obj_t* obj, gcode_viewer_load_callback_t callback,
                                       void* user_data);

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

/**
 * @brief Set camera azimuth angle directly
 * @param obj Viewer widget
 * @param azimuth Horizontal rotation in degrees (0-360)
 */
void ui_gcode_viewer_set_camera_azimuth(lv_obj_t* obj, float azimuth);

/**
 * @brief Set camera elevation angle directly
 * @param obj Viewer widget
 * @param elevation Vertical rotation in degrees (-90 to 90)
 */
void ui_gcode_viewer_set_camera_elevation(lv_obj_t* obj, float elevation);

/**
 * @brief Set camera zoom level directly
 * @param obj Viewer widget
 * @param zoom Zoom factor (>0, 1.0 = default)
 */
void ui_gcode_viewer_set_camera_zoom(lv_obj_t* obj, float zoom);

/**
 * @brief Enable/disable per-face debug coloring
 * @param obj Viewer widget
 * @param enable true to enable debug colors, false for normal rendering
 */
void ui_gcode_viewer_set_debug_colors(lv_obj_t* obj, bool enable);

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
// Color & Rendering Control
// ==============================================

/**
 * @brief Set custom extrusion color
 * @param obj Viewer widget
 * @param color Color for extrusion moves
 *
 * Overrides theme default color for extrusions.
 */
void ui_gcode_viewer_set_extrusion_color(lv_obj_t* obj, lv_color_t color);

/**
 * @brief Set custom travel move color
 * @param obj Viewer widget
 * @param color Color for travel moves
 *
 * Overrides theme default color for travels.
 */
void ui_gcode_viewer_set_travel_color(lv_obj_t* obj, lv_color_t color);

/**
 * @brief Enable/disable automatic filament color from G-code metadata
 * @param obj Viewer widget
 * @param enable true to use parsed filament color, false to use theme/custom colors
 *
 * When enabled, extrusion color is automatically set from G-code metadata (if available).
 */
void ui_gcode_viewer_use_filament_color(lv_obj_t* obj, bool enable);

/**
 * @brief Set global rendering opacity
 * @param obj Viewer widget
 * @param opacity Opacity value (0-255, where 255 = fully opaque)
 *
 * Affects all rendered segments. Useful for fade effects or overlays.
 */
void ui_gcode_viewer_set_opacity(lv_obj_t* obj, lv_opa_t opacity);

/**
 * @brief Set brightness factor
 * @param obj Viewer widget
 * @param factor Brightness multiplier (0.5-2.0, where 1.0 = normal)
 *
 * Values > 1.0 brighten colors, < 1.0 darken them.
 */
void ui_gcode_viewer_set_brightness(lv_obj_t* obj, float factor);

/**
 * @brief Set material specular lighting parameters (TinyGL 3D only)
 * @param obj Viewer widget
 * @param intensity Specular intensity (0.0-0.2, where 0.0 = matte, 0.075 = OrcaSlicer default)
 * @param shininess Specular shininess/focus (5.0-50.0, where 20.0 = OrcaSlicer default)
 *
 * Controls the appearance of reflective highlights on G-code extrusion surfaces.
 * Higher intensity = brighter highlights. Higher shininess = tighter/sharper highlights.
 * Only affects TinyGL 3D renderer; ignored by 2D renderer.
 */
void ui_gcode_viewer_set_specular(lv_obj_t* obj, float intensity, float shininess);

// ==============================================
// Layer Control Extensions
// ==============================================

/**
 * @brief Set single layer mode
 * @param obj Viewer widget
 * @param layer Layer number (0-based) to display alone
 *
 * Convenience function equivalent to set_layer_range(layer, layer).
 */
void ui_gcode_viewer_set_single_layer(lv_obj_t* obj, int layer);

/**
 * @brief Get current layer range start
 * @param obj Viewer widget
 * @return Starting layer index
 */
int ui_gcode_viewer_get_current_layer_start(lv_obj_t* obj);

/**
 * @brief Get current layer range end
 * @param obj Viewer widget
 * @return Ending layer index (-1 = all layers)
 */
int ui_gcode_viewer_get_current_layer_end(lv_obj_t* obj);

// ==============================================
// Print Progress / Ghost Layer Visualization
// ==============================================

/**
 * @brief Set print progress layer for ghost visualization
 * @param obj Viewer widget
 * @param current_layer Layer index representing current print progress.
 *                      Layers 0..current_layer render solid (printed).
 *                      Layers current_layer+1..max render as dimmed ghost (unprinted).
 *                      Set to -1 to disable ghost mode (render all solid).
 *
 * This enables a two-pass rendering mode useful for visualizing print progress
 * during a print job. The "ghost" layers appear dimmed/faded to indicate
 * they haven't been printed yet.
 *
 * Performance: Layer changes are instant (<1ms) - no geometry rebuild needed.
 */
void ui_gcode_viewer_set_print_progress(lv_obj_t* obj, int current_layer);

/**
 * @brief Set ghost layer opacity
 * @param obj Viewer widget
 * @param opacity Opacity value (0=invisible, 255=fully opaque, default: 77 = ~30%)
 *
 * Controls how visible the ghost (unprinted) layers appear.
 */
void ui_gcode_viewer_set_ghost_opacity(lv_obj_t* obj, lv_opa_t opacity);

/**
 * @brief Set ghost layer rendering mode
 * @param obj Viewer widget
 * @param mode Rendering mode: 0=Dimmed, 1=Stipple, 2=Wireframe, 4=DepthOnly
 *
 * Controls how ghost (unprinted) layers are rendered:
 * - 0 (Dimmed): Darker color but fully opaque (default)
 * - 1 (Stipple): Screen-door transparency pattern
 * - 2 (Wireframe): Only edges visible
 * - 4 (DepthOnly): No depth write - see through to solid layers
 */
void ui_gcode_viewer_set_ghost_mode(lv_obj_t* obj, int mode);

/**
 * @brief Get maximum layer index in current geometry
 * @param obj Viewer widget
 * @return Max layer index (0-based), or -1 if no geometry loaded
 */
int ui_gcode_viewer_get_max_layer(lv_obj_t* obj);

// ==============================================
// Metadata Access
// ==============================================

/**
 * @brief Get filament color from G-code metadata
 * @param obj Viewer widget
 * @return Hex color string (e.g., "#26A69A") or NULL if not available
 *
 * String is valid until next file load.
 */
const char* ui_gcode_viewer_get_filament_color(lv_obj_t* obj);

/**
 * @brief Get filament type from metadata
 * @param obj Viewer widget
 * @return Filament type (e.g., "PLA", "PETG") or NULL if not available
 */
const char* ui_gcode_viewer_get_filament_type(lv_obj_t* obj);

/**
 * @brief Get printer model from metadata
 * @param obj Viewer widget
 * @return Printer model name or NULL if not available
 */
const char* ui_gcode_viewer_get_printer_model(lv_obj_t* obj);

/**
 * @brief Get estimated print time
 * @param obj Viewer widget
 * @return Print time in minutes, or 0.0 if not available
 */
float ui_gcode_viewer_get_estimated_time_minutes(lv_obj_t* obj);

/**
 * @brief Get filament weight
 * @param obj Viewer widget
 * @return Filament weight in grams, or 0.0 if not available
 */
float ui_gcode_viewer_get_filament_weight_g(lv_obj_t* obj);

/**
 * @brief Get filament length
 * @param obj Viewer widget
 * @return Filament length in mm, or 0.0 if not available
 */
float ui_gcode_viewer_get_filament_length_mm(lv_obj_t* obj);

/**
 * @brief Get filament cost
 * @param obj Viewer widget
 * @return Estimated cost, or 0.0 if not available
 */
float ui_gcode_viewer_get_filament_cost(lv_obj_t* obj);

/**
 * @brief Get nozzle diameter
 * @param obj Viewer widget
 * @return Nozzle diameter in mm, or 0.0 if not available
 */
float ui_gcode_viewer_get_nozzle_diameter_mm(lv_obj_t* obj);

// ==============================================
// Statistics
// ==============================================

/**
 * @brief Get loaded filename
 * @param obj Viewer widget
 * @return Filename string or NULL if no file loaded
 *
 * Returns the filename from the loaded G-code file. String is valid
 * until next file load.
 */
const char* ui_gcode_viewer_get_filename(lv_obj_t* obj);

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

// ==============================================
// C++ API Extensions
// ==============================================

#include <string>
#include <unordered_set>

/**
 * @brief Set highlighted objects (multi-select support)
 * @param obj Viewer widget
 * @param object_names Set of object names to highlight (empty to clear all)
 *
 * Allows multiple objects to be highlighted simultaneously. Objects in the set
 * will be rendered with brightened colors and bounding box wireframes.
 */
void ui_gcode_viewer_set_highlighted_objects(lv_obj_t* obj,
                                             const std::unordered_set<std::string>& object_names);

/**
 * @brief Set excluded objects
 * @param obj Viewer widget
 * @param object_names Set of object names that are excluded from print
 *
 * Excluded objects are rendered with a red/orange strikethrough style at
 * reduced opacity to indicate they won't be printed. Use this to sync the
 * visual state with Klipper's exclude_object feature.
 */
void ui_gcode_viewer_set_excluded_objects(lv_obj_t* obj,
                                          const std::unordered_set<std::string>& object_names);

/**
 * @brief Callback type for object tap events
 * @param viewer The viewer widget
 * @param object_name Name of the tapped object (empty if no object hit)
 * @param user_data User-provided context
 */
typedef void (*gcode_viewer_object_tap_callback_t)(lv_obj_t* viewer, const char* object_name,
                                                   void* user_data);

/**
 * @brief Register callback for object tap events
 * @param obj Viewer widget
 * @param callback Function to call when an object is tapped (NULL to clear)
 * @param user_data User data passed to callback
 *
 * The callback is invoked when user taps on an object in the 3D view.
 * Use this to implement exclude object confirmation UI.
 */
void ui_gcode_viewer_set_object_tap_callback(lv_obj_t* obj, gcode_viewer_object_tap_callback_t callback,
                                             void* user_data);

/**
 * @brief Callback type for object long-press events
 * @param viewer The viewer widget
 * @param object_name Name of the long-pressed object (empty if no object hit)
 * @param user_data User-provided context
 *
 * Long-press is triggered after holding for 500ms without moving.
 */
typedef void (*gcode_viewer_object_long_press_callback_t)(lv_obj_t* viewer, const char* object_name,
                                                          void* user_data);

/**
 * @brief Register callback for object long-press events
 * @param obj Viewer widget
 * @param callback Function to call when an object is long-pressed (NULL to clear)
 * @param user_data User data passed to callback
 *
 * The callback is invoked when user long-presses (500ms) on an object without moving.
 * Use this to implement the exclude object confirmation flow - long-press to exclude
 * is more intentional than tap, preventing accidental exclusions.
 */
void ui_gcode_viewer_set_object_long_press_callback(lv_obj_t* obj,
                                                     gcode_viewer_object_long_press_callback_t callback,
                                                     void* user_data);

#endif
