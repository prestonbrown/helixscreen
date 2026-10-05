// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_canvas.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>

namespace helix::plugin {

size_t DisplayList::bytes() const {
    size_t n = sizeof(DisplayList) + prims.size() * sizeof(CanvasPrim) +
               points.size() * sizeof(lv_point_precise_t) + text.size();
    for (const std::string& t : tokens)
        n += t.size() + sizeof(std::string);
    return n;
}

namespace {

struct Slot {
    std::unique_ptr<DisplayList> list;
    std::function<void(int32_t, int32_t)> listener;
    std::vector<lv_obj_t*> instances;
    int32_t w = 0;
    int32_t h = 0;
};

std::map<std::string, Slot>& slots() {
    static std::map<std::string, Slot> registry;
    return registry;
}

bool slot_is_empty(const Slot& s) {
    return s.instances.empty() && !s.list && !s.listener;
}

/// Stores the instance's content size and notifies on a real change. Duplicate
/// reports (layout refreshes that change nothing) fall through silently.
void report_size(Slot& s, lv_obj_t* obj) {
    const int32_t w = lv_obj_get_content_width(obj);
    const int32_t h = lv_obj_get_content_height(obj);
    if (w <= 0 || h <= 0 || (w == s.w && h == s.h))
        return;
    s.w = w;
    s.h = h;
    if (s.listener)
        s.listener(w, h);
}

void canvas_on_size(lv_event_t* e) {
    auto* name = static_cast<const std::string*>(lv_event_get_user_data(e));
    auto it = slots().find(*name);
    if (it != slots().end())
        report_size(it->second, lv_event_get_current_target_obj(e));
}

void canvas_on_draw(lv_event_t* e) {
    // The slot is looked up by name on every draw, never cached: a slot erased
    // between frames (last instance deleted) cannot dangle here.
    auto* name = static_cast<const std::string*>(lv_event_get_user_data(e));
    auto it = slots().find(*name);
    if (it == slots().end() || !it->second.list)
        return;
    lv_area_t content;
    lv_obj_get_content_coords(lv_event_get_current_target_obj(e), &content);
    draw_display_list(lv_event_get_layer(e), content, *it->second.list);
}

void canvas_on_delete(lv_event_t* e) {
    auto* name = static_cast<const std::string*>(lv_event_get_user_data(e));
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    auto it = slots().find(*name);
    if (it != slots().end()) {
        auto& inst = it->second.instances;
        inst.erase(std::remove(inst.begin(), inst.end(), obj), inst.end());
        if (slot_is_empty(it->second))
            slots().erase(it);
    }
    delete name;
}

void* canvas_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    const char* name_attr = nullptr;
    for (int i = 0; attrs && attrs[i]; i += 2) {
        if (strcmp(attrs[i], "name") == 0)
            name_attr = attrs[i + 1];
    }

    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // The heap name is the per-instance state: every callback receives it as
    // user_data and LV_EVENT_DELETE frees it with the instance. An unnamed
    // canvas still gets the callbacks so the string is freed, but never joins
    // a slot, so it draws nothing.
    auto* name = new std::string(name_attr ? name_attr : "");
    if (!name->empty()) {
        Slot& s = slots()[*name];
        s.instances.push_back(obj);
        report_size(s, obj);
    }
    lv_obj_add_event_cb(obj, canvas_on_size, LV_EVENT_SIZE_CHANGED, name);
    lv_obj_add_event_cb(obj, canvas_on_draw, LV_EVENT_DRAW_MAIN_END, name);
    lv_obj_add_event_cb(obj, canvas_on_delete, LV_EVENT_DELETE, name);
    return obj;
}

/// A non-finite coordinate or size skips the primitive outright: the
/// float-to-int casts are undefined on NaN, and range comparisons alone
/// cannot reject one (nan > limit is false).
bool finite_coords(std::initializer_list<lv_value_precise_t> vals) {
    for (lv_value_precise_t v : vals)
        if (!std::isfinite(v))
            return false;
    return true;
}

} // namespace

void register_plugin_canvas_widget() {
    lv_xml_register_widget("plugin_canvas", canvas_xml_create, lv_xml_obj_apply);
}

void canvas_commit(const std::string& name, std::unique_ptr<DisplayList> list) {
    auto& reg = slots();
    auto it = reg.find(name);
    if (it == reg.end()) {
        if (!list)
            return;
        it = reg.emplace(name, Slot{}).first;
    }
    // Replacing the unique_ptr frees the previous list. Commit and draw are
    // both main-thread and draw_display_list never re-enters the registry, so
    // no draw is iterating the old list when it dies.
    it->second.list = std::move(list);
    if (slot_is_empty(it->second)) {
        reg.erase(it);
        return;
    }
    for (lv_obj_t* obj : it->second.instances)
        lv_obj_invalidate(obj);
}

const DisplayList* canvas_committed(const std::string& name) {
    auto it = slots().find(name);
    return it == slots().end() ? nullptr : it->second.list.get();
}

std::pair<int32_t, int32_t> canvas_size(const std::string& name) {
    auto it = slots().find(name);
    if (it == slots().end() || it->second.instances.empty())
        return {0, 0};
    return {it->second.w, it->second.h};
}

size_t canvas_instance_count(const std::string& name) {
    auto it = slots().find(name);
    return it == slots().end() ? 0 : it->second.instances.size();
}

void canvas_set_size_listener(const std::string& name, std::function<void(int32_t, int32_t)> fn) {
    auto& reg = slots();
    auto it = reg.find(name);
    if (!fn) {
        if (it != reg.end()) {
            it->second.listener = nullptr;
            if (slot_is_empty(it->second))
                reg.erase(it);
        }
        return;
    }
    if (it == reg.end())
        it = reg.emplace(name, Slot{}).first;
    it->second.listener = std::move(fn);
}

const lv_font_t* canvas_resolve_font(const std::string& name) {
    char token[64];
    snprintf(token, sizeof(token), "font_%s", name.c_str());
    // theme_manager_get_font falls back to the default font on an unknown token;
    // the const existing is what proves the token is real. Base tokens only: a
    // size-suffixed variant names a face registered from its tier up, so on a
    // smaller display it would silently draw text in the default font.
    return theme_manager_font_token_is_base(token) && lv_xml_get_const_silent(nullptr, token)
               ? theme_manager_get_font(token)
               : nullptr;
}

void draw_display_list(lv_layer_t* layer, const lv_area_t& content, const DisplayList& list) {
    // Tokens resolve here, at draw time, so a dark-mode switch costs only an
    // invalidate. Color names and font names never collide (fonts carry the
    // font_ prefix), so one memo per role is enough; the fixed arrays keep the
    // replay free of heap work.
    lv_color_t colors[kMaxCanvasTokens] = {};
    const lv_font_t* fonts[kMaxCanvasTokens] = {};
    uint32_t colors_done = 0;
    uint32_t fonts_done = 0;

    auto color_of = [&](uint8_t idx) {
        if (idx >= list.tokens.size() || idx >= kMaxCanvasTokens)
            return lv_color_hex(0x000000);
        if (!(colors_done & (1u << idx))) {
            colors[idx] = theme_manager_get_color(list.tokens[idx].c_str());
            colors_done |= 1u << idx;
        }
        return colors[idx];
    };
    auto font_of = [&](uint8_t idx) -> const lv_font_t* {
        if (idx >= list.tokens.size() || idx >= kMaxCanvasTokens)
            return nullptr;
        if (!(fonts_done & (1u << idx))) {
            fonts[idx] = canvas_resolve_font(list.tokens[idx]);
            fonts_done |= 1u << idx;
        }
        return fonts[idx];
    };

    const lv_value_precise_t ox = content.x1;
    const lv_value_precise_t oy = content.y1;

    for (const CanvasPrim& p : list.prims) {
        switch (p.op) {
        case CanvasOp::Line: {
            if (!finite_coords({p.a, p.b, p.c, p.d}))
                break;
            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.p1 = {ox + p.a, oy + p.b};
            dsc.p2 = {ox + p.c, oy + p.d};
            dsc.color = color_of(p.color);
            dsc.width = p.width;
            dsc.opa = p.opa;
            lv_draw_line(layer, &dsc);
            break;
        }
        case CanvasOp::Polyline: {
            // Overflow-free range check: first + count can wrap past the size.
            if (p.count < 2 || p.first > list.points.size() ||
                p.count > list.points.size() - p.first)
                break;
            // lv_draw_line copies the point array into its draw task, so the
            // translated local copy can go out of scope right after.
            std::vector<lv_point_precise_t> pts(p.count);
            bool finite = true;
            for (uint32_t i = 0; i < p.count; ++i) {
                const lv_point_precise_t& src = list.points[p.first + i];
                if (!std::isfinite(src.x) || !std::isfinite(src.y)) {
                    finite = false;
                    break;
                }
                pts[i] = {ox + src.x, oy + src.y};
            }
            if (!finite)
                break;
            // An area fill is two triangles per segment down to the baseline,
            // rasterized from the same points as the stroke, so the fill's
            // edge follows the line instead of stepping at the samples.
            if (p.border != kNoToken && std::isfinite(p.a)) {
                const lv_value_precise_t base = oy + p.a;
                lv_draw_triangle_dsc_t fdsc;
                lv_draw_triangle_dsc_init(&fdsc);
                fdsc.color = color_of(p.border);
                fdsc.opa = p.fill_opa;
                for (uint32_t i = 0; i + 1 < p.count; ++i) {
                    const lv_point_precise_t& p1 = pts[i];
                    const lv_point_precise_t& p2 = pts[i + 1];
                    // A segment end sitting on the baseline yields a triangle
                    // with two identical vertices: geometrically empty, so it
                    // is skipped rather than handed to the rasterizer.
                    if (p2.y != base) {
                        fdsc.p[0] = p1;
                        fdsc.p[1] = p2;
                        fdsc.p[2] = {p2.x, base};
                        lv_draw_triangle(layer, &fdsc);
                    }
                    if (p1.y != base) {
                        fdsc.p[0] = p1;
                        fdsc.p[1] = {p2.x, base};
                        fdsc.p[2] = {p1.x, base};
                        lv_draw_triangle(layer, &fdsc);
                    }
                }
            }
            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.points = pts.data();
            dsc.point_cnt = static_cast<int32_t>(p.count);
            dsc.color = color_of(p.color);
            dsc.width = p.width;
            dsc.opa = p.opa;
            lv_draw_line(layer, &dsc);
            break;
        }
        case CanvasOp::Rect: {
            if (!finite_coords({p.a, p.b, p.c, p.d}))
                break;
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            if (p.color != kNoToken) {
                dsc.bg_color = color_of(p.color);
                dsc.bg_opa = p.opa;
            }
            if (p.border != kNoToken) {
                dsc.border_color = color_of(p.border);
                dsc.border_width = p.width;
                dsc.border_opa = p.opa;
            }
            dsc.radius = p.radius;
            const lv_area_t a = {
                static_cast<int32_t>(content.x1 + p.a),
                static_cast<int32_t>(content.y1 + p.b),
                static_cast<int32_t>(content.x1 + p.a + p.c - 1),
                static_cast<int32_t>(content.y1 + p.b + p.d - 1),
            };
            lv_draw_rect(layer, &dsc, &a);
            break;
        }
        case CanvasOp::Circle: {
            if (!finite_coords({p.a, p.b}))
                break;
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            if (p.color != kNoToken) {
                dsc.bg_color = color_of(p.color);
                dsc.bg_opa = p.opa;
            }
            if (p.border != kNoToken) {
                dsc.border_color = color_of(p.border);
                dsc.border_width = p.width;
                dsc.border_opa = p.opa;
            }
            dsc.radius = LV_RADIUS_CIRCLE;
            const lv_area_t a = {
                static_cast<int32_t>(content.x1 + p.a - p.radius),
                static_cast<int32_t>(content.y1 + p.b - p.radius),
                static_cast<int32_t>(content.x1 + p.a + p.radius),
                static_cast<int32_t>(content.y1 + p.b + p.radius),
            };
            lv_draw_rect(layer, &dsc, &a);
            break;
        }
        case CanvasOp::Arc: {
            if (!finite_coords({p.a, p.b, p.c, p.d}))
                break;
            lv_draw_arc_dsc_t dsc;
            lv_draw_arc_dsc_init(&dsc);
            dsc.center = {static_cast<int32_t>(content.x1 + p.a),
                          static_cast<int32_t>(content.y1 + p.b)};
            // A negative radius would wrap to ~65k in the uint16 cast.
            dsc.radius = static_cast<uint16_t>(p.radius > 0 ? p.radius : 0);
            dsc.start_angle = p.c;
            dsc.end_angle = p.d;
            dsc.color = color_of(p.color);
            dsc.width = p.width;
            dsc.opa = p.opa;
            lv_draw_arc(layer, &dsc);
            break;
        }
        case CanvasOp::Text: {
            // Overflow-free range check, same shape as the Polyline one.
            if (p.first > list.text.size() || p.count > list.text.size() - p.first)
                break;
            const lv_font_t* font = p.font == kNoToken ? nullptr : font_of(p.font);
            if (!font || !finite_coords({p.a, p.b}))
                break;
            lv_draw_label_dsc_t dsc;
            lv_draw_label_dsc_init(&dsc);
            dsc.color = color_of(p.color);
            dsc.font = font;
            // text_local copies the bytes into the draw task, so the list can
            // be replaced the moment this call returns.
            dsc.text = list.text.c_str() + p.first;
            dsc.text_length = p.count;
            dsc.text_local = 1;
            const lv_area_t a = {
                static_cast<int32_t>(content.x1 + p.a),
                static_cast<int32_t>(content.y1 + p.b),
                content.x2,
                static_cast<int32_t>(content.y1 + p.b + font->line_height),
            };
            lv_draw_label(layer, &dsc, &a);
            break;
        }
        }
    }
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
