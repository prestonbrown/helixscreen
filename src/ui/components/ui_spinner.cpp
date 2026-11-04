// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spinner.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>

// Material Design indeterminate spinner timing constants
namespace {
constexpr uint32_t kRotationDurationMs = 1568; // Full 360° rotation of the arc
constexpr uint32_t kSweepDurationMs = 667;     // Half of grow/shrink cycle
constexpr int32_t kArcMinSweep = 45;  // Minimum arc length (degrees) - larger for visible rotation
constexpr int32_t kArcMaxSweep = 270; // Maximum arc length (degrees)

// Debug: set to true to log animation values
constexpr bool kDebugSpinner = false;
} // namespace

/**
 * @brief Get integer value from a responsive token
 *
 * The responsive spacing system auto-registers base tokens (e.g., "spinner_lg")
 * from globals.xml triplets (spinner_lg_small/medium/large) based on breakpoint.
 *
 * @param token_name Token name without size suffix
 * @param fallback Default value if token not found
 * @return Pixel value for current breakpoint
 */
static int32_t get_responsive_px(const char* token_name, int32_t fallback) {
    const char* val = lv_xml_get_const(nullptr, token_name);
    if (val) {
        return static_cast<int32_t>(atoi(val));
    }
    spdlog::warn("[ui_spinner] Token '{}' not found, using fallback {}", token_name, fallback);
    return fallback;
}

// Animation callbacks - these update the arc properties each frame
// Store last values for combined debug output
static int32_t g_last_start = 0;
static int32_t g_last_end = 0;

// Clamp an angle so the sweep (from..to) never falls below kArcMinSweep.
// At animation repeat boundaries the two independent animations can desync
// by one frame, creating an invalid negative sweep that crashes NEON blend.
static int32_t clamp_sweep(int32_t angle, int32_t reference, bool angle_is_start) {
    int32_t sweep = angle_is_start ? (reference - angle) : (angle - reference);
    if (sweep < 0)
        sweep += 360;
    if (sweep >= kArcMinSweep)
        return angle;

    int32_t clamped = angle_is_start ? (reference - kArcMinSweep) : (reference + kArcMinSweep);
    if (clamped < 0)
        clamped += 360;
    if (clamped >= 360)
        clamped -= 360;
    return clamped;
}

static void arc_anim_start_angle(void* obj, int32_t value) {
    value = clamp_sweep(value, g_last_end, /*angle_is_start=*/true);
    g_last_start = value;

    if (kDebugSpinner) {
        static int counter = 0;
        static int32_t min_sweep = 999, max_sweep = 0;
        int32_t cur_sweep = g_last_end - value;
        if (cur_sweep < 0)
            cur_sweep += 360; // Handle wrap

        if (cur_sweep < min_sweep)
            min_sweep = cur_sweep;
        if (cur_sweep > max_sweep)
            max_sweep = cur_sweep;

        // Log every 8 frames with min/max tracking
        if (counter++ % 8 == 0) {
            spdlog::info("[SPIN] start={:3d} end={:3d} sweep={:3d} [range: {:3d}-{:3d}]", value,
                         g_last_end % 360, cur_sweep, min_sweep, max_sweep);
        }
    }
    lv_arc_set_start_angle(static_cast<lv_obj_t*>(obj), static_cast<uint32_t>(value));
}

static void arc_anim_end_angle(void* obj, int32_t value) {
    value = clamp_sweep(value, g_last_start, /*angle_is_start=*/false);
    g_last_end = value;

    lv_arc_set_end_angle(static_cast<lv_obj_t*>(obj), static_cast<uint32_t>(value));
}

static void arc_anim_rotation(void* obj, int32_t value) {
    lv_arc_set_rotation(static_cast<lv_obj_t*>(obj), static_cast<int32_t>(value));
}

/**
 * @brief Cleanup callback when spinner is deleted
 *
 * Removes all running animations to prevent dangling pointer access.
 */
static void spinner_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_anim_delete(obj, arc_anim_start_angle);
    lv_anim_delete(obj, arc_anim_end_angle);
    lv_anim_delete(obj, arc_anim_rotation);
}

/**
 * @brief Start Material Design indeterminate spinner animations
 *
 * Uses LVGL's proven approach: both start and end angles go 0→360,
 * same duration, but different easing curves. NO playback!
 *
 * Both edges always move FORWARD (clockwise). Different speeds create grow/shrink:
 * - When start moves slower than end: arc grows
 * - When start moves faster than end: arc shrinks
 *
 * The bezier curve (0.4, 0.0, 0.2, 1.0) makes start:
 * - Slow at the beginning (arc grows as end pulls ahead)
 * - Fast in the middle (arc shrinks as start catches up)
 * - Slow at the end (arc stabilizes, then repeats)
 *
 * End angle offset by kArcMaxSweep ensures arc is never zero.
 *
 * @param arc The arc object to animate
 */
static void start_material_spinner_animations(lv_obj_t* arc) {
    // Material Design indeterminate spinner using OPPOSITE BEZIER CURVES
    //
    // Mathematical key: sweep(t) = offset + 360 * (ease_end(t) - ease_start(t))
    //
    // Using opposite curves creates maximum sweep variation:
    // - End uses EASE-OUT (fast start, slow end): races ahead early
    // - Start uses EASE-IN (slow start, fast end): catches up late
    //
    // At t=0 and t=1: both curves at same point → sweep = offset (minimum)
    // At t=0.5: maximum difference between curves → sweep = maximum

    // Animation 1: END angle (leading edge) - AGGRESSIVE EASE-OUT
    // Races ahead VERY fast early, then crawls → maximizes sweep differential
    lv_anim_t anim_end;
    lv_anim_init(&anim_end);
    lv_anim_set_var(&anim_end, arc);
    lv_anim_set_exec_cb(&anim_end, arc_anim_end_angle);
    lv_anim_set_duration(&anim_end, kSweepDurationMs * 2);
    lv_anim_set_values(&anim_end, kArcMinSweep, kArcMinSweep + 360);
    lv_anim_set_repeat_count(&anim_end, LV_ANIM_REPEAT_INFINITE);
    // Aggressive ease-out: (0, 0, 0.2, 1) - very fast start, very slow end
    lv_anim_set_path_cb(&anim_end, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(&anim_end, LV_BEZIER_VAL_FLOAT(0.0), LV_BEZIER_VAL_FLOAT(0.0),
                              LV_BEZIER_VAL_FLOAT(0.2), LV_BEZIER_VAL_FLOAT(1.0));
    lv_anim_start(&anim_end);

    // Animation 2: START angle (trailing edge) - AGGRESSIVE EASE-IN
    // Lingers VERY long at start, then races to catch up
    lv_anim_t anim_start;
    lv_anim_init(&anim_start);
    lv_anim_set_var(&anim_start, arc);
    lv_anim_set_exec_cb(&anim_start, arc_anim_start_angle);
    lv_anim_set_duration(&anim_start, kSweepDurationMs * 2);
    lv_anim_set_values(&anim_start, 0, 360);
    lv_anim_set_repeat_count(&anim_start, LV_ANIM_REPEAT_INFINITE);
    // Aggressive ease-in: (0.8, 0, 1, 1) - very slow start, very fast end
    lv_anim_set_path_cb(&anim_start, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(&anim_start, LV_BEZIER_VAL_FLOAT(0.8), LV_BEZIER_VAL_FLOAT(0.0),
                              LV_BEZIER_VAL_FLOAT(1.0), LV_BEZIER_VAL_FLOAT(1.0));
    lv_anim_start(&anim_start);

    // Animation 3: ROTATION - continuous clockwise spin of the whole arc
    // This adds a base "progression around the circle" feeling
    lv_anim_t anim_rot;
    lv_anim_init(&anim_rot);
    lv_anim_set_var(&anim_rot, arc);
    lv_anim_set_exec_cb(&anim_rot, arc_anim_rotation);
    lv_anim_set_duration(&anim_rot, kRotationDurationMs); // ~1.5s for full rotation
    lv_anim_set_values(&anim_rot, 270, 270 + 360);        // Start at top, go full circle
    lv_anim_set_repeat_count(&anim_rot, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim_rot, lv_anim_path_linear); // Constant rotation speed
    lv_anim_start(&anim_rot);
}

/**
 * @brief XML create callback for <spinner> widget
 *
 * Creates a Material Design-style indeterminate spinner with:
 * - Responsive size based on "size" attribute (xs, sm, md, lg)
 * - Primary color indicator arc
 * - "Chasing tail" animation where arc grows/shrinks while rotating, unless
 *   animated="false"
 *
 * @param state XML parser state
 * @param attrs XML attributes
 * @return Created spinner object
 */
static void* ui_spinner_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));

    // Create arc directly (not lv_spinner) for custom animation control
    lv_obj_t* arc = lv_arc_create(parent);

    // Parse size attribute (default: lg)
    const char* size_str = lv_xml_get_value_of(attrs, "size");
    if (!size_str) {
        size_str = "lg";
    }

    // Get responsive size and arc width from tokens
    int32_t size = 64;
    int32_t arc_width = 4;

    if (strcmp(size_str, "xs") == 0) {
        size = get_responsive_px("spinner_xs", 16);
        arc_width = get_responsive_px("spinner_arc_xs", 2);
    } else if (strcmp(size_str, "sm") == 0) {
        size = get_responsive_px("spinner_sm", 20);
        arc_width = get_responsive_px("spinner_arc_sm", 2);
    } else if (strcmp(size_str, "md") == 0) {
        size = get_responsive_px("spinner_md", 32);
        arc_width = get_responsive_px("spinner_arc_md", 3);
    } else { // lg (default)
        size = get_responsive_px("spinner_lg", 64);
        arc_width = get_responsive_px("spinner_arc_lg", 4);
    }

    // Configure arc appearance
    lv_obj_set_size(arc, size, size);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(arc, 0, 360); // Full circle background (hidden)
    lv_arc_set_rotation(arc, 270);     // Start at top (12 o'clock)

    // Hide knob (arc widgets have a draggable knob by default)
    lv_obj_set_style_opa(arc, LV_OPA_0, LV_PART_KNOB);

    // Apply consistent styling - primary color indicator via shared reactive style
    lv_obj_add_style(arc, ThemeManager::instance().get_style(StyleRole::Spinner),
                     LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, arc_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    // Hide background track for clean modern look
    lv_obj_set_style_arc_opa(arc, LV_OPA_0, LV_PART_MAIN);

    // Set initial arc angles (will be animated immediately)
    // start=0, end=270 gives a large initial arc
    lv_arc_set_angles(arc, 0, kArcMaxSweep);

    // animated="false" leaves a static arc: a busy mark that never repaints, for screens
    // that cannot afford a continuous partial redraw.
    const char* animated = lv_xml_get_value_of(attrs, "animated");
    if (!animated || strcmp(animated, "false") != 0) {
        start_material_spinner_animations(arc);
    }

    // Register cleanup callback to stop animations when spinner is deleted
    lv_obj_add_event_cb(arc, spinner_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::trace("[ui_spinner] Created Material spinner size='{}' ({}px, arc={}px)", size_str,
                  size, arc_width);

    return arc;
}

/**
 * @brief XML apply callback for <spinner> widget
 *
 * Delegates to standard object parser for base properties (align, hidden, etc.)
 *
 * @param state XML parser state
 * @param attrs XML attributes
 */
static void ui_spinner_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_xml_obj_apply(state, attrs);
}

void ui_spinner_init() {
    lv_xml_register_widget("spinner", ui_spinner_create, ui_spinner_apply);
    spdlog::trace("[ui_spinner] Registered Material Design spinner widget");
}
