// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The current-spool highlight: an accent glow grown from the spool's own
// silhouette. Pixels are read straight from the canvas draw buffer; a "glow
// pixel" is one the highlighted render paints where the plain render is
// transparent.

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "filament_tube_stroker.h"
#include "lvgl/src/core/lv_obj_draw_private.h"

#include <cstdlib>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

constexpr int32_t SPOOL = 60;

struct Pixels {
    int32_t w = 0;
    int32_t h = 0;
    std::vector<lv_color32_t> px;
    const lv_color32_t& at(int32_t x, int32_t y) const {
        return px[y * w + x];
    }
};

/// A canvas's draw buffer, or the draw buffer an image (the glow layer) shows.
const lv_draw_buf_t* buf_of(lv_obj_t* obj) {
    if (lv_obj_check_type(obj, &lv_canvas_class))
        return lv_canvas_get_draw_buf(obj);
    return static_cast<const lv_draw_buf_t*>(lv_image_get_src(obj));
}

Pixels snapshot(lv_obj_t* canvas) {
    const lv_draw_buf_t* buf = buf_of(canvas);
    REQUIRE(buf != nullptr);
    Pixels p;
    p.w = buf->header.w;
    p.h = buf->header.h;
    p.px.resize(static_cast<size_t>(p.w * p.h));
    for (int32_t y = 0; y < p.h; y++) {
        const auto* row = reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride);
        for (int32_t x = 0; x < p.w; x++)
            p.px[y * p.w + x] = row[x];
    }
    return p;
}

bool same(const Pixels& a, const Pixels& b) {
    if (a.w != b.w || a.h != b.h)
        return false;
    for (size_t i = 0; i < a.px.size(); i++) {
        const auto &p = a.px[i], &q = b.px[i];
        if (p.red != q.red || p.green != q.green || p.blue != q.blue || p.alpha != q.alpha)
            return false;
    }
    return true;
}

/// Chebyshev distance from every glow pixel to the plain silhouette, with the
/// plain render sitting @p m px in from the highlighted buffer's corner.
struct GlowReach {
    int count = 0;
    int max_dist = 0;
    lv_color32_t farthest{};
};

GlowReach glow_reach(const Pixels& plain, const Pixels& lit, int32_t m) {
    std::vector<std::pair<int32_t, int32_t>> solid;
    for (int32_t y = 0; y < plain.h; y++)
        for (int32_t x = 0; x < plain.w; x++)
            if (plain.at(x, y).alpha > 0)
                solid.emplace_back(x + m, y + m);
    REQUIRE_FALSE(solid.empty());

    GlowReach r;
    for (int32_t y = 0; y < lit.h; y++) {
        for (int32_t x = 0; x < lit.w; x++) {
            const int32_t px = x - m, py = y - m;
            const bool inside = px >= 0 && py >= 0 && px < plain.w && py < plain.h;
            if ((inside && plain.at(px, py).alpha > 0) || lit.at(x, y).alpha == 0)
                continue;
            int best = 1 << 30;
            for (auto [sx, sy] : solid)
                best = std::min(best, std::max(std::abs(sx - x), std::abs(sy - y)));
            r.count++;
            if (best > r.max_dist) {
                r.max_dist = best;
                r.farthest = lit.at(x, y);
            }
        }
    }
    return r;
}

lv_obj_t* make_canvas(lv_obj_t* parent) {
    ui_spool_canvas_invalidate_cache();
    lv_obj_t* c = ui_spool_canvas_create(parent, SPOOL);
    REQUIRE(c != nullptr);
    ui_spool_canvas_set_color(c, lv_color_hex(0xDE5923));
    ui_spool_canvas_set_fill_level(c, 0.8f);
    return c;
}

} // namespace

/// The glow layer for @p c, painted; its buffer sits @p m px outside the spool.
lv_obj_t* make_glow(lv_obj_t* c, bool simple, int32_t* m) {
    lv_obj_t* g = helix::ui::spool_glow_create(lv_obj_get_parent(c));
    REQUIRE(g != nullptr);
    helix::ui::spool_glow_paint(g, c, SPOOL, helix::ui::tube_accent(), simple);
    const Pixels px = snapshot(g);
    REQUIRE(px.w == px.h);
    REQUIRE(px.w >= SPOOL);
    REQUIRE((px.w - SPOOL) % 2 == 0);
    *m = (px.w - SPOOL) / 2;
    return g;
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: simple glow is a tight outline, no tail",
                 "[spool_canvas][highlight]") {
    lv_obj_t* c = make_canvas(test_screen());
    const Pixels plain = snapshot(c);
    int32_t m = 0;
    lv_obj_t* g = make_glow(c, /*simple=*/true, &m);

    const GlowReach r = glow_reach(plain, snapshot(g), m);
    CHECK(r.count > 20);
    CHECK(r.max_dist <= 2);
    // The spool's own buffer is untouched.
    CHECK(same(plain, snapshot(c)));

    lv_obj_delete(g);
    lv_obj_delete(c);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: capable glow reaches past 3px, inside margin",
                 "[spool_canvas][highlight]") {
    lv_obj_t* c = make_canvas(test_screen());
    const Pixels plain = snapshot(c);
    int32_t m = 0;
    lv_obj_t* g = make_glow(c, /*simple=*/false, &m);
    const Pixels lit = snapshot(g);

    // The layer's object keeps the spool's size; the buffer overhangs it.
    lv_obj_update_layout(g);
    CHECK(lv_obj_get_width(g) == SPOOL);
    CHECK(lv_obj_get_height(g) == SPOOL);
    CHECK(plain.w == SPOOL);

    const GlowReach r = glow_reach(plain, lit, m);
    CHECK(r.max_dist > 3);
    CHECK(r.max_dist <= m);

    // The far halo is the accent itself.
    const lv_color_t accent = helix::ui::tube_accent();
    CHECK(std::abs(r.farthest.red - accent.red) <= 2);
    CHECK(std::abs(r.farthest.green - accent.green) <= 2);
    CHECK(std::abs(r.farthest.blue - accent.blue) <= 2);

    // Nothing clips: the outermost ring of the buffer is (near) transparent.
    int edge_max = 0;
    for (int32_t i = 0; i < lit.w; i++) {
        edge_max = std::max({edge_max, static_cast<int>(lit.at(i, 0).alpha),
                             static_cast<int>(lit.at(i, lit.h - 1).alpha),
                             static_cast<int>(lit.at(0, i).alpha),
                             static_cast<int>(lit.at(lit.w - 1, i).alpha)});
    }
    CHECK(edge_max <= 4);

    lv_obj_delete(g);
    lv_obj_delete(c);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: glow follows the shape, not the color",
                 "[spool_canvas][highlight]") {
    for (bool simple : {true, false}) {
        CAPTURE(simple);
        lv_obj_t* c = make_canvas(test_screen());
        int32_t m = 0;
        lv_obj_t* g = make_glow(c, simple, &m);
        const Pixels full = snapshot(g);

        // A new color is the same silhouette: same glow.
        ui_spool_canvas_set_color(c, lv_color_hex(0x2BD3D1));
        helix::ui::spool_glow_paint(g, c, SPOOL, helix::ui::tube_accent(), simple);
        CHECK(same(full, snapshot(g)));

        // A thinner wind is a different silhouette. The 2px outline closes
        // the notch between the flanges either way, so only the halo shows it.
        ui_spool_canvas_set_fill_level(c, 0.1f);
        helix::ui::spool_glow_paint(g, c, SPOOL, helix::ui::tube_accent(), simple);
        if (!simple)
            CHECK_FALSE(same(full, snapshot(g)));

        // Back to full: the cached glow, identical.
        ui_spool_canvas_set_fill_level(c, 0.8f);
        helix::ui::spool_glow_paint(g, c, SPOOL, helix::ui::tube_accent(), simple);
        CHECK(same(full, snapshot(g)));

        lv_obj_delete(g);
        lv_obj_delete(c);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: layers with one shape share one glow image",
                 "[spool_canvas][highlight]") {
    lv_obj_t* c = make_canvas(test_screen());
    int32_t m = 0;
    lv_obj_t* a = make_glow(c, /*simple=*/false, &m);
    lv_obj_t* b = make_glow(c, /*simple=*/false, &m);
    CHECK(buf_of(a) == buf_of(b));

    helix::ui::spool_glow_clear(a);
    CHECK(lv_image_get_src(a) == nullptr);
    CHECK(lv_obj_get_ext_draw_size(a) == 0);
    CHECK(buf_of(b) != nullptr); // the other layer keeps its reference

    lv_obj_delete(a);
    lv_obj_delete(b);
    lv_obj_delete(c);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: no glow before the spool has rendered",
                 "[spool_canvas][highlight]") {
    lv_obj_t* c = make_canvas(test_screen());
    const Pixels plain = snapshot(c);
    // Invalidation leaves the spool unrendered with its old pixels in place.
    ui_spool_canvas_invalidate_cache();

    lv_obj_t* g = helix::ui::spool_glow_create(test_screen());
    helix::ui::spool_glow_paint(g, c, SPOOL, helix::ui::tube_accent(), /*simple=*/false);
    CHECK(lv_image_get_src(g) == nullptr);

    // The first render paints the bound glow.
    ui_spool_canvas_redraw(c);
    REQUIRE(lv_image_get_src(g) != nullptr);
    const Pixels lit = snapshot(g);
    const int32_t m = (lit.w - SPOOL) / 2;
    const GlowReach r = glow_reach(plain, lit, m);
    CHECK(r.max_dist > 3);
    CHECK(r.max_dist <= m);

    lv_obj_delete(g);
    lv_obj_delete(c);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas: face-on spools share one glow whatever their fill",
                 "[spool_canvas][highlight]") {
    // Face-on, the flange always hides the filament's edge, so the outline is
    // the same disc at every fill.
    lv_obj_t* full = make_canvas(test_screen());
    helix::ui::spool_canvas_set_view(full, helix::ui::SpoolView::Front);
    lv_obj_t* low = ui_spool_canvas_create(test_screen(), SPOOL);
    REQUIRE(low != nullptr);
    helix::ui::spool_canvas_set_view(low, helix::ui::SpoolView::Front);
    ui_spool_canvas_set_color(low, lv_color_hex(0xDE5923));
    ui_spool_canvas_set_fill_level(low, 0.2f);

    int32_t m = 0;
    lv_obj_t* a = make_glow(full, /*simple=*/false, &m);
    lv_obj_t* b = make_glow(low, /*simple=*/false, &m);
    CHECK(buf_of(a) == buf_of(b));

    lv_obj_delete(a);
    lv_obj_delete(b);
    lv_obj_delete(low);
    lv_obj_delete(full);
}
