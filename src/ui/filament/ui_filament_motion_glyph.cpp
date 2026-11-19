// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_motion_glyph.h"

#include "ui_toolhead_badge.h"

#include "ams_types.h"
#include "display_settings_manager.h"
#include "filament_path_geometry.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "nozzle_renderer_dispatch.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace pg = pathgeo;

namespace {

/// One chevron spacing of travel per cycle.
constexpr uint32_t kCycleMs = 800;
/// The anim value runs over one spacing in thousandths, so the stream keeps its
/// speed whatever size the box is drawn at.
constexpr int32_t kPhaseSteps = 1000;
/// Reference scale the toolhead's bounds are measured at to fit it to the box.
constexpr int32_t kProbeScale = 10;
/// Share of the box height the toolhead may take; the column gets the rest.
constexpr float kToolheadHeightShare = 0.55f;

struct GlyphData {
    FilamentMotion motion = FilamentMotion::Idle;
    int32_t phase = 0;                 ///< 0..kPhaseSteps through one chevron spacing
    uint32_t color = kNoFilamentPaint; ///< the loaded filament's color, when known
};

GlyphData* data_of(lv_obj_t* obj) {
    return obj ? static_cast<GlyphData*>(lv_obj_get_user_data(obj)) : nullptr;
}

void phase_anim_cb(void* var, int32_t value) {
    auto* obj = static_cast<lv_obj_t*>(var);
    GlyphData* d = data_of(obj);
    if (!d)
        return;
    d->phase = value;
    lv_obj_invalidate(obj);
}

void start_anim(lv_obj_t* obj) {
    if (lv_anim_get(obj, phase_anim_cb))
        return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 0, kPhaseSteps);
    lv_anim_set_duration(&a, kCycleMs);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, phase_anim_cb);
    lv_anim_start(&a);
}

void draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    GlyphData* d = data_of(obj);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!d || !layer)
        return;

    lv_area_t box;
    lv_obj_get_content_coords(obj, &box);
    const int32_t w = lv_area_get_width(&box);
    const int32_t h = lv_area_get_height(&box);
    if (w <= 0 || h <= 0)
        return;

    const bool color_known = d->color != kNoFilamentPaint;
    const lv_color_t wall_color = lv_obj_get_style_border_color(obj, LV_PART_MAIN);
    const lv_color_t filament_color =
        color_known ? lv_color_hex(d->color) : lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    // Over a known color the chevrons take a contrasting shade: the theme's accent
    // can match the filament.
    const lv_color_t chevron_color =
        color_known ? theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"),
                                                               filament_color)
                    : lv_obj_get_style_line_color(obj, LV_PART_MAIN);
    const lv_opa_t opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);

    // Fit the configured toolhead into the lower part of the box.
    const ToolheadStyle style = SettingsManager::instance().get_effective_toolhead_style();
    const GlyphBounds probe = toolhead_bounds(style, 0, 0, kProbeScale);
    const float probe_w = static_cast<float>(probe.right - probe.left + 1);
    const float probe_h = static_cast<float>(probe.bottom - probe.top + 1);
    const float fit = std::min(static_cast<float>(w) / probe_w,
                               static_cast<float>(h) * kToolheadHeightShare / probe_h);
    const int32_t scale = std::max<int32_t>(2, static_cast<int32_t>(kProbeScale * fit));
    const int32_t cx = box.x1 + w / 2;
    const GlyphBounds at_zero = toolhead_bounds(style, cx, 0, scale);
    const int32_t nozzle_y = box.y2 - at_zero.bottom;
    const int32_t head_top = at_zero.top + nozzle_y;

    // The filament column runs from the top of the box into the toolhead.
    const int32_t gauge = pg::centered_gauge(std::max<int32_t>(4, w / 5));
    const float column_top = static_cast<float>(box.y1) + gauge * 0.5f;
    pg::FilamentPath column;
    column.add_line(static_cast<float>(cx), column_top, static_cast<float>(cx),
                    static_cast<float>(head_top + gauge));
    const LaneStyle lane =
        lane_style(true, false, filament_color, wall_color, wall_color, filament_color, gauge);
    TubePass passes[4];
    const int n = lane_passes(lane, passes, /*simple=*/true);
    for (int i = 0; i < n; ++i)
        passes[i].opa = static_cast<lv_opa_t>(passes[i].opa * opa / LV_OPA_COVER);
    stroke_path(layer, column, passes, n);

    draw_nozzle_for_style(layer, cx, nozzle_y, filament_color, scale, opa);

    if (d->motion == FilamentMotion::Idle)
        return;

    // Chevrons ride the visible column only, packed tighter than on the AMS
    // path so a short column still shows a stream.
    pg::FilamentPath visible;
    visible.add_line(static_cast<float>(cx), column_top, static_cast<float>(cx),
                     static_cast<float>(head_top));
    pg::ChevronSize size =
        pg::pixel_centered(pg::chevron_size_for_gauge(static_cast<float>(gauge)));
    size.spacing = size.depth * 2.5f;
    const float phase = size.spacing * static_cast<float>(d->phase) / kPhaseSteps;
    draw_flow_chevrons(layer, visible, size, phase, d->motion == FilamentMotion::Retract,
                       chevron_color, opa);
}

void delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_anim_delete(obj, phase_anim_cb);
    delete data_of(obj);
    lv_obj_set_user_data(obj, nullptr);
}

void color_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    filament_motion_glyph_set_color(static_cast<lv_obj_t*>(lv_observer_get_target(observer)),
                                    static_cast<uint32_t>(lv_subject_get_int(subject)));
}

void motion_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    filament_motion_glyph_set_motion(obj, filament_motion_from_int(lv_subject_get_int(subject)));
}

void* xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_user_data(obj, new GlyphData{});

    // DECLARATIVE_OK: a custom widget's own draw and teardown hooks.
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, nullptr);
    return obj;
}

void xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    lv_xml_obj_apply(state, attrs);

    if (const char* color_name = lv_xml_get_value_of(attrs, "bind_color");
        color_name && color_name[0] != '\0') {
        if (lv_subject_t* subject = lv_xml_get_subject(&state->scope, color_name)) {
            lv_subject_add_observer_obj(subject, color_observer_cb, obj, nullptr);
        } else {
            spdlog::debug(
                "[FilamentMotionGlyph] subject '{}' does not exist; the column stays neutral",
                color_name);
        }
    }

    const char* subject_name = lv_xml_get_value_of(attrs, "bind_motion");
    if (subject_name && subject_name[0] != '\0') {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, subject_name);
        if (subject) {
            lv_subject_add_observer_obj(subject, motion_observer_cb, obj, nullptr);
        } else {
            spdlog::warn("[FilamentMotionGlyph] subject '{}' does not exist; the glyph stays idle",
                         subject_name);
        }
    }
}

} // namespace

void register_filament_motion_glyph_widget() {
    lv_xml_register_widget("filament_motion_glyph", xml_create, xml_apply);
    spdlog::trace("[FilamentMotionGlyph] Registered <filament_motion_glyph> widget");
}

void filament_motion_glyph_set_motion(lv_obj_t* obj, FilamentMotion motion) {
    GlyphData* d = data_of(obj);
    if (!d || d->motion == motion)
        return;
    d->motion = motion;
    if (motion != FilamentMotion::Idle &&
        DisplaySettingsManager::instance().get_animations_enabled()) {
        start_anim(obj);
    } else {
        lv_anim_delete(obj, phase_anim_cb);
        d->phase = 0;
    }
    lv_obj_invalidate(obj);
}

void filament_motion_glyph_set_color(lv_obj_t* obj, uint32_t rgb) {
    GlyphData* d = data_of(obj);
    if (!d || d->color == rgb)
        return;
    d->color = rgb;
    lv_obj_invalidate(obj);
}

uint32_t filament_motion_glyph_color(lv_obj_t* obj) {
    const GlyphData* d = data_of(obj);
    return d ? d->color : kNoFilamentPaint;
}

FilamentMotion filament_motion_glyph_motion(lv_obj_t* obj) {
    const GlyphData* d = data_of(obj);
    return d ? d->motion : FilamentMotion::Idle;
}

bool filament_motion_glyph_animating(lv_obj_t* obj) {
    return obj && lv_anim_get(obj, phase_anim_cb) != nullptr;
}

} // namespace helix::ui
