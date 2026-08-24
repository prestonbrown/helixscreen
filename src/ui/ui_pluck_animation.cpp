// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Isometric pluck-instruction illustration for the belt tuner's POSITION and
// LISTEN states. Ported from the agreed scene in
// .superpowers/brainstorm/3846895-1786305129/content/pluck-iso-generator.py
// (rendered previews: frame1.png/frame2.png/frame3.png beside it) - that
// script is scratch, not shipping code, but its projection, scene
// coordinates and twist/pull profile are the settled design and are ported
// here rather than re-derived. Confirmed routing: an inner run against the
// extrusion, a 180-degree wrap around an upright idler at the near front
// corner, and an outer return run nearest the viewer. The outer run is the
// one plucked - the finger hooks its top edge from above and pulls it
// toward the viewer (+x), so the deflection and the twist are one motion.
// Teeth face outward on the outer run and around the wrap.
//
// The spectrum being measured (tens of Hz) aliases against a 60 Hz display,
// so the ring-down is a stylised decaying wobble, not the true frequency -
// see the design doc's correction note. The twist is exaggerated well past
// physical accuracy for the same reason: at true amplitude it is invisible
// at this size.
#include "ui_pluck_animation.h"

#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace helix {
namespace ui {

// ---- pure helpers (unit tested; no LVGL) ----

PluckPoint pluck_iso_project(float x, float y, float z, float ox, float oy, float scale) {
    constexpr float COS30 = 0.8660254f;
    PluckPoint p;
    p.x = ox + (x - y) * COS30 * scale;
    p.y = oy + ((x + y) * 0.5f - z) * scale;
    return p;
}

namespace {
// Loop thirds, shared by pluck_frame_at_ms(), pluck_deflection_at_ms() and
// the widget's finger placement so all three agree on where one phase ends
// and the next begins.
constexpr uint32_t PHASE1_END_MS = PLUCK_LOOP_MS / 3;
constexpr uint32_t PHASE2_END_MS = (PLUCK_LOOP_MS * 2) / 3;
} // namespace

int pluck_frame_at_ms(uint32_t t) {
    const uint32_t phase = t % PLUCK_LOOP_MS;
    if (phase < PHASE1_END_MS)
        return 0;
    if (phase < PHASE2_END_MS)
        return 1;
    return 2;
}

float pluck_deflection_at_ms(uint32_t t) {
    const uint32_t phase = t % PLUCK_LOOP_MS;
    if (phase < PHASE1_END_MS) {
        // Reach: the hand is still approaching, the belt is untouched.
        return 0.0f;
    }
    if (phase < PHASE2_END_MS) {
        // Hook and pull: ease up to full deflection by the phase midpoint,
        // then hold - this is the frame that teaches the gesture.
        const float u = static_cast<float>(phase - PHASE1_END_MS) /
                        static_cast<float>(PHASE2_END_MS - PHASE1_END_MS);
        const float rise = std::min(1.0f, u / 0.5f);
        return rise * rise * (3.0f - 2.0f * rise); // smoothstep
    }
    // Release: a decaying, alternating ring-down back through and past taut.
    const float u = static_cast<float>(phase - PHASE2_END_MS) /
                    static_cast<float>(PLUCK_LOOP_MS - PHASE2_END_MS);
    const float decay = std::exp(-4.0f * u);
    const float osc = std::cos(2.0f * static_cast<float>(M_PI) * 2.2f * u);
    return decay * osc;
}

} // namespace ui
} // namespace helix

// ---- widget ----

namespace {

using helix::ui::pluck_deflection_at_ms;
using helix::ui::pluck_frame_at_ms;
using helix::ui::pluck_iso_project;
using helix::ui::PLUCK_LOOP_MS;
using helix::ui::PluckPoint;

// Design canvas: the widget fits this box into its own content area at
// runtime, so the scene re-renders at any panel size instead of being tied
// to one resolution (480x272 and 1024x600 both need to look right).
constexpr float ART_W = 300.0f;
constexpr float ART_H = 187.0f;
// Origin and overall zoom from the generator's iso(), before per-widget
// fit-scaling is folded in at draw time.
constexpr float DESIGN_OX = 218.0f;
constexpr float DESIGN_OY = 70.0f;
constexpr float DESIGN_SC = 1.1f;

// Scene geometry - ported verbatim from pluck-iso-generator.py's named
// constants. Values are the generator's arbitrary scene units, not mm.
constexpr float EXT_Z = 40.0f;    // extrusion height
constexpr float EY0 = -85.0f;     // extrusion far end (y, front-to-back)
constexpr float EY1 = 210.0f;     // extrusion near end
constexpr float CZ = 41.0f;       // belt centreline height
constexpr float HW = 7.0f;        // belt half-width (vertical, on edge)
constexpr float HT = 1.5f;        // belt half-thickness
constexpr float CXI = 23.5f;      // inner run centreline x - against the extrusion
constexpr float CXO = 49.5f;      // outer run centreline x - the plucked run
constexpr float WY = 185.0f;      // idler centre y - the near front corner
constexpr float WX = 36.5f;       // idler axis x
constexpr float R_TOOTH = 14.5f;  // idler pitch radius - both runs are tangent here
constexpr float R_FLANGE = 16.5f; // idler flange radius
constexpr float FZ0 = 32.0f;      // lower flange z
constexpr float FZ1 = 50.0f;      // upper flange z
constexpr float RUN_Y0 = -65.0f;  // runs exit the frame toward the gantry
constexpr float YC = 75.0f;       // finger / mark / peak position along the outer run
constexpr float MARK_Y0 = 68.0f;
constexpr float MARK_Y1 = 82.0f;
constexpr float PULL_D = 12.0f; // outward displacement at full deflection
const float TWIST_MAX = 52.0f * static_cast<float>(M_PI) / 180.0f;

// Loop thirds - same formula as the PHASE1_END_MS/PHASE2_END_MS pair in the
// helix::ui namespace above, needed again here for the finger's placement.
constexpr uint32_t PHASE1_END_MS = PLUCK_LOOP_MS / 3;
constexpr uint32_t PHASE2_END_MS = (PLUCK_LOOP_MS * 2) / 3;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

struct ProjCtx {
    float ox;
    float oy;
    float scale;
};

PluckPoint proj(const ProjCtx& c, float x, float y, float z) {
    return pluck_iso_project(x, y, z, c.ox, c.oy, c.scale);
}

lv_point_precise_t to_lv(const PluckPoint& p) {
    return {static_cast<lv_value_precise_t>(p.x), static_cast<lv_value_precise_t>(p.y)};
}

// ---- draw primitives ----

void fill_tri(lv_layer_t* layer, const PluckPoint& a, const PluckPoint& b, const PluckPoint& c,
              lv_color_t color) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    dsc.p[0] = to_lv(a);
    dsc.p[1] = to_lv(b);
    dsc.p[2] = to_lv(c);
    lv_draw_triangle(layer, &dsc);
}

// Filled quad from 4 corners in order - two triangles, per the task brief.
void fill_quad(lv_layer_t* layer, const PluckPoint& p0, const PluckPoint& p1, const PluckPoint& p2,
               const PluckPoint& p3, lv_color_t color) {
    fill_tri(layer, p0, p1, p2, color);
    fill_tri(layer, p0, p2, p3, color);
}

// Quad strip between two equal-length point sequences - the belt-band and
// wrap-ribbon primitive: a filled quad between each consecutive index pair.
void fill_ribbon(lv_layer_t* layer, const std::vector<PluckPoint>& a,
                 const std::vector<PluckPoint>& b, lv_color_t color) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i + 1 < n; ++i) {
        fill_quad(layer, a[i], a[i + 1], b[i + 1], b[i], color);
    }
}

void stroke_line(lv_layer_t* layer, const PluckPoint& a, const PluckPoint& b, lv_color_t color,
                 int32_t width, lv_opa_t opa) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = opa;
    dsc.p1 = to_lv(a);
    dsc.p2 = to_lv(b);
    lv_draw_line(layer, &dsc);
}

void stroke_path(lv_layer_t* layer, const std::vector<PluckPoint>& pts, lv_color_t color,
                 int32_t width, lv_opa_t opa) {
    for (size_t i = 1; i < pts.size(); ++i) {
        stroke_line(layer, pts[i - 1], pts[i], color, width, opa);
    }
}

// A filled disc (pulley flange, adjuster knob) as a triangle fan from its
// projected centre - "the pulley discs are triangle fans" per the brief.
// Circle lies in the x-y plane (fixed z).
void fill_disc_xy(lv_layer_t* layer, const ProjCtx& c, float cx, float cy, float z, float r,
                  lv_color_t color, int n = 16) {
    const PluckPoint center = proj(c, cx, cy, z);
    PluckPoint prev = proj(c, cx + r, cy, z);
    for (int i = 1; i <= n; ++i) {
        const float t =
            2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / static_cast<float>(n);
        const PluckPoint cur = proj(c, cx + r * std::cos(t), cy + r * std::sin(t), z);
        fill_tri(layer, center, prev, cur, color);
        prev = cur;
    }
}

// Same fan, but the circle lies in the x-z plane (fixed y) - the adjuster
// knob's face, which looks at the viewer rather than up.
void fill_disc_xz(lv_layer_t* layer, const ProjCtx& c, float cx, float y, float cz, float r,
                  lv_color_t color, int n = 14) {
    const PluckPoint center = proj(c, cx, y, cz);
    PluckPoint prev = proj(c, cx + r, y, cz);
    for (int i = 1; i <= n; ++i) {
        const float t =
            2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / static_cast<float>(n);
        const PluckPoint cur = proj(c, cx + r * std::cos(t), y, cz + r * std::sin(t));
        fill_tri(layer, center, prev, cur, color);
        prev = cur;
    }
}

// A small filled disc already in screen space - the finger's rounded ends.
void fill_screen_disc(lv_layer_t* layer, const PluckPoint& center, float r, lv_color_t color,
                      int n = 10) {
    PluckPoint prev{center.x + r, center.y};
    for (int i = 1; i <= n; ++i) {
        const float t =
            2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / static_cast<float>(n);
        const PluckPoint cur{center.x + r * std::cos(t), center.y + r * std::sin(t)};
        fill_tri(layer, center, prev, cur, color);
        prev = cur;
    }
}

std::vector<PluckPoint> circle_ring_xy(const ProjCtx& c, float cx, float cy, float z, float r,
                                       float a0_deg, float a1_deg, int n) {
    std::vector<PluckPoint> pts;
    pts.reserve(static_cast<size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
        const float deg =
            a0_deg + (a1_deg - a0_deg) * static_cast<float>(i) / static_cast<float>(n);
        const float rad = deg * static_cast<float>(M_PI) / 180.0f;
        pts.push_back(proj(c, cx + r * std::cos(rad), cy + r * std::sin(rad), z));
    }
    return pts;
}

std::vector<PluckPoint> circle_ring_xz(const ProjCtx& c, float cx, float y, float cz, float r,
                                       float a0_deg, float a1_deg, int n) {
    std::vector<PluckPoint> pts;
    pts.reserve(static_cast<size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
        const float deg =
            a0_deg + (a1_deg - a0_deg) * static_cast<float>(i) / static_cast<float>(n);
        const float rad = deg * static_cast<float>(M_PI) / 180.0f;
        pts.push_back(proj(c, cx + r * std::cos(rad), y, cz + r * std::sin(rad)));
    }
    return pts;
}

// Three visible faces of an axis-aligned box (top, +x, +y) - port of the
// generator's box() helper.
void fill_box(lv_layer_t* layer, const ProjCtx& c, float x0, float x1, float y0, float y1, float z0,
              float z1, lv_color_t top_color, lv_color_t side_color, lv_color_t outline) {
    const PluckPoint top[4] = {proj(c, x0, y0, z1), proj(c, x1, y0, z1), proj(c, x1, y1, z1),
                               proj(c, x0, y1, z1)};
    fill_quad(layer, top[0], top[1], top[2], top[3], top_color);

    const PluckPoint side_x[4] = {proj(c, x1, y0, z0), proj(c, x1, y1, z0), proj(c, x1, y1, z1),
                                  proj(c, x1, y0, z1)};
    fill_quad(layer, side_x[0], side_x[1], side_x[2], side_x[3], side_color);

    const PluckPoint side_y[4] = {proj(c, x0, y1, z0), proj(c, x1, y1, z0), proj(c, x1, y1, z1),
                                  proj(c, x0, y1, z1)};
    fill_quad(layer, side_y[0], side_y[1], side_y[2], side_y[3], side_color);

    stroke_path(layer, {top[0], top[1], top[2], top[3], top[0]}, outline, 1, LV_OPA_40);
}

// ---- outer (plucked) run: pull and twist are one motion, driven by k ----

// 0..1 bell profile along the span: zero at the window ends, 1 at the
// finger's position (YC) - so deflection tapers to nothing away from where
// the hand actually is.
float prof_at(float y) {
    constexpr float Y0 = -30.0f;
    constexpr float Y1 = 178.0f;
    if (y <= Y0 || y >= Y1)
        return 0.0f;
    const float u = (y < YC) ? (y - Y0) / (YC - Y0) : (Y1 - y) / (Y1 - YC);
    const float s = std::sin(static_cast<float>(M_PI) / 2.0f * std::min(u, 1.0f));
    return s * s;
}

// The belt's top-edge point at a given y for deflection k - shared by the
// band geometry and by the finger, so the fingertip actually tracks the
// edge it is supposedly hooking.
Vec3 belt_top_at(float y, float k) {
    const float p = prof_at(y) * k;
    const float th = TWIST_MAX * p;
    const float cx = CXO + PULL_D * p;
    return {cx + HW * std::sin(th), y, CZ + HW * std::cos(th)};
}

constexpr int OUTER_N = 22;

// k scales both the pull and the twist together, continuously - not three
// discrete poses. k can go negative during the release phase's ring-down.
void outer_geo(float k, std::vector<Vec3>& top, std::vector<Vec3>& bot) {
    top.clear();
    bot.clear();
    top.reserve(OUTER_N);
    bot.reserve(OUTER_N);
    for (int i = 0; i < OUTER_N; ++i) {
        const float y =
            RUN_Y0 + (WY - RUN_Y0) * static_cast<float>(i) / static_cast<float>(OUTER_N - 1);
        const float p = prof_at(y) * k;
        const float th = TWIST_MAX * p;
        const float cx = CXO + PULL_D * p;
        const float ex = HW * std::sin(th);
        const float ez = HW * std::cos(th);
        top.push_back({cx + ex, y, CZ + ez});
        bot.push_back({cx - ex, y, CZ - ez});
    }
}

void draw_outer_run(lv_layer_t* layer, const ProjCtx& c, float k, lv_color_t belt,
                    lv_color_t belt_dk, lv_color_t belt_lt, lv_color_t mark) {
    std::vector<Vec3> top, bot;
    outer_geo(k, top, bot);

    std::vector<PluckPoint> ptop(OUTER_N), pbot(OUTER_N);
    for (int i = 0; i < OUTER_N; ++i) {
        ptop[static_cast<size_t>(i)] =
            proj(c, top[static_cast<size_t>(i)].x, top[static_cast<size_t>(i)].y,
                 top[static_cast<size_t>(i)].z);
        pbot[static_cast<size_t>(i)] =
            proj(c, bot[static_cast<size_t>(i)].x, bot[static_cast<size_t>(i)].y,
                 bot[static_cast<size_t>(i)].z);
    }
    fill_ribbon(layer, ptop, pbot, belt);

    // Lit top edge: rolling the toothed face toward the viewer turns it away
    // in isometric, so the twist has to read through what the roll exposes
    // instead - a highlight riding the top edge so its path visibly bows.
    stroke_path(layer, ptop, belt_lt, 2, LV_OPA_90);

    // Teeth on the outward face, compressing and fading as the band rolls
    // over through the twist.
    for (int i = 1; i < OUTER_N - 1; i += 2) {
        const float y = top[static_cast<size_t>(i)].y;
        const float p = prof_at(y) * k;
        const float th = TWIST_MAX * p;
        const float fade = std::cos(th) * std::cos(th);
        const auto opa = static_cast<lv_opa_t>(std::clamp(fade, 0.0f, 1.0f) * 220.0f);
        if (opa < 8)
            continue;
        stroke_line(layer, ptop[static_cast<size_t>(i)], pbot[static_cast<size_t>(i)], belt_dk, 1,
                    opa);
    }

    // Green pluck mark, riding the deformation so it stays visible through
    // the twist and the pull.
    std::vector<PluckPoint> mtop, mbot;
    for (int i = 0; i < OUTER_N; ++i) {
        const float y = top[static_cast<size_t>(i)].y;
        if (y >= MARK_Y0 && y <= MARK_Y1) {
            mtop.push_back(ptop[static_cast<size_t>(i)]);
            mbot.push_back(pbot[static_cast<size_t>(i)]);
        }
    }
    if (mtop.size() >= 2) {
        fill_ribbon(layer, mtop, mbot, mark);
    }
}

void draw_inner_run(lv_layer_t* layer, const ProjCtx& c, lv_color_t belt_dk) {
    // The static, boxed-in run: it never moves, so a flat quad is enough -
    // there is nothing here for the eye to read except "not this one".
    const PluckPoint p0 = proj(c, CXI, RUN_Y0, CZ - HW);
    const PluckPoint p1 = proj(c, CXI, WY, CZ - HW);
    const PluckPoint p2 = proj(c, CXI, WY, CZ + HW);
    const PluckPoint p3 = proj(c, CXI, RUN_Y0, CZ + HW);
    fill_quad(layer, p0, p1, p2, p3, belt_dk);
}

void draw_extrusion(lv_layer_t* layer, const ProjCtx& c, lv_color_t metal, lv_color_t metal_dk,
                    lv_color_t outline) {
    fill_box(layer, c, 0.0f, 20.0f, EY0, EY1, 0.0f, EXT_Z, metal, metal_dk, outline);
}

void draw_idler(lv_layer_t* layer, const ProjCtx& c, lv_color_t plastic, lv_color_t plastic_dk,
                lv_color_t outline, lv_color_t belt, lv_color_t belt_dk, lv_color_t belt_lt,
                lv_color_t red, lv_color_t red_dk) {
    // Mounting plinth under the idler.
    fill_box(layer, c, 18.0f, 58.0f, 168.0f, 205.0f, 22.0f, FZ0, plastic, plastic_dk, outline);

    // Lower flange.
    fill_disc_xy(layer, c, WX, WY, FZ0, R_FLANGE, plastic);

    // 180-degree belt wrap, toothed face outward, at the pitch radius (the
    // spacing between the two runs).
    const auto wrap_top = circle_ring_xy(c, WX, WY, CZ + HW, R_TOOTH, 0.0f, 180.0f, 16);
    const auto wrap_bot = circle_ring_xy(c, WX, WY, CZ - HW, R_TOOTH, 0.0f, 180.0f, 16);
    fill_ribbon(layer, wrap_top, wrap_bot, belt);
    stroke_path(layer, wrap_top, belt_lt, 1, LV_OPA_70);
    for (int i = 2; i < 15; i += 3) {
        stroke_line(layer, wrap_top[static_cast<size_t>(i)], wrap_bot[static_cast<size_t>(i)],
                    belt_dk, 1, LV_OPA_80);
    }

    // Upper flange.
    fill_disc_xy(layer, c, WX, WY, FZ1, R_FLANGE, plastic);

    // Printed cap over the idler, with the red tension adjuster on its
    // viewer-facing wall - red is the machine's real colour, not a theme
    // token (see class comment).
    fill_box(layer, c, 26.0f, 48.0f, 172.0f, 200.0f, FZ1, 58.0f, plastic, plastic_dk, outline);
    fill_disc_xz(layer, c, 37.0f, 203.5f, 54.0f, 4.0f, red_dk);
    fill_disc_xz(layer, c, 37.0f, 200.5f, 54.0f, 4.0f, red);
}

void draw_ring_hint(lv_layer_t* layer, const ProjCtx& c, float k, lv_color_t color) {
    const float amp = std::clamp(std::fabs(k), 0.0f, 1.0f);
    const auto opa = static_cast<lv_opa_t>(60.0f + 120.0f * amp);
    const float span = 45.0f + 35.0f * amp;
    const float center_deg = (k >= 0.0f) ? 0.0f : 180.0f;
    const auto arc =
        circle_ring_xz(c, CXO, YC, CZ, 13.0f, center_deg - span, center_deg + span, 12);
    stroke_path(layer, arc, color, 2, opa);
}

// ---- the finger: a single tapered fingertip, not a pinch (three attempts
// at a pinch failed to read at this size, per the design doc) ----

void finger_pose(uint32_t phase_ms, float k, Vec3& tip, Vec3& base) {
    if (phase_ms < PHASE1_END_MS) {
        // Reach: approach from above, arriving at the untouched top edge.
        const float u = static_cast<float>(phase_ms) / static_cast<float>(PHASE1_END_MS);
        const Vec3 approach_tip{CXO + 26.0f, YC + 30.0f, CZ + 34.0f};
        const Vec3 approach_base{CXO + 55.0f, YC + 10.0f, CZ + 58.0f};
        const Vec3 touch = belt_top_at(YC, 0.0f);
        const Vec3 touch_base{touch.x + 30.0f, touch.y - 15.0f, touch.z + 24.0f};
        tip = lerp(approach_tip, touch, u);
        base = lerp(approach_base, touch_base, u);
    } else if (phase_ms < PHASE2_END_MS) {
        // Hook and pull: the fingertip tracks the real deflected edge, so it
        // visibly draws the belt out rather than floating near it.
        const Vec3 hooked = belt_top_at(YC, k);
        tip = hooked;
        base = {hooked.x + 30.0f, hooked.y - 15.0f, hooked.z + 24.0f};
    } else {
        // Release: withdraw while the belt keeps ringing behind.
        const float u = static_cast<float>(phase_ms - PHASE2_END_MS) /
                        static_cast<float>(PLUCK_LOOP_MS - PHASE2_END_MS);
        const Vec3 withdraw_tip{CXO + 30.0f, YC - 30.0f, CZ + 42.0f};
        const Vec3 withdraw_base{CXO + 58.0f, YC - 55.0f, CZ + 62.0f};
        const Vec3 release = belt_top_at(YC, k);
        const Vec3 release_base{release.x + 30.0f, release.y - 15.0f, release.z + 24.0f};
        tip = lerp(release, withdraw_tip, u);
        base = lerp(release_base, withdraw_base, u);
    }
}

void draw_finger(lv_layer_t* layer, const ProjCtx& c, const Vec3& tip_world, const Vec3& base_world,
                 lv_color_t fill, lv_color_t fill_lt, lv_color_t outline) {
    const PluckPoint tip = proj(c, tip_world.x, tip_world.y, tip_world.z);
    const PluckPoint base = proj(c, base_world.x, base_world.y, base_world.z);
    const float dx = base.x - tip.x;
    const float dy = base.y - tip.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f)
        return;
    const float nx = -dy / len;
    const float ny = dx / len;
    constexpr float TIP_HW = 1.6f;  // half-width at the fingertip
    constexpr float BASE_HW = 6.5f; // half-width at the knuckle

    const PluckPoint t0{tip.x + nx * TIP_HW, tip.y + ny * TIP_HW};
    const PluckPoint t1{tip.x - nx * TIP_HW, tip.y - ny * TIP_HW};
    const PluckPoint b0{base.x + nx * BASE_HW, base.y + ny * BASE_HW};
    const PluckPoint b1{base.x - nx * BASE_HW, base.y - ny * BASE_HW};

    fill_quad(layer, t0, t1, b1, b0, fill);
    fill_screen_disc(layer, base, BASE_HW, fill);
    fill_screen_disc(layer, tip, TIP_HW, fill);
    stroke_line(layer, t0, b0, fill_lt, 1, LV_OPA_60);
    stroke_line(layer, t1, b1, outline, 1, LV_OPA_50);
}

// ---- the widget itself ----

class PluckAnimation {
  public:
    static lv_obj_t* create(lv_obj_t* parent);

  private:
    PluckAnimation() = default;
    ~PluckAnimation() = default;
    PluckAnimation(const PluckAnimation&) = delete;
    PluckAnimation& operator=(const PluckAnimation&) = delete;

    static void on_draw(lv_event_t* e);
    static void on_delete(lv_event_t* e);
    static void on_size_changed(lv_event_t* e);
    static void anim_exec_cb(void* var, int32_t value);

    lv_obj_t* obj_ = nullptr;
    // Held-pull pose (frame 1, the one that teaches the gesture) is the
    // default so a static render - animations disabled - still shows
    // something useful rather than an arbitrary instant.
    uint32_t phase_ms_ = PLUCK_LOOP_MS / 2;
};

lv_obj_t* PluckAnimation::create(lv_obj_t* parent) {
    auto* impl = new PluckAnimation();
    impl->obj_ = lv_obj_create(parent);
    lv_obj_set_user_data(impl->obj_, impl);
    lv_obj_remove_style_all(impl->obj_);
    lv_obj_set_width(impl->obj_, LV_PCT(100));
    lv_obj_set_height(impl->obj_, static_cast<int32_t>(ART_H)); // corrected by on_size_changed
    lv_obj_clear_flag(impl->obj_, LV_OBJ_FLAG_SCROLLABLE);

    // No wrapping XML component provides styling (the panel embeds this tag
    // bare, unchanged from before this widget existed), so the card look is
    // set here, matching the stub it replaces (screen_bg, border_radius).
    lv_obj_set_style_bg_color(impl->obj_, theme_manager_get_color("screen_bg"), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(impl->obj_, LV_OPA_COVER, LV_PART_MAIN);
    const char* radius_str = lv_xml_get_const(nullptr, "border_radius");
    lv_obj_set_style_radius(impl->obj_, radius_str ? atoi(radius_str) : 8, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(impl->obj_, true, LV_PART_MAIN);

    lv_obj_add_event_cb(impl->obj_, on_draw, LV_EVENT_DRAW_MAIN_END, impl);
    lv_obj_add_event_cb(impl->obj_, on_delete, LV_EVENT_DELETE, impl);
    lv_obj_add_event_cb(impl->obj_, on_size_changed, LV_EVENT_SIZE_CHANGED, impl);

    if (helix::DisplaySettingsManager::instance().get_animations_enabled()) {
        // var is the lv_obj_t*, not the impl pointer: LVGL's own object
        // destructor purges every animation keyed on a deleted object
        // (lv_obj_destructor() -> lv_anim_delete(obj, NULL)), so this can
        // never fire against freed `impl`. on_delete() also cancels it
        // explicitly before deleting `impl`, belt-and-braces.
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, impl->obj_);
        lv_anim_set_values(&anim, 0, static_cast<int32_t>(PLUCK_LOOP_MS));
        lv_anim_set_duration(&anim, PLUCK_LOOP_MS);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_linear);
        lv_anim_set_exec_cb(&anim, anim_exec_cb);
        lv_anim_start(&anim);
    }
    // else: leave phase_ms_ at its held-pull default and never start the
    // anim - a static draw of the frame that most needs to be seen.

    return impl->obj_;
}

void PluckAnimation::on_draw(lv_event_t* e) {
    auto* self = static_cast<PluckAnimation*>(lv_event_get_user_data(e));
    if (!self || !self->obj_)
        return;
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer)
        return;

    lv_area_t area;
    lv_obj_get_coords(self->obj_, &area);
    const float w = static_cast<float>(lv_area_get_width(&area));
    const float h = static_cast<float>(lv_area_get_height(&area));
    if (w < 2.0f || h < 2.0f)
        return;

    // on_size_changed keeps the object at the ART_W:ART_H aspect, so fitting
    // by width alone is enough - no letterboxing to compute.
    const float fit = w / ART_W;
    const ProjCtx c{static_cast<float>(area.x1) + DESIGN_OX * fit,
                    static_cast<float>(area.y1) + DESIGN_OY * fit, fit * DESIGN_SC};

    const uint32_t phase = self->phase_ms_;
    const float k = pluck_deflection_at_ms(phase);

    const lv_color_t metal = theme_manager_get_color("elevated_bg");
    const lv_color_t metal_dk = lv_color_darken(metal, LV_OPA_20);
    const lv_color_t outline = theme_manager_get_color("border");
    const lv_color_t plastic = theme_manager_get_color("overlay_bg");
    const lv_color_t plastic_dk = lv_color_darken(plastic, LV_OPA_20);
    const lv_color_t belt = theme_manager_get_color("primary");
    const lv_color_t belt_lt = lv_color_lighten(belt, LV_OPA_30);
    const lv_color_t belt_dk = lv_color_darken(belt, LV_OPA_30);
    const lv_color_t mark = theme_manager_get_color("success");
    // The tensioner's adjuster is red on the real machine - a fixed hex via
    // theme_manager_parse_hex_color(), not a theme token, per the task brief.
    const lv_color_t red = theme_manager_parse_hex_color("#C84B4B");
    const lv_color_t red_dk = theme_manager_parse_hex_color("#8F3434");
    const lv_color_t finger_c = theme_manager_get_color("text_muted");
    const lv_color_t finger_lt = lv_color_lighten(finger_c, LV_OPA_20);

    draw_extrusion(layer, c, metal, metal_dk, outline);
    draw_inner_run(layer, c, belt_dk);
    draw_idler(layer, c, plastic, plastic_dk, outline, belt, belt_dk, belt_lt, red, red_dk);
    draw_outer_run(layer, c, k, belt, belt_dk, belt_lt, mark);

    if (pluck_frame_at_ms(phase) == 2 && std::fabs(k) > 0.08f) {
        draw_ring_hint(layer, c, k, mark);
    }

    Vec3 tip;
    Vec3 base;
    finger_pose(phase, k, tip, base);
    draw_finger(layer, c, tip, base, finger_c, finger_lt, outline);
}

void PluckAnimation::on_delete(lv_event_t* e) {
    auto* self = static_cast<PluckAnimation*>(lv_event_get_user_data(e));
    if (!self)
        return;
    if (self->obj_) {
        lv_anim_delete(self->obj_, anim_exec_cb);
    }
    delete self;
}

void PluckAnimation::on_size_changed(lv_event_t* e) {
    auto* self = static_cast<PluckAnimation*>(lv_event_get_user_data(e));
    if (!self || !self->obj_)
        return;
    const int32_t w = lv_obj_get_width(self->obj_);
    if (w <= 0)
        return;
    const auto target_h =
        static_cast<int32_t>(std::lround(static_cast<float>(w) * (ART_H / ART_W)));
    if (target_h > 0 && target_h != lv_obj_get_height(self->obj_)) {
        // Triggers another SIZE_CHANGED with the width unchanged, which
        // computes the same target_h and stops - converges in one extra call.
        lv_obj_set_height(self->obj_, target_h);
    }
}

void PluckAnimation::anim_exec_cb(void* var, int32_t value) {
    auto* obj = static_cast<lv_obj_t*>(var);
    auto* self = static_cast<PluckAnimation*>(lv_obj_get_user_data(obj));
    if (!self)
        return;
    self->phase_ms_ = static_cast<uint32_t>(value);
    lv_obj_invalidate(obj);
}

void* pluck_animation_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    return PluckAnimation::create(parent);
}

} // namespace

namespace helix {
namespace ui {

void register_pluck_animation_widget() {
    // No apply-time attrs are needed (see PluckAnimation::create), so
    // lv_xml_obj_apply is enough - same convention as belt_trace.
    lv_xml_register_widget("pluck_animation", pluck_animation_xml_create, lv_xml_obj_apply);
    spdlog::trace("[PluckAnimation] Widget registered with XML system");
}

} // namespace ui
} // namespace helix
