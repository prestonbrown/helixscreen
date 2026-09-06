// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_filament_path_internal.h
 * @brief Shared internals of the filament_path_canvas widget (AMS detail panel).
 *
 * ARCHITECTURE
 *
 * The widget follows the project's XML → Subjects → C++ pattern: panels create
 * it from XML (or via ui_filament_path_canvas_create) and push printer state in
 * through the C setters in ui_filament_path_canvas.h. All per-widget state
 * lives in one FilamentPathData, owned by a registry keyed on the lv_obj_t*.
 *
 * Rendering is split across three layers so per-frame animation never repaints
 * the expensive tube geometry:
 *   1. static canvas  — lv_canvas child, reserved for state-independent content
 *   2. overlay canvas — lv_canvas child; the full topology render (lanes, hub,
 *      sensors, nozzle) painted by the active topology renderer
 *   3. DRAW_POST pass — flow dots / heat glow / moving filament tip, drawn
 *      directly every frame on top of the cached canvases
 * Setters call layered_mark_dirty() which flags the canvases and schedules an
 * async repaint (ui_filament_path_layers.cpp). Animation ticks only invalidate
 * the widget, re-running the cheap DRAW_POST pass.
 *
 * The render pass also RECORDS two things consumed later:
 *   - the active filament path (FilamentPathData::path_cache) — replayed by the
 *     DRAW_POST pass to place flow dots and the segment-transition tip
 *   - hit rectangles (FilamentPathData::hits) — the exact boxes drawn for the
 *     hub/selector, buffer, and bypass, read by the click handler so taps test
 *     against precisely what is on screen (no geometry re-derivation)
 *
 * FILE MAP
 *   ui_filament_path_canvas.cpp   widget lifecycle, theme, click dispatch, C API
 *   ui_filament_path_topology.cpp the three topology renderers + DRAW_POST pass
 *   ui_filament_path_glyphs.cpp   sensor dots, hub box, buffer, nozzle, badges
 *   ui_filament_path_anim.cpp     the five lv_anim-driven animation systems
 *   ui_filament_path_layers.cpp   canvas buffer management + async refresh
 */

#include "ui_coalesced_timer.h"
#include "ui_filament_path_canvas.h"

#include "ams_types.h"
#include "filament_path_geometry.h"
#include "filament_tube_stroker.h"
#include "lvgl/lvgl.h"

#include <array>

namespace helix::ui::fpath {

namespace pg = helix::ui::pathgeo;

// ============================================================================
// Constants
// ============================================================================

// Default dimensions
inline constexpr int32_t DEFAULT_WIDTH = 300;
inline constexpr int32_t DEFAULT_HEIGHT = 200;
inline constexpr int DEFAULT_SLOT_COUNT = 4;

// Nozzle tip color when no filament is loaded (light charcoal)
inline constexpr uint32_t NOZZLE_UNLOADED_COLOR = 0x3A3A3A;

// LINEAR/HUB layout ratios (as fraction of widget height)
// Entry points at very top to connect visually with slot grid above
inline constexpr float ENTRY_Y_RATIO =
    -0.12f; // Top entry points (above canvas, very close to spool box)
inline constexpr float PREP_Y_RATIO = 0.10f;     // Prep sensor position
inline constexpr float MERGE_Y_RATIO = 0.20f;    // Where lanes merge
inline constexpr float HUB_Y_RATIO = 0.30f;      // Hub/selector center
inline constexpr float HUB_HEIGHT_RATIO = 0.10f; // Hub box height
// Note: output sensor Y is computed as hub_bottom (butted against hub, no separate ratio)
inline constexpr float TOOLHEAD_Y_RATIO = 0.68f; // Toolhead sensor
inline constexpr float NOZZLE_Y_RATIO =
    0.82f; // Nozzle/extruder center (needs more room for larger extruder)

// Bypass position (right side of widget)
inline constexpr float BYPASS_X_RATIO = 0.85f; // Right side for bypass spool
// Bypass merge point: where bypass path joins the center path, BELOW the hub output sensor.
// This is where a physical or virtual bypass sensor lives.
inline constexpr float BYPASS_MERGE_Y_RATIO = 0.58f;
// Buffer element position (between hub output and bypass merge)
inline constexpr float BUFFER_Y_RATIO = 0.46f;

// PARALLEL topology (tool changer) Y ratios. Shared by the PARALLEL renderer
// and the PARALLEL branch of the click hit-test so the two never drift.
inline constexpr float PARALLEL_SENSOR_Y_RATIO = 0.38f;   // Toolhead entry sensor
inline constexpr float PARALLEL_TOOLHEAD_Y_RATIO = 0.55f; // Nozzle/toolhead per slot

// Slot-entry click hit-test padding (click handler only; no renderer
// counterpart — these widen the entry band so taps near the spool grid still
// register on the nearest slot).
inline constexpr int32_t ENTRY_HIT_MARGIN_TOP = 10;    // px above entry_y
inline constexpr int32_t ENTRY_HIT_MARGIN_BOTTOM = 20; // px below prep_y

// Line widths (scaled by space_xs for responsiveness)
inline constexpr int LINE_WIDTH_IDLE_BASE = 2;
inline constexpr int LINE_WIDTH_ACTIVE_BASE = 3;
inline constexpr int SENSOR_RADIUS_BASE = 4;

// Default filament color (used when no active filament)
inline constexpr uint32_t DEFAULT_FILAMENT_COLOR = 0x4488FF;

// Animation constants
inline constexpr int SEGMENT_ANIM_DURATION_MS = 300; // Duration for segment-to-segment animation
inline constexpr int ERROR_PULSE_DURATION_MS = 800;  // Error pulse cycle duration
inline constexpr lv_opa_t ERROR_PULSE_OPA_MIN = 100; // Minimum opacity during error pulse
inline constexpr lv_opa_t ERROR_PULSE_OPA_MAX = 255; // Maximum opacity during error pulse
inline constexpr int HEAT_PULSE_DURATION_MS = 800;   // Heat pulse cycle duration
inline constexpr lv_opa_t HEAT_PULSE_OPA_MIN = 100;  // Minimum opacity during heat pulse
inline constexpr lv_opa_t HEAT_PULSE_OPA_MAX = 255;  // Maximum opacity during heat pulse
inline constexpr int FLOW_ANIM_DURATION_MS = 1500;   // Full cycle for flow dot animation
inline constexpr int FLOW_DOT_SPACING = 20;          // Pixels between flow dots
inline constexpr int FLOW_DOT_RADIUS = 1;            // Radius of each flow particle
inline constexpr lv_opa_t FLOW_DOT_OPA = 90;         // Opacity of flow dots
inline constexpr int OUTPUT_X_ANIM_DURATION_MS = 250;

// ============================================================================
// Widget state
// ============================================================================

// Animation direction
enum class AnimDirection {
    NONE = 0,
    LOADING = 1,  // Animating toward nozzle
    UNLOADING = 2 // Animating away from nozzle
};

// Per-slot filament state for visualizing all installed filaments
struct SlotFilamentState {
    PathSegment segment = PathSegment::NONE; // How far filament extends
    uint32_t color = 0x808080;               // Filament color (gray default)
};

// Theme-derived colors, sizes, and font — loaded once by load_theme_colors()
// and cached so the render path never queries the theme manager per frame.
struct ThemeCache {
    lv_color_t color_idle;
    lv_color_t color_error;
    lv_color_t color_hub_bg;
    lv_color_t color_hub_border;
    lv_color_t color_nozzle;
    lv_color_t color_text;
    lv_color_t color_bg;      // Canvas background (for hollow tube bore)
    lv_color_t color_success; // Success color (cached for draw callbacks)

    int32_t line_width_idle = LINE_WIDTH_IDLE_BASE;
    int32_t line_width_active = LINE_WIDTH_ACTIVE_BASE;
    int32_t sensor_radius = SENSOR_RADIUS_BASE;
    int32_t hub_width = 60;
    int32_t border_radius = 6;
    int32_t extruder_scale = 10; // Scale unit for extruder (based on space_md)

    const lv_font_t* label_font = nullptr;
};

// State of the five lv_anim-driven animation systems
// (ui_filament_path_anim.cpp). The render path only READS these; the anim
// callbacks write them and invalidate the widget.
struct AnimState {
    // Segment transition (filament tip slides between path segments)
    int prev_segment = 0; // Previous segment (for smooth transition)
    AnimDirection direction = AnimDirection::NONE;
    bool segment_active = false; // Segment transition animation running
    int progress = 0;            // Transition progress 0-100

    // Error pulse (error segment opacity throb)
    bool error_pulse_active = false;
    lv_opa_t error_pulse_opa = LV_OPA_COVER;

    // Heat glow pulse around the nozzle tip
    bool heat_active = false; // input: nozzle actively heating (drives the pulse)
    bool heat_pulse_active = false;
    lv_opa_t heat_pulse_opa = LV_OPA_COVER;

    // Flow particles along the active path during load/unload
    bool flow_active = false;
    int32_t flow_offset = 0; // 0 → FLOW_DOT_SPACING, cycles continuously

    // Output-X slide (LINEAR: output exits beneath active slot)
    int32_t output_x_current = 0;
    int32_t output_x_target = 0;
    bool output_x_active = false;
};

// Layered renderer state. The widget hosts two lv_canvas children backed by
// ARGB8888 draw_bufs; LVGL composites them natively under a DRAW_POST
// animation pass. Managed by ui_filament_path_layers.cpp.
struct LayerState {
    lv_obj_t* static_canvas = nullptr;
    lv_obj_t* overlay_canvas = nullptr;
    lv_draw_buf_t* static_buf = nullptr;
    lv_draw_buf_t* overlay_buf = nullptr;
    bool static_dirty = true;  // canvas needs repaint of idle topology
    bool overlay_dirty = true; // canvas needs repaint of state-tied content
    int32_t canvas_w = 0;      // current canvas buffer size — tracks widget resize
    int32_t canvas_h = 0;
    // Carries the pending out-of-render-pass repaint. Leading-edge, so a state
    // update that moves ten setters costs one repaint rather than ten, and the
    // repaint still lands on the next tick however many setters follow. Owning
    // it here is also what makes the repaint safe: this state dies with the
    // widget's FilamentPathData, and ~CoalescedTimer cancels the timer, so a
    // scheduled repaint can never reach a freed widget.
    helix::ui::CoalescedTimer refresh_timer{0};
};

// Hit rectangles recorded by the renderer (absolute display coords) so the
// click handler tests against EXACTLY what was drawn. The LINEAR selector's Y
// is butted against the prep sensors and its width spans the slot row; the
// buffer box internally clamps its size; the bypass rect tracks the visibility
// gate (!hub_only && show_bypass). Any re-derivation in the click handler
// would drift from the visible geometry. Single source of truth: render
// writes, click reads. valid flags reset each render.
struct HitRects {
    lv_area_t hub = {0, 0, 0, 0};
    bool hub_valid = false;
    lv_area_t buffer = {0, 0, 0, 0};
    bool buffer_valid = false;
    lv_area_t bypass = {0, 0, 0, 0};
    bool bypass_valid = false;
};

// LINEAR/HUB active-path cache. Populated by the state-tied renderer;
// consumed by the animation DRAW_POST pass (flow dots, segment tip position)
// without re-running the state-tied code each animation tick. Path length is
// cheap to recompute from the path so it isn't cached.
struct PathCache {
    pg::FilamentPath path = {};
    int32_t center_x = 0; // last state-tied draw's center_x
    int32_t nozzle_y = 0; // last state-tied draw's nozzle_y
    int32_t sensor_r = 0; // last state-tied draw's sensor radius
    bool valid = false;
};

// All per-widget state. Plain fields are the widget's CONFIG (pushed in via
// the public setters / XML attributes); the named sub-structs hold derived or
// subsystem-owned state.
struct FilamentPathData {
    // --- Configuration ---
    int topology = 1;                    // 0=LINEAR, 1=HUB (PathTopology values)
    int slot_count = DEFAULT_SLOT_COUNT; // Number of slots
    int active_slot = -1;                // Currently active slot (-1=none)
    int filament_segment = 0;            // PathSegment enum value (target)
    int error_segment = 0;               // Error location (0=none)
    uint32_t filament_color = DEFAULT_FILAMENT_COLOR;
    int32_t slot_overlap = 0; // Overlap between slots in pixels (for 5+ gates)
    int32_t slot_width = 90;  // Dynamic slot width (fallback when slot_grid unavailable)

    // Live slot position measurement: slot_grid pointer + cached spool_container
    // pointers for pixel-perfect lane alignment at any screen size.
    lv_obj_t* slot_grid = nullptr;
    static constexpr int MAX_SLOTS = 16;
    lv_obj_t* spool_containers[MAX_SLOTS] = {};

    // Per-slot filament state (for showing all installed filaments, not just active)
    SlotFilamentState slot_filament_states[MAX_SLOTS] = {};

    // Per-slot prep sensor capability (true = slot has prep/pre-gate sensor)
    bool slot_has_prep_sensor[MAX_SLOTS] = {};

    // Per-slot tool mapping (actual AFC map values, not slot index)
    int mapped_tool[MAX_SLOTS];              // -1 = use slot index as fallback
    bool slot_is_hub_routed[MAX_SLOTS] = {}; // true = lane routes through hub (MIXED topology)

    // Per-lane Klipper extruder identity, derived from the lane's extruder name
    // (#1229). Toolhead badges read "E<n>" from this when every lane resolved;
    // otherwise they fall back to the AFC lane aliases in mapped_tool.
    int extruder_tool[MAX_SLOTS];       // -1 = this lane's extruder is unknown
    bool use_extruder_identity = false; // true = badge toolheads "E<n>"

    // Bypass mode state
    bool bypass_active = false;       // External spool bypass mode
    uint32_t bypass_color = 0x888888; // Default gray for bypass filament
    bool bypass_has_spool = false;    // true when external spool is assigned
    bool show_bypass = true; // false = hide bypass path/spool entirely (e.g. tool changers)

    // Rendering mode
    bool hub_only = false; // true = stop rendering at hub (skip downstream)
    // Hub co-located with the toolhead: the merge box sits just above the
    // toolhead and the shared hub->nozzle run is a short stub (printers whose
    // combiner mounts on the print head; the per-lane tubes run the whole way).
    bool hub_on_toolhead = false;
    bool eject_mode = false; // true = allow segment to drop below LANE (past slot sensor)

    // Buffer element (TurtleNeck / eSpooler visualization)
    int buffer_fault_state = 0;  // 0=healthy, 1=warning/approaching, 2=fault
    bool buffer_present = false; // true = draw buffer box between hub and toolhead
    int buffer_state = 0;        // 0=neutral, 1=compressed, 2=tension (coil icon spacing)
    float buffer_bias = -2.0f;   ///< Proportional bias [-1.0,1.0], -2=unavailable (use discrete)

    // Callbacks
    filament_path_slot_cb_t slot_callback = nullptr;
    void* slot_user_data = nullptr;
    filament_path_bypass_cb_t bypass_callback = nullptr;
    void* bypass_user_data = nullptr;
    filament_path_buffer_cb_t buffer_callback = nullptr;
    void* buffer_user_data = nullptr;
    hub_callback_t hub_callback = nullptr;
    void* hub_user_data = nullptr;

    // --- Subsystem state ---
    ThemeCache theme;
    AnimState anim;
    LayerState layers;
    HitRects hits;
    PathCache path_cache;

    FilamentPathData() {
        std::fill(std::begin(mapped_tool), std::end(mapped_tool), -1);
        std::fill(std::begin(extruder_tool), std::end(extruder_tool), -1);
    }
};

/**
 * @brief Write a toolhead badge string ("E5" or "T0") for one lane
 *
 * Prefers the lane's Klipper extruder identity; falls back to @p fallback_tool,
 * the AFC lane alias, when identity is unavailable. #1229: the two disagree on
 * tool changers, so the letter tells the user which number they are reading.
 *
 * @param data          Canvas data block
 * @param lane          Lane index owning the toolhead, or <0 when unknown
 * @param fallback_tool Legacy T-label number to use without extruder identity
 */
void format_tool_badge_label(const FilamentPathData* data, int lane, int fallback_tool, char* out,
                             size_t out_size);

// ============================================================================
// Registry + geometry helpers (ui_filament_path_canvas.cpp)
// ============================================================================

/// Look up the widget's data block (nullptr when obj isn't a filament path canvas).
FilamentPathData* get_data(lv_obj_t* obj);

/// Slot center X relative to the canvas left edge.
/// Primary: uses cached spool_container pointers for pixel-perfect alignment.
/// Fallback: computes position from slot_width/overlap when slot_grid unavailable.
int32_t get_slot_x(const FilamentPathData* data, int slot_index, int32_t canvas_x1);

// Canvas dimensions + pre-computed per-slot X positions, shared across all
// topology renderers. Computed once per draw to avoid repeated get_slot_x()
// calls inside the per-slot phase loops.
struct BaseGeometry {
    int32_t x_off = 0; // canvas left edge (absolute display coords)
    int32_t y_off = 0; // canvas top edge
    int32_t width = 0;
    int32_t height = 0;
    int slot_count = 0;
    int32_t slot_x[FilamentPathData::MAX_SLOTS] = {}; // absolute X per slot
    int32_t center_x = 0; // midpoint between first and last slot, or canvas mid
};

BaseGeometry compute_base_geometry(lv_obj_t* obj, const FilamentPathData* data);

// Derived per-slot drawing state. Computed in one pass so each topology body
// reads from the array instead of rederiving inline.
struct SlotRenderState {
    PathSegment segment = PathSegment::NONE;
    lv_color_t color = lv_color_make(0, 0, 0); // valid only when has_filament
    bool has_filament = false;
    bool is_mounted = false;
    bool at_sensor = false; // segment >= TOOLHEAD (consumed by PARALLEL/MIXED)
    bool at_nozzle = false; // segment >= NOZZLE
};

using SlotRenderStates = std::array<SlotRenderState, FilamentPathData::MAX_SLOTS>;

SlotRenderStates compute_slot_render_states(const FilamentPathData* data);

/// True when a segment should be drawn "active" (filament present at or past it).
bool is_segment_active(PathSegment segment, PathSegment filament_segment);

// Color manipulation helpers — thin aliases over the shared stroker color math
// so the many local call sites (sensor dots, hub tinting, buffer coil) stay
// terse.
inline lv_color_t ph_darken(lv_color_t c, uint8_t amt) {
    return helix::ui::tube_darken(c, amt);
}
inline lv_color_t ph_lighten(lv_color_t c, uint8_t amt) {
    return helix::ui::tube_lighten(c, amt);
}
inline lv_color_t ph_blend(lv_color_t c1, lv_color_t c2, float factor) {
    return helix::ui::tube_blend(c1, c2, factor);
}

// Everything a draw helper needs besides its own glyph parameters: the target
// layer, the widget state (theme cache, config), and the per-draw geometry.
// Passed by reference through the topology phases and glyph helpers so their
// signatures carry only the truly per-call values.
struct RenderCtx {
    lv_layer_t* layer;
    FilamentPathData* data;
    BaseGeometry geo;
};

// ============================================================================
// Glyphs (ui_filament_path_glyphs.cpp)
// ============================================================================

/// Push-to-connect fitting at a sensor position (shadow + body + highlight).
void draw_sensor_dot(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color, bool filled,
                     int32_t radius);

/// Labeled rounded box (HUB / SELECTOR / BUF). Text color, font and corner
/// radius come from the theme cache in ctx. Returns the number of pixels the
/// interactive gear badge extends to the RIGHT of the box's right edge (0 when
/// the gear fits inside, or when not interactive) — callers recording a click
/// hit-rect widen it by this amount.
int32_t draw_hub_box(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t width, int32_t height,
                     lv_color_t bg_color, lv_color_t border_color, const char* label,
                     lv_opa_t bg_opa = LV_OPA_COVER, bool interactive = false);

/// Buffer box element ("BUF"), border colored by fault state / proportional bias.
/// Box width derives from the theme hub_width; height from hub_h (both clamped).
void draw_buffer_coil(const RenderCtx& ctx, int32_t cx, int32_t cy, int32_t hub_h,
                      bool has_filament, lv_color_t filament_color);

/// Animated filament tip: a glowing dot that moves along the path.
void draw_filament_tip(lv_layer_t* layer, int32_t x, int32_t y, lv_color_t color, int32_t radius);

/// Pulsing orange glow halo around the nozzle tip while heating.
void draw_heat_glow(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t radius, lv_opa_t pulse_opa);

/// Flow dots along a straight line segment (load/unload particle stream).
void draw_flow_dots_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                         lv_color_t color, int32_t flow_offset, bool reverse);

/// Flow dots along an entire FilamentPath as a continuous stream.
void draw_flow_dots_path(lv_layer_t* layer, const pg::FilamentPath& path, lv_color_t color,
                         int32_t flow_offset, bool reverse);

/// Toolhead/extruder glyph in the user's configured style. Applies the A4T
/// style's 6/5 scale boost internally — pass the base scale.
void draw_toolhead(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color, int32_t scale,
                   lv_opa_t opa = LV_OPA_COVER);

/// Y of the nozzle tip for the configured toolhead style (heat glow anchor).
int32_t toolhead_tip_y(int32_t nozzle_y, int32_t extruder_scale);

/// Tool badge ("T0", "T1", …) beneath a nozzle: rounded rect + centered label.
void draw_tool_badge(const RenderCtx& ctx, int32_t cx, int32_t badge_top, const char* label,
                     lv_color_t text_color, lv_opa_t opa);

// ============================================================================
// Animations (ui_filament_path_anim.cpp)
// ============================================================================

void start_segment_animation(lv_obj_t* obj, FilamentPathData* data, int from_segment,
                             int to_segment);
void stop_segment_animation(lv_obj_t* obj, FilamentPathData* data);
void start_error_pulse(lv_obj_t* obj, FilamentPathData* data);
void stop_error_pulse(lv_obj_t* obj, FilamentPathData* data);
void start_heat_pulse(lv_obj_t* obj, FilamentPathData* data);
void stop_heat_pulse(lv_obj_t* obj, FilamentPathData* data);
void start_flow_animation(lv_obj_t* obj, FilamentPathData* data);
void stop_flow_animation(lv_obj_t* obj, FilamentPathData* data);
void start_output_x_animation(lv_obj_t* obj, FilamentPathData* data, int32_t from_x, int32_t to_x);

/// Delete every lv_anim this widget may have running (widget teardown).
void delete_all_animations(lv_obj_t* obj);

// ============================================================================
// Layered canvas machinery (ui_filament_path_layers.cpp)
// ============================================================================

/// Create the two canvas children, allocate buffers, schedule the first render.
bool layered_setup_canvases(lv_obj_t* obj, FilamentPathData* data);

/// Mark which layered surfaces need a repaint and schedule an async refresh.
/// Use this from setters instead of bare lv_obj_invalidate().
void layered_mark_dirty(lv_obj_t* obj, bool static_dirty, bool overlay_dirty);

/// LV_EVENT_SIZE_CHANGED handler — re-schedules the refresh once layout
/// assigns a real size (the create-time refresh may have bailed pre-layout).
void layered_size_changed_cb(lv_event_t* e);

/// Widget teardown: cancel the pending async refresh and free canvas buffers.
/// The lv_canvas children themselves are deleted by LVGL with the parent.
void layered_teardown(lv_obj_t* obj, FilamentPathData* data);

// ============================================================================
// Topology renderers (ui_filament_path_topology.cpp)
// ============================================================================

/// Paint the full state-tied topology (dispatches on data->topology) into the
/// given layer. Called by the layers module when repainting the overlay canvas.
void render_overlay_content(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data);

/// Paint the per-frame animation overlay (flow dots, heat glow, segment tip).
/// Called from the widget's DRAW_POST event every frame.
void render_animation_overlay(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data);

} // namespace helix::ui::fpath
