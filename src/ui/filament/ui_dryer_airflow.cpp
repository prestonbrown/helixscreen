// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_dryer_airflow.h"

#include "display_settings_manager.h"
#include "dryer_airflow_geometry.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace helix::ui {

namespace af = dryer_airflow;

namespace {

/// Slow enough to cost little on the smallest boards, fast enough to read as motion.
constexpr uint32_t kTickMs = 100;

struct AirflowData {
    DryingPhase phase = DryingPhase::Unknown;
};

std::vector<lv_obj_t*> s_widgets;
lv_timer_t* s_timer = nullptr;
/// Starts a third of a rise in, so a still stream sits spread through the box.
float s_position = 1.0f / (2 * af::kChevronCount);

AirflowData* data_of(lv_obj_t* obj) {
    return obj ? static_cast<AirflowData*>(lv_obj_get_user_data(obj)) : nullptr;
}

/// The timer can outlive its pointer: lv_deinit() frees every timer before the
/// widgets that would delete it.
bool timer_alive(lv_timer_t* t) {
    for (lv_timer_t* i = lv_timer_get_next(nullptr); i; i = lv_timer_get_next(i)) {
        if (i == t)
            return true;
    }
    return false;
}

void tick_cb(lv_timer_t* /*timer*/) {
    dryer_airflow_tick();
}

void track(lv_obj_t* obj) {
    s_widgets.push_back(obj);
    if (!s_timer || !timer_alive(s_timer))
        s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);
}

void untrack(lv_obj_t* obj) {
    s_widgets.erase(std::remove(s_widgets.begin(), s_widgets.end(), obj), s_widgets.end());
    if (s_widgets.empty() && s_timer) {
        if (lv_is_initialized() && timer_alive(s_timer))
            lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
}

void draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    const AirflowData* d = data_of(obj);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!d || !layer)
        return;

    lv_area_t coords;
    lv_obj_get_content_coords(obj, &coords);
    const int32_t w = lv_area_get_width(&coords);
    const int32_t h = lv_area_get_height(&coords);
    if (w <= 0 || h <= 0)
        return;

    // Each chevron spans most of the width; its arms drop a third of that.
    const int32_t half = std::max<int32_t>(w * 3 / 10, 2);
    const int32_t drop = std::max<int32_t>(half * 2 / 3, 1);
    const int32_t stroke = std::max<int32_t>(w / 14, 2);
    const int32_t cx = coords.x1 + w / 2;
    // The tip travels from just above the bottom to just below the top, so the arms
    // never leave the box.
    const int32_t travel = std::max<int32_t>(h - drop - stroke * 2, 1);
    const lv_opa_t base_opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = dryer_airflow_color(d->phase);
    dsc.width = stroke;
    dsc.round_start = 1;
    dsc.round_end = 1;

    for (const af::Chevron& c : af::chevrons(s_position)) {
        if (c.opa == 0)
            continue;
        const int32_t tip_y = coords.y2 - stroke - drop - static_cast<int32_t>(c.rise * travel);
        dsc.opa = static_cast<lv_opa_t>((static_cast<uint32_t>(c.opa) * base_opa) / 255);
        dsc.p1 = {static_cast<lv_value_precise_t>(cx - half),
                  static_cast<lv_value_precise_t>(tip_y + drop)};
        dsc.p2 = {static_cast<lv_value_precise_t>(cx), static_cast<lv_value_precise_t>(tip_y)};
        lv_draw_line(layer, &dsc);
        dsc.p1 = {static_cast<lv_value_precise_t>(cx + half),
                  static_cast<lv_value_precise_t>(tip_y + drop)};
        lv_draw_line(layer, &dsc);
    }
}

void delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    untrack(obj);
    delete data_of(obj);
    lv_obj_set_user_data(obj, nullptr);
}

void phase_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    dryer_airflow_set_phase(obj, static_cast<DryingPhase>(lv_subject_get_int(subject)));
}

void* xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, new AirflowData{});

    // DECLARATIVE_OK: a custom widget's own draw and teardown hooks.
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, nullptr);
    track(obj);
    return obj;
}

void xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    lv_xml_obj_apply(state, attrs);

    const char* subject_name = lv_xml_get_value_of(attrs, "bind_phase");
    if (subject_name && subject_name[0] != '\0') {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, subject_name);
        if (subject) {
            lv_subject_add_observer_obj(subject, phase_observer_cb, obj, nullptr);
        } else {
            spdlog::warn("[DryerAirflow] subject '{}' does not exist; drawing as heating",
                         subject_name);
        }
    }
}

} // namespace

void register_dryer_airflow_widget() {
    lv_xml_register_widget("dryer_airflow", xml_create, xml_apply);
    spdlog::trace("[DryerAirflow] Registered <dryer_airflow> widget");
}

void dryer_airflow_set_phase(lv_obj_t* obj, DryingPhase phase) {
    AirflowData* d = data_of(obj);
    if (!d || d->phase == phase)
        return;
    d->phase = phase;
    lv_obj_invalidate(obj);
}

lv_color_t dryer_airflow_color(DryingPhase phase) {
    const ThemePalette& p = ThemeManager::instance().current_palette();
    return phase == DryingPhase::Holding ? p.info : p.warning;
}

float dryer_airflow_position() {
    return s_position;
}

void dryer_airflow_tick() {
    const bool animate = DisplaySettingsManager::instance().get_animations_enabled();
    const float next = af::advance(s_position, animate);
    if (next == s_position)
        return;
    s_position = next;
    for (lv_obj_t* obj : s_widgets) {
        if (lv_obj_is_visible(obj))
            lv_obj_invalidate(obj);
    }
}

} // namespace helix::ui
