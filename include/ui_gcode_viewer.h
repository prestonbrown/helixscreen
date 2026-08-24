// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl/lvgl.h>

#ifdef __cplusplus
namespace helix {

/**
 * @brief Loading state for async file parsing
 */
enum class GcodeViewerState {
    Empty,   ///< No file loaded
    Loading, ///< File is being parsed
    Loaded,  ///< File loaded and ready to render
    Error    ///< Error during loading
};

/**
 * @brief Render mode for G-code visualization
 *
 * Controls which renderer is used for displaying G-code:
 * - Auto: Uses GLES 3D if available, falls back to 2D layer view.
 *         Can be overridden via HELIX_GCODE_MODE env var.
 * - Render3D: Forces 3D GLES renderer (isometric ribbon view with full camera control).
 * - Layer2D: Forces 2D orthographic layer view (front/top view, single layer at a time)
 *
 * Environment variable override (checked at widget creation):
 * - HELIX_GCODE_MODE=3D  -> Use 3D GLES renderer
 * - HELIX_GCODE_MODE=2D  -> Use 2D layer view (explicit)
 * - Not set              -> Auto-detect
 */
enum class GcodeViewerRenderMode {
    Auto,     ///< Auto-select (GLES 3D if available, else 2D)
    Render3D, ///< Force 3D GLES renderer
    Layer2D   ///< Force 2D orthographic layer view
};

/**
 * @brief Camera preset views
 */
enum class GcodeViewerPresetView {
    Isometric, ///< Default isometric view (45 deg, 30 deg)
    Top,       ///< Top-down view
    Front,     ///< Front view
    Side       ///< Side view (right)
};

} // namespace helix

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
 * @endcode
 */

/**
 * @brief Callback invoked when async file loading completes
 * @param viewer The viewer widget that finished loading
 * @param user_data User data pointer passed during callback registration
 * @param success true if loading succeeded, false on error
 */
typedef void (*gcode_viewer_load_callback_t)(lv_obj_t* viewer, void* user_data, bool success);

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
 * @brief Register a one-shot callback that fires when the viewer produces its
 *        first complete rendered frame (after VBO upload / parse, not on a
 *        skipped or failed frame).
 *
 * Callers that overlay a thumbnail behind the viewer use this to defer hiding
 * the thumbnail until the viewer actually has pixels to show, avoiding a gray
 * flash during the load-to-render gap.
 *
 * The callback is invoked deferred (UpdateQueue), not inside the draw pass, so
 * it may safely write subjects and hide widgets. Pass a null callback to
 * unregister — required from any teardown that outlives the viewer widget, or
 * the stored user_data pointer and whatever it owns (subjects, `this`) are
 * dangling by the time the next frame renders.
 *
 * @param obj       Viewer widget
 * @param callback  Fires once with success=true when the first frame renders,
 *                  or nullptr to unregister
 * @param user_data Passed through to the callback
 */
void ui_gcode_viewer_set_first_frame_callback(lv_obj_t* obj, gcode_viewer_load_callback_t callback,
                                              void* user_data);

/**
 * @brief Clear loaded G-code
 * @param obj Viewer widget
 *
 * Frees internal G-code data and resets to empty state.
 */
void ui_gcode_viewer_clear(lv_obj_t* obj);

/**
 * @brief Clear every live G-code viewer widget process-wide
 *
 * Iterates the internal registry of created viewers and calls
 * ui_gcode_viewer_clear() on each. Used by the memory pressure responder
 * to release ParsedGCodeFile + renderer geometry on every active viewer
 * (print_status + print_select_detail) when the system goes low on memory.
 *
 * Safe to call from the main thread only.
 */
void ui_gcode_viewer_clear_all_active();

/**
 * @brief Install a callback invoked when this viewer is cleared
 *
 * Fires from inside ui_gcode_viewer_clear() (and therefore also from
 * ui_gcode_viewer_clear_all_active()). The owning panel uses this to flip
 * its mode subject back to thumbnail so the user doesn't see a transparent
 * rectangle where the rendered model used to be.
 *
 * Set during panel widget setup; auto-fired on every clear thereafter.
 */
typedef void (*ui_gcode_viewer_clear_cb_t)(lv_obj_t* viewer, void* user_data);
void ui_gcode_viewer_set_clear_callback(lv_obj_t* obj, ui_gcode_viewer_clear_cb_t cb,
                                        void* user_data);

/**
 * @brief Query whether the viewer currently holds renderable geometry
 *
 * Returns true after a successful load_file, false after
 * ui_gcode_viewer_clear() or before any load (and false if @p obj is null).
 * Used by the print status panel to reconcile the preview against the real
 * widget state on re-entry instead of trusting intent bools.
 *
 * @param obj Viewer widget (may be null)
 * @return true if geometry is loaded and renderable
 */
bool ui_gcode_viewer_has_content(lv_obj_t* obj);

// ==============================================
// Rendering Pause Control
// ==============================================

/**
 * @brief Pause or resume rendering
 * @param obj Viewer widget
 * @param paused true to pause rendering (skip draw callbacks), false to resume
 *
 * When paused, the draw callback returns immediately without performing
 * any 3D rendering. Use this to stop rendering when the viewer is
 * not visible (panel navigated away, obscured by overlay, or in thumbnail mode).
 *
 * Resuming triggers an immediate invalidate to refresh the view.
 */
void ui_gcode_viewer_set_paused(lv_obj_t* obj, bool paused);

/**
 * @brief Check if rendering is paused
 * @param obj Viewer widget
 * @return true if rendering is currently paused
 */
bool ui_gcode_viewer_is_paused(lv_obj_t* obj);

/**
 * @brief Force a full redraw of the viewer
 *
 * Marks the cached frame stale (3D renderer's frame_dirty_, 2D's last-render
 * state) and invalidates the widget so DRAW_POST regenerates the image from
 * scratch. Use on display wake — the cached-blit fast path can leave a blank
 * widget when LVGL's image cache is invalidated by the framebuffer unblank.
 */
void ui_gcode_viewer_force_redraw(lv_obj_t* obj);

// ==============================================
// Render Mode Control
// ==============================================

/**
 * @brief Set render mode (AUTO, 3D, or 2D Layer view)
 * @param obj Viewer widget
 * @param mode Render mode to use
 *
 * - AUTO: Uses GLES 3D if available, falls back to 2D layer view
 * - 3D: Forces 3D GLES renderer with full camera control
 * - 2D_LAYER: Forces top-down orthographic single-layer view (fast on AD5M)
 *
 * Default is AUTO. Settings are persisted in SettingsManager.
 */
void ui_gcode_viewer_set_render_mode(lv_obj_t* obj, helix::GcodeViewerRenderMode mode);

/**
 * @brief Check if currently using 2D layer renderer
 * @param obj Viewer widget
 * @return true if 2D layer renderer is active (either forced or via AUTO fallback)
 */
bool ui_gcode_viewer_is_using_2d_mode(lv_obj_t* obj);

/**
 * @brief Disable streaming mode for this viewer instance.
 *
 * When disabled, large files will use full-load + budget system instead of
 * streaming 2D layer renderer. Use for detail panel previews where 3D is
 * preferred and 2D streaming is not useful.
 */
void ui_gcode_viewer_disable_streaming(lv_obj_t* obj);

// ==============================================
// Camera Controls
// ==============================================

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
void ui_gcode_viewer_set_view(lv_obj_t* obj, helix::GcodeViewerPresetView preset);

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
 * @brief Set material specular lighting parameters (3D only)
 * @param obj Viewer widget
 * @param intensity Specular intensity (0.0-0.2, where 0.0 = matte, 0.075 = OrcaSlicer default)
 * @param shininess Specular shininess/focus (5.0-50.0, where 20.0 = OrcaSlicer default)
 *
 * Controls the appearance of reflective highlights on G-code extrusion surfaces.
 * Higher intensity = brighter highlights. Higher shininess = tighter/sharper highlights.
 * Only affects 3D GLES renderer; ignored by 2D renderer.
 */
void ui_gcode_viewer_set_specular(lv_obj_t* obj, float intensity, float shininess);

// ==============================================
// Layer Control Extensions
// ==============================================

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
 * @brief Name the widget that covers the bottom of this viewer.
 * @param obj      Viewer widget
 * @param occluder Overlapping widget (the translucent metadata strip), or null
 *                 to clear
 *
 * The viewer measures the real overlap on every draw and derives its vertical
 * shift from that plus the model's fitted height, so the framing follows
 * breakpoints, orientation, and the strip growing at runtime with no repush.
 * Layouts where the strip sits flush BELOW the preview measure zero overlap and
 * get no shift, which is the correct answer there.
 *
 * The reference is dropped by the occluder's own LV_EVENT_DELETE, so callers do
 * not have to unwire it during teardown.
 *
 * @see helix::gcode::compute_content_offset_y() for the rule being applied.
 */
void ui_gcode_viewer_set_bottom_occluder(lv_obj_t* obj, lv_obj_t* occluder);

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
 * @brief Get filament type from metadata
 * @param obj Viewer widget
 * @return Filament type (e.g., "PLA", "PETG") or NULL if not available
 */
const char* ui_gcode_viewer_get_filament_type(lv_obj_t* obj);

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

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

/**
 * @brief Set per-tool AMS color overrides for multi-color prints
 * @param obj Viewer widget
 * @param colors Vector of RGB colors indexed by tool (0xRRGGBB)
 *
 * Overrides slicer-embedded colors with real AMS filament colors.
 * For 3D: updates geometry palette and triggers VBO re-upload.
 * For 2D: replaces tool_palette_ entries; resolves at render time.
 */
void ui_gcode_viewer_set_tool_colors(lv_obj_t* obj, const std::vector<uint32_t>& colors);

/**
 * @brief Apply AMS filament colors to the viewer from AmsState
 * @param obj Viewer widget
 * @return true if AMS colors were applied, false if no AMS backend or all defaults
 *
 * Reads the current AMS tool-to-slot mapping and applies slot colors.
 * Shared by print status panel and print file detail view.
 */
bool ui_gcode_viewer_apply_ams_tool_colors(lv_obj_t* obj);

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
void ui_gcode_viewer_set_object_tap_callback(lv_obj_t* obj,
                                             gcode_viewer_object_tap_callback_t callback,
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
void ui_gcode_viewer_set_object_long_press_callback(
    lv_obj_t* obj, gcode_viewer_object_long_press_callback_t callback, void* user_data);

// ==============================================
// Parsed Data Access
// ==============================================

namespace helix::gcode {
struct ParsedGCodeFile;
} // namespace helix::gcode

/**
 * @brief Get the parsed G-code file data from the viewer
 * @param obj Viewer widget
 * @return Pointer to parsed G-code file, or nullptr if no data loaded or segments cleared
 *
 * The returned pointer is valid as long as the viewer widget exists and has data loaded.
 * In streaming mode, this returns nullptr (streaming mode doesn't hold the full file).
 */
const helix::gcode::ParsedGCodeFile* ui_gcode_viewer_get_parsed_file(lv_obj_t* obj);

#endif
