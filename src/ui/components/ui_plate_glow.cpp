// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plate_glow.h"

#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lv_draw_buf_guard.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h" // not in the lvgl.h umbrella
#include "ui/opa_pulse.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::ui {

namespace {

/// A climbing heater's glow breathes between these image opacities; its level
/// (the XML's style_opa) scales both.
constexpr OpaPulse kHeatGlowPulse{150, LV_OPA_COVER, 1200};

lv_draw_buf_t* mask_buf_of(lv_obj_t* obj) {
    return static_cast<lv_draw_buf_t*>(lv_obj_get_user_data(obj));
}

void release_buf(lv_obj_t* obj) {
    lv_draw_buf_t* buf = mask_buf_of(obj);
    if (!buf)
        return;
    if (lv_is_initialized())
        lv_image_cache_drop(buf);
    safe_draw_buf_destroy(buf, "plate_glow");
    lv_obj_set_user_data(obj, nullptr);
}

void delete_cb(lv_event_t* e) {
    release_buf(lv_event_get_target_obj(e));
}

/// The mask is freed with its image, whether XML or C++ made the image.
void ensure_delete_cb(lv_obj_t* obj) {
    const uint32_t count = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc && lv_event_dsc_get_cb(dsc) == delete_cb)
            return;
    }
    // DECLARATIVE_OK: a custom widget's own teardown hook frees the pixels it owns.
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, nullptr);
}

void* xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_image_create(parent);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

void xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_xml_obj_apply(state, attrs);
}

} // namespace

void register_plate_glow_widget() {
    lv_xml_register_widget("plate_glow", xml_create, xml_apply);
    spdlog::trace("[PlateGlow] Registered <plate_glow> widget");
}

bool plate_glow_set_mask(lv_obj_t* obj, const PlateGlowMask& mask) {
    if (!obj)
        return false;
    if (mask.w <= 0 || mask.h <= 0) {
        lv_image_set_src(obj, nullptr);
        release_buf(obj);
        return true;
    }
    lv_draw_buf_t* buf =
        lv_draw_buf_create(uint32_t(mask.w), uint32_t(mask.h), LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!buf) {
        spdlog::error("[PlateGlow] Failed to create a {}x{} mask", mask.w, mask.h);
        return false;
    }
    for (int row = 0; row < mask.h; ++row) {
        std::memcpy(buf->data + size_t(row) * buf->header.stride,
                    mask.alpha.data() + size_t(row) * size_t(mask.w), size_t(mask.w));
    }
    // The new pixels are in place before the old buffer goes, so the image
    // never points at freed memory.
    lv_image_set_src(obj, buf);
    release_buf(obj);
    ensure_delete_cb(obj);
    lv_obj_set_user_data(obj, buf);
    // DECLARATIVE_OK: measured callout layout places the mask over the plate.
    lv_obj_set_pos(obj, mask.x, mask.y);
    // DECLARATIVE_OK: measured callout layout
    lv_obj_set_size(obj, mask.w * mask.step, mask.h * mask.step);
    // A coarse mask stretches back over the area it stands for.
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH);
    return true;
}

void heat_glow_update_pulse(lv_obj_t* glow, temperature::HeatState heat, bool shown) {
    if (!glow)
        return;
    const bool on = shown && temperature::heater_region_glows(heat) &&
                    DisplaySettingsManager::instance().get_animations_enabled();
    set_image_opa_pulse(glow, on, kHeatGlowPulse);
}

} // namespace helix::ui
