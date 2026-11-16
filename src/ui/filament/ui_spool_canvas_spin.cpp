// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spool_canvas.h"
#include "ui_spool_canvas_internal.h"

#include "lvgl/lvgl.h"
#include "spool_vents.h"

namespace helix::ui {

// One step of a turning spool. Runs at LVGL's animation tick but acts only when
// the phase changes, and only for a canvas on screen. The silhouette is the same
// at every phase, so the glow bound to this spool is left alone.
static void spool_spin_anim_cb(void* var, int32_t value) {
    auto* canvas = static_cast<lv_obj_t*>(var);
    SpoolCanvasData* data = spool_canvas_data(canvas);
    if (!data)
        return;
    const int phases = spool_vent_phase_count(spool_vent_detail(data->size));
    const int phase = ((value % phases) + phases) % phases;
    // Off screen the animation stops, so LVGL's timer can idle. Clearing the spin lets the next
    // state update start it again.
    if (!lv_obj_is_visible(canvas)) {
        data->spin = SpoolSpin{};
        lv_anim_delete(canvas, spool_spin_anim_cb);
        return;
    }
    if (phase == data->vent_phase)
        return;
    data->vent_phase = phase;
    redraw_spool_pixels(data);
}

void spool_canvas_set_spin(lv_obj_t* canvas, const SpoolSpin& spin) {
    SpoolCanvasData* data = spool_canvas_data(canvas);
    if (!data || spin == data->spin)
        return;
    if (!lv_obj_is_visible(canvas) && spin.direction != 0)
        return;
    data->spin = spin;
    lv_anim_delete(canvas, spool_spin_anim_cb);
    const int phases = spool_vent_phase_count(spool_vent_detail(data->size));
    if (spin.direction == 0 || phases <= 1)
        return;

    // Starts from the current phase, so a change of speed never jumps the vents.
    const int32_t from = data->vent_phase;
    const int32_t to = from + spin.direction * phases;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, canvas);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, static_cast<uint32_t>(spin.step_ms) * phases);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&anim, spool_spin_anim_cb);
    lv_anim_start(&anim);
}

} // namespace helix::ui
