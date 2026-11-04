// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_humidity_drop.h"

#include "ui_icon.h"
#include "ui_icon_codepoints.h"

#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "helix/ui/shared_font_style.h"
#include "helix/ui/text_metrics.h"
#include "humidity_drop_geometry.h"
#include "lv_draw_buf_guard.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h" // not in the lvgl.h umbrella
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace helix::ui {

namespace hd = humidity_drop;

namespace {

/// Wave tick: slow enough to cost little on the smallest boards, fast enough to
/// read as drift rather than steps.
constexpr uint32_t kTickMs = 100;
/// One wavelength every ~4 s.
constexpr float kPhaseStep = 6.28318530718f * kTickMs / 4000.0f;

struct RenderKey {
    int32_t w = 0;
    int32_t h = 0;
    int value = hd::kUnknown;
    float phase = 0;
    uint32_t outline = 0;
    uint32_t back = 0;
    uint32_t front = 0;
    uint32_t highlight = 0;

    bool operator==(const RenderKey& o) const {
        return w == o.w && h == o.h && value == o.value && phase == o.phase &&
               outline == o.outline && back == o.back && front == o.front &&
               highlight == o.highlight;
    }
};

struct HumidityDropData {
    int value = hd::kUnknown;
    bool auto_size = true;
    lv_draw_buf_t* buf = nullptr;
    RenderKey key;
    bool rendered = false;
};

std::vector<lv_obj_t*> s_drops;
lv_timer_t* s_timer = nullptr;
float s_phase = 0.0f;

HumidityDropData* data_of(lv_obj_t* obj) {
    return obj ? static_cast<HumidityDropData*>(lv_obj_get_user_data(obj)) : nullptr;
}

uint32_t pack_rgb(hd::Rgb c) {
    return (uint32_t{c.r} << 16) | (uint32_t{c.g} << 8) | c.b;
}

hd::Rgb to_rgb(lv_color_t c) {
    return {c.red, c.green, c.blue};
}

hd::DropColors drop_colors() {
    // One theme colour is the water; every tone is derived from it and the
    // background, so a theme (or a custom one) defines the drop with one token.
    const ThemePalette& p = ThemeManager::instance().current_palette();
    return hd::water_colors(to_rgb(p.info), to_rgb(p.screen_bg));
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
    humidity_drop_tick();
}

void track(lv_obj_t* obj) {
    s_drops.push_back(obj);
    if (!s_timer || !timer_alive(s_timer))
        s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);
}

void untrack(lv_obj_t* obj) {
    s_drops.erase(std::remove(s_drops.begin(), s_drops.end(), obj), s_drops.end());
    if (s_drops.empty() && s_timer) {
        if (lv_is_initialized() && timer_alive(s_timer))
            lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
}

/// The box an icon drawing the water glyph in @p font takes. Set only on a
/// change: setting a size refreshes the style, which re-enters this.
void size_to_font(lv_obj_t* obj, const lv_font_t* font) {
    if (!font)
        return;
    const char* glyph = icon::lookup_codepoint("water");
    const int32_t h = lv_font_get_line_height(font);
    const int32_t w = glyph ? text_width(glyph, font) : h;
    if (lv_obj_get_style_width(obj, LV_PART_MAIN) == w &&
        lv_obj_get_style_height(obj, LV_PART_MAIN) == h)
        return;
    // DECLARATIVE_OK: the widget's own box, measured from the face it draws in.
    lv_obj_set_size(obj, w, h);
}

bool sync_buf(HumidityDropData* d, int32_t w, int32_t h) {
    if (d->buf && static_cast<int32_t>(d->buf->header.w) == w &&
        static_cast<int32_t>(d->buf->header.h) == h)
        return true;
    if (d->buf) {
        lv_image_cache_drop(d->buf);
        safe_draw_buf_destroy(d->buf, "humidity_drop");
    }
    d->buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
    d->rendered = false;
    if (!d->buf) {
        spdlog::error("[HumidityDrop] Failed to create a {}x{} draw buffer", w, h);
        return false;
    }
    return true;
}

void draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    HumidityDropData* d = data_of(obj);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!d || !layer)
        return;

    lv_area_t coords;
    lv_obj_get_content_coords(obj, &coords);
    const int32_t w = lv_area_get_width(&coords);
    const int32_t h = lv_area_get_height(&coords);
    if (w <= 0 || h <= 0 || !sync_buf(d, w, h))
        return;

    const hd::DropColors colors = drop_colors();
    // Only a partial fill shows the wave, so only it re-renders as the phase moves.
    const bool wavy = d->value > 0 && d->value < 1000;
    const RenderKey key{w,
                        h,
                        d->value,
                        wavy ? s_phase : 0.0f,
                        pack_rgb(colors.outline),
                        pack_rgb(colors.back),
                        pack_rgb(colors.front),
                        pack_rgb(colors.highlight)};
    if (!d->rendered || !(key == d->key)) {
        hd::rasterize(d->buf->data, w, h, static_cast<int>(d->buf->header.stride), colors, d->value,
                      s_phase);
        lv_image_cache_drop(d->buf);
        d->key = key;
        d->rendered = true;
    }

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = d->buf;
    dsc.opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    const lv_area_t area = draw_buf_blit_area(*d->buf, coords);
    lv_draw_image(layer, &dsc, &area);
}

void style_changed_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    const HumidityDropData* d = data_of(obj);
    if (d && d->auto_size)
        size_to_font(obj, lv_obj_get_style_text_font(obj, LV_PART_MAIN));
}

void delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    HumidityDropData* d = data_of(obj);
    untrack(obj);
    if (d) {
        if (d->buf && lv_is_initialized())
            lv_image_cache_drop(d->buf);
        safe_draw_buf_destroy(d->buf, "humidity_drop");
        delete d;
    }
    lv_obj_set_user_data(obj, nullptr);
}

void value_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    humidity_drop_set_value(obj, lv_subject_get_int(subject));
}

void* xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, new HumidityDropData{});

    // DECLARATIVE_OK: a custom widget's own draw, resize and teardown hooks.
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(obj, style_changed_cb, LV_EVENT_STYLE_CHANGED, nullptr);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, nullptr);

    apply_font_style(obj, icon::font_for_size("xl"));
    size_to_font(obj, lv_obj_get_style_text_font(obj, LV_PART_MAIN));
    track(obj);
    return obj;
}

void xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    HumidityDropData* d = data_of(obj);

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];
        if (std::strcmp(name, "size") == 0) {
            apply_font_style(obj, icon::font_for_size(value));
        } else if (std::strcmp(name, "width") == 0 || std::strcmp(name, "height") == 0) {
            if (d)
                d->auto_size = false;
        }
    }
    if (d && d->auto_size)
        size_to_font(obj, lv_obj_get_style_text_font(obj, LV_PART_MAIN));

    // Common lv_obj properties last, so an explicit width/height wins.
    lv_xml_obj_apply(state, attrs);

    const char* subject_name = lv_xml_get_value_of(attrs, "bind_value");
    if (subject_name && subject_name[0] != '\0') {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, subject_name);
        if (subject) {
            lv_subject_add_observer_obj(subject, value_observer_cb, obj, nullptr);
        } else {
            spdlog::warn("[HumidityDrop] subject '{}' does not exist; the drop shows no reading",
                         subject_name);
        }
    }
}

} // namespace

void register_humidity_drop_widget() {
    lv_xml_register_widget("humidity_drop", xml_create, xml_apply);
    spdlog::trace("[HumidityDrop] Registered <humidity_drop> widget");
}

void humidity_drop_set_value(lv_obj_t* obj, int humidity_x10) {
    HumidityDropData* d = data_of(obj);
    if (!d)
        return;
    const int value = humidity_x10 < 0 ? hd::kUnknown : humidity_x10;
    if (value == d->value)
        return;
    d->value = value;
    lv_obj_invalidate(obj);
}

bool is_humidity_drop(const lv_obj_t* obj) {
    return obj && std::find(s_drops.begin(), s_drops.end(), obj) != s_drops.end();
}

int humidity_drop_value(lv_obj_t* obj) {
    const HumidityDropData* d = data_of(obj);
    return d ? d->value : hd::kUnknown;
}

humidity_drop::DropColors humidity_drop_colors() {
    return drop_colors();
}

float humidity_drop_phase() {
    return s_phase;
}

void humidity_drop_tick() {
    if (!DisplaySettingsManager::instance().get_animations_enabled())
        return;
    s_phase += kPhaseStep;
    if (s_phase > 6.28318530718f)
        s_phase -= 6.28318530718f;
    for (lv_obj_t* obj : s_drops) {
        const HumidityDropData* d = data_of(obj);
        if (d && hd::should_animate(true, lv_obj_is_visible(obj), d->value))
            lv_obj_invalidate(obj);
    }
}

} // namespace helix::ui
