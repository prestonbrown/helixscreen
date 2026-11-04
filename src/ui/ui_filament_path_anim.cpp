// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The five animation systems of the filament_path_canvas widget, all driven
// by lv_anim with the widget object as the animation variable:
//
//   segment   one-shot 0→100 ease-out; slides the filament tip between path
//             segments and couples the flow particles on/off
//   error     infinite opacity pulse on the segment that reported an error
//   heat      infinite opacity pulse for the nozzle heat glow halo
//   flow      infinite linear 0→kFlowDotSpacing cycle moving the particles
//   output_x  one-shot slide of the LINEAR output exit under the active slot
//
// All systems share run_anim() (the lv_anim boilerplate) and write only their
// AnimState fields + invalidate the widget — the actual painting happens in
// the DRAW_POST pass (ui_filament_path_topology.cpp). Invalidation is deferred
// via queue_invalidate because exec callbacks can run inside lv_timer_handler()
// overlapping the render phase. See ui_filament_path_internal.h.

#include "ui_filament_path_internal.h"

#include "core/update_queue.h"
#include "display_settings_manager.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>

namespace helix::ui::fpath {

namespace {

// Shared lv_anim boilerplate. `playback` mirrors the value range back each
// cycle (pulse); `infinite` repeats forever.
void run_anim(lv_obj_t* obj, lv_anim_exec_xcb_t exec_cb, int32_t v_from, int32_t v_to,
              uint32_t duration_ms, lv_anim_path_cb_t path_cb, bool infinite, bool playback) {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, v_from, v_to);
    lv_anim_set_duration(&anim, duration_ms);
    if (infinite)
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    if (playback)
        lv_anim_set_playback_duration(&anim, duration_ms);
    lv_anim_set_path_cb(&anim, path_cb);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_start(&anim);
}

bool animations_enabled() {
    return DisplaySettingsManager::instance().get_animations_enabled();
}

// --- exec callbacks (distinct function pointers — lv_anim_delete keys on them) ---

void segment_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    data->anim.progress = value;

    // Animation complete
    if (value >= 100) {
        data->anim.segment_active = false;
        data->anim.direction = AnimDirection::NONE;
        data->anim.prev_segment = data->filament_segment;
        spdlog::info("[FilamentPath] Segment anim complete at segment {} (flow_active={})",
                     data->filament_segment, data->anim.flow_active);
        // Stop flow at terminal positions (NONE=unload complete, NOZZLE=load complete)
        // or when no further transitions are expected. Flow between intermediate steps
        // is stopped here rather than relying solely on set_filament_segment, which
        // may not fire again after the final step.
        if (data->anim.flow_active) {
            int seg = data->filament_segment;
            bool is_terminal = (seg == 0 || seg == kPathSegmentCount - 1);
            if (is_terminal) {
                stop_flow_animation(obj, data);
            }
        }
    }

    queue_invalidate(obj);
}

void error_pulse_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;
    data->anim.error_pulse_opa = static_cast<lv_opa_t>(value);
    queue_invalidate(obj);
}

void heat_pulse_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;
    data->anim.heat_pulse_opa = static_cast<lv_opa_t>(value);
    queue_invalidate(obj);
}

void flow_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(var);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    int old_offset = data->anim.flow_offset;
    data->anim.flow_offset = value;

    // Throttle redraws: only invalidate when dots visibly move (~2px change).
    // Flow dots are 1px radius at low opacity — sub-pixel changes are invisible.
    if (std::abs(value - old_offset) >= 2) {
        queue_invalidate(obj);
    }
}

void output_x_anim_cb(void* var, int32_t value) {
    auto* obj = static_cast<lv_obj_t*>(var);
    auto* data = get_data(obj);
    if (!data)
        return;
    data->anim.output_x_current = value;
    lv_obj_invalidate(obj);
}

} // namespace

// ============================================================================
// Segment transition
// ============================================================================

void start_segment_animation(lv_obj_t* obj, FilamentPathData* data, int from_segment,
                             int to_segment) {
    if (!obj || !data)
        return;

    // Stop any existing animation
    lv_anim_delete(obj, segment_anim_cb);

    // Determine animation direction
    if (to_segment > from_segment) {
        data->anim.direction = AnimDirection::LOADING;
    } else if (to_segment < from_segment) {
        data->anim.direction = AnimDirection::UNLOADING;
    } else {
        data->anim.direction = AnimDirection::NONE;
        return; // No change, no animation needed
    }

    data->anim.prev_segment = from_segment;
    data->anim.segment_active = true;
    data->anim.progress = 0;

    // Skip animation if disabled - jump to final state
    if (!animations_enabled()) {
        data->anim.progress = 100;
        data->anim.segment_active = false;
        data->anim.direction = AnimDirection::NONE;
        data->anim.prev_segment = data->filament_segment;
        lv_obj_invalidate(obj);
        spdlog::trace("[FilamentPath] Animations disabled - skipping segment animation");
        return;
    }

    run_anim(obj, segment_anim_cb, 0, 100, kSegmentAnimDurationMs, lv_anim_path_ease_out,
             /*infinite=*/false, /*playback=*/false);

    // Start flow particles only for real filament movement (small steps).
    // Big jumps (e.g., 0→4 on initial state setup) are not real flow operations.
    int step = std::abs(to_segment - from_segment);
    if (step <= 2) {
        start_flow_animation(obj, data);
    }

    spdlog::trace("[FilamentPath] Started segment animation: {} -> {} ({}, step={})", from_segment,
                  to_segment,
                  data->anim.direction == AnimDirection::LOADING ? "loading" : "unloading", step);
}

void stop_segment_animation(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data)
        return;

    lv_anim_delete(obj, segment_anim_cb);
    data->anim.segment_active = false;
    data->anim.progress = 100;
    data->anim.direction = AnimDirection::NONE;
    stop_flow_animation(obj, data);
}

// ============================================================================
// Error pulse
// ============================================================================

void start_error_pulse(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data || data->anim.error_pulse_active)
        return;

    data->anim.error_pulse_active = true;
    data->anim.error_pulse_opa = kErrorPulseOpaMax;

    // Skip animation if disabled - just show static error state
    if (!animations_enabled()) {
        lv_obj_invalidate(obj);
        spdlog::trace("[FilamentPath] Animations disabled - showing static error state");
        return;
    }

    run_anim(obj, error_pulse_anim_cb, kErrorPulseOpaMin, kErrorPulseOpaMax, kErrorPulseDurationMs,
             lv_anim_path_ease_in_out, /*infinite=*/true,
             /*playback=*/true);

    spdlog::trace("[FilamentPath] Started error pulse animation");
}

void stop_error_pulse(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data)
        return;

    lv_anim_delete(obj, error_pulse_anim_cb);
    data->anim.error_pulse_active = false;
    data->anim.error_pulse_opa = LV_OPA_COVER;
}

// ============================================================================
// Heat glow pulse
// ============================================================================

void start_heat_pulse(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data || data->anim.heat_pulse_active)
        return;

    data->anim.heat_pulse_active = true;
    data->anim.heat_pulse_opa = kHeatPulseOpaMax;

    // Skip animation if disabled - just show static heat state
    if (!animations_enabled()) {
        lv_obj_invalidate(obj);
        spdlog::trace("[FilamentPath] Animations disabled - showing static heat state");
        return;
    }

    run_anim(obj, heat_pulse_anim_cb, kHeatPulseOpaMin, kHeatPulseOpaMax, kHeatPulseDurationMs,
             lv_anim_path_ease_in_out, /*infinite=*/true,
             /*playback=*/true);

    spdlog::trace("[FilamentPath] Started heat pulse animation");
}

void stop_heat_pulse(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data)
        return;

    lv_anim_delete(obj, heat_pulse_anim_cb);
    data->anim.heat_pulse_active = false;
    data->anim.heat_pulse_opa = LV_OPA_COVER;
}

// ============================================================================
// Flow particles
// ============================================================================

void start_flow_animation(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data || data->anim.flow_active)
        return;
    if (!animations_enabled())
        return;

    data->anim.flow_active = true;
    data->anim.flow_offset = 0;
    spdlog::info("[FilamentPath] Flow animation STARTED");

    run_anim(obj, flow_anim_cb, 0, kFlowDotSpacing, kFlowAnimDurationMs, lv_anim_path_linear,
             /*infinite=*/true, /*playback=*/false);
}

void stop_flow_animation(lv_obj_t* obj, FilamentPathData* data) {
    if (!obj || !data)
        return;
    // Debug mode: never stop flow animation
    static const bool dbg_flow = (getenv("HELIX_FLOW_SEGMENT") != nullptr);
    if (dbg_flow) {
        return;
    }
    if (data->anim.flow_active) {
        spdlog::info("[FilamentPath] Flow animation STOPPED");
    }
    lv_anim_delete(obj, flow_anim_cb);
    data->anim.flow_active = false;
    data->anim.flow_offset = 0;
}

// ============================================================================
// Output-X slide (LINEAR topology)
// ============================================================================

void start_output_x_animation(lv_obj_t* obj, FilamentPathData* data, int32_t from_x, int32_t to_x) {
    if (!obj || !data)
        return;
    lv_anim_delete(obj, output_x_anim_cb);

    if (!animations_enabled()) {
        data->anim.output_x_current = to_x;
        data->anim.output_x_target = to_x;
        data->anim.output_x_active = false;
        lv_obj_invalidate(obj);
        return;
    }

    data->anim.output_x_active = true;
    data->anim.output_x_target = to_x;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, from_x, to_x);
    lv_anim_set_duration(&anim, kOutputXAnimDurationMs);
    lv_anim_set_exec_cb(&anim, output_x_anim_cb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* obj = static_cast<lv_obj_t*>(a->var);
        auto* data = get_data(obj);
        if (data) {
            data->anim.output_x_active = false;
        }
    });
    lv_anim_start(&anim);
}

// ============================================================================
// Teardown
// ============================================================================

// output_x_anim_cb is intentionally not deleted here (matching the widget's
// teardown contract): its exec/completed callbacks no-op once the registry
// entry is gone, so a still-running slide is harmless.
void delete_all_animations(lv_obj_t* obj) {
    lv_anim_delete(obj, segment_anim_cb);
    lv_anim_delete(obj, error_pulse_anim_cb);
    lv_anim_delete(obj, heat_pulse_anim_cb);
    lv_anim_delete(obj, flow_anim_cb);
}

} // namespace helix::ui::fpath
