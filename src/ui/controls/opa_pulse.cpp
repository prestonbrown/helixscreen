// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/opa_pulse.h"

namespace helix::ui {

namespace {

void image_opa_anim_cb(void* image, int32_t opa) {
    lv_obj_set_style_image_opa(static_cast<lv_obj_t*>(image), static_cast<lv_opa_t>(opa), 0);
}

} // namespace

void set_image_opa_pulse(lv_obj_t* image, bool on, const OpaPulse& pulse) {
    const bool running = lv_anim_get(image, image_opa_anim_cb) != nullptr;
    if (on == running)
        return;
    if (!on) {
        lv_anim_delete(image, image_opa_anim_cb);
        image_opa_anim_cb(image, pulse.high);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, image);
    lv_anim_set_values(&a, pulse.low, pulse.high);
    lv_anim_set_duration(&a, pulse.half_period_ms);
    lv_anim_set_reverse_duration(&a, pulse.half_period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, image_opa_anim_cb);
    lv_anim_start(&a);
}

} // namespace helix::ui
