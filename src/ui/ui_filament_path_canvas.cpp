// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_path_canvas.h"

#include "ui_fonts.h"
#include "ui_spool_drawing.h"
#include "ui_update_queue.h"
#include "ui_widget_memory.h"

#include "ams_types.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "nozzle_renderer_bambu.h"
#include "nozzle_renderer_faceted.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace helix;

// ============================================================================
// Constants
// ============================================================================

// Default dimensions
static constexpr int32_t DEFAULT_WIDTH = 300;
static constexpr int32_t DEFAULT_HEIGHT = 200;
static constexpr int DEFAULT_SLOT_COUNT = 4;

// Nozzle tip color when no filament is loaded (light charcoal)
static constexpr uint32_t NOZZLE_UNLOADED_COLOR = 0x3A3A3A;

// Layout ratios (as fraction of widget height)
// Entry points at very top to connect visually with slot grid above
static constexpr float ENTRY_Y_RATIO =
    -0.12f; // Top entry points (above canvas, very close to spool box)
static constexpr float PREP_Y_RATIO = 0.10f;     // Prep sensor position
static constexpr float MERGE_Y_RATIO = 0.20f;    // Where lanes merge
static constexpr float HUB_Y_RATIO = 0.30f;      // Hub/selector center
static constexpr float HUB_HEIGHT_RATIO = 0.10f; // Hub box height
// Note: output sensor Y is computed as hub_bottom (butted against hub, no separate ratio)
static constexpr float TOOLHEAD_Y_RATIO = 0.54f; // Toolhead sensor
static constexpr float NOZZLE_Y_RATIO =
    0.75f; // Nozzle/extruder center (needs more room for larger extruder)

// Bypass position (right side of widget)
static constexpr float BYPASS_X_RATIO = 0.85f; // Right side for bypass spool
// Bypass merge point: where bypass path joins the center path, BELOW the hub output sensor.
// This is where a physical or virtual bypass sensor lives.
static constexpr float BYPASS_MERGE_Y_RATIO = 0.44f;

// Line widths (scaled by space_xs for responsiveness)
static constexpr int LINE_WIDTH_IDLE_BASE = 2;
static constexpr int LINE_WIDTH_ACTIVE_BASE = 4;
static constexpr int SENSOR_RADIUS_BASE = 4;

// Default filament color (used when no active filament)
static constexpr uint32_t DEFAULT_FILAMENT_COLOR = 0x4488FF;

// ============================================================================
// Widget State
// ============================================================================

// Animation constants
static constexpr int SEGMENT_ANIM_DURATION_MS = 300; // Duration for segment-to-segment animation
static constexpr int ERROR_PULSE_DURATION_MS = 800;  // Error pulse cycle duration
static constexpr lv_opa_t ERROR_PULSE_OPA_MIN = 100; // Minimum opacity during error pulse
static constexpr lv_opa_t ERROR_PULSE_OPA_MAX = 255; // Maximum opacity during error pulse
static constexpr int FLOW_ANIM_DURATION_MS = 1500;   // Full cycle for flow dot animation
static constexpr int FLOW_DOT_SPACING = 20;          // Pixels between flow dots
static constexpr int FLOW_DOT_RADIUS = 1;            // Radius of each flow particle
static constexpr lv_opa_t FLOW_DOT_OPA = 90;         // Opacity of flow dots

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

struct FilamentPathData {
    int topology = 1;                    // 0=LINEAR, 1=HUB
    int slot_count = DEFAULT_SLOT_COUNT; // Number of slots
    int active_slot = -1;                // Currently active slot (-1=none)
    int filament_segment = 0;            // PathSegment enum value (target)
    int error_segment = 0;               // Error location (0=none)
    int anim_progress = 0;               // Animation progress 0-100 (for segment transition)
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

    // Animation state
    int prev_segment = 0; // Previous segment (for smooth transition)
    AnimDirection anim_direction = AnimDirection::NONE;
    bool segment_anim_active = false;        // Segment transition animation running
    bool error_pulse_active = false;         // Error pulse animation running
    lv_opa_t error_pulse_opa = LV_OPA_COVER; // Current error segment opacity

    // Bypass mode state
    bool bypass_active = false;       // External spool bypass mode
    uint32_t bypass_color = 0x888888; // Default gray for bypass filament
    bool bypass_has_spool = false;    // true when external spool is assigned
    bool show_bypass = true; // false = hide bypass path/spool entirely (e.g. tool changers)

    // Rendering mode
    bool hub_only = false;             // true = stop rendering at hub (skip downstream)
    bool use_faceted_toolhead = false; // false = Bambu-style, true = faceted red style

    // Buffer fault state (0=healthy, 1=warning/approaching, 2=fault)
    int buffer_fault_state = 0;

    // Heat glow state
    bool heat_active = false;               // true when nozzle is actively heating
    bool heat_pulse_active = false;         // Animation running
    lv_opa_t heat_pulse_opa = LV_OPA_COVER; // Current heat glow opacity

    // Flow animation state (particles flowing along active path during load/unload)
    bool flow_anim_active = false;
    int32_t flow_offset = 0; // 0 → FLOW_DOT_SPACING, cycles continuously

    // Output-X slide animation (LINEAR: output exits beneath active slot)
    int32_t output_x_current = 0; // Current animated X position
    int32_t output_x_target = 0;  // Target X position
    bool output_x_anim_active = false;

    // Callbacks
    filament_path_slot_cb_t slot_callback = nullptr;
    void* slot_user_data = nullptr;
    filament_path_bypass_cb_t bypass_callback = nullptr;
    void* bypass_user_data = nullptr;

    // Theme-derived colors (cached for performance)
    lv_color_t color_idle;
    lv_color_t color_error;
    lv_color_t color_hub_bg;
    lv_color_t color_hub_border;
    lv_color_t color_nozzle;
    lv_color_t color_text;
    lv_color_t color_bg; // Canvas background (for hollow tube bore)

    // Theme-derived sizes
    int32_t line_width_idle = LINE_WIDTH_IDLE_BASE;
    int32_t line_width_active = LINE_WIDTH_ACTIVE_BASE;
    int32_t sensor_radius = SENSOR_RADIUS_BASE;
    int32_t hub_width = 60;
    int32_t border_radius = 6;
    int32_t extruder_scale = 10; // Scale unit for extruder (based on space_md)

    // Theme-derived font
    const lv_font_t* label_font = nullptr;
};

// Load theme-aware colors, fonts, and sizes
static void load_theme_colors(FilamentPathData* data) {
    bool dark_mode = theme_manager_is_dark_mode();

    // Use theme tokens with dark/light mode awareness
    data->color_idle =
        theme_manager_get_color(dark_mode ? "filament_idle_dark" : "filament_idle_light");
    data->color_error = theme_manager_get_color("filament_error");
    data->color_hub_bg =
        theme_manager_get_color(dark_mode ? "filament_hub_bg_dark" : "filament_hub_bg_light");
    data->color_hub_border = theme_manager_get_color(dark_mode ? "filament_hub_border_dark"
                                                               : "filament_hub_border_light");
    data->color_nozzle = lv_color_hex(NOZZLE_UNLOADED_COLOR);
    data->color_text = theme_manager_get_color("text");
    data->color_bg = theme_manager_get_color("card_bg");

    // Get responsive sizing from theme
    int32_t space_xs = theme_manager_get_spacing("space_xs");
    int32_t space_md = theme_manager_get_spacing("space_md");

    // Scale line widths based on spacing (responsive)
    data->line_width_idle = LV_MAX(2, space_xs / 2);
    data->line_width_active = LV_MAX(3, space_xs - 2);
    data->sensor_radius = LV_MAX(4, space_xs);
    data->hub_width = LV_MAX(50, space_md * 5);
    data->border_radius = LV_MAX(4, space_xs);
    data->extruder_scale = LV_MAX(8, space_md); // Extruder scales with space_md

    // Get responsive font from globals.xml (font_small → responsive variant)
    const char* font_name = lv_xml_get_const(nullptr, "font_small");
    data->label_font = font_name ? lv_xml_get_font(nullptr, font_name) : &noto_sans_12;

    spdlog::trace("[FilamentPath] Theme colors loaded (dark={}, font={})", dark_mode,
                  font_name ? font_name : "fallback");
}

static std::unordered_map<lv_obj_t*, FilamentPathData*> s_registry;

static FilamentPathData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

// ============================================================================
// Helper Functions
// ============================================================================

// Get slot center X relative to the canvas left edge.
// Primary: uses cached spool_container pointers for pixel-perfect alignment.
// Fallback: computes position from slot_width/overlap when slot_grid unavailable.
static int32_t get_slot_x(const FilamentPathData* data, int slot_index, int32_t canvas_x1) {
    if (slot_index >= 0 && slot_index < FilamentPathData::MAX_SLOTS) {
        lv_obj_t* spool_cont = data->spool_containers[slot_index];
        if (spool_cont) {
            // When spool_container is hidden (cleared slot), its flex-computed coords
            // are invalid. Use the parent slot widget center instead — it always stays
            // visible and at the correct fixed width.
            lv_obj_t* target = lv_obj_has_flag(spool_cont, LV_OBJ_FLAG_HIDDEN)
                                   ? lv_obj_get_parent(spool_cont)
                                   : spool_cont;
            if (target) {
                lv_area_t coords;
                lv_obj_get_coords(target, &coords);
                return (coords.x1 + coords.x2) / 2 - canvas_x1;
            }
        }
    }

    // Fallback: computed position (no slot_grid available)
    int32_t slot_width = data->slot_width;
    if (data->slot_count <= 1) {
        return slot_width / 2;
    }
    int32_t slot_spacing = slot_width - data->slot_overlap;
    return slot_width / 2 + slot_index * slot_spacing;
}

// Check if a segment should be drawn as "active" (filament present at or past it)
static bool is_segment_active(PathSegment segment, PathSegment filament_segment) {
    return static_cast<int>(segment) <= static_cast<int>(filament_segment) &&
           filament_segment != PathSegment::NONE;
}

// ============================================================================
// Animation Callbacks
// ============================================================================

// Forward declarations for animation callbacks
static void segment_anim_cb(void* var, int32_t value);
static void error_pulse_anim_cb(void* var, int32_t value);
static void heat_pulse_anim_cb(void* var, int32_t value);
static void flow_anim_cb(void* var, int32_t value);
static void start_flow_animation(lv_obj_t* obj, FilamentPathData* data);
static void stop_flow_animation(lv_obj_t* obj, FilamentPathData* data);
static void output_x_anim_cb(void* var, int32_t value);

// Start segment transition animation
static void start_segment_animation(lv_obj_t* obj, FilamentPathData* data, int from_segment,
                                    int to_segment) {
    if (!obj || !data)
        return;

    // Stop any existing animation
    lv_anim_delete(obj, segment_anim_cb);

    // Determine animation direction
    if (to_segment > from_segment) {
        data->anim_direction = AnimDirection::LOADING;
    } else if (to_segment < from_segment) {
        data->anim_direction = AnimDirection::UNLOADING;
    } else {
        data->anim_direction = AnimDirection::NONE;
        return; // No change, no animation needed
    }

    data->prev_segment = from_segment;
    data->segment_anim_active = true;
    data->anim_progress = 0;

    // Skip animation if disabled - jump to final state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        data->anim_progress = 100;
        data->segment_anim_active = false;
        data->anim_direction = AnimDirection::NONE;
        data->prev_segment = data->filament_segment;
        lv_obj_invalidate(obj);
        spdlog::trace("[FilamentPath] Animations disabled - skipping segment animation");
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, 0, 100);
    lv_anim_set_duration(&anim, SEGMENT_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, segment_anim_cb);
    lv_anim_start(&anim);

    // Start flow particles only for real filament movement (small steps).
    // Big jumps (e.g., 0→4 on initial state setup) are not real flow operations.
    int step = std::abs(to_segment - from_segment);
    if (step <= 2) {
        start_flow_animation(obj, data);
    }

    spdlog::trace("[FilamentPath] Started segment animation: {} -> {} ({}, step={})", from_segment,
                  to_segment,
                  data->anim_direction == AnimDirection::LOADING ? "loading" : "unloading", step);
}

// Stop segment animation
static void stop_segment_animation(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data)
        return;

    lv_anim_delete(obj, segment_anim_cb);
    data->segment_anim_active = false;
    data->anim_progress = 100;
    data->anim_direction = AnimDirection::NONE;
    stop_flow_animation(obj, data);
}

// Segment animation callback
static void segment_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    data->anim_progress = value;

    // Animation complete
    if (value >= 100) {
        data->segment_anim_active = false;
        data->anim_direction = AnimDirection::NONE;
        data->prev_segment = data->filament_segment;
        spdlog::info("[FilamentPath] Segment anim complete at segment {} (flow_active={})",
                     data->filament_segment, data->flow_anim_active);
        // Stop flow at terminal positions (NONE=unload complete, NOZZLE=load complete)
        // or when no further transitions are expected. Flow between intermediate steps
        // is stopped here rather than relying solely on set_filament_segment, which
        // may not fire again after the final step.
        if (data->flow_anim_active) {
            int seg = data->filament_segment;
            bool is_terminal = (seg == 0 || seg == PATH_SEGMENT_COUNT - 1);
            if (is_terminal) {
                stop_flow_animation(obj, data);
            }
        }
    }

    // Defer invalidation to avoid calling during render phase
    // Animation exec callbacks can run during lv_timer_handler() which may overlap with rendering
    helix::ui::async_call(
        obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
}

// Start error pulse animation
static void start_error_pulse(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data || data->error_pulse_active)
        return;

    data->error_pulse_active = true;
    data->error_pulse_opa = ERROR_PULSE_OPA_MAX;

    // Skip animation if disabled - just show static error state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_invalidate(obj);
        spdlog::trace("[FilamentPath] Animations disabled - showing static error state");
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, ERROR_PULSE_OPA_MIN, ERROR_PULSE_OPA_MAX);
    lv_anim_set_duration(&anim, ERROR_PULSE_DURATION_MS);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&anim, ERROR_PULSE_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim, error_pulse_anim_cb);
    lv_anim_start(&anim);

    spdlog::trace("[FilamentPath] Started error pulse animation");
}

// Stop error pulse animation
static void stop_error_pulse(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data)
        return;

    lv_anim_delete(obj, error_pulse_anim_cb);
    data->error_pulse_active = false;
    data->error_pulse_opa = LV_OPA_COVER;
}

// Error pulse animation callback
static void error_pulse_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    data->error_pulse_opa = static_cast<lv_opa_t>(value);
    // Defer invalidation to avoid calling during render phase
    helix::ui::async_call(
        obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
}

// Heat pulse animation constants (same timing as error pulse)
static constexpr int HEAT_PULSE_DURATION_MS = 800;  // Heat pulse cycle duration
static constexpr lv_opa_t HEAT_PULSE_OPA_MIN = 100; // Minimum opacity during heat pulse
static constexpr lv_opa_t HEAT_PULSE_OPA_MAX = 255; // Maximum opacity during heat pulse

// Start heat pulse animation
static void start_heat_pulse(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data || data->heat_pulse_active)
        return;

    data->heat_pulse_active = true;
    data->heat_pulse_opa = HEAT_PULSE_OPA_MAX;

    // Skip animation if disabled - just show static heat state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_invalidate(obj);
        spdlog::trace("[FilamentPath] Animations disabled - showing static heat state");
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, HEAT_PULSE_OPA_MIN, HEAT_PULSE_OPA_MAX);
    lv_anim_set_duration(&anim, HEAT_PULSE_DURATION_MS);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&anim, HEAT_PULSE_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim, heat_pulse_anim_cb);
    lv_anim_start(&anim);

    spdlog::trace("[FilamentPath] Started heat pulse animation");
}

// Stop heat pulse animation
static void stop_heat_pulse(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data)
        return;

    lv_anim_delete(obj, heat_pulse_anim_cb);
    data->heat_pulse_active = false;
    data->heat_pulse_opa = LV_OPA_COVER;
}

// Heat pulse animation callback
static void heat_pulse_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    data->heat_pulse_opa = static_cast<lv_opa_t>(value);
    // Defer invalidation to avoid calling during render phase
    helix::ui::async_call(
        obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
}

// Start flow animation (particles flowing along active path during load/unload)
static void start_flow_animation(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data || data->flow_anim_active)
        return;
    if (!DisplaySettingsManager::instance().get_animations_enabled())
        return;

    data->flow_anim_active = true;
    data->flow_offset = 0;
    spdlog::info("[FilamentPath] Flow animation STARTED");

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, 0, FLOW_DOT_SPACING);
    lv_anim_set_duration(&anim, FLOW_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&anim, flow_anim_cb);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

static void stop_flow_animation(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data)
        return;
    if (data->flow_anim_active) {
        spdlog::info("[FilamentPath] Flow animation STOPPED");
    }
    lv_anim_delete(obj, flow_anim_cb);
    data->flow_anim_active = false;
    data->flow_offset = 0;
}

static void flow_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    int old_offset = data->flow_offset;
    data->flow_offset = value;

    // Throttle redraws: only invalidate when dots visibly move (~2px change).
    // Flow dots are 1px radius at low opacity — sub-pixel changes are invisible.
    if (std::abs(value - old_offset) >= 2) {
        helix::ui::async_call(
            obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
    }
}

// Output X slide animation callback (LINEAR topology)
static void output_x_anim_cb(void* var, int32_t value) {
    auto* obj = static_cast<lv_obj_t*>(var);
    auto* data = get_data(obj);
    if (!data)
        return;
    data->output_x_current = value;
    lv_obj_invalidate(obj);
}

static constexpr int OUTPUT_X_ANIM_DURATION_MS = 250;

static void start_output_x_animation(lv_obj_t* obj, FilamentPathData* data, int32_t from_x,
                                     int32_t to_x) {
    if (!obj || !data)
        return;
    lv_anim_delete(obj, output_x_anim_cb);

    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        data->output_x_current = to_x;
        data->output_x_target = to_x;
        data->output_x_anim_active = false;
        lv_obj_invalidate(obj);
        return;
    }

    data->output_x_anim_active = true;
    data->output_x_target = to_x;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, from_x, to_x);
    lv_anim_set_duration(&anim, OUTPUT_X_ANIM_DURATION_MS);
    lv_anim_set_exec_cb(&anim, output_x_anim_cb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* obj = static_cast<lv_obj_t*>(a->var);
        auto* data = get_data(obj);
        if (data) {
            data->output_x_anim_active = false;
        }
    });
    lv_anim_start(&anim);
}

// ============================================================================
// Color Manipulation Helpers
// ============================================================================

static lv_color_t ph_darken(lv_color_t c, uint8_t amt) {
    return lv_color_make(c.red > amt ? c.red - amt : 0, c.green > amt ? c.green - amt : 0,
                         c.blue > amt ? c.blue - amt : 0);
}

static lv_color_t ph_lighten(lv_color_t c, uint8_t amt) {
    return lv_color_make((c.red + amt > 255) ? 255 : c.red + amt,
                         (c.green + amt > 255) ? 255 : c.green + amt,
                         (c.blue + amt > 255) ? 255 : c.blue + amt);
}

static lv_color_t ph_blend(lv_color_t c1, lv_color_t c2, float factor) {
    factor = LV_CLAMP(factor, 0.0f, 1.0f);
    return lv_color_make((uint8_t)(c1.red + (c2.red - c1.red) * factor),
                         (uint8_t)(c1.green + (c2.green - c1.green) * factor),
                         (uint8_t)(c1.blue + (c2.blue - c1.blue) * factor));
}

// ============================================================================
// Glow Effect
// ============================================================================
// Soft bloom behind active filament paths. Uses a wide, low-opacity line in a
// lighter tint of the filament color. For very dark filaments (black), uses a
// contrasting blue tint so the glow is still visible.

static constexpr int CURVE_SEGMENTS = 10;      // Segments per bezier curve (fwd-decl for glow)
static constexpr lv_opa_t GLOW_OPA = 60;       // Base glow opacity
static constexpr int32_t GLOW_WIDTH_EXTRA = 6; // Extra width beyond tube on each side

// Get a suitable glow color from a filament color
static lv_color_t get_glow_color(lv_color_t color) {
    // If the filament is very dark, use a contrasting blue tint
    int brightness = color.red + color.green + color.blue;
    if (brightness < 120) {
        return lv_color_hex(0x4466AA); // Dark blue glow for black/dark filaments
    }
    return ph_lighten(color, 60);
}

// Draw a glow line (wide, low-opacity backdrop)
static void draw_glow_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           lv_color_t filament_color, int32_t tube_width) {
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = get_glow_color(filament_color);
    line_dsc.width = tube_width + GLOW_WIDTH_EXTRA;
    line_dsc.opa = GLOW_OPA;
    line_dsc.p1.x = x1;
    line_dsc.p1.y = y1;
    line_dsc.p2.x = x2;
    line_dsc.p2.y = y2;
    line_dsc.round_start = true;
    line_dsc.round_end = true;
    lv_draw_line(layer, &line_dsc);
}

// Draw glow along a cubic bezier curve.
// Uses butt caps on interior segment joints to prevent opacity compounding
// where semi-transparent segments overlap. Round caps only on the very first
// and last endpoints for clean termination.
static void draw_glow_curve(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t cx1, int32_t cy1,
                            int32_t cx2, int32_t cy2, int32_t x1, int32_t y1,
                            lv_color_t filament_color, int32_t tube_width) {
    lv_color_t glow_color = get_glow_color(filament_color);
    int32_t glow_width = tube_width + GLOW_WIDTH_EXTRA;

    int32_t prev_x = x0;
    int32_t prev_y = y0;
    for (int i = 1; i <= CURVE_SEGMENTS; i++) {
        float t = (float)i / CURVE_SEGMENTS;
        float inv = 1.0f - t;
        float b0 = inv * inv * inv;
        float b1 = 3.0f * inv * inv * t;
        float b2 = 3.0f * inv * t * t;
        float b3 = t * t * t;
        int32_t bx = (int32_t)(b0 * x0 + b1 * cx1 + b2 * cx2 + b3 * x1);
        int32_t by = (int32_t)(b0 * y0 + b1 * cy1 + b2 * cy2 + b3 * y1);

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = glow_color;
        line_dsc.width = glow_width;
        line_dsc.opa = GLOW_OPA;
        line_dsc.p1.x = prev_x;
        line_dsc.p1.y = prev_y;
        line_dsc.p2.x = bx;
        line_dsc.p2.y = by;
        // Butt caps on interior joints to prevent opacity overlap;
        // round caps only on the curve endpoints
        line_dsc.round_start = (i == 1);
        line_dsc.round_end = (i == CURVE_SEGMENTS);
        lv_draw_line(layer, &line_dsc);

        prev_x = bx;
        prev_y = by;
    }
}

// ============================================================================
// Flow Particle Drawing
// ============================================================================
// Draws small bright dots flowing along an active tube segment to indicate
// filament motion during load/unload. Dots are spaced at FLOW_DOT_SPACING
// and offset by flow_offset for animation.

// Draw flow dots along a straight line segment
static void draw_flow_dots_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                lv_color_t color, int32_t flow_offset, bool reverse) {
    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    float len = sqrtf((float)(dx * dx + dy * dy));
    if (len < 1.0f)
        return;

    lv_color_t dot_color = ph_lighten(color, 70);
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;
    arc_dsc.radius = static_cast<uint16_t>(FLOW_DOT_RADIUS);
    arc_dsc.width = static_cast<uint16_t>(FLOW_DOT_RADIUS * 2);
    arc_dsc.color = dot_color;
    arc_dsc.opa = FLOW_DOT_OPA;

    // Place dots along the line at FLOW_DOT_SPACING intervals
    int32_t offset = reverse ? (FLOW_DOT_SPACING - flow_offset) : flow_offset;
    for (float d = (float)offset; d < len; d += FLOW_DOT_SPACING) {
        float t = d / len;
        arc_dsc.center.x = x1 + (int32_t)(dx * t);
        arc_dsc.center.y = y1 + (int32_t)(dy * t);
        lv_draw_arc(layer, &arc_dsc);
    }
}

// Draw flow dots along a quadratic bezier curve
static void draw_flow_dots_curve(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t cx1,
                                 int32_t cy1, int32_t cx2, int32_t cy2, int32_t x1, int32_t y1,
                                 lv_color_t color, int32_t flow_offset, bool reverse) {
    // Approximate curve length and place dots along it
    lv_color_t dot_color = ph_lighten(color, 70);
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;
    arc_dsc.radius = static_cast<uint16_t>(FLOW_DOT_RADIUS);
    arc_dsc.width = static_cast<uint16_t>(FLOW_DOT_RADIUS * 2);
    arc_dsc.color = dot_color;
    arc_dsc.opa = FLOW_DOT_OPA;

    // Sample curve at fine resolution and accumulate arc length
    constexpr int SAMPLES = 40;
    float cumulative_len[SAMPLES + 1];
    int32_t sx[SAMPLES + 1];
    int32_t sy[SAMPLES + 1];
    sx[0] = x0;
    sy[0] = y0;
    cumulative_len[0] = 0.0f;

    for (int i = 1; i <= SAMPLES; i++) {
        float t = (float)i / SAMPLES;
        float inv = 1.0f - t;
        float b0 = inv * inv * inv;
        float b1 = 3.0f * inv * inv * t;
        float b2 = 3.0f * inv * t * t;
        float b3 = t * t * t;
        sx[i] = (int32_t)(b0 * x0 + b1 * cx1 + b2 * cx2 + b3 * x1);
        sy[i] = (int32_t)(b0 * y0 + b1 * cy1 + b2 * cy2 + b3 * y1);
        float seg_dx = (float)(sx[i] - sx[i - 1]);
        float seg_dy = (float)(sy[i] - sy[i - 1]);
        cumulative_len[i] = cumulative_len[i - 1] + sqrtf(seg_dx * seg_dx + seg_dy * seg_dy);
    }

    float total_len = cumulative_len[SAMPLES];
    if (total_len < 1.0f)
        return;

    // Place dots at FLOW_DOT_SPACING intervals along the curve
    float offset = reverse ? (float)(FLOW_DOT_SPACING - flow_offset) : (float)flow_offset;
    int sample_idx = 0;
    for (float d = offset; d < total_len; d += FLOW_DOT_SPACING) {
        // Find which sample segment this distance falls in
        while (sample_idx < SAMPLES && cumulative_len[sample_idx + 1] < d)
            sample_idx++;
        if (sample_idx >= SAMPLES)
            break;

        float seg_start = cumulative_len[sample_idx];
        float seg_end = cumulative_len[sample_idx + 1];
        float seg_len = seg_end - seg_start;
        float t = (seg_len > 0.001f) ? (d - seg_start) / seg_len : 0.0f;

        arc_dsc.center.x = sx[sample_idx] + (int32_t)((sx[sample_idx + 1] - sx[sample_idx]) * t);
        arc_dsc.center.y = sy[sample_idx] + (int32_t)((sy[sample_idx + 1] - sy[sample_idx]) * t);
        lv_draw_arc(layer, &arc_dsc);
    }
}

// ============================================================================
// Drawing Functions
// ============================================================================

// Draw a push-to-connect fitting at a sensor position.
// Uses same shadow/highlight language as tubes: shadow (darker) behind, highlight (lighter) offset.
// Same overall size as before — no bigger than the original radius.
static void draw_sensor_dot(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color,
                            bool filled, int32_t radius) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.center.x = cx;
    arc_dsc.center.y = cy;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    // Shadow: same darkening as tube shadow (ph_darken 35), drawn at full radius
    arc_dsc.radius = static_cast<uint16_t>(radius);
    arc_dsc.width = static_cast<uint16_t>(radius * 2);
    arc_dsc.color = ph_darken(color, 35);
    lv_draw_arc(layer, &arc_dsc);

    if (filled) {
        // Body: slightly inset from shadow edge
        int32_t body_r = LV_MAX(1, radius - 1);
        arc_dsc.radius = static_cast<uint16_t>(body_r);
        arc_dsc.width = static_cast<uint16_t>(body_r * 2);
        arc_dsc.color = color;
        lv_draw_arc(layer, &arc_dsc);

        // Highlight: small bright dot offset toward top-right (matching tube light direction)
        int32_t hl_r = LV_MAX(1, radius / 3);
        int32_t hl_off = LV_MAX(1, radius / 3);
        arc_dsc.center.x = cx + hl_off;
        arc_dsc.center.y = cy - hl_off;
        arc_dsc.radius = static_cast<uint16_t>(hl_r);
        arc_dsc.width = static_cast<uint16_t>(hl_r * 2);
        arc_dsc.color = ph_lighten(color, 44);
        lv_draw_arc(layer, &arc_dsc);
    } else {
        // Empty fitting: outline ring only (no fill)
        arc_dsc.radius = static_cast<uint16_t>(radius - 1);
        arc_dsc.width = 2;
        arc_dsc.color = color;
        lv_draw_arc(layer, &arc_dsc);
    }
}

static void draw_flat_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           lv_color_t color, int32_t width, bool cap_start = true,
                           bool cap_end = true) {
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = width;
    line_dsc.p1.x = x1;
    line_dsc.p1.y = y1;
    line_dsc.p2.x = x2;
    line_dsc.p2.y = y2;
    line_dsc.round_start = cap_start;
    line_dsc.round_end = cap_end;
    lv_draw_line(layer, &line_dsc);
}

// ============================================================================
// 3D Tube Drawing
// ============================================================================
// Draws lines as cylindrical PTFE tubes with shadow/body/highlight layers.
// The 3-layer approach creates the illusion of a 3D tube catching light
// from the top-left, which is cheap (3 line draws per segment) but has
// significant visual impact.

// Draw a 3D tube effect for any line segment (angled or straight)
// Shadow (wider, darker) → Body (base color) → Highlight (narrower, lighter, offset)
static void draw_tube_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           lv_color_t color, int32_t width) {
    // Shadow: wider, darker — provides depth beneath the tube
    int32_t shadow_extra = LV_MAX(2, width / 2);
    lv_color_t shadow_color = ph_darken(color, 35);
    draw_flat_line(layer, x1, y1, x2, y2, shadow_color, width + shadow_extra);

    // Body: main tube surface
    draw_flat_line(layer, x1, y1, x2, y2, color, width);

    // Highlight: narrower, lighter — specular reflection along tube surface
    // Offset 1px toward top-left to simulate light source direction
    int32_t hl_width = LV_MAX(1, width * 2 / 5);
    lv_color_t hl_color = ph_lighten(color, 44);

    // Calculate perpendicular offset for highlight (toward top-left light source)
    // For vertical lines: offset left; for angled lines: offset perpendicular
    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    if (dx == 0) {
        // Vertical line — highlight offset to the right
        offset_x = (width / 4 + 1);
    } else if (dy == 0) {
        // Horizontal line — highlight offset upward
        offset_y = -(width / 4 + 1);
    } else {
        // Angled line — offset perpendicular toward top-left
        // Perpendicular direction: (-dy, dx) normalized, scaled by offset amount
        float len = sqrtf((float)(dx * dx + dy * dy));
        float px = -(float)dy / len;
        float py = (float)dx / len;
        // Choose direction that goes toward top-left (negative x or y)
        if (px + py > 0) {
            px = -px;
            py = -py;
        }
        int32_t off_amount = width / 4 + 1;
        offset_x = (int32_t)(px * off_amount);
        offset_y = (int32_t)(py * off_amount);
    }

    draw_flat_line(layer, x1 + offset_x, y1 + offset_y, x2 + offset_x, y2 + offset_y, hl_color,
                   hl_width);
}

// Draw a hollow tube (clear PTFE tubing look): walls + see-through bore
// Same outer diameter as a solid tube, but the center shows the background
static void draw_hollow_tube_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                  lv_color_t wall_color, lv_color_t bg_color, int32_t width) {
    // Shadow: same outer diameter as solid tube
    int32_t shadow_extra = LV_MAX(2, width / 2);
    lv_color_t shadow_color = ph_darken(wall_color, 25); // Lighter shadow for clear tube
    draw_flat_line(layer, x1, y1, x2, y2, shadow_color, width + shadow_extra);

    // Tube wall: the PTFE material
    draw_flat_line(layer, x1, y1, x2, y2, wall_color, width);

    // Bore: background color fill to simulate clear center
    int32_t bore_width = LV_MAX(1, width - 2);
    draw_flat_line(layer, x1, y1, x2, y2, bg_color, bore_width);

    // Highlight on outer wall surface (same offset logic as solid tube)
    int32_t hl_width = LV_MAX(1, width * 2 / 5);
    lv_color_t hl_color = ph_lighten(wall_color, 44);

    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    if (dx == 0) {
        offset_x = (width / 4 + 1);
    } else if (dy == 0) {
        offset_y = -(width / 4 + 1);
    } else {
        float len = sqrtf((float)(dx * dx + dy * dy));
        float px = -(float)dy / len;
        float py = (float)dx / len;
        if (px + py > 0) {
            px = -px;
            py = -py;
        }
        int32_t off_amount = width / 4 + 1;
        offset_x = (int32_t)(px * off_amount);
        offset_y = (int32_t)(py * off_amount);
    }

    draw_flat_line(layer, x1 + offset_x, y1 + offset_y, x2 + offset_x, y2 + offset_y, hl_color,
                   hl_width);
}

// Convenience: draw a solid vertical tube segment
static void draw_vertical_line(lv_layer_t* layer, int32_t x, int32_t y1, int32_t y2,
                               lv_color_t color, int32_t width) {
    draw_tube_line(layer, x, y1, x, y2, color, width);
}

// Convenience: draw a solid tube segment between two arbitrary points
static void draw_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      lv_color_t color, int32_t width) {
    draw_tube_line(layer, x1, y1, x2, y2, color, width);
}

// Convenience: draw a hollow vertical tube segment
static void draw_hollow_vertical_line(lv_layer_t* layer, int32_t x, int32_t y1, int32_t y2,
                                      lv_color_t wall_color, lv_color_t bg_color, int32_t width) {
    draw_hollow_tube_line(layer, x, y1, x, y2, wall_color, bg_color, width);
}

// Convenience: draw a hollow tube segment between two arbitrary points
static void draw_hollow_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                             lv_color_t wall_color, lv_color_t bg_color, int32_t width) {
    draw_hollow_tube_line(layer, x1, y1, x2, y2, wall_color, bg_color, width);
}

// ============================================================================
// Curved Tube Drawing (Bezier Approximation)
// ============================================================================
// Quadratic bezier evaluated as N line segments for smooth tube routing.
// Uses a control point to create natural-looking bends like actual tube routing.

// Helper: evaluate cubic bezier point at parameter t
// P(t) = (1-t)^3*P0 + 3*(1-t)^2*t*C1 + 3*(1-t)*t^2*C2 + t^3*P1
struct BezierPt {
    int32_t x, y;
};

static BezierPt bezier_eval(int32_t x0, int32_t y0, int32_t cx1, int32_t cy1, int32_t cx2,
                            int32_t cy2, int32_t x1, int32_t y1, float t) {
    float inv = 1.0f - t;
    float b0 = inv * inv * inv;
    float b1 = 3.0f * inv * inv * t;
    float b2 = 3.0f * inv * t * t;
    float b3 = t * t * t;
    return {(int32_t)(b0 * x0 + b1 * cx1 + b2 * cx2 + b3 * x1),
            (int32_t)(b0 * y0 + b1 * cy1 + b2 * cy2 + b3 * y1)};
}

// Draw a solid tube along a cubic bezier curve (p0 → cp1 → cp2 → p1)
// Renders each layer (shadow, body, highlight) as a complete pass to avoid
// visible joints between bezier segments.
static void draw_curved_tube(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t cx1, int32_t cy1,
                             int32_t cx2, int32_t cy2, int32_t x1, int32_t y1, lv_color_t color,
                             int32_t width, bool cap_start = true, bool cap_end = true) {
    // Pre-compute all bezier points
    BezierPt pts[CURVE_SEGMENTS + 1];
    pts[0] = {x0, y0};
    for (int i = 1; i <= CURVE_SEGMENTS; i++) {
        pts[i] = bezier_eval(x0, y0, cx1, cy1, cx2, cy2, x1, y1, (float)i / CURVE_SEGMENTS);
    }

    // Round caps between interior segments (overdraw is invisible since same color).
    // Optionally suppress start/end caps at junction with adjacent straight segments.
    // Pass 1: Shadow
    int32_t shadow_extra = LV_MAX(2, width / 2);
    lv_color_t shadow_color = ph_darken(color, 35);
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, shadow_color,
                       width + shadow_extra, cs, ce);
    }

    // Pass 2: Body
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, color, width, cs, ce);
    }

    // Pass 3: Highlight (use average curve direction for consistent offset)
    int32_t hl_width = LV_MAX(1, width * 2 / 5);
    lv_color_t hl_color = ph_lighten(color, 44);
    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    if (dx == 0) {
        offset_x = (width / 4 + 1);
    } else if (dy == 0) {
        offset_y = -(width / 4 + 1);
    } else {
        float len = sqrtf((float)(dx * dx + dy * dy));
        float px = -(float)dy / len;
        float py = (float)dx / len;
        if (px + py > 0) {
            px = -px;
            py = -py;
        }
        int32_t off_amount = width / 4 + 1;
        offset_x = (int32_t)(px * off_amount);
        offset_y = (int32_t)(py * off_amount);
    }
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x + offset_x, pts[i].y + offset_y, pts[i + 1].x + offset_x,
                       pts[i + 1].y + offset_y, hl_color, hl_width, cs, ce);
    }
}

// Draw a hollow tube along a cubic bezier curve (p0 → cp1 → cp2 → p1)
// Same layer-by-layer approach for smooth joints.
static void draw_curved_hollow_tube(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t cx1,
                                    int32_t cy1, int32_t cx2, int32_t cy2, int32_t x1, int32_t y1,
                                    lv_color_t wall_color, lv_color_t bg_color, int32_t width,
                                    bool cap_start = true, bool cap_end = true) {
    BezierPt pts[CURVE_SEGMENTS + 1];
    pts[0] = {x0, y0};
    for (int i = 1; i <= CURVE_SEGMENTS; i++) {
        pts[i] = bezier_eval(x0, y0, cx1, cy1, cx2, cy2, x1, y1, (float)i / CURVE_SEGMENTS);
    }

    // Round caps between interior segments (overdraw is invisible since same color).
    // Optionally suppress start/end caps at junction with adjacent straight segments.
    // Pass 1: Shadow
    int32_t shadow_extra = LV_MAX(2, width / 2);
    lv_color_t shadow_color = ph_darken(wall_color, 25);
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, shadow_color,
                       width + shadow_extra, cs, ce);
    }

    // Pass 2: Tube wall
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, wall_color, width, cs,
                       ce);
    }

    // Pass 3: Bore (background fill)
    int32_t bore_width = LV_MAX(1, width - 2);
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, bg_color, bore_width,
                       cs, ce);
    }

    // Pass 4: Highlight
    int32_t hl_width = LV_MAX(1, width * 2 / 5);
    lv_color_t hl_color = ph_lighten(wall_color, 44);
    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    if (dx == 0) {
        offset_x = (width / 4 + 1);
    } else if (dy == 0) {
        offset_y = -(width / 4 + 1);
    } else {
        float len = sqrtf((float)(dx * dx + dy * dy));
        float px = -(float)dy / len;
        float py = (float)dx / len;
        if (px + py > 0) {
            px = -px;
            py = -py;
        }
        int32_t off_amount = width / 4 + 1;
        offset_x = (int32_t)(px * off_amount);
        offset_y = (int32_t)(py * off_amount);
    }
    for (int i = 0; i < CURVE_SEGMENTS; i++) {
        bool cs = (i == 0) ? cap_start : true;
        bool ce = (i == CURVE_SEGMENTS - 1) ? cap_end : true;
        draw_flat_line(layer, pts[i].x + offset_x, pts[i].y + offset_y, pts[i + 1].x + offset_x,
                       pts[i + 1].y + offset_y, hl_color, hl_width, cs, ce);
    }
}

static void draw_hub_box(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t width, int32_t height,
                         lv_color_t bg_color, lv_color_t border_color, lv_color_t text_color,
                         const lv_font_t* font, int32_t radius, const char* label) {
    // Background
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = bg_color;
    fill_dsc.radius = radius;

    lv_area_t box_area = {cx - width / 2, cy - height / 2, cx + width / 2, cy + height / 2};
    lv_draw_fill(layer, &fill_dsc, &box_area);

    // Border
    lv_draw_border_dsc_t border_dsc;
    lv_draw_border_dsc_init(&border_dsc);
    border_dsc.color = border_color;
    border_dsc.width = 2;
    border_dsc.radius = radius;
    lv_draw_border(layer, &border_dsc, &box_area);

    // Label
    if (label && label[0] && font) {
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = text_color;
        label_dsc.font = font;
        label_dsc.align = LV_TEXT_ALIGN_CENTER;
        label_dsc.text = label;

        int32_t font_h = lv_font_get_line_height(font);
        lv_area_t label_area = {cx - width / 2, cy - font_h / 2, cx + width / 2, cy + font_h / 2};
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

// ============================================================================
// Isometric Print Head Drawing
// ============================================================================
// Creates a Bambu-style 3D print head with:
// - Heater block (main body with gradient shading)
// - Heat break throat (narrower section)
// - Nozzle tip (tapered bottom)
// - Cooling fan hint (side detail)
// Uses isometric projection with gradients for 3D depth effect.

// Draw animated filament tip (a glowing dot that moves along the path)
static void draw_filament_tip(lv_layer_t* layer, int32_t x, int32_t y, lv_color_t color,
                              int32_t radius) {
    // Outer glow (lighter, larger)
    lv_color_t glow_color = ph_lighten(color, 60);
    draw_sensor_dot(layer, x, y, glow_color, true, radius + 2);

    // Inner core (bright)
    lv_color_t core_color = ph_lighten(color, 100);
    draw_sensor_dot(layer, x, y, core_color, true, radius);
}

// Draw heat glow effect around nozzle tip
// Creates a pulsing orange/red glow halo to indicate heating
static void draw_heat_glow(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t radius,
                           lv_opa_t pulse_opa) {
    // Heat glow color - warm orange (#FF6B35) at full opacity
    lv_color_t heat_color = lv_color_hex(0xFF6B35);

    // Outer soft glow (larger, more transparent)
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.center.x = cx;
    arc_dsc.center.y = cy;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    // Multiple rings for soft glow effect
    // Outer ring (widest, most transparent)
    arc_dsc.radius = static_cast<uint16_t>(radius + 8);
    arc_dsc.width = 6;
    arc_dsc.color = heat_color;
    arc_dsc.opa = static_cast<lv_opa_t>(pulse_opa / 4);
    lv_draw_arc(layer, &arc_dsc);

    // Middle ring
    arc_dsc.radius = static_cast<uint16_t>(radius + 4);
    arc_dsc.width = 4;
    arc_dsc.opa = static_cast<lv_opa_t>(pulse_opa / 2);
    lv_draw_arc(layer, &arc_dsc);

    // Inner ring (brightest)
    arc_dsc.radius = static_cast<uint16_t>(radius + 1);
    arc_dsc.width = 2;
    arc_dsc.opa = pulse_opa;
    lv_draw_arc(layer, &arc_dsc);
}

// ============================================================================
// Parallel Topology Drawing (Tool Changers)
// ============================================================================
// Tool changers have independent toolheads - each slot represents a complete
// tool with its own extruder. Unlike hub/linear topologies where filaments
// converge to a single toolhead, parallel topology shows separate paths.

static void draw_parallel_topology(lv_event_t* e, FilamentPathData* data) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);

    // Get widget dimensions
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // Layout ratios for parallel topology (adjusted for per-slot toolheads)
    constexpr float ENTRY_Y = -0.12f;   // Top entry (connects to spool)
    constexpr float SENSOR_Y = 0.38f;   // Toolhead entry sensor (analogous to hub topology)
    constexpr float TOOLHEAD_Y = 0.55f; // Nozzle/toolhead position per slot

    int32_t entry_y = y_off + (int32_t)(height * ENTRY_Y);
    int32_t sensor_y = y_off + (int32_t)(height * SENSOR_Y);
    int32_t toolhead_y = y_off + (int32_t)(height * TOOLHEAD_Y);

    // Colors
    lv_color_t idle_color = data->color_idle;
    lv_color_t bg_color = data->color_bg;
    lv_color_t nozzle_color = data->color_nozzle;

    // Line sizes
    int32_t line_active = data->line_width_active;
    int32_t sensor_r = data->sensor_radius;

    // Draw each tool as an independent column
    for (int i = 0; i < data->slot_count; i++) {
        int32_t slot_x = x_off + get_slot_x(data, i, x_off);
        bool is_mounted = (i == data->active_slot);

        // Determine filament reach for this slot from per-slot state
        lv_color_t tool_color = idle_color;
        bool has_filament = false;
        PathSegment slot_segment = PathSegment::NONE;

        if (i < FilamentPathData::MAX_SLOTS &&
            data->slot_filament_states[i].segment != PathSegment::NONE) {
            has_filament = true;
            tool_color = lv_color_hex(data->slot_filament_states[i].color);
            slot_segment = data->slot_filament_states[i].segment;
        }

        // For mounted tool, use active filament color and segment if available
        if (is_mounted && data->filament_segment > 0) {
            tool_color = lv_color_hex(data->filament_color);
            has_filament = true;
            slot_segment = static_cast<PathSegment>(data->filament_segment);
        }

        bool at_sensor = has_filament && (slot_segment >= PathSegment::TOOLHEAD);
        bool at_nozzle = has_filament && (slot_segment >= PathSegment::NOZZLE);

        int32_t tool_scale = LV_MAX(6, data->extruder_scale * 2 / 3);
        int32_t nozzle_top = toolhead_y - tool_scale * 2; // Top of heater block

        // Entry → sensor line: colored if filament present, hollow if idle
        if (has_filament) {
            draw_glow_line(layer, slot_x, entry_y, slot_x, sensor_y - sensor_r, tool_color,
                           line_active);
            draw_vertical_line(layer, slot_x, entry_y, sensor_y - sensor_r, tool_color,
                               line_active);
        } else {
            draw_hollow_vertical_line(layer, slot_x, entry_y, sensor_y - sensor_r, idle_color,
                                      bg_color, line_active);
        }

        // Toolhead entry sensor dot
        lv_color_t sensor_color = at_sensor ? tool_color : idle_color;
        draw_sensor_dot(layer, slot_x, sensor_y, sensor_color, at_sensor, sensor_r);

        // Sensor → nozzle line: colored if filament reaches nozzle, hollow if idle
        if (at_nozzle) {
            draw_glow_line(layer, slot_x, sensor_y + sensor_r, slot_x, nozzle_top, tool_color,
                           line_active);
            draw_vertical_line(layer, slot_x, sensor_y + sensor_r, nozzle_top, tool_color,
                               line_active);
        } else {
            draw_hollow_vertical_line(layer, slot_x, sensor_y + sensor_r, nozzle_top, idle_color,
                                      bg_color, line_active);
        }

        // Determine nozzle color - only show filament color when actually at nozzle
        lv_color_t noz_color = is_mounted ? nozzle_color : ph_darken(nozzle_color, 60);
        if (at_nozzle) {
            noz_color = tool_color;
        }

        // Docked toolheads rendered at reduced opacity to visually distinguish from active
        lv_opa_t toolhead_opa = is_mounted ? LV_OPA_COVER : LV_OPA_40;

        // Flow particles for active slot during load/unload
        // Drawn BEFORE nozzle so the extruder body covers any nearby dots
        if (is_mounted && data->flow_anim_active && has_filament) {
            bool reverse = (data->anim_direction == AnimDirection::UNLOADING);
            draw_flow_dots_line(layer, slot_x, entry_y, slot_x, sensor_y - sensor_r, tool_color,
                                data->flow_offset, reverse);
        }

        // Use the proper nozzle renderers (same as hub topology)
        if (data->use_faceted_toolhead) {
            draw_nozzle_faceted(layer, slot_x, toolhead_y, noz_color, tool_scale, toolhead_opa);
        } else {
            draw_nozzle_bambu(layer, slot_x, toolhead_y, noz_color, tool_scale, toolhead_opa);
        }

        // Tool badge (T0, T1, etc.) below nozzle — matches system_path_canvas style
        if (data->label_font) {
            char tool_label[16];
            snprintf(tool_label, sizeof(tool_label), "T%d", i);

            int32_t font_h = lv_font_get_line_height(data->label_font);
            int32_t label_len = (int32_t)strlen(tool_label);
            int32_t badge_w = LV_MAX(24, label_len * (font_h * 3 / 5) + 6);
            int32_t badge_h = font_h + 4;
            int32_t badge_top = toolhead_y + tool_scale * 3 + 4;
            int32_t badge_left = slot_x - badge_w / 2;

            // Badge background (rounded rect)
            lv_area_t badge_area = {badge_left, badge_top, badge_left + badge_w,
                                    badge_top + badge_h};
            lv_draw_fill_dsc_t fill_dsc;
            lv_draw_fill_dsc_init(&fill_dsc);
            fill_dsc.color = data->color_idle;
            fill_dsc.opa = (lv_opa_t)LV_MIN(200, toolhead_opa);
            fill_dsc.radius = 4;
            lv_draw_fill(layer, &fill_dsc, &badge_area);

            // Badge text
            lv_draw_label_dsc_t label_dsc;
            lv_draw_label_dsc_init(&label_dsc);
            label_dsc.color = is_mounted ? theme_manager_get_color("success") : data->color_text;
            label_dsc.opa = toolhead_opa;
            label_dsc.font = data->label_font;
            label_dsc.align = LV_TEXT_ALIGN_CENTER;
            label_dsc.text = tool_label;
            label_dsc.text_local = 1;

            lv_area_t text_area = {badge_left, badge_top + 2, badge_left + badge_w,
                                   badge_top + 2 + font_h};
            lv_draw_label(layer, &label_dsc, &text_area);
        }
    }
}

// ============================================================================
// Main Draw Callback
// ============================================================================

static void filament_path_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    // For PARALLEL topology (tool changers), use dedicated drawing function
    // This shows independent toolheads per slot instead of converging to a hub
    if (data->topology == static_cast<int>(PathTopology::PARALLEL)) {
        draw_parallel_topology(e, data);
        return;
    }

    // Get widget dimensions
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int32_t width = lv_area_get_width(&obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // Calculate Y positions
    int32_t entry_y = y_off + (int32_t)(height * ENTRY_Y_RATIO);
    int32_t prep_y = y_off + (int32_t)(height * PREP_Y_RATIO);
    int32_t merge_y = y_off + (int32_t)(height * MERGE_Y_RATIO);
    int32_t hub_y = y_off + (int32_t)(height * HUB_Y_RATIO);
    int32_t hub_h = (int32_t)(height * HUB_HEIGHT_RATIO);
    // Output sensor butted directly against hub bottom (mirrors input sensors at hub top)
    int32_t output_y = hub_y + hub_h / 2;
    int32_t toolhead_y = y_off + (int32_t)(height * TOOLHEAD_Y_RATIO);
    int32_t nozzle_y = y_off + (int32_t)(height * NOZZLE_Y_RATIO);
    int32_t center_x = x_off + width / 2;

    // Colors from theme
    lv_color_t idle_color = data->color_idle;
    lv_color_t bg_color = data->color_bg;
    lv_color_t active_color = lv_color_hex(data->filament_color);
    lv_color_t hub_bg = data->color_hub_bg;
    lv_color_t hub_border = data->color_hub_border;
    lv_color_t nozzle_color = data->color_nozzle;

    // Error color with pulse effect - blend toward idle based on opacity
    lv_color_t error_color = data->color_error;
    if (data->error_pulse_active && data->error_pulse_opa < LV_OPA_COVER) {
        // Blend error color with a darker version for pulsing effect
        float blend_factor = (float)(LV_OPA_COVER - data->error_pulse_opa) /
                             (float)(LV_OPA_COVER - ERROR_PULSE_OPA_MIN);
        error_color = ph_blend(data->color_error, ph_darken(data->color_error, 80), blend_factor);
    }

    // Sizes from theme
    int32_t line_active = data->line_width_active;
    int32_t sensor_r = data->sensor_radius;

    // LINEAR topology: butt SELECTOR directly against prep sensors (no gap/lines between)
    if (data->topology == 0) {
        hub_y = prep_y + sensor_r + hub_h / 2;
        output_y = hub_y + hub_h / 2;
    }

    // LINEAR: output exits beneath the active slot, not center
    int32_t output_x = center_x; // default for HUB
    if (data->topology == 0 && data->active_slot >= 0) {
        int32_t target_x = x_off + get_slot_x(data, data->active_slot, x_off);
        // Use animated position if available, otherwise snap
        if (data->output_x_anim_active) {
            output_x = data->output_x_current;
        } else {
            output_x = target_x;
            data->output_x_current = target_x;
            data->output_x_target = target_x;
        }
    }

    // Determine which segment has error (if any)
    bool has_error = data->error_segment > 0;
    PathSegment error_seg = static_cast<PathSegment>(data->error_segment);
    PathSegment fil_seg = static_cast<PathSegment>(data->filament_segment);

    // Animation state
    bool is_animating = data->segment_anim_active;
    int anim_progress = data->anim_progress;
    PathSegment prev_seg = static_cast<PathSegment>(data->prev_segment);
    bool is_loading = (data->anim_direction == AnimDirection::LOADING);

    // ========================================================================
    // Draw lane lines (one per slot, from entry to merge point)
    // Shows all installed filaments' colors, not just the active slot
    // ========================================================================
    for (int i = 0; i < data->slot_count; i++) {
        int32_t slot_x = x_off + get_slot_x(data, i, x_off);
        bool is_active_slot = (i == data->active_slot);

        // Determine line color and width for this slot's lane
        // Priority: active slot > per-slot filament state > idle
        lv_color_t lane_color = idle_color;
        int32_t lane_width = line_active;
        bool has_filament = false;
        PathSegment slot_segment = PathSegment::NONE;

        if (is_active_slot && data->filament_segment > 0) {
            // Active slot - use active filament color
            has_filament = true;
            lane_color = active_color;
            lane_width = line_active;
            slot_segment = fil_seg;

            // Check for error in lane segments
            if (has_error && (error_seg == PathSegment::PREP || error_seg == PathSegment::LANE)) {
                lane_color = error_color;
            }
        } else if (i < FilamentPathData::MAX_SLOTS &&
                   data->slot_filament_states[i].segment != PathSegment::NONE) {
            // Non-active slot with installed filament - show its color to its sensor position
            has_filament = true;
            lane_color = lv_color_hex(data->slot_filament_states[i].color);
            lane_width = line_active;
            slot_segment = data->slot_filament_states[i].segment;
        }

        // For non-active slots with filament:
        // - Color the line FROM spool TO sensor (we know filament is here)
        // - Color the sensor dot (filament detected)
        // - Gray the line PAST sensor to merge (we don't know extent beyond sensor)
        bool is_non_active_with_filament = !is_active_slot && has_filament;

        // Line from entry to prep sensor: colored if filament present, hollow if idle
        if (has_filament) {
            draw_glow_line(layer, slot_x, entry_y, slot_x, prep_y - sensor_r, lane_color,
                           lane_width);
            draw_vertical_line(layer, slot_x, entry_y, prep_y - sensor_r, lane_color, lane_width);
        } else {
            draw_hollow_vertical_line(layer, slot_x, entry_y, prep_y - sensor_r, idle_color,
                                      bg_color, line_active);
        }

        // Draw prep sensor dot (per-slot capability flag)
        if (data->slot_has_prep_sensor[i]) {
            bool prep_active = has_filament && is_segment_active(PathSegment::PREP, slot_segment);
            lv_color_t prep_dot_color = prep_active ? lane_color : idle_color;
            bool prep_dot_filled = prep_active;
            // Error on prep dot: only for the active slot when error is at PREP
            if (has_error && is_active_slot && error_seg == PathSegment::PREP) {
                prep_dot_color = error_color;
                prep_dot_filled = true;
            }
            draw_sensor_dot(layer, slot_x, prep_y, prep_dot_color, prep_dot_filled, sensor_r);
        }

        // Line from prep sensor to hub/merge target
        // For HUB topology: each lane targets its own hub sensor dot on top of the hub box
        // For other topologies: all lanes converge to the center merge point
        bool slot_past_prep = (slot_segment >= PathSegment::LANE);
        bool slot_at_hub = (slot_segment >= PathSegment::HUB);
        lv_color_t merge_line_color =
            (is_non_active_with_filament && !slot_past_prep) ? idle_color : lane_color;
        bool merge_is_idle = !has_filament || (is_non_active_with_filament && !slot_past_prep);
        if (!has_filament) {
            merge_line_color = idle_color;
        }

        if (data->topology == 1) { // HUB topology - each lane targets its own hub sensor
            int32_t hub_top = hub_y - hub_h / 2;
            // Space hub sensor dots evenly across the hub box width
            int32_t hub_dot_spacing =
                (data->slot_count > 1) ? (data->hub_width - 2 * sensor_r) / (data->slot_count - 1)
                                       : 0;
            int32_t hub_dot_x =
                center_x - (data->hub_width - 2 * sensor_r) / 2 + i * hub_dot_spacing;
            if (data->slot_count == 1)
                hub_dot_x = center_x;

            // Draw curved tube from prep to hub sensor dot
            // S-curve: CP1 below start (departs downward), CP2 above end (arrives from top)
            // cap_start=false eliminates visible endcap seam at straight→curve junction
            int32_t start_y = prep_y + sensor_r;
            int32_t end_y = hub_top - sensor_r;
            int32_t drop = end_y - start_y;
            int32_t cp1_x = slot_x;
            int32_t cp1_y = start_y + drop * 2 / 5;
            int32_t cp2_x = hub_dot_x;
            int32_t cp2_y = end_y - drop * 2 / 5;
            if (merge_is_idle) {
                draw_curved_hollow_tube(layer, slot_x, start_y, cp1_x, cp1_y, cp2_x, cp2_y,
                                        hub_dot_x, end_y, idle_color, bg_color, line_active,
                                        /*cap_start=*/false);
            } else {
                draw_glow_curve(layer, slot_x, start_y, cp1_x, cp1_y, cp2_x, cp2_y, hub_dot_x,
                                end_y, merge_line_color, lane_width);
                draw_curved_tube(layer, slot_x, start_y, cp1_x, cp1_y, cp2_x, cp2_y, hub_dot_x,
                                 end_y, merge_line_color, lane_width, /*cap_start=*/false);
            }

            // Draw hub sensor dot - colored with filament color if loaded to hub
            bool dot_active = has_filament && slot_at_hub;
            lv_color_t dot_color = dot_active ? lane_color : idle_color;
            bool dot_filled = dot_active;
            // Error on hub dot: only for the active slot when error is at HUB
            if (has_error && is_active_slot && error_seg == PathSegment::HUB) {
                dot_color = error_color;
                dot_filled = true;
            }
            draw_sensor_dot(layer, hub_dot_x, hub_top, dot_color, dot_filled, sensor_r);
        } else if (data->topology == 0) {
            // LINEAR topology: SELECTOR is butted against prep sensors — no lines between
        } else {
            // Other non-hub topologies: converge to center merge point (S-curve)
            int32_t start_y_other = prep_y + sensor_r;
            int32_t drop_other = merge_y - start_y_other;
            int32_t cp1_x = slot_x;
            int32_t cp1_y = start_y_other + drop_other * 2 / 5;
            int32_t cp2_x = center_x;
            int32_t cp2_y = merge_y - drop_other * 2 / 5;
            if (merge_is_idle) {
                draw_curved_hollow_tube(layer, slot_x, start_y_other, cp1_x, cp1_y, cp2_x, cp2_y,
                                        center_x, merge_y, idle_color, bg_color, line_active,
                                        /*cap_start=*/false);
            } else {
                draw_glow_curve(layer, slot_x, start_y_other, cp1_x, cp1_y, cp2_x, cp2_y, center_x,
                                merge_y, merge_line_color, lane_width);
                draw_curved_tube(layer, slot_x, start_y_other, cp1_x, cp1_y, cp2_x, cp2_y, center_x,
                                 merge_y, merge_line_color, lane_width, /*cap_start=*/false);
            }
        }
    }

    // ========================================================================
    // Draw bypass entry and path
    // Topology:
    //   Hub Output Sensor (center_x, output_y)
    //        │ (hub-to-merge segment — idle when bypass active)
    //        ▼
    //   Merge Point / Bypass Sensor (center_x, bypass_merge_y)
    //        ▲
    //        │ horizontal from bypass spool
    //   Bypass Spool (bypass_x) → vertical down → horizontal to merge
    //
    // From merge point → toolhead → nozzle (shared downstream path)
    // Skipped in hub_only mode (bypass is a system-level path)
    // ========================================================================
    int32_t bypass_merge_y = y_off + (int32_t)(height * BYPASS_MERGE_Y_RATIO);

    if (!data->hub_only && data->show_bypass) {
        int32_t bypass_x = x_off + (int32_t)(width * BYPASS_X_RATIO);

        // Determine bypass colors
        lv_color_t bypass_line_color = idle_color;
        if (data->bypass_active) {
            bypass_line_color = lv_color_hex(data->bypass_color);
        }

        // Draw bypass filament path: horizontal line from merge sensor to spool area.
        // Line stops at spool's left edge (spool widget centered at bypass_x).
        // Spool is ~10% of width wide, so stop ~5% before bypass_x.
        int32_t bypass_line_end_x = x_off + (int32_t)(width * (BYPASS_X_RATIO - 0.05f));
        if (data->bypass_active) {
            draw_glow_line(layer, center_x + sensor_r, bypass_merge_y, bypass_line_end_x,
                           bypass_merge_y, bypass_line_color, line_active);
            draw_line(layer, center_x + sensor_r, bypass_merge_y, bypass_line_end_x, bypass_merge_y,
                      bypass_line_color, line_active);
        } else {
            draw_hollow_line(layer, center_x + sensor_r, bypass_merge_y, bypass_line_end_x,
                             bypass_merge_y, idle_color, bg_color, line_active);
        }

        // Draw bypass merge sensor dot (where bypass path joins center path)
        bool bypass_merge_active =
            data->bypass_active ||
            (data->active_slot >= 0 && is_segment_active(PathSegment::OUTPUT, fil_seg));
        lv_color_t bypass_merge_color = idle_color;
        if (data->bypass_active) {
            bypass_merge_color = lv_color_hex(data->bypass_color);
        } else if (data->active_slot >= 0 && is_segment_active(PathSegment::OUTPUT, fil_seg)) {
            bypass_merge_color = active_color;
        }
        draw_sensor_dot(layer, center_x, bypass_merge_y, bypass_merge_color, bypass_merge_active,
                        sensor_r);

        // Draw "Bypass" label beneath the spool widget
        if (data->label_font) {
            lv_draw_label_dsc_t label_dsc;
            lv_draw_label_dsc_init(&label_dsc);
            label_dsc.color = data->bypass_active ? bypass_line_color : data->color_text;
            label_dsc.font = data->label_font;
            label_dsc.align = LV_TEXT_ALIGN_CENTER;
            label_dsc.text = "Bypass";

            int32_t font_h = lv_font_get_line_height(data->label_font);
            int32_t label_half_w = width / 8;
            // Place below spool: spool is centered on bypass_merge_y, ~sensor_r*4 tall
            int32_t label_top = bypass_merge_y + sensor_r * 4;
            lv_area_t label_area = {bypass_x - label_half_w, label_top, bypass_x + label_half_w,
                                    label_top + font_h};
            lv_draw_label(layer, &label_dsc, &label_area);
        }
    }

    // ========================================================================
    // Draw hub/selector section
    // ========================================================================
    {
        bool hub_has_filament = false;

        if (data->topology == 0) {
            // LINEAR topology: lanes go straight to hub box (no merge line needed)
            if (data->active_slot >= 0 && is_segment_active(PathSegment::HUB, fil_seg)) {
                hub_has_filament = true;
            }
        } else if (data->topology != 1) {
            // Other non-hub topologies: draw single merge->hub line
            if (data->active_slot >= 0 && is_segment_active(PathSegment::HUB, fil_seg)) {
                lv_color_t hub_line_color = active_color;
                hub_has_filament = true;
                if (has_error && error_seg == PathSegment::HUB) {
                    hub_line_color = error_color;
                }
                draw_glow_line(layer, center_x, merge_y, center_x, hub_y - hub_h / 2,
                               hub_line_color, line_active);
                draw_vertical_line(layer, center_x, merge_y, hub_y - hub_h / 2, hub_line_color,
                                   line_active);
            } else {
                draw_hollow_vertical_line(layer, center_x, merge_y, hub_y - hub_h / 2, idle_color,
                                          bg_color, line_active);
            }
        } else {
            // HUB topology: lane lines go directly to hub sensor dots (drawn in lane loop above)
            // Check if any slot has filament at hub for tinting
            if (data->active_slot >= 0 && is_segment_active(PathSegment::HUB, fil_seg)) {
                hub_has_filament = true;
            } else {
                for (int i = 0; i < data->slot_count && i < FilamentPathData::MAX_SLOTS; i++) {
                    if (data->slot_filament_states[i].segment >= PathSegment::HUB) {
                        hub_has_filament = true;
                        break;
                    }
                }
            }
        }

        // Hub box - tint based on error state, buffer fault state, or filament color
        lv_color_t hub_bg_tinted = hub_bg;
        lv_color_t hub_border_final = hub_border;
        if (has_error && error_seg == PathSegment::HUB) {
            // Error at hub — red tint with pulsing error color
            hub_bg_tinted = ph_blend(hub_bg, error_color, 0.40f);
            hub_border_final = error_color;
        } else if (data->buffer_fault_state == 2) {
            // Fault detected — red tint
            hub_bg_tinted = ph_blend(hub_bg, data->color_error, 0.50f);
            hub_border_final = data->color_error;
        } else if (data->buffer_fault_state == 1) {
            // Approaching fault — yellow/warning tint
            lv_color_t warning = lv_color_hex(0xFFA500);
            hub_bg_tinted = ph_blend(hub_bg, warning, 0.40f);
            hub_border_final = warning;
        } else if (hub_has_filament) {
            // Healthy — subtle filament color tint (use first loaded slot's color)
            lv_color_t tint_color = active_color;
            if (data->active_slot < 0) {
                // No active slot — find first slot loaded to hub for tint
                for (int i = 0; i < data->slot_count && i < FilamentPathData::MAX_SLOTS; i++) {
                    if (data->slot_filament_states[i].segment >= PathSegment::HUB) {
                        tint_color = lv_color_hex(data->slot_filament_states[i].color);
                        break;
                    }
                }
            }
            hub_bg_tinted = ph_blend(hub_bg, tint_color, 0.33f);
        }

        const char* hub_label = (data->topology == 0) ? "SELECTOR" : "HUB";

        // For LINEAR topology, hub box spans the full slot area width.
        // get_slot_x returns slot centers, so we need to add half a slot width
        // on each side to cover the full visual extent of the outermost slots.
        int32_t hub_w = data->hub_width;
        if (data->topology == 0 && data->slot_count > 1) {
            int32_t first_slot_x = x_off + get_slot_x(data, 0, x_off);
            int32_t last_slot_x = x_off + get_slot_x(data, data->slot_count - 1, x_off);
            int32_t slot_pad = LV_MAX(data->slot_width, sensor_r * 4);
            hub_w = (last_slot_x - first_slot_x) + slot_pad;
        }

        draw_hub_box(layer, center_x, hub_y, hub_w, hub_h, hub_bg_tinted, hub_border_final,
                     data->color_text, data->label_font, data->border_radius, hub_label);
    }

    // ========================================================================
    // Draw output section (hub output sensor + hub-to-merge/toolhead segment)
    // Output sensor is butted against hub bottom (mirrors input sensors at hub top).
    // When bypass is shown, segment goes output → bypass merge point.
    // When bypass is hidden, segment goes output → toolhead directly.
    // ========================================================================
    if (!data->hub_only) {
        // Hub output sensor — determine if AMS filament is passing through
        bool ams_output_active = !data->bypass_active && data->active_slot >= 0 &&
                                 is_segment_active(PathSegment::OUTPUT, fil_seg);
        lv_color_t output_dot_color = idle_color;
        bool output_dot_filled = false;
        if (ams_output_active) {
            output_dot_color = active_color;
            output_dot_filled = true;
            if (has_error && error_seg == PathSegment::OUTPUT) {
                output_dot_color = error_color;
            }
        } else if (has_error && error_seg == PathSegment::OUTPUT) {
            output_dot_color = error_color;
            output_dot_filled = true;
        }
        draw_sensor_dot(layer, output_x, output_y, output_dot_color, output_dot_filled, sensor_r);

        // When bypass is hidden, output connects directly to toolhead (no merge point gap)
        int32_t output_end_y = data->show_bypass ? bypass_merge_y : toolhead_y;

        // Segment: output sensor → merge point (or toolhead when bypass hidden)
        // LINEAR: S-curve from output_x down to center_x
        // HUB: straight vertical at center_x
        if (data->topology == 0 && output_x != center_x) {
            // S-curve from (output_x, output_y+sensor_r) to (center_x, output_end_y-sensor_r)
            int32_t oc_start_y = output_y + sensor_r;
            int32_t oc_end_y = output_end_y - sensor_r;
            int32_t oc_drop = oc_end_y - oc_start_y;
            int32_t oc_cp1_x = output_x;
            int32_t oc_cp1_y = oc_start_y + oc_drop * 2 / 5;
            int32_t oc_cp2_x = center_x;
            int32_t oc_cp2_y = oc_end_y - oc_drop * 2 / 5;
            if (ams_output_active) {
                lv_color_t seg_color = active_color;
                if (has_error && error_seg == PathSegment::OUTPUT) {
                    seg_color = error_color;
                }
                draw_glow_curve(layer, output_x, oc_start_y, oc_cp1_x, oc_cp1_y, oc_cp2_x, oc_cp2_y,
                                center_x, oc_end_y, seg_color, line_active);
                draw_curved_tube(layer, output_x, oc_start_y, oc_cp1_x, oc_cp1_y, oc_cp2_x,
                                 oc_cp2_y, center_x, oc_end_y, seg_color, line_active,
                                 /*cap_start=*/false);
            } else {
                draw_curved_hollow_tube(layer, output_x, oc_start_y, oc_cp1_x, oc_cp1_y, oc_cp2_x,
                                        oc_cp2_y, center_x, oc_end_y, idle_color, bg_color,
                                        line_active, /*cap_start=*/false);
            }
        } else {
            // HUB or LINEAR with output at center: straight vertical
            if (ams_output_active) {
                lv_color_t seg_color = active_color;
                if (has_error && error_seg == PathSegment::OUTPUT) {
                    seg_color = error_color;
                }
                draw_glow_line(layer, center_x, output_y + sensor_r, center_x,
                               output_end_y - sensor_r, seg_color, line_active);
                draw_vertical_line(layer, center_x, output_y + sensor_r, output_end_y - sensor_r,
                                   seg_color, line_active);
            } else {
                draw_hollow_vertical_line(layer, center_x, output_y + sensor_r,
                                          output_end_y - sensor_r, idle_color, bg_color,
                                          line_active);
            }
        }
    }

    // ========================================================================
    // Draw toolhead section (bypass merge → toolhead)
    // Active when ANY filament is flowing (AMS or bypass)
    // Skipped when bypass is hidden — output section draws directly to toolhead
    // ========================================================================
    if (!data->hub_only && data->show_bypass) {
        lv_color_t toolhead_color = idle_color;
        bool toolhead_active = false;
        if (data->bypass_active) {
            toolhead_color = lv_color_hex(data->bypass_color);
            toolhead_active = true;
        } else if (data->active_slot >= 0 && is_segment_active(PathSegment::TOOLHEAD, fil_seg)) {
            toolhead_color = active_color;
            toolhead_active = true;
            if (has_error && error_seg == PathSegment::TOOLHEAD) {
                toolhead_color = error_color;
            }
        }

        // Line from bypass merge point to toolhead sensor
        if (toolhead_active) {
            draw_glow_line(layer, center_x, bypass_merge_y + sensor_r, center_x,
                           toolhead_y - sensor_r, toolhead_color, line_active);
            draw_vertical_line(layer, center_x, bypass_merge_y + sensor_r, toolhead_y - sensor_r,
                               toolhead_color, line_active);
        } else {
            draw_hollow_vertical_line(layer, center_x, bypass_merge_y + sensor_r,
                                      toolhead_y - sensor_r, idle_color, bg_color, line_active);
        }

        // Toolhead sensor
        lv_color_t toolhead_dot_color = toolhead_active ? toolhead_color : idle_color;
        bool toolhead_dot_filled = toolhead_active;
        if (has_error && error_seg == PathSegment::TOOLHEAD) {
            toolhead_dot_color = error_color;
            toolhead_dot_filled = true;
        }
        draw_sensor_dot(layer, center_x, toolhead_y, toolhead_dot_color, toolhead_dot_filled,
                        sensor_r);
    }

    // ========================================================================
    // Draw flow particles along active path (during load/unload animation)
    // Rendered BEFORE nozzle so the extruder body covers any dots that get close
    // ========================================================================
    if (data->flow_anim_active && data->active_slot >= 0 && !data->hub_only) {
        int32_t slot_x = x_off + get_slot_x(data, data->active_slot, x_off);
        bool reverse = (data->anim_direction == AnimDirection::UNLOADING);
        lv_color_t flow_color = active_color;

        // Flow dots on lane: entry → prep sensor
        draw_flow_dots_line(layer, slot_x, entry_y, slot_x, prep_y, flow_color, data->flow_offset,
                            reverse);

        // Flow dots on lane → hub curve
        if (data->topology == 1) {
            int32_t hub_top = hub_y - hub_h / 2;
            int32_t hub_dot_spacing =
                (data->slot_count > 1) ? (data->hub_width - 2 * sensor_r) / (data->slot_count - 1)
                                       : 0;
            int32_t hub_dot_x = center_x - (data->hub_width - 2 * sensor_r) / 2 +
                                data->active_slot * hub_dot_spacing;
            if (data->slot_count == 1)
                hub_dot_x = center_x;
            int32_t fd_start_y = prep_y + sensor_r;
            int32_t fd_end_y = hub_top - sensor_r;
            int32_t fd_drop = fd_end_y - fd_start_y;
            int32_t fd_cp1_x = slot_x;
            int32_t fd_cp1_y = fd_start_y + fd_drop * 2 / 5;
            int32_t fd_cp2_x = hub_dot_x;
            int32_t fd_cp2_y = fd_end_y - fd_drop * 2 / 5;
            draw_flow_dots_curve(layer, slot_x, fd_start_y, fd_cp1_x, fd_cp1_y, fd_cp2_x, fd_cp2_y,
                                 hub_dot_x, fd_end_y, flow_color, data->flow_offset, reverse);
        } else if (data->topology == 0) {
            // LINEAR: no gap between prep sensors and SELECTOR — skip flow dots here
        }

        // Flow dots on center path: hub → output → toolhead sensor
        int32_t hub_bottom = hub_y + hub_h / 2;
        if (data->topology == 0 && output_x != center_x) {
            // LINEAR: flow dots from hub bottom to output_x, then curve to center
            draw_flow_dots_line(layer, output_x, hub_bottom, output_x, output_y + sensor_r,
                                flow_color, data->flow_offset, reverse);
            int32_t oc_start_y = output_y + sensor_r;
            int32_t oc_end_y = (data->show_bypass ? bypass_merge_y : toolhead_y) - sensor_r;
            int32_t oc_drop = oc_end_y - oc_start_y;
            int32_t oc_cp1_x = output_x;
            int32_t oc_cp1_y = oc_start_y + oc_drop * 2 / 5;
            int32_t oc_cp2_x = center_x;
            int32_t oc_cp2_y = oc_end_y - oc_drop * 2 / 5;
            draw_flow_dots_curve(layer, output_x, oc_start_y, oc_cp1_x, oc_cp1_y, oc_cp2_x,
                                 oc_cp2_y, center_x, oc_end_y, flow_color, data->flow_offset,
                                 reverse);
            // Continue straight from merge/bypass to toolhead if bypass shown
            if (data->show_bypass) {
                draw_flow_dots_line(layer, center_x, bypass_merge_y, center_x,
                                    toolhead_y - sensor_r, flow_color, data->flow_offset, reverse);
            }
        } else {
            draw_flow_dots_line(layer, center_x, hub_bottom, center_x, toolhead_y - sensor_r,
                                flow_color, data->flow_offset, reverse);
        }
    }

    // ========================================================================
    // Draw nozzle
    // ========================================================================
    if (!data->hub_only) {
        lv_color_t noz_color = nozzle_color;

        // Bypass or normal slot active?
        if (data->bypass_active) {
            // Bypass active - use bypass color for nozzle
            noz_color = lv_color_hex(data->bypass_color);
        } else if (data->active_slot >= 0 && is_segment_active(PathSegment::NOZZLE, fil_seg)) {
            noz_color = active_color;
            if (has_error && error_seg == PathSegment::NOZZLE) {
                noz_color = error_color;
            }
        }

        // Line from toolhead sensor to extruder (adjust gap for tall extruder body)
        // Use toolhead color (idle gray when no filament) for the connecting line,
        // not nozzle color which is always tinted
        bool nozzle_has_filament =
            data->bypass_active ||
            (data->active_slot >= 0 && is_segment_active(PathSegment::NOZZLE, fil_seg));
        int32_t extruder_half_height = data->extruder_scale * 2; // Half of body_height
        if (nozzle_has_filament) {
            draw_glow_line(layer, center_x, toolhead_y + sensor_r, center_x,
                           nozzle_y - extruder_half_height, noz_color, line_active);
            draw_vertical_line(layer, center_x, toolhead_y + sensor_r,
                               nozzle_y - extruder_half_height, noz_color, line_active);
        } else {
            draw_hollow_vertical_line(layer, center_x, toolhead_y + sensor_r,
                                      nozzle_y - extruder_half_height, idle_color, bg_color,
                                      line_active);
        }

        // Extruder/print head icon (responsive size)
        // Draw nozzle first so heat glow can render on top
        if (data->use_faceted_toolhead) {
            draw_nozzle_faceted(layer, center_x, nozzle_y, noz_color, data->extruder_scale);
        } else {
            draw_nozzle_bambu(layer, center_x, nozzle_y, noz_color, data->extruder_scale);
        }

        // Draw heat glow around nozzle tip when heating (after nozzle so glow is visible)
        if (data->heat_active) {
            int32_t tip_y;
            if (data->use_faceted_toolhead) {
                // Stealthburner: nozzle tip is further below center due to larger body
                // Tip is at cy + (460 * scale) - 6 where scale = extruder_scale / 100
                tip_y = nozzle_y + (data->extruder_scale * 46) / 10 - 6;
            } else {
                // Bambu: tip is at cy + body_height/2 + tip_height
                // = cy + scale*2 + scale*0.6 = cy + scale*2.6
                tip_y = nozzle_y + (data->extruder_scale * 26) / 10;
            }
            draw_heat_glow(layer, center_x, tip_y, sensor_r, data->heat_pulse_opa);
        }
    }

    // ========================================================================
    // ========================================================================
    // Draw animated filament tip (during segment transitions)
    // ========================================================================
    if (is_animating && data->active_slot >= 0 && !data->hub_only) {
        float progress_factor = anim_progress / 100.0f;
        int32_t slot_x = x_off + get_slot_x(data, data->active_slot, x_off);
        int32_t hub_top = hub_y - hub_h / 2;

        // Helper to evaluate cubic bezier (1D) at parameter t
        auto bezier_eval_1d = [](float t, int32_t p0, int32_t c1, int32_t c2,
                                 int32_t p1) -> int32_t {
            float inv = 1.0f - t;
            float b0 = inv * inv * inv;
            float b1 = 3.0f * inv * inv * t;
            float b2 = 3.0f * inv * t * t;
            float b3 = t * t * t;
            return (int32_t)(b0 * p0 + b1 * c1 + b2 * c2 + b3 * p1);
        };

        // Determine if the current transition crosses the curved lane-to-hub segment
        // The curve runs from prep sensor (slot_x) to hub entry (hub_dot_x) for HUB topology
        bool on_curve_segment = false;
        if (data->topology == 1) { // HUB topology has curved lanes
            // Loading: PREP→LANE or LANE→HUB cross the curve
            // Unloading: HUB→LANE or LANE→PREP cross the curve
            on_curve_segment = (prev_seg == PathSegment::PREP && fil_seg == PathSegment::LANE) ||
                               (prev_seg == PathSegment::LANE && fil_seg == PathSegment::HUB) ||
                               (prev_seg == PathSegment::HUB && fil_seg == PathSegment::LANE) ||
                               (prev_seg == PathSegment::LANE && fil_seg == PathSegment::PREP);
        }

        int32_t tip_x, tip_y;

        if (on_curve_segment) {
            // Follow the bezier curve from prep sensor to hub entry
            int32_t hub_dot_spacing =
                (data->slot_count > 1) ? (data->hub_width - 2 * sensor_r) / (data->slot_count - 1)
                                       : 0;
            int32_t hub_dot_x = center_x - (data->hub_width - 2 * sensor_r) / 2 +
                                data->active_slot * hub_dot_spacing;
            if (data->slot_count == 1)
                hub_dot_x = center_x;

            // Cubic bezier: start=(slot_x, prep_y+sensor_r), end=(hub_dot_x, hub_top-sensor_r)
            int32_t bz_x0 = slot_x, bz_y0 = prep_y + sensor_r;
            int32_t bz_x1 = hub_dot_x, bz_y1 = hub_top - sensor_r;
            int32_t bz_drop = bz_y1 - bz_y0;
            int32_t bz_cx1 = slot_x, bz_cy1 = bz_y0 + bz_drop * 2 / 5;
            int32_t bz_cx2 = hub_dot_x, bz_cy2 = bz_y1 - bz_drop * 2 / 5;

            // Map segment pair to curve parameter range (curve spans PREP→HUB = two segments)
            float t;
            if ((is_loading && prev_seg == PathSegment::PREP) ||
                (!is_loading && fil_seg == PathSegment::PREP)) {
                // First half of curve (0.0 → 0.5)
                t = is_loading ? progress_factor * 0.5f : (1.0f - progress_factor) * 0.5f;
            } else {
                // Second half of curve (0.5 → 1.0)
                t = is_loading ? 0.5f + progress_factor * 0.5f
                               : 0.5f + (1.0f - progress_factor) * 0.5f;
            }

            tip_x = bezier_eval_1d(t, bz_x0, bz_cx1, bz_cx2, bz_x1);
            tip_y = bezier_eval_1d(t, bz_y0, bz_cy1, bz_cy2, bz_y1);
        } else {
            // Straight segments — use Y mapping and simple X interpolation
            auto get_segment_y = [&](PathSegment seg) -> int32_t {
                switch (seg) {
                case PathSegment::NONE:
                case PathSegment::SPOOL:
                    return entry_y;
                case PathSegment::PREP:
                    return prep_y;
                case PathSegment::LANE:
                    return merge_y;
                case PathSegment::HUB:
                    return hub_y;
                case PathSegment::OUTPUT:
                    return output_y;
                case PathSegment::TOOLHEAD:
                    return toolhead_y;
                case PathSegment::NOZZLE:
                    return nozzle_y - data->extruder_scale * 2; // Top of extruder
                default:
                    return entry_y;
                }
            };

            int32_t from_y = get_segment_y(prev_seg);
            int32_t to_y = get_segment_y(fil_seg);
            tip_y = from_y + (int32_t)((to_y - from_y) * progress_factor);

            // X position: on lane (at slot_x), on center path (at center_x)
            tip_x = center_x;
            if (prev_seg <= PathSegment::PREP && fil_seg <= PathSegment::PREP) {
                // Both ends on lane — stay at slot_x
                tip_x = slot_x;
            } else if (data->topology == 0 && output_x != center_x) {
                // LINEAR: tip follows the output curve when crossing HUB<>OUTPUT or
                // OUTPUT<>TOOLHEAD
                bool on_output_curve =
                    (prev_seg == PathSegment::HUB && fil_seg == PathSegment::OUTPUT) ||
                    (prev_seg == PathSegment::OUTPUT && fil_seg == PathSegment::HUB) ||
                    (prev_seg == PathSegment::OUTPUT && fil_seg == PathSegment::TOOLHEAD) ||
                    (prev_seg == PathSegment::TOOLHEAD && fil_seg == PathSegment::OUTPUT);
                if (on_output_curve) {
                    int32_t oc_start_y = output_y + sensor_r;
                    int32_t oc_end_y = (data->show_bypass ? bypass_merge_y : toolhead_y) - sensor_r;
                    int32_t oc_drop = oc_end_y - oc_start_y;
                    int32_t oc_cp1_x = output_x;
                    int32_t oc_cp1_y = oc_start_y + oc_drop * 2 / 5;
                    int32_t oc_cp2_x = center_x;
                    int32_t oc_cp2_y = oc_end_y - oc_drop * 2 / 5;
                    // Map tip_y to bezier t parameter (approximate via Y range)
                    float t_approx = 0.0f;
                    if (oc_end_y != oc_start_y) {
                        t_approx = (float)(tip_y - oc_start_y) / (float)(oc_end_y - oc_start_y);
                        t_approx = LV_CLAMP(t_approx, 0.0f, 1.0f);
                    }
                    tip_x = bezier_eval_1d(t_approx, output_x, oc_cp1_x, oc_cp2_x, center_x);
                }
            }
        }

        // Skip drawing the tip when it's inside the extruder body (TOOLHEAD↔NOZZLE).
        // The filament is hidden inside the nozzle — no visible dot makes sense.
        bool in_nozzle_body =
            (prev_seg == PathSegment::TOOLHEAD && fil_seg == PathSegment::NOZZLE) ||
            (prev_seg == PathSegment::NOZZLE && fil_seg == PathSegment::TOOLHEAD);
        if (!in_nozzle_body) {
            draw_filament_tip(layer, tip_x, tip_y, active_color, sensor_r);
        }
    }

    spdlog::trace("[FilamentPath] Draw: slots={}, active={}, segment={}, anim={}", data->slot_count,
                  data->active_slot, data->filament_segment, is_animating ? anim_progress : -1);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void filament_path_click_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    lv_point_t point;
    lv_indev_t* indev = lv_indev_active();
    lv_indev_get_point(indev, &point);

    // Get widget dimensions
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int32_t width = lv_area_get_width(&obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // For PARALLEL topology (tool changers), also accept clicks on the toolhead area
    if (data->topology == static_cast<int>(PathTopology::PARALLEL) && data->slot_callback) {
        constexpr float PARALLEL_TOOLHEAD_Y = 0.55f;
        int32_t toolhead_y = y_off + (int32_t)(height * PARALLEL_TOOLHEAD_Y);
        int32_t tool_scale = LV_MAX(6, data->extruder_scale * 2 / 3);
        int32_t hit_radius_y = tool_scale * 4; // Generous vertical hit area around toolhead

        if (abs(point.y - toolhead_y) < hit_radius_y) {
            for (int i = 0; i < data->slot_count; i++) {
                int32_t slot_x = x_off + get_slot_x(data, i, x_off);
                int32_t hit_radius_x = LV_MAX(20, tool_scale * 3);
                if (abs(point.x - slot_x) < hit_radius_x) {
                    spdlog::debug("[FilamentPath] Toolhead {} clicked (parallel topology)", i);
                    data->slot_callback(i, data->slot_user_data);
                    return;
                }
            }
        }
    }

    // Check if bypass spool box was clicked (right side) — check before entry area
    // Y-range guard because the spool box may be outside the slot entry area
    if (data->show_bypass && data->bypass_callback) {
        int32_t bypass_x = x_off + (int32_t)(width * BYPASS_X_RATIO);
        int32_t bypass_merge_y = y_off + (int32_t)(height * BYPASS_MERGE_Y_RATIO);
        int32_t sensor_r = data->sensor_radius;
        int32_t box_w = sensor_r * 3;
        int32_t box_h = sensor_r * 4;
        if (abs(point.x - bypass_x) < box_w && abs(point.y - bypass_merge_y) < box_h) {
            spdlog::debug("[FilamentPath] Bypass spool box clicked");
            data->bypass_callback(data->bypass_user_data);
            return;
        }
    }

    // Check if click is in the entry area (top portion)
    int32_t entry_y = y_off + (int32_t)(height * ENTRY_Y_RATIO);
    int32_t prep_y = y_off + (int32_t)(height * PREP_Y_RATIO);

    if (point.y < entry_y - 10 || point.y > prep_y + 20)
        return; // Click not in entry area

    // Find which slot was clicked
    if (data->slot_callback) {
        for (int i = 0; i < data->slot_count; i++) {
            int32_t slot_x = x_off + get_slot_x(data, i, x_off);
            if (abs(point.x - slot_x) < 20) {
                spdlog::debug("[FilamentPath] Slot {} clicked", i);
                data->slot_callback(i, data->slot_user_data);
                return;
            }
        }
    }
}

static void filament_path_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto it = s_registry.find(obj);
    if (it != s_registry.end()) {
        // Stop any running animations before deleting
        std::unique_ptr<FilamentPathData> data(it->second);
        if (data) {
            lv_anim_delete(obj, segment_anim_cb);
            lv_anim_delete(obj, error_pulse_anim_cb);
            lv_anim_delete(obj, heat_pulse_anim_cb);
            lv_anim_delete(obj, flow_anim_cb);
        }
        s_registry.erase(it);
        // data automatically freed when unique_ptr goes out of scope
    }
}

// ============================================================================
// XML Widget Interface
// ============================================================================

static void* filament_path_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create(static_cast<lv_obj_t*>(parent));
    if (!obj)
        return nullptr;

    auto data_ptr = std::make_unique<FilamentPathData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    // Load theme-aware colors, fonts, and sizes
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, filament_path_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, filament_path_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(obj, filament_path_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[FilamentPath] Created widget");
    return obj;
}

static void filament_path_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);
    if (!obj)
        return;

    lv_xml_obj_apply(state, attrs);

    auto* data = get_data(obj);
    if (!data)
        return;

    bool needs_redraw = false;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "topology") == 0) {
            if (strcmp(value, "linear") == 0 || strcmp(value, "0") == 0) {
                data->topology = 0;
            } else {
                data->topology = 1; // default to hub
            }
            needs_redraw = true;
        } else if (strcmp(name, "slot_count") == 0) {
            data->slot_count = LV_CLAMP(atoi(value), 1, 16);
            needs_redraw = true;
        } else if (strcmp(name, "active_slot") == 0) {
            data->active_slot = atoi(value);
            needs_redraw = true;
        } else if (strcmp(name, "filament_segment") == 0) {
            data->filament_segment = LV_CLAMP(atoi(value), 0, PATH_SEGMENT_COUNT - 1);
            needs_redraw = true;
        } else if (strcmp(name, "error_segment") == 0) {
            data->error_segment = LV_CLAMP(atoi(value), 0, PATH_SEGMENT_COUNT - 1);
            needs_redraw = true;
        } else if (strcmp(name, "anim_progress") == 0) {
            data->anim_progress = LV_CLAMP(atoi(value), 0, 100);
            needs_redraw = true;
        } else if (strcmp(name, "filament_color") == 0) {
            data->filament_color = strtoul(value, nullptr, 0);
            needs_redraw = true;
        } else if (strcmp(name, "bypass_active") == 0) {
            data->bypass_active = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        } else if (strcmp(name, "show_bypass") == 0) {
            data->show_bypass = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        } else if (strcmp(name, "faceted_toolhead") == 0) {
            data->use_faceted_toolhead = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        } else if (strcmp(name, "hub_only") == 0) {
            data->hub_only = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        }
    }

    if (needs_redraw) {
        lv_obj_invalidate(obj);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_filament_path_canvas_register(void) {
    lv_xml_register_widget("filament_path_canvas", filament_path_xml_create,
                           filament_path_xml_apply);
    spdlog::info("[FilamentPath] Registered filament_path_canvas widget with XML system");
}

lv_obj_t* ui_filament_path_canvas_create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[FilamentPath] Cannot create: parent is null");
        return nullptr;
    }

    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj) {
        spdlog::error("[FilamentPath] Failed to create object");
        return nullptr;
    }

    auto data_ptr = std::make_unique<FilamentPathData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    // Load theme-aware colors, fonts, and sizes
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, filament_path_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, filament_path_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(obj, filament_path_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[FilamentPath] Created widget programmatically");
    return obj;
}

void ui_filament_path_canvas_set_topology(lv_obj_t* obj, int topology) {
    auto* data = get_data(obj);
    if (data) {
        data->topology = topology;
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_slot_count(lv_obj_t* obj, int count) {
    auto* data = get_data(obj);
    if (data) {
        data->slot_count = LV_CLAMP(count, 1, 16);
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_slot_overlap(lv_obj_t* obj, int32_t overlap) {
    auto* data = get_data(obj);
    if (data) {
        data->slot_overlap = LV_MAX(overlap, 0);
        spdlog::trace("[FilamentPath] Slot overlap set to {}px", data->slot_overlap);
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_slot_width(lv_obj_t* obj, int32_t width) {
    auto* data = get_data(obj);
    if (data) {
        data->slot_width = LV_MAX(width, 20); // Minimum 20px
        spdlog::trace("[FilamentPath] Slot width set to {}px", data->slot_width);
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_slot_grid(lv_obj_t* obj, lv_obj_t* slot_grid) {
    auto* data = get_data(obj);
    if (!data)
        return;

    data->slot_grid = slot_grid;

    // Pre-cache spool_container pointers to avoid per-frame lv_obj_find_by_name
    std::memset(data->spool_containers, 0, sizeof(data->spool_containers));
    if (slot_grid) {
        int child_count =
            LV_MIN((int)lv_obj_get_child_count(slot_grid), FilamentPathData::MAX_SLOTS);
        for (int i = 0; i < child_count; i++) {
            lv_obj_t* slot = lv_obj_get_child(slot_grid, i);
            if (slot) {
                data->spool_containers[i] = lv_obj_find_by_name(slot, "spool_container");
            }
        }
        spdlog::debug("[FilamentPath] Cached {} spool_container pointers from slot_grid",
                      child_count);
    }
}

void ui_filament_path_canvas_set_active_slot(lv_obj_t* obj, int slot) {
    auto* data = get_data(obj);
    if (data) {
        int old_slot = data->active_slot;
        data->active_slot = slot;

        // LINEAR topology: animate output_x sliding to new slot position
        if (data->topology == 0 && slot >= 0 && old_slot >= 0 && old_slot != slot) {
            lv_area_t coords;
            lv_obj_get_coords(obj, &coords);
            int32_t x_off = coords.x1;
            int32_t new_x = x_off + get_slot_x(data, slot, x_off);
            int32_t old_x = data->output_x_current;
            if (old_x == 0)
                old_x = x_off + get_slot_x(data, old_slot, x_off);
            start_output_x_animation(obj, data, old_x, new_x);
        }

        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_filament_segment(lv_obj_t* obj, int segment) {
    auto* data = get_data(obj);
    if (!data)
        return;

    int new_segment = LV_CLAMP(segment, 0, PATH_SEGMENT_COUNT - 1);
    int old_segment = data->filament_segment;

    if (new_segment != old_segment) {
        // Start animation from old to new segment
        start_segment_animation(obj, data, old_segment, new_segment);
        data->filament_segment = new_segment;
        spdlog::info("[FilamentPath] Segment changed: {} -> {} (animating)", old_segment,
                     new_segment);
    }

    // Stop flow animation when filament reaches a terminal position via a
    // single-step transition (normal operation). Big jumps (e.g., 0→7 initial
    // setup) are not real flow operations — don't stop flow for those.
    if (data->flow_anim_active && new_segment != old_segment) {
        int step = std::abs(new_segment - old_segment);
        bool is_terminal = (new_segment == 0 || new_segment == PATH_SEGMENT_COUNT - 1);
        if (is_terminal && step <= 2) {
            stop_flow_animation(obj, data);
        }
    }

    lv_obj_invalidate(obj);
}

void ui_filament_path_canvas_set_error_segment(lv_obj_t* obj, int segment) {
    auto* data = get_data(obj);
    if (!data)
        return;

    int new_error = LV_CLAMP(segment, 0, PATH_SEGMENT_COUNT - 1);
    int old_error = data->error_segment;

    data->error_segment = new_error;

    // Start or stop error pulse animation
    if (new_error > 0 && old_error == 0) {
        // Error appeared - start pulsing
        start_error_pulse(obj, data);
        spdlog::debug("[FilamentPath] Error at segment {} - starting pulse", new_error);
    } else if (new_error == 0 && old_error > 0) {
        // Error cleared - stop pulsing
        stop_error_pulse(obj, data);
        spdlog::debug("[FilamentPath] Error cleared - stopping pulse");
    }

    lv_obj_invalidate(obj);
}

void ui_filament_path_canvas_set_anim_progress(lv_obj_t* obj, int progress) {
    auto* data = get_data(obj);
    if (data) {
        data->anim_progress = LV_CLAMP(progress, 0, 100);
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_filament_color(lv_obj_t* obj, uint32_t color) {
    auto* data = get_data(obj);
    if (data) {
        data->filament_color = color;
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_refresh(lv_obj_t* obj) {
    lv_obj_invalidate(obj);
}

void ui_filament_path_canvas_set_slot_callback(lv_obj_t* obj, filament_path_slot_cb_t cb,
                                               void* user_data) {
    auto* data = get_data(obj);
    if (data) {
        data->slot_callback = cb;
        data->slot_user_data = user_data;
    }
}

void ui_filament_path_canvas_animate_segment(lv_obj_t* obj, int from_segment, int to_segment) {
    auto* data = get_data(obj);
    if (!data)
        return;

    int from = LV_CLAMP(from_segment, 0, PATH_SEGMENT_COUNT - 1);
    int to = LV_CLAMP(to_segment, 0, PATH_SEGMENT_COUNT - 1);

    if (from != to) {
        start_segment_animation(obj, data, from, to);
        data->filament_segment = to;
    }
}

bool ui_filament_path_canvas_is_animating(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (!data)
        return false;

    return data->segment_anim_active || data->error_pulse_active || data->flow_anim_active;
}

void ui_filament_path_canvas_stop_animations(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (!data)
        return;

    stop_segment_animation(obj, data);
    stop_error_pulse(obj, data);
    stop_flow_animation(obj, data);
    stop_heat_pulse(obj, data);
    lv_obj_invalidate(obj);
}

void ui_filament_path_canvas_set_slot_filament(lv_obj_t* obj, int slot_index, int segment,
                                               uint32_t color) {
    auto* data = get_data(obj);
    if (!data || slot_index < 0 || slot_index >= FilamentPathData::MAX_SLOTS)
        return;

    auto& state = data->slot_filament_states[slot_index];
    PathSegment new_segment = static_cast<PathSegment>(segment);

    if (state.segment != new_segment || state.color != color) {
        state.segment = new_segment;
        state.color = color;
        spdlog::trace("[FilamentPath] Slot {} filament: segment={}, color=0x{:06X}", slot_index,
                      segment, color);
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_slot_prep_sensor(lv_obj_t* obj, int slot, bool has_sensor) {
    auto* data = get_data(obj);
    if (!data || slot < 0 || slot >= FilamentPathData::MAX_SLOTS)
        return;
    if (data->slot_has_prep_sensor[slot] != has_sensor) {
        data->slot_has_prep_sensor[slot] = has_sensor;
        spdlog::trace("[FilamentPath] Slot {} prep sensor: {}", slot, has_sensor);
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_clear_slot_filaments(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (!data)
        return;

    bool changed = false;
    for (int i = 0; i < FilamentPathData::MAX_SLOTS; i++) {
        if (data->slot_filament_states[i].segment != PathSegment::NONE) {
            data->slot_filament_states[i].segment = PathSegment::NONE;
            data->slot_filament_states[i].color = 0x808080;
            changed = true;
        }
    }

    if (changed) {
        spdlog::trace("[FilamentPath] Cleared all slot filament states");
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_show_bypass(lv_obj_t* obj, bool show) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->show_bypass != show) {
        data->show_bypass = show;
        spdlog::debug("[FilamentPath] Show bypass: {}", show ? "yes" : "no");
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_bypass_active(lv_obj_t* obj, bool active) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->bypass_active != active) {
        data->bypass_active = active;
        spdlog::debug("[FilamentPath] Bypass mode: {}", active ? "active" : "inactive");
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_bypass_callback(lv_obj_t* obj, filament_path_bypass_cb_t cb,
                                                 void* user_data) {
    auto* data = get_data(obj);
    if (data) {
        data->bypass_callback = cb;
        data->bypass_user_data = user_data;
    }
}

void ui_filament_path_canvas_set_hub_only(lv_obj_t* obj, bool hub_only) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->hub_only != hub_only) {
        data->hub_only = hub_only;
        spdlog::debug("[FilamentPath] Hub-only mode: {}", hub_only ? "on" : "off");
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_faceted_toolhead(lv_obj_t* obj, bool faceted) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->use_faceted_toolhead != faceted) {
        data->use_faceted_toolhead = faceted;
        spdlog::debug("[FilamentPath] Toolhead style: {}", faceted ? "faceted" : "bambu");
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_heat_active(lv_obj_t* obj, bool active) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->heat_active != active) {
        data->heat_active = active;

        if (active) {
            start_heat_pulse(obj, data);
            spdlog::debug("[FilamentPath] Heat glow: active");
        } else {
            stop_heat_pulse(obj, data);
            spdlog::debug("[FilamentPath] Heat glow: inactive");
        }

        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_buffer_fault_state(lv_obj_t* obj, int state) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->buffer_fault_state != state) {
        data->buffer_fault_state = state;
        spdlog::debug("[FilamentPath] Buffer fault state: {}", state);
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_bypass_color(lv_obj_t* obj, uint32_t color) {
    auto* data = get_data(obj);
    if (data) {
        data->bypass_color = color;
        lv_obj_invalidate(obj);
    }
}

void ui_filament_path_canvas_set_bypass_has_spool(lv_obj_t* obj, bool has_spool) {
    auto* data = get_data(obj);
    if (data) {
        data->bypass_has_spool = has_spool;
        lv_obj_invalidate(obj);
    }
}
