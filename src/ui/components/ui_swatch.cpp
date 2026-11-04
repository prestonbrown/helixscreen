// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_swatch.h"

#include "ams_types.h"
#include "color_utils.h"
#include "filament_variants.h"
#if !defined(HELIX_PLATFORM_ESP32)
#include "glass_painter.h"
#endif
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lv_draw_buf_guard.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h" // not in the lvgl.h umbrella
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace helix::ui {

namespace {

constexpr float kPiF = 3.14159265358979f;

uint32_t rgb_of(lv_color_t c) {
    return lv_color_to_u32(c) & 0xFFFFFF;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

float clamped_radius(const lv_area_t& a, int32_t radius) {
    const float w = static_cast<float>(lv_area_get_width(&a));
    const float h = static_cast<float>(lv_area_get_height(&a));
    const float half_short = std::min(w, h) / 2.0f;
    if (radius == LV_RADIUS_CIRCLE) {
        return half_short;
    }
    return std::clamp(static_cast<float>(radius), 0.0f, half_short);
}

// Position along the top-left to bottom-right diagonal, projected onto it: 0
// at the top-left corner, 1 at the bottom-right. Its iso-lines are the band
// boundaries, and it is exactly the parameter of a linear gradient from
// (0, 0) to (100%, 100%), which is how the bands are drawn.
struct DiagField {
    float x0, y0, w, h, inv_len2;
    float operator()(const SwatchPoint& p) const {
        return ((p.x - x0) * w + (p.y - y0) * h) * inv_len2;
    }
};

DiagField diag_field(const lv_area_t& a) {
    // A w-pixel object spans [x1, x2 + 1) in sub-pixel coordinates.
    const float w = static_cast<float>(lv_area_get_width(&a));
    const float h = static_cast<float>(lv_area_get_height(&a));
    return {static_cast<float>(a.x1), static_cast<float>(a.y1), w, h, 1.0f / (w * w + h * h)};
}

// Sutherland-Hodgman clip of a convex polygon by one half-plane of @p f.
SwatchPolygon clip_halfplane(const SwatchPolygon& poly, const DiagField& f, float t, bool keep_ge) {
    SwatchPolygon out;
    if (poly.empty()) {
        return out;
    }
    out.reserve(poly.size() + 2);
    for (size_t i = 0; i < poly.size(); ++i) {
        const SwatchPoint& a = poly[i];
        const SwatchPoint& b = poly[(i + 1) % poly.size()];
        const float fa = f(a);
        const float fb = f(b);
        const bool a_in = keep_ge ? (fa >= t) : (fa <= t);
        const bool b_in = keep_ge ? (fb >= t) : (fb <= t);
        if (a_in) {
            out.push_back(a);
        }
        if (a_in != b_in) {
            const float denom = fb - fa;
            const float alpha = denom != 0.0f ? (t - fa) / denom : 0.0f;
            out.push_back({a.x + alpha * (b.x - a.x), a.y + alpha * (b.y - a.y)});
        }
    }
    return out;
}

// The diagonal-field threshold below which @p fraction of the shape's area lies.
float threshold_for_fraction(const SwatchPolygon& shape, const DiagField& f, float total,
                             float fraction) {
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float a = swatch_polygon_area(clip_halfplane(shape, f, mid, false));
        if (a < fraction * total) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}

// Small deterministic generator so a swatch's texture is stable across redraws.
struct Lcg {
    uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    }
};

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void fill_shape(lv_layer_t* layer, const lv_area_t& area, int32_t radius, lv_color_t color,
                lv_opa_t opa) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_color = color;
    dsc.bg_opa = opa;
    dsc.border_width = 0;
    lv_draw_rect(layer, &dsc, &area);
}

// The bands of a multi-colour swatch as one rounded rect with a hard-stopped
// linear gradient: one draw, an anti-aliased rim, no seams between bands.
// LV_GRADIENT_MAX_STOPS holds two stops per colour for up to four colours;
// past that each colour gets one stop at its band's centre and they blend.
void fill_split(lv_layer_t* layer, const lv_area_t& area, int32_t radius,
                const std::vector<lv_color_t>& colors, const std::vector<float>& t, lv_opa_t opa) {
    const size_t n = colors.size();
    auto frac = [](float v) {
        return static_cast<uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
    };
    lv_color_t stop_colors[LV_GRADIENT_MAX_STOPS];
    uint8_t stop_fracs[LV_GRADIENT_MAX_STOPS];
    int count = 0;
    if (2 * n <= LV_GRADIENT_MAX_STOPS) {
        for (size_t k = 0; k < n; ++k) {
            const uint8_t lo = k == 0 ? 0 : static_cast<uint8_t>(std::min(255, frac(t[k - 1]) + 1));
            const uint8_t hi = k + 1 == n ? 255 : frac(t[k]);
            stop_colors[count] = colors[k];
            stop_fracs[count++] = lo;
            stop_colors[count] = colors[k];
            stop_fracs[count++] = std::max(lo, hi);
        }
    } else {
        const size_t used = std::min<size_t>(n, LV_GRADIENT_MAX_STOPS);
        for (size_t k = 0; k < used; ++k) {
            const float lo = k == 0 ? 0.0f : t[k - 1];
            const float hi = k + 1 == n ? 1.0f : t[k];
            stop_colors[count] = colors[k];
            stop_fracs[count++] = frac(0.5f * (lo + hi));
        }
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = opa;
    dsc.border_width = 0;
    dsc.bg_color = colors[0];
    lv_grad_init_stops(&dsc.bg_grad, stop_colors, nullptr, stop_fracs, count);
    lv_grad_linear_init(&dsc.bg_grad, lv_pct(0), lv_pct(0), lv_pct(100), lv_pct(100),
                        LV_GRAD_EXTEND_PAD);
    lv_draw_rect(layer, &dsc, &area);
}

int32_t short_side(const lv_area_t& a) {
    return std::min(lv_area_get_width(&a), lv_area_get_height(&a));
}

uint32_t area_seed(const lv_area_t& a) {
    return static_cast<uint32_t>(lv_area_get_width(&a)) * 7919u +
           static_cast<uint32_t>(lv_area_get_height(&a)) * 104729u;
}

// The theme colours the edge reads, re-read only when the theme generation
// moves, so a redraw does no token lookups.
struct ThemeRgbs {
    int generation = -1;
    uint32_t screen_bg = 0;
    uint32_t elevated_bg = 0;
    lv_color_t border{};
    int32_t border_width = 1;
};

const ThemeRgbs& theme_rgbs() {
    static ThemeRgbs t;
    lv_subject_t* changed = theme_manager_get_changed_subject();
    const int generation =
        (changed && changed->type == LV_SUBJECT_TYPE_INT) ? lv_subject_get_int(changed) : 0;
    if (generation != t.generation) {
        t.generation = generation;
        t.screen_bg = rgb_of(theme_manager_get_color("screen_bg"));
        t.elevated_bg = rgb_of(theme_manager_get_color("elevated_bg"));
        t.border = theme_manager_get_color("border");
        t.border_width = std::max<int32_t>(1, theme_manager_get_spacing("border_width"));
    }
    return t;
}

void draw_edge(lv_layer_t* layer, const lv_area_t& area, int32_t radius, lv_opa_t opa) {
    const ThemeRgbs& t = theme_rgbs();
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = t.border;
    dsc.border_width = t.border_width;
    dsc.border_opa = opa;
    lv_draw_rect(layer, &dsc, &area);
}

// ---------------------------------------------------------------------------
// Finish pixels
// ---------------------------------------------------------------------------

// Straight-alpha "over": @p src at coverage @p a onto @p dst.
void blend_over(lv_color32_t& dst, lv_color_t src, uint8_t a) {
    if (a == 0) {
        return;
    }
    const uint32_t da = dst.alpha;
    const uint32_t out_a = a + da * (255 - a) / 255;
    if (out_a == 0) {
        return;
    }
    auto mix = [&](uint8_t s_ch, uint8_t d_ch) {
        return static_cast<uint8_t>((s_ch * a + d_ch * da * (255 - a) / 255) / out_a);
    };
    dst.red = mix(src.red, dst.red);
    dst.green = mix(src.green, dst.green);
    dst.blue = mix(src.blue, dst.blue);
    dst.alpha = static_cast<uint8_t>(out_a);
}

struct PixelTarget {
    lv_draw_buf_t* buf;
    lv_area_t area;
    const SwatchCoverage& cover;
    bool density_by_area; ///< speckle count follows the area, uncapped

    lv_color32_t* at(int32_t x, int32_t y) const {
        if (x < 0 || y < 0 || x >= static_cast<int32_t>(buf->header.w) ||
            y >= static_cast<int32_t>(buf->header.h)) {
            return nullptr;
        }
        return reinterpret_cast<lv_color32_t*>(buf->data + y * buf->header.stride) + x;
    }

    // A @p size square of @p color at alpha @p a centred on (x, y), clipped to the shape.
    void dot(float x, float y, int32_t size, lv_color_t color, uint8_t a) const {
        const int32_t x1 = static_cast<int32_t>(std::lround(x)) - size / 2;
        const int32_t y1 = static_cast<int32_t>(std::lround(y)) - size / 2;
        for (int32_t py = y1; py < y1 + size; ++py) {
            for (int32_t px = x1; px < x1 + size; ++px) {
                lv_color32_t* p = at(px, py);
                const float cov = cover(px + 0.5f, py + 0.5f);
                if (p && cov > 0.0f) {
                    blend_over(*p, color, static_cast<uint8_t>(a * cov));
                }
            }
        }
    }
};

// Silk: a diagonal specular ridge spanning the whole shape: faint at both
// corners, brightest along the top-left to bottom-right diagonal. The low
// tails are what keep a tall region (a spool body) sheened end to end instead
// of carrying a bare band across its middle.
void silk_pixels(const PixelTarget& t) {
    static constexpr float kFracs[] = {4, 55, 128, 200, 251};
    static constexpr float kOpas[] = {22, 85, 170, 75, 18};
    const lv_area_t& a = t.area;
    const float w = static_cast<float>(lv_area_get_width(&a));
    const float h = static_cast<float>(lv_area_get_height(&a));
    const float inv = 255.0f / (w * w + h * h);
    for (int32_t y = a.y1; y <= a.y2; ++y) {
        for (int32_t x = a.x1; x <= a.x2; ++x) {
            const float f = ((x + 0.5f - a.x1) * w + (y + 0.5f - a.y1) * h) * inv;
            if (f <= kFracs[0] || f >= kFracs[4]) {
                continue;
            }
            size_t k = 0;
            while (f > kFracs[k + 1]) {
                ++k;
            }
            const float u = (f - kFracs[k]) / (kFracs[k + 1] - kFracs[k]);
            const float op = kOpas[k] + u * (kOpas[k + 1] - kOpas[k]);
            const float cov = t.cover(x + 0.5f, y + 0.5f);
            if (lv_color32_t* p = t.at(x, y); p && cov > 0.0f) {
                blend_over(*p, lv_color_white(), static_cast<uint8_t>(op * cov));
            }
        }
    }
}

// Points of @p area where @p cover is whole, spread on a jittered grid.
std::vector<SwatchPoint> texture_points(const lv_area_t& area, const SwatchCoverage& cover,
                                        size_t count, uint32_t seed) {
    std::vector<SwatchPoint> points;
    points.reserve(count);
    const float x0 = static_cast<float>(area.x1);
    const float y0 = static_cast<float>(area.y1);
    const float w = static_cast<float>(lv_area_get_width(&area));
    const float h = static_cast<float>(lv_area_get_height(&area));
    if (w <= 0.0f || h <= 0.0f) {
        return points;
    }
    // A jittered grid with offset rows spreads the texture evenly; plain
    // random sampling clumps, which reads as bare patches at small sizes.
    Lcg rng{seed ? seed : 1u};
    const float step = std::sqrt((w * h) / static_cast<float>(count));
    bool offset = false;
    for (float gy = y0 + step / 2.0f; gy < y0 + h; gy += step) {
        const float dx = offset ? step / 2.0f : 0.0f;
        offset = !offset;
        for (float gx = x0 + step / 2.0f + dx; gx < x0 + w; gx += step) {
            const float x = gx + (rng.next() - 0.5f) * step * 0.7f;
            const float y = gy + (rng.next() - 0.5f) * step * 0.7f;
            if (cover(x, y) >= 1.0f) {
                points.push_back({x, y});
            }
        }
    }
    // Top up anywhere the shape clipped the grid, so a requested count holds.
    for (size_t tries = 0; points.size() < count && tries < count * 8; ++tries) {
        const float x = x0 + rng.next() * w;
        const float y = y0 + rng.next() * h;
        if (cover(x, y) >= 1.0f) {
            points.push_back({x, y});
        }
    }
    return points;
}

// Marble: a fine speckle of darker and lighter flecks.
void marble_pixels(const PixelTarget& t) {
    const int32_t s = short_side(t.area);
    const int32_t dense = t.density_by_area ? std::max<int32_t>(12, lv_area_get_size(&t.area) / 14)
                                            : std::clamp<int32_t>(s * s / 14, 12, 340);
    const size_t count = static_cast<size_t>(dense);
    const int32_t fleck = s >= 48 ? 2 : 1;
    const auto points = texture_points(t.area, t.cover, count, area_seed(t.area));
    for (size_t i = 0; i < points.size(); ++i) {
        const bool dark = (i % 3) != 0;
        t.dot(points[i].x, points[i].y, fleck, dark ? lv_color_black() : lv_color_white(),
              dark ? 90 : 120);
    }
}

// Sparkle: a few small four-pointed glints.
void sparkle_pixels(const PixelTarget& t) {
    const int32_t s = short_side(t.area);
    const size_t count = static_cast<size_t>(std::clamp<int32_t>(s * s / 70, 6, 44));
    const int32_t arm = std::max<int32_t>(1, s / 14);
    const auto points = texture_points(t.area, t.cover, count, area_seed(t.area) ^ 0x5A5Au);
    const lv_color_t white = lv_color_white();
    for (size_t i = 0; i < points.size(); ++i) {
        const float x = points[i].x;
        const float y = points[i].y;
        const bool big = (i % 3) == 0;
        t.dot(x, y, big ? 2 : 1, white, 240);
        if (big) {
            for (int32_t d = 1; d <= arm; ++d) {
                const auto o = static_cast<uint8_t>(170 / d);
                const float fd = static_cast<float>(d);
                t.dot(x + fd, y, 1, white, o);
                t.dot(x - fd, y, 1, white, o);
                t.dot(x, y + fd, 1, white, o);
                t.dot(x, y - fd, 1, white, o);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Object paint
// ---------------------------------------------------------------------------

// Retained per-object paint, keyed by object. Main (UI) thread only: the
// setters, the draw hook and the DELETE hook all run there.
struct ObjectPaint {
    std::vector<lv_color_t> colors; ///< empty: paint the object's bg_color / bg_grad
    bool style_color = false;       ///< a colour style was given, so bg_color is meant
    SwatchFinish finish = SwatchFinish::Plain;
    bool edge = true;
    std::unique_ptr<SwatchCache> cache = std::make_unique<SwatchCache>();
};

std::unordered_map<const lv_obj_t*, ObjectPaint>& paint_registry() {
    static std::unordered_map<const lv_obj_t*, ObjectPaint> registry;
    return registry;
}

// The colour behind @p obj: its nearest ancestor with an opaque background.
std::optional<uint32_t> backdrop_of(const lv_obj_t* obj) {
    for (const lv_obj_t* p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        if (lv_obj_get_style_bg_opa(p, LV_PART_MAIN) >= LV_OPA_MAX) {
            return rgb_of(lv_obj_get_style_bg_color(p, LV_PART_MAIN));
        }
    }
    return std::nullopt;
}

void draw_object_paint(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    if (it == registry.end()) {
        return;
    }
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    if (lv_area_get_width(&coords) <= 0 || lv_area_get_height(&coords) <= 0) {
        return;
    }

    SwatchPaint paint;
    paint.radius = lv_obj_get_style_radius(obj, LV_PART_MAIN);
    paint.finish = it->second.finish;
    paint.edge = it->second.edge;
    paint.opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    paint.cache = it->second.cache.get();
    paint.backdrop = backdrop_of(obj);
    if (!it->second.colors.empty()) {
        paint.colors = it->second.colors;
    } else {
        const lv_grad_dsc_t* grad = lv_obj_get_style_bg_grad(obj, LV_PART_MAIN);
        if (grad && grad->stops_count > 0) {
            paint.gradient = grad;
        } else if (it->second.style_color) {
            paint.colors = {lv_obj_get_style_bg_color(obj, LV_PART_MAIN)};
        } else {
            return; // no colour yet: an empty slot, or one C++ has not painted
        }
    }
    draw_swatch(lv_event_get_layer(e), coords, paint);
}

// The edge is judged against the live theme at draw time, so a theme change
// only needs the swatch redrawn.
void theme_changed_cb(lv_observer_t* observer, lv_subject_t* /*subject*/) {
    lv_obj_invalidate(static_cast<lv_obj_t*>(lv_observer_get_target(observer)));
}

void on_paint_deleted(lv_event_t* e) {
    paint_registry().erase(static_cast<lv_obj_t*>(lv_event_get_current_target(e)));
}

ObjectPaint& ensure_paint(lv_obj_t* obj) {
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    if (it != registry.end()) {
        return it->second;
    }
    // DECLARATIVE_OK: the swatch painter's own draw and teardown hooks. Painting
    // before the class draw lets a style border (a warning or selection mark)
    // land on top of the fill.
    lv_obj_add_event_cb(obj, draw_object_paint, LV_EVENT_DRAW_MAIN_BEGIN, nullptr);
    lv_obj_add_event_cb(obj, on_paint_deleted, LV_EVENT_DELETE, nullptr);
    // The painter draws the fill; the style's own background would cover it.
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
#if !defined(HELIX_PLATFORM_ESP32)
    // A swatch's color identifies a spool: glass must never shade it.
    helix::ui::glass_mark(obj, helix::ui::GlassFamily::None);
#endif
    if (lv_subject_t* theme = theme_manager_get_changed_subject()) {
        lv_subject_add_observer_obj(theme, theme_changed_cb, obj, nullptr);
    }
    return registry[obj];
}

// ---------------------------------------------------------------------------
// <swatch_paint> XML widget
// ---------------------------------------------------------------------------

void colors_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    const char* csv = lv_subject_get_string(subject);
    std::vector<lv_color_t> colors = parse_multi_colors(csv ? csv : "");
    ObjectPaint& p = ensure_paint(obj);
    if (colors.size() >= 2) {
        p.colors = std::move(colors);
    } else if (p.style_color) {
        p.colors.clear(); // the colour binding's bg_color paints
    } else {
        // No colour binding: keep painting the one colour already set.
        p.colors = {lv_obj_get_style_bg_color(obj, LV_PART_MAIN)};
    }
    lv_obj_invalidate(obj);
}

void color_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    lv_obj_set_style_bg_color(obj, lv_subject_get_color(subject), LV_PART_MAIN);
    ObjectPaint& p = ensure_paint(obj);
    // A bound colour always paints, a clear_swatch_color() before it included.
    p.style_color = true;
    if (p.colors.size() < 2) {
        p.colors.clear();
    }
    lv_obj_invalidate(obj);
}

void finish_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    const char* name = lv_subject_get_string(subject);
    ensure_paint(obj).finish = swatch_finish_from_variant(name ? name : "");
    lv_obj_invalidate(obj);
}

// A <color_swatch> holder paints through its disc; anything else paints itself.
lv_obj_t* paint_target(lv_obj_t* obj) {
    if (paint_registry().count(obj) > 0) {
        return obj;
    }
    lv_obj_t* disc = lv_obj_find_by_name(obj, "color_swatch_disc");
    return disc ? disc : obj;
}

bool non_empty(const char* v) {
    return v && v[0] != '\0';
}

void* swatch_paint_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    ensure_paint(obj);
    return obj;
}

void bind_string_subject(lv_xml_parser_state_t* state, lv_obj_t* obj, const char* name,
                         lv_observer_cb_t cb, const char* attr) {
    lv_subject_t* subject = lv_xml_get_subject(&state->scope, name);
    if (!subject) {
        spdlog::warn("[Swatch] {} subject '{}' does not exist", attr, name);
        return;
    }
    lv_subject_add_observer_obj(subject, cb, obj, nullptr);
}

void swatch_paint_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    lv_xml_obj_apply(state, attrs);
    // A style the attributes set must not bring the background back.
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    ObjectPaint& p = ensure_paint(obj);

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];
        if (!non_empty(value)) {
            continue;
        }
        if (std::strcmp(name, "color") == 0) {
            lv_obj_set_style_bg_color(obj, lv_xml_to_color(value), LV_PART_MAIN);
            p.style_color = true;
        } else if (std::strcmp(name, "bind_color") == 0) {
            p.style_color = true;
            bind_string_subject(state, obj, value, color_observer_cb, "bind_color");
        } else if (std::strcmp(name, "colors") == 0) {
            std::vector<lv_color_t> colors = parse_multi_colors(value);
            if (colors.size() >= 2) {
                p.colors = std::move(colors);
            } else if (colors.size() == 1) {
                lv_obj_set_style_bg_color(obj, colors[0], LV_PART_MAIN);
                p.style_color = true;
            }
        } else if (std::strcmp(name, "bind_colors") == 0) {
            bind_string_subject(state, obj, value, colors_observer_cb, "bind_colors");
        } else if (std::strcmp(name, "finish") == 0) {
            p.finish = swatch_finish_from_variant(value);
        } else if (std::strcmp(name, "bind_finish") == 0) {
            bind_string_subject(state, obj, value, finish_observer_cb, "bind_finish");
        } else if (std::strcmp(name, "edge") == 0) {
            p.edge = std::strcmp(value, "off") != 0 && std::strcmp(value, "false") != 0;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Pure decisions
// ---------------------------------------------------------------------------

bool swatch_needs_edge(uint32_t rgb, uint32_t surface) {
    return helix::contrast_ratio(lv_color_hex(rgb), lv_color_hex(surface)) <
           kSwatchEdgeContrastRatio;
}

std::vector<uint32_t> swatch_edge_surfaces(std::optional<uint32_t> backdrop) {
    if (backdrop) {
        return {*backdrop};
    }
    // Swatches sit on panels (screen_bg) and in dialogs (elevated_bg).
    const ThemeRgbs& t = theme_rgbs();
    return {t.screen_bg, t.elevated_bg};
}

bool swatch_needs_edge_here(uint32_t rgb) {
    return swatch_colors_need_edge({lv_color_hex(rgb)}, swatch_edge_surfaces());
}

bool swatch_colors_need_edge(const std::vector<lv_color_t>& colors,
                             const std::vector<uint32_t>& surfaces) {
    for (lv_color_t c : colors) {
        for (uint32_t s : surfaces) {
            if (swatch_needs_edge(rgb_of(c), s)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<lv_color_t> parse_multi_colors(const std::string& csv) {
    std::vector<lv_color_t> colors;
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t comma = csv.find(',', start);
        const std::string token =
            csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t b = token.find_first_not_of(" \t");
        const size_t e = token.find_last_not_of(" \t");
        if (b != std::string::npos) {
            if (auto rgb = helix::parse_hex_color(token.substr(b, e - b + 1))) {
                colors.push_back(lv_color_hex(*rgb));
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return colors;
}

SwatchFinish swatch_finish(helix::FilamentFinish finish) {
    switch (finish) {
    case helix::FilamentFinish::Silk:
        return SwatchFinish::Silk;
    case helix::FilamentFinish::Matte:
        return SwatchFinish::Matte;
    case helix::FilamentFinish::Marble:
        return SwatchFinish::Marble;
    case helix::FilamentFinish::Sparkle:
        return SwatchFinish::Sparkle;
    case helix::FilamentFinish::None:
        break;
    }
    return SwatchFinish::Plain;
}

SwatchFinish swatch_finish_from_variant(std::string_view name) {
    return swatch_finish(helix::filament_finish_of(name));
}

SwatchFinish swatch_finish_of(const helix::SlotInfo& slot) {
    return swatch_finish(slot.finish());
}

SwatchFinish swatch_effective_finish(SwatchFinish finish, int32_t w, int32_t h) {
    // By area, so a long thin band or a spool's wound body keeps its finish
    // while a dot of the same short side does not.
    return w * h < kSwatchFinishMinPx * kSwatchFinishMinPx ? SwatchFinish::Plain : finish;
}

lv_color_t swatch_matte_color(lv_color_t c) {
    // Rec. 601 luma; a fifth of the way to grey keeps the hue readable.
    const float grey = 0.299f * c.red + 0.587f * c.green + 0.114f * c.blue;
    auto mix = [grey](uint8_t ch) {
        return static_cast<uint8_t>(std::lround(ch + (grey - ch) * 0.2f));
    };
    return lv_color_make(mix(c.red), mix(c.green), mix(c.blue));
}

SwatchPolygon swatch_shape_polygon(const lv_area_t& area, int32_t radius) {
    const float x0 = static_cast<float>(area.x1);
    const float y0 = static_cast<float>(area.y1);
    const float x1 = static_cast<float>(area.x2 + 1);
    const float y1 = static_cast<float>(area.y2 + 1);
    const float r = clamped_radius(area, radius);
    if (r < 0.5f) {
        return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
    // About one vertex per 1.5px of arc keeps the outline round at swatch sizes.
    const int seg = std::clamp(static_cast<int>(r * kPiF / 2.0f / 1.5f), 3, 24);
    SwatchPolygon poly;
    poly.reserve(static_cast<size_t>(4 * (seg + 1)));
    const SwatchPoint centres[4] = {
        {x1 - r, y0 + r}, // top-right: -90..0 degrees
        {x1 - r, y1 - r}, // bottom-right: 0..90
        {x0 + r, y1 - r}, // bottom-left: 90..180
        {x0 + r, y0 + r}, // top-left: 180..270
    };
    for (int c = 0; c < 4; ++c) {
        const float start = -kPiF / 2.0f + static_cast<float>(c) * kPiF / 2.0f;
        for (int i = 0; i <= seg; ++i) {
            const float t = start + (kPiF / 2.0f) * static_cast<float>(i) / static_cast<float>(seg);
            poly.push_back({centres[c].x + r * std::cos(t), centres[c].y + r * std::sin(t)});
        }
    }
    return poly;
}

float swatch_shape_coverage(const lv_area_t& area, int32_t radius, float x, float y) {
    const float x0 = static_cast<float>(area.x1);
    const float y0 = static_cast<float>(area.y1);
    const float x1 = static_cast<float>(area.x2 + 1);
    const float y1 = static_cast<float>(area.y2 + 1);
    const float r = clamped_radius(area, radius);
    // Signed distance to the rounded rect's outline, negative inside.
    const float qx = std::fabs(x - 0.5f * (x0 + x1)) - (0.5f * (x1 - x0) - r);
    const float qy = std::fabs(y - 0.5f * (y0 + y1)) - (0.5f * (y1 - y0) - r);
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
    return std::clamp(0.5f - d, 0.0f, 1.0f);
}

bool swatch_shape_contains(const lv_area_t& area, int32_t radius, float x, float y) {
    const float x0 = static_cast<float>(area.x1);
    const float y0 = static_cast<float>(area.y1);
    const float x1 = static_cast<float>(area.x2 + 1);
    const float y1 = static_cast<float>(area.y2 + 1);
    if (x < x0 || x > x1 || y < y0 || y > y1) {
        return false;
    }
    const float r = clamped_radius(area, radius);
    const float cx = std::clamp(x, x0 + r, x1 - r);
    const float cy = std::clamp(y, y0 + r, y1 - r);
    const float dx = x - cx;
    const float dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

float swatch_polygon_area(const SwatchPolygon& poly) {
    float a = 0.0f;
    for (size_t i = 0; i < poly.size(); ++i) {
        const SwatchPoint& p = poly[i];
        const SwatchPoint& q = poly[(i + 1) % poly.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return std::fabs(a) * 0.5f;
}

std::vector<float> swatch_split_thresholds(const lv_area_t& area, int32_t radius, size_t n) {
    std::vector<float> thresholds;
    if (n < 2 || lv_area_get_width(&area) <= 0 || lv_area_get_height(&area) <= 0) {
        return thresholds;
    }
    const SwatchPolygon shape = swatch_shape_polygon(area, radius);
    const DiagField f = diag_field(area);
    const float total = swatch_polygon_area(shape);
    thresholds.reserve(n - 1);
    for (size_t k = 1; k < n; ++k) {
        thresholds.push_back(
            threshold_for_fraction(shape, f, total, static_cast<float>(k) / static_cast<float>(n)));
    }
    return thresholds;
}

std::vector<SwatchPolygon> swatch_split_polygons(const lv_area_t& area, int32_t radius, size_t n) {
    const SwatchPolygon shape = swatch_shape_polygon(area, radius);
    if (n < 2) {
        return {shape};
    }
    const DiagField f = diag_field(area);
    const std::vector<float> thresholds = swatch_split_thresholds(area, radius, n);
    std::vector<SwatchPolygon> bands;
    bands.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        SwatchPolygon band = shape;
        if (k > 0) {
            band = clip_halfplane(band, f, thresholds[k - 1], true);
        }
        if (k + 1 < n) {
            band = clip_halfplane(band, f, thresholds[k], false);
        }
        bands.push_back(std::move(band));
    }
    return bands;
}

std::vector<SwatchPoint> swatch_texture_points(const lv_area_t& area, int32_t radius, size_t count,
                                               uint32_t seed) {
    // Keep texture a pixel inside the rim, where the edge would cover it.
    lv_area_t inner = area;
    lv_area_increase(&inner, -1, -1);
    const SwatchCoverage inside = [&inner, radius](float x, float y) {
        return swatch_shape_contains(inner, radius, x, y) ? 1.0f : 0.0f;
    };
    return texture_points(area, inside, count, seed);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

SwatchCache::~SwatchCache() {
    drop_texture();
}

void SwatchCache::drop_texture() {
    if (texture_) {
        if (lv_is_initialized()) {
            lv_image_cache_drop(texture_);
        }
        helix::safe_draw_buf_destroy(texture_, "swatch");
    }
}

const lv_draw_buf_t* SwatchCache::texture(int32_t w, int32_t h, int32_t radius,
                                          SwatchFinish finish) {
    if (texture_ && w == tex_w_ && h == tex_h_ && radius == tex_radius_ && finish == tex_finish_) {
        return texture_;
    }
    drop_texture();
    tex_w_ = w;
    tex_h_ = h;
    tex_radius_ = radius;
    tex_finish_ = finish;
    // LVGL's heap: on the ESP32 a buffer this size comes from PSRAM.
    texture_ = lv_draw_buf_create(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                  LV_COLOR_FORMAT_ARGB8888, 0);
    if (!texture_) {
        spdlog::warn("[Swatch] no memory for a {}x{} finish texture", w, h);
        return nullptr;
    }
    lv_draw_buf_clear(texture_, nullptr);
    swatch_paint_finish_pixels(texture_, {0, 0, w - 1, h - 1}, radius, finish);
    ++texture_builds_;
    return texture_;
}

const std::vector<float>& SwatchCache::thresholds(const lv_area_t& area, int32_t radius, size_t n) {
    const int32_t w = lv_area_get_width(&area);
    const int32_t h = lv_area_get_height(&area);
    if (w != thr_w_ || h != thr_h_ || radius != thr_radius_ || n != thr_n_) {
        // The thresholds are fractions of the shape, so its position does not matter.
        thresholds_ = swatch_split_thresholds({0, 0, w - 1, h - 1}, radius, n);
        thr_w_ = w;
        thr_h_ = h;
        thr_radius_ = radius;
        thr_n_ = n;
    }
    return thresholds_;
}

namespace {
void paint_finish(lv_draw_buf_t* buf, const lv_area_t& area, const SwatchCoverage& cover,
                  bool density_by_area, SwatchFinish finish) {
    if (!buf || buf->header.cf != LV_COLOR_FORMAT_ARGB8888) {
        return;
    }
    const PixelTarget t{buf, area, cover, density_by_area};
    switch (swatch_effective_finish(finish, lv_area_get_width(&area), lv_area_get_height(&area))) {
    case SwatchFinish::Silk:
        silk_pixels(t);
        break;
    case SwatchFinish::Marble:
        marble_pixels(t);
        break;
    case SwatchFinish::Sparkle:
        sparkle_pixels(t);
        break;
    case SwatchFinish::Matte:
    case SwatchFinish::Plain:
        break;
    }
}
} // namespace

void swatch_paint_finish_pixels(lv_draw_buf_t* buf, const lv_area_t& area, int32_t radius,
                                SwatchFinish finish) {
    const SwatchCoverage shape = [&area, radius](float x, float y) {
        return swatch_shape_coverage(area, radius, x, y);
    };
    paint_finish(buf, area, shape, false, finish);
}

void swatch_paint_finish_pixels(lv_draw_buf_t* buf, const lv_area_t& area,
                                const SwatchCoverage& cover, SwatchFinish finish) {
    paint_finish(buf, area, cover, true, finish);
}

void draw_swatch(lv_layer_t* layer, const lv_area_t& area, const SwatchPaint& paint) {
    const int32_t w = lv_area_get_width(&area);
    const int32_t h = lv_area_get_height(&area);
    if (!layer || paint.opa <= LV_OPA_MIN || w <= 0 || h <= 0) {
        return;
    }
    if (!paint.gradient && paint.colors.empty()) {
        return;
    }
    const SwatchFinish finish = swatch_effective_finish(paint.finish, w, h);
    std::vector<lv_color_t> colors = paint.colors;
    if (finish == SwatchFinish::Matte) {
        for (auto& c : colors) {
            c = swatch_matte_color(c);
        }
    }

    if (paint.gradient) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = paint.radius;
        dsc.bg_opa = paint.opa;
        dsc.bg_grad = *paint.gradient;
        dsc.bg_color = paint.gradient->stops[0].color;
        lv_draw_rect(layer, &dsc, &area);
    } else if (colors.size() < 2) {
        fill_shape(layer, area, paint.radius, colors[0], paint.opa);
    } else if (paint.cache) {
        fill_split(layer, area, paint.radius, colors,
                   paint.cache->thresholds(area, paint.radius, colors.size()), paint.opa);
    } else {
        fill_split(layer, area, paint.radius, colors,
                   swatch_split_thresholds(area, paint.radius, colors.size()), paint.opa);
    }

    if (paint.cache && finish != SwatchFinish::Plain && finish != SwatchFinish::Matte) {
        if (const lv_draw_buf_t* tex = paint.cache->texture(w, h, paint.radius, finish)) {
            lv_draw_image_dsc_t dsc;
            lv_draw_image_dsc_init(&dsc);
            dsc.src = tex;
            dsc.opa = paint.opa;
            lv_draw_image(layer, &dsc, &area);
        }
    }

    if (paint.edge && !paint.gradient &&
        swatch_colors_need_edge(colors, swatch_edge_surfaces(paint.backdrop))) {
        draw_edge(layer, area, paint.radius, paint.opa);
    }
}

SwatchPaint swatch_paint_for(uint32_t rgb, const std::string& multi_color_hexes,
                             SwatchFinish finish) {
    SwatchPaint paint;
    paint.colors = parse_multi_colors(multi_color_hexes);
    if (paint.colors.size() < 2) {
        paint.colors = {lv_color_hex(rgb)};
    }
    paint.finish = finish;
    return paint;
}

SwatchPaint swatch_paint_for(const helix::SlotInfo& slot) {
    return swatch_paint_for(slot.color_rgb, slot.multi_color_hexes, swatch_finish_of(slot));
}

// ---------------------------------------------------------------------------
// Object paint
// ---------------------------------------------------------------------------

void apply_swatch_color(lv_obj_t* swatch, uint32_t primary_rgb,
                        const std::string& multi_color_hexes, SwatchFinish finish) {
    if (!swatch) {
        return;
    }
    swatch = paint_target(swatch);
    ObjectPaint& p = ensure_paint(swatch);
    std::vector<lv_color_t> colors = parse_multi_colors(multi_color_hexes);
    p.colors =
        colors.size() >= 2 ? std::move(colors) : std::vector<lv_color_t>{lv_color_hex(primary_rgb)};
    p.finish = finish;
    // Click handlers read a swatch's colour back from its bg_color.
    lv_obj_set_style_bg_color(swatch, lv_color_hex(primary_rgb), LV_PART_MAIN);
    lv_obj_invalidate(swatch);
}

void apply_swatch_color(lv_obj_t* swatch, const helix::SlotInfo& slot) {
    apply_swatch_color(swatch, slot.color_rgb, slot.multi_color_hexes, swatch_finish_of(slot));
}

void attach_swatch_paint(lv_obj_t* obj) {
    if (obj) {
        ensure_paint(obj).style_color = true;
    }
}

void clear_swatch_color(lv_obj_t* swatch) {
    if (!swatch) {
        return;
    }
    swatch = paint_target(swatch);
    ObjectPaint& p = ensure_paint(swatch);
    p.colors.clear();
    p.style_color = false;
    lv_obj_invalidate(swatch);
}

bool has_swatch_paint(const lv_obj_t* obj) {
    return paint_registry().count(obj) > 0;
}

std::vector<lv_color_t> swatch_paint_colors(const lv_obj_t* obj) {
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    return it == registry.end() ? std::vector<lv_color_t>{} : it->second.colors;
}

SwatchFinish swatch_paint_finish(const lv_obj_t* obj) {
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    return it == registry.end() ? SwatchFinish::Plain : it->second.finish;
}

lv_color_t swatch_color_of(lv_obj_t* obj) {
    if (!obj) {
        return lv_color_black();
    }
    lv_obj_t* target = paint_target(obj);
    const std::vector<lv_color_t> colors = swatch_paint_colors(target);
    if (colors.size() == 1) {
        return colors[0];
    }
    return lv_obj_get_style_bg_color(target, LV_PART_MAIN);
}

void register_swatch_paint_widget() {
    lv_xml_register_widget("swatch_paint", swatch_paint_xml_create, swatch_paint_xml_apply);
}

void reset_swatch_state() {
    paint_registry().clear();
}

} // namespace helix::ui
