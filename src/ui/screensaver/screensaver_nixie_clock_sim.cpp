// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_nixie_clock_sim.h"

#include "screensaver_buffer.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_print_colors.h"
#include "screensaver_stroke_digits.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

// ---------- The tube's parts, in glyph units ----------

/// The glass and socket's inset from the card's sides.
constexpr float kInset = 0.05f;
/// Glass below the numerals before the socket starts: past the halo, so a fade never redraws
/// the socket and the AM/PM marker on it; the bottom disc and the stem sit in it.
constexpr float kGlassBelow = 0.3f;
/// The glass's crown: rounded shoulders on the straight cylinder, a flat top between them.
constexpr float kShoulder = 0.32f;
/// The sealed exhaust tip on the crown, where the tube was pumped out and closed: its top, how
/// far it stands, and the bulb, neck and base of the nub.
constexpr float kTipTop = 0.03f;
constexpr float kTipHeight = 0.17f;
constexpr float kTipBulb = 0.038f;
constexpr float kTipNeck = 0.022f;
constexpr float kTipBase = 0.075f;
/// Width of the glass's lit rim.
constexpr float kRimWidth = 0.035f;
/// The halo's reach beyond the wire, and how fast it falls off.
constexpr float kHaloReach = 0.2f;
constexpr float kHaloFalloff = 0.06f;
/// The halo's brightness next to the wire, against the wire's own.
constexpr float kHaloPeak = 0.42f;
/// The anode mesh: honeycomb cell size, wire width, and how far it reaches past the numerals.
constexpr float kMeshCell = 0.1f;
constexpr float kMeshWire = 0.012f;
constexpr float kMeshMargin = 0.09f;
/// The mica discs above and below the stack: their centres from the numeral box, and their
/// half thickness seen nearly edge-on.
constexpr float kTopDisc = 0.17f;
constexpr float kBottomDisc = 0.17f;
constexpr float kDiscHalf = 0.032f;
/// The posts standing on the top disc: height, offset from the centre, half width.
constexpr float kPostHeight = 0.12f;
constexpr float kPostOffset = 0.3f;
constexpr float kPostHalf = 0.014f;
/// The support rods down each side of the mesh, and the terminal pins above and below the stack.
constexpr float kRodHalf = 0.012f;
constexpr float kPinR = 0.03f;
constexpr float kPinGap = 0.07f;
/// The socket's rounded bottom corners.
constexpr float kSocketCorner = 0.08f;
/// The colon tube: its glass width, the crown below the digit tubes', and its two bulbs (half
/// width and half height, and their height in the numeral box).
constexpr float kColonGlassW = 0.4f;
constexpr float kColonCrownDrop = 0.14f;
constexpr float kColonBulbHalfW = 0.045f;
constexpr float kColonBulbHalfH = 0.085f;
constexpr float kColonRows[] = {0.34f, 0.66f};

constexpr Rgb kGlassInside{9, 8, 11};
constexpr Rgb kGlassRim{64, 60, 68};
constexpr Rgb kGlassStreak{46, 46, 54};
/// The glint on the tip's bulb.
constexpr Rgb kTipGlint{150, 150, 162};
constexpr Rgb kMesh{46, 42, 40};
/// The mesh at the cylinder's sides, where it turns away.
constexpr Rgb kMeshSide{20, 18, 18};
constexpr Rgb kMica{40, 38, 42};
constexpr Rgb kMicaEdge{92, 88, 94};
constexpr Rgb kMetal{82, 78, 80};
constexpr Rgb kLead{48, 44, 44};
/// The unlit wire numerals stacked behind the lit one.
constexpr Rgb kWire{54, 42, 34};
constexpr Rgb kSocketTop{58, 55, 60};
constexpr Rgb kSocketBottom{16, 16, 18};
constexpr Rgb kSocketEdge{96, 92, 98};
/// How far the core of a lit stroke runs from the ink toward white, per channel: the red and
/// green further than the blue, so it burns warm whatever the ink.
constexpr float kCoreLiftRg = 0.45f;
constexpr float kCoreLiftB = 0.2f;

int32_t px(float v, int32_t unit) {
    return static_cast<int32_t>(std::lround(v * static_cast<float>(unit)));
}

/// Glow for a point `d` px from a wire `half` px in half-width with a halo reaching `reach` px:
/// the wire's coverage plus a halo falling to 0 at `reach`.
float glow_at(float d, float half, float reach, float falloff) {
    const float core = edge_coverage(half - d);
    const float out = std::max(0.0f, d - half);
    const float tail = std::exp(-reach / falloff);
    const float halo =
        out >= reach ? 0.0f : kHaloPeak * (std::exp(-out / falloff) - tail) / (1.0f - tail);
    return std::min(1.0f, core + halo);
}

uint8_t saturating_add(uint8_t a, uint32_t b) {
    return static_cast<uint8_t>(std::min<uint32_t>(255, a + b));
}

/// `base` with `glow` added at `amount` (0 to 256).
Rgb add_glow(Rgb base, Rgb glow, uint32_t amount) {
    return {saturating_add(base.r, glow.r * amount >> 8),
            saturating_add(base.g, glow.g * amount >> 8),
            saturating_add(base.b, glow.b * amount >> 8)};
}

/// Card `i` of `face` in layout order: the main row, then the small row.
char face_char(const FlipFace& face, size_t i) {
    const size_t main = face.shape.card_count();
    if (i < main) {
        return face.cards[i].digit;
    }
    return i - main < kFlipMaxSubCards ? face.sub[i - main].digit : '\0';
}

bool shape_differs(const FlipFace& face, const FlipLayout& layout) {
    return face.shape != layout.shape || face.sub_shape != layout.sub_shape ||
           face.sub_colons != layout.sub_colons;
}

uint32_t to_amount(float share) {
    return static_cast<uint32_t>(std::lround(std::clamp(share, 0.0f, 1.0f) * 256.0f));
}

/// A tube's glass seen from the front: a straight cylinder from its crown down, the crown's
/// corners rounded into shoulders. inside() is the distance into the glass in px, negative
/// outside.
struct GlassOutline {
    float cx;
    float half;
    float crown;
    float shoulder;

    float inside(float fx, float fy) const {
        const float qx = std::fabs(fx - cx) - (half - shoulder);
        const float qy = crown + shoulder - fy;
        if (qx > 0.0f && qy > 0.0f) {
            return shoulder - std::hypot(qx, qy);
        }
        return std::min(half - std::fabs(fx - cx), fy - crown);
    }
};

/// The sealed exhaust tip on a crown: a nub flaring into the crown, narrowing to a neck, a small
/// bulb, and a point above it.
struct GlassTip {
    float cx;
    float top;
    float crown;
    float bulb_cy;
    float bulb_r;
    float neck;
    float base;

    GlassTip(float cx_, float top_, float crown_, float u, float scale)
        : cx(cx_), top(top_), crown(crown_) {
        bulb_r = std::max(1.2f, kTipBulb * u * scale);
        neck = std::max(0.7f, kTipNeck * u * scale);
        base = std::max(1.5f, kTipBase * u * scale);
        bulb_cy = top + 0.42f * (crown - top);
    }

    float inside(float fx, float fy) const {
        const float dx = std::fabs(fx - cx);
        const float overlap = 2.0f;
        float best = bulb_r - std::hypot(fx - cx, fy - bulb_cy);
        if (fy >= bulb_cy && fy <= crown + overlap) {
            const float t =
                std::clamp((fy - bulb_cy) / std::max(1.0f, crown - bulb_cy), 0.0f, 1.0f);
            const float hw = neck + (base - neck) * t * t;
            best = std::max(best, std::min({hw - dx, fy - bulb_cy, crown + overlap - fy}));
        }
        if (fy >= top && fy <= bulb_cy) {
            const float hw = 0.7f * bulb_r * (fy - top) / std::max(1.0f, bulb_cy - top);
            best = std::max(best, std::min(hw - dx, fy - top));
        }
        return best;
    }

    DirtyRect box() const {
        return {static_cast<int32_t>(std::floor(cx - base)) - 1,
                std::max(0, static_cast<int32_t>(std::floor(top)) - 1),
                static_cast<int32_t>(std::ceil(cx + base)), static_cast<int32_t>(std::ceil(crown))};
    }
};

/// Clear glass `inside` px in from its edge, its rim lit over `rim` px.
Rgb glass_at(float inside, float rim) {
    return mix_rgb(kGlassInside, kGlassRim, std::clamp(1.0f - inside / rim, 0.0f, 1.0f));
}

/// The tip's thin glass, with a glint on its bulb.
Rgb tip_at(const GlassTip& tip, float inside, float rim, float fx, float fy) {
    const Rgb t = glass_at(inside, std::max(1.0f, 0.5f * rim));
    const float gr = std::max(0.8f, 0.4f * tip.bulb_r);
    const float glint = edge_coverage(gr - std::hypot(fx - (tip.cx - 0.35f * tip.bulb_r),
                                                      fy - (tip.bulb_cy - 0.35f * tip.bulb_r)));
    return add_glow(t, kTipGlint, to_amount(glint));
}

/// The glass's highlights: a soft streak down the left and a fainter edge reflection on the
/// right, both fading toward the socket.
Rgb glass_highlights(Rgb c, const GlassOutline& g, float fx, float fy, float bottom) {
    const float along = 1.0f - 0.6f * std::clamp(fy / bottom, 0.0f, 1.0f);
    const float left = (fx - (g.cx - 0.55f * g.half)) / std::max(1.0f, 0.13f * g.half);
    const float right = (fx - (g.cx + 0.8f * g.half)) / std::max(0.8f, 0.06f * g.half);
    const float streak = (std::exp(-left * left) + 0.45f * std::exp(-right * right)) * along;
    return streak > 0.01f ? add_glow(c, kGlassStreak, to_amount(streak)) : c;
}

/// Distance in px from (x, y) to the nearest wire of a honeycomb of `cell` px cells.
float honeycomb_edge(float x, float y, float cell) {
    constexpr float kSy = 1.7320508f;
    const auto wrap = [](float v, float m) { return v - m * std::floor(v / m); };
    const float px = x / cell;
    const float py = y / cell;
    const float ax = wrap(px, 1.0f) - 0.5f;
    const float ay = wrap(py, kSy) - 0.5f * kSy;
    const float bx = wrap(px - 0.5f, 1.0f) - 0.5f;
    const float by = wrap(py - 0.5f * kSy, kSy) - 0.5f * kSy;
    const bool use_a = ax * ax + ay * ay < bx * bx + by * by;
    const float gx = std::fabs(use_a ? ax : bx);
    const float gy = std::fabs(use_a ? ay : by);
    const float hex = std::max(gx * 0.5f + gy * 0.8660254f, gx);
    return (0.5f - hex) * cell;
}

/// A dark metal socket over rows [`top`, `h`) between `x1` and `x2`, its top edge lit and its
/// bottom corners rounded by `corner` px.
void paint_socket(std::vector<Rgb>& base, int32_t w, int32_t h, int32_t x1, int32_t x2, int32_t top,
                  int32_t corner) {
    const auto height = static_cast<float>(std::max<int32_t>(1, h - top));
    for (int32_t y = top; y < h; y++) {
        const Rgb shade =
            y == top ? kSocketEdge
                     : mix_rgb(kSocketTop, kSocketBottom, static_cast<float>(y - top) / height);
        for (int32_t x = x1; x < x2; x++) {
            const int32_t ccx = std::clamp(x, x1 + corner, x2 - corner - 1);
            const int32_t ccy = std::min(y, h - corner - 1);
            float cov = 1.0f;
            if (ccx != x && ccy != y) {
                cov = edge_coverage(
                    static_cast<float>(corner) -
                    std::hypot(static_cast<float>(x - ccx), static_cast<float>(y - ccy)));
            }
            base[static_cast<size_t>(y) * w + x] = mix_rgb(Rgb{}, shade, cov);
        }
    }
}

} // namespace

NixieFadeAmounts nixie_fade_amounts(float p) {
    const float t = std::clamp(p, 0.0f, 1.0f);
    const float warm = std::min(1.0f, t / kNixieWarmShare);
    // The new cathode lights almost at once; the old one's afterglow dies away throughout.
    return {1.0f - t, 1.0f - (1.0f - warm) * (1.0f - warm)};
}

// ---------- NixieTubeArt ----------

void NixieTubeArt::build(const FlipRowMetrics& row, int32_t card_w, int32_t card_h) {
    clear();
    const CardGeometry& g = kNixieTubeGeometry;
    unit = std::max<int32_t>(1, row.unit);
    w = card_w;
    h = card_h;
    const auto u = static_cast<float>(unit);
    const int32_t gw = px(1.0f + g.stroke, unit);
    const int32_t gh = px(kStrokeDigitPathH + g.stroke, unit);
    halo = std::max<int32_t>(2, px(kHaloReach, unit));
    glow_x = row.pad_x - halo;
    glow_y = row.pad_y - halo;
    glow_w = gw + 2 * halo;
    glow_h = gh + 2 * halo;
    inset = std::max<int32_t>(1, px(kInset, unit));
    socket_y = std::min(h - 2, row.pad_y + gh + px(kGlassBelow, unit));

    base.assign(static_cast<size_t>(w) * h, Rgb{});
    const float half_stroke = g.stroke / 2.0f;
    const auto pad_x = static_cast<float>(row.pad_x);
    const auto pad_y = static_cast<float>(row.pad_y);
    const auto stack_bottom = static_cast<float>(row.pad_y + gh);

    const float cx = static_cast<float>(w) / 2.0f;
    const float tip_top = std::max(0.5f, kTipTop * u);
    const float crown = tip_top + std::max(3.0f, kTipHeight * u);
    const GlassOutline glass{cx, cx - static_cast<float>(inset), crown,
                             std::min(kShoulder * u, cx - static_cast<float>(inset))};
    const GlassTip nub(cx, tip_top, crown, u, 1.0f);
    tip = nub.box();
    const float rim = std::max(1.5f, kRimWidth * u);

    // The scaffolding: the mesh cylinder between the discs, its rods, posts and pins.
    const float margin = kMeshMargin * u;
    const float mesh_x1 = pad_x - margin;
    const float mesh_x2 = pad_x + static_cast<float>(gw) + margin;
    const float mesh_r = (mesh_x2 - mesh_x1) / 2.0f;
    const float top_disc = pad_y - kTopDisc * u;
    const float bottom_disc = stack_bottom + kBottomDisc * u;
    const float disc_half = std::max(1.0f, kDiscHalf * u);
    const float disc_a = glass.half * 0.9f;
    const float cell = std::max(3.0f, kMeshCell * u);
    const float mesh_wire = std::max(0.5f, kMeshWire * u);
    const float rod = std::max(0.6f, kRodHalf * u);
    const float post = std::max(0.6f, kPostHalf * u);
    const float pin = std::max(1.0f, kPinR * u);
    const float pin_gap = kPinGap * u;

    for (int32_t y = 0; y < socket_y; y++) {
        const float fy = static_cast<float>(y) + 0.5f;
        for (int32_t x = 0; x < w; x++) {
            const float fx = static_cast<float>(x) + 0.5f;
            const float body = glass.inside(fx, fy);
            const float nub_in = nub.inside(fx, fy);
            const float cov = edge_coverage(std::max(body, nub_in));
            if (cov <= 0.0f) {
                continue;
            }
            const size_t at = static_cast<size_t>(y) * w + x;
            if (nub_in > body) {
                base[at] = mix_rgb(Rgb{}, tip_at(nub, nub_in, rim, fx, fy), cov);
                continue;
            }
            Rgb c = glass_at(body, rim);
            const float dx = fx - cx;
            // The anode mesh, wrapped round the stack: cells narrow and darken toward the sides.
            if (fx >= mesh_x1 && fx <= mesh_x2 && fy >= top_disc && fy <= bottom_disc) {
                const float side = std::clamp(dx / mesh_r, -1.0f, 1.0f);
                const float around = mesh_r * std::asin(side);
                const float wire = edge_coverage(mesh_wire - honeycomb_edge(cx + around, fy, cell));
                c = mix_rgb(c, mix_rgb(kMesh, kMeshSide, side * side), wire);
            }
            // The rods down each side of the mesh, from disc to disc.
            if (fy >= top_disc && fy <= bottom_disc) {
                const float d = std::min(std::fabs(fx - mesh_x1), std::fabs(fx - mesh_x2));
                c = mix_rgb(c, kMetal, edge_coverage(rod - d));
            }
            // The posts standing on the top disc.
            if (fy >= top_disc - kPostHeight * u && fy <= top_disc) {
                const float d = std::fabs(std::fabs(dx) - kPostOffset * u);
                c = mix_rgb(c, kMetal, edge_coverage(post - d));
            }
            // The leads from the bottom disc down through the stem.
            if (fy >= bottom_disc && fx >= mesh_x1 && fx <= mesh_x2) {
                const float pitch = (mesh_x2 - mesh_x1) / 6.0f;
                const float lead = std::fabs(std::fmod(fx - mesh_x1, pitch) - pitch / 2.0f);
                c = mix_rgb(c, kLead, edge_coverage(rod - lead));
            }
            // The two mica discs, nearly edge-on: thin plates with a lit edge.
            for (const float disc : {top_disc, bottom_disc}) {
                const float e = std::hypot(dx / disc_a, (fy - disc) / disc_half);
                if (e < 1.2f) {
                    const float edge = edge_coverage((1.0f - e) * disc_half);
                    c = mix_rgb(c, mix_rgb(kMica, kMicaEdge, std::clamp(e, 0.0f, 1.0f)), edge);
                }
            }
            // The terminal pins above and below the stack.
            for (const float py : {pad_y - pin_gap, stack_bottom + pin_gap}) {
                c = mix_rgb(c, kMetal, edge_coverage(pin - std::hypot(dx, fy - py)));
            }
            if (x >= row.pad_x && x < row.pad_x + gw && y >= row.pad_y && y < row.pad_y + gh) {
                const float gx = (fx - pad_x) / u - half_stroke;
                const float gy = (fy - pad_y) / u - half_stroke;
                float nearest = 1e9f;
                for (int d = 0; d < 10; d++) {
                    nearest = std::min(nearest, stroke_digit_distance(d, gx, gy));
                }
                c = mix_rgb(c, kWire, edge_coverage((half_stroke - nearest) * u));
            }
            c = glass_highlights(c, glass, fx, fy, static_cast<float>(socket_y));
            base[at] = mix_rgb(Rgb{}, c, cov);
        }
    }
    paint_socket(base, w, h, inset, w - inset, socket_y,
                 std::max<int32_t>(1, px(kSocketCorner, unit)));

    for (char d = '0'; d <= '9'; d++) {
        ensure(d);
    }
}

void NixieTubeArt::ensure(char c) {
    const int i = stroke_glyph_index(c);
    if (i < 0 || glow_w <= 0 || !glow[static_cast<size_t>(i)].empty()) {
        return;
    }
    std::vector<uint8_t>& map = glow[static_cast<size_t>(i)];
    map.assign(static_cast<size_t>(glow_w) * glow_h, 0);
    const auto u = static_cast<float>(unit);
    const float half_stroke = kNixieTubeGeometry.stroke / 2.0f;
    const float half_px = half_stroke * u;
    const auto reach = static_cast<float>(halo);
    const float falloff = std::max(1.0f, kHaloFalloff * u);
    for (int32_t y = 0; y < glow_h; y++) {
        const float gy = (static_cast<float>(y - halo) + 0.5f) / u - half_stroke;
        for (int32_t x = 0; x < glow_w; x++) {
            const float gx = (static_cast<float>(x - halo) + 0.5f) / u - half_stroke;
            const float d = stroke_glyph_distance(c, gx, gy) * u;
            map[static_cast<size_t>(y) * glow_w + x] =
                unit_to_u8(glow_at(d, half_px, reach, falloff));
        }
    }
}

const uint8_t* NixieTubeArt::glow_of(char c) const {
    const int i = stroke_glyph_index(c);
    if (i < 0 || glow[static_cast<size_t>(i)].empty()) {
        return nullptr;
    }
    return glow[static_cast<size_t>(i)].data();
}

size_t NixieTubeArt::heap_bytes() const {
    size_t total = buffer_bytes(base);
    for (const std::vector<uint8_t>& g : glow) {
        total += buffer_bytes(g);
    }
    return total;
}

void NixieTubeArt::clear() {
    release_buffer(base);
    for (std::vector<uint8_t>& g : glow) {
        release_buffer(g);
    }
    unit = 0;
    w = 0;
    h = 0;
}

// ---------- NixieColonArt ----------

void NixieColonArt::build(const FlipRowMetrics& row, int32_t gap_w, int32_t gap_h,
                          int32_t socket_y) {
    clear();
    unit = std::max<int32_t>(1, row.unit);
    w = gap_w;
    h = gap_h;
    const auto u = static_cast<float>(unit);
    base.assign(static_cast<size_t>(w) * h, Rgb{});
    glow.assign(static_cast<size_t>(w) * h, 0);
    const float cx = static_cast<float>(w) / 2.0f;
    const float half = std::min(cx - 1.0f, kColonGlassW * u / 2.0f);
    const auto pad_y = static_cast<float>(row.pad_y);
    const float gh = (kStrokeDigitPathH + kNixieTubeGeometry.stroke) * u;
    const float crown = pad_y - (kTopDisc - kColonCrownDrop) * u;
    const float tip_top = std::max(0.5f, crown - 0.8f * kTipHeight * u);
    const GlassOutline glass{cx, half, crown, 0.8f * half};
    const GlassTip nub(cx, tip_top, crown, u, 0.8f);
    tip = nub.box();
    const float rim = std::max(1.2f, kRimWidth * u);
    const float bulb_w = std::max(1.0f, kColonBulbHalfW * u);
    const float bulb_h = std::max(1.5f, kColonBulbHalfH * u);
    const float reach = std::max(2.0f, kHaloReach * u * 0.8f);
    const float falloff = std::max(0.8f, kHaloFalloff * u);
    const float lead = std::max(0.6f, kRodHalf * u);

    for (int32_t y = 0; y < socket_y; y++) {
        const float fy = static_cast<float>(y) + 0.5f;
        for (int32_t x = 0; x < w; x++) {
            const float fx = static_cast<float>(x) + 0.5f;
            const size_t at = static_cast<size_t>(y) * w + x;
            // The bulbs' glow, elongated, as distance from each bulb's edge.
            float d = 1e9f;
            for (const float share : kColonRows) {
                const float by = pad_y + gh * share;
                const float e = std::hypot((fx - cx) / bulb_w, (fy - by) / bulb_h);
                d = std::min(d, (e - 1.0f) * bulb_w);
            }
            glow[at] = unit_to_u8(glow_at(d + 1.0f, 1.0f, reach, falloff));
            const float body = glass.inside(fx, fy);
            const float nub_in = nub.inside(fx, fy);
            const float cov = edge_coverage(std::max(body, nub_in));
            if (cov <= 0.0f) {
                continue;
            }
            if (nub_in > body) {
                base[at] = mix_rgb(Rgb{}, tip_at(nub, nub_in, rim, fx, fy), cov);
                continue;
            }
            Rgb c = glass_at(body, rim);
            // Two leads from the lower bulb down to the socket.
            const float low = pad_y + gh * kColonRows[1];
            if (fy > low) {
                const float dl = std::fabs(std::fabs(fx - cx) - 0.4f * half);
                c = mix_rgb(c, kLead, edge_coverage(lead - dl));
            }
            c = glass_highlights(c, glass, fx, fy, static_cast<float>(socket_y));
            base[at] = mix_rgb(Rgb{}, c, cov);
        }
    }
    const auto socket_x1 = static_cast<int32_t>(std::floor(cx - half - 1.0f));
    const auto socket_x2 = static_cast<int32_t>(std::ceil(cx + half + 1.0f));
    paint_socket(base, w, h, std::max(0, socket_x1), std::min(w, socket_x2), socket_y,
                 std::max<int32_t>(1, px(kSocketCorner * 0.6f, unit)));
}

size_t NixieColonArt::heap_bytes() const {
    return buffer_bytes(base) + buffer_bytes(glow);
}

void NixieColonArt::clear() {
    release_buffer(base);
    release_buffer(glow);
    unit = 0;
    w = 0;
    h = 0;
}

// ---------- NixieClockSim ----------

void NixieClockSim::init(FrameTarget& frame, size_t level, const FlipFace& face) {
    level_ = std::min(level, kNixieClockLevelCount - 1);
    frame_w_ = static_cast<int32_t>(frame.w);
    frame_h_ = static_cast<int32_t>(frame.h);
    drift_.reset();
    drift_shape_ = face.shape;
    drift_sub_shape_ = face.sub_shape;
    drift_.set_range(flip_centred_bounds(frame_w_, frame_h_, drift_shape_, drift_sub_shape_,
                                         face.sub_colons, kNixieTubeGeometry),
                     frame_w_, frame_h_);
    since_cycle_ms_ = 0;
    cycle_ms_ = kNixieCycleMs;
    PixelWriter(frame).fill(kBackground);
    ink_ = print_state_color(face.mode, kNeon);
    build_ramp();
    layout_ = layout_flip_cards(frame_w_, frame_h_, face.shape, face.sub_shape, 0, 0,
                                face.sub_colons, kNixieTubeGeometry);
    build_art(face);
    for (size_t i = 0; i < anims_.size(); i++) {
        const char c = face_char(face, i);
        anims_[i] = {c, c, kNixieFadeMs};
    }
    for (size_t i = 0; i < tube_count(); i++) {
        draw_tube(frame, i);
    }
    draw_colons(frame);
}

void NixieClockSim::build_ramp() {
    for (size_t v = 0; v < ramp_.size(); v++) {
        const float t = static_cast<float>(v) / 255.0f;
        // The halo is the ink, dim; the wire's core runs hotter, toward white.
        const Rgb glow = scale_rgb_by(ink_, std::min(1.0f, t * 1.5f));
        const float hot = std::clamp((t - 0.6f) / 0.4f, 0.0f, 1.0f);
        const auto lift = [](uint8_t c, float k) {
            return to_u8(static_cast<float>(c) + (255.0f - static_cast<float>(c)) * k);
        };
        const Rgb core{lift(ink_.r, kCoreLiftRg), lift(ink_.g, kCoreLiftRg),
                       lift(ink_.b, kCoreLiftB)};
        ramp_[v] = mix_rgb(glow, core, hot * hot);
    }
}

void NixieClockSim::build_art(const FlipFace& face) {
    const DirtyRect& tube = layout_.cards[0];
    const int32_t tw = tube.x2 - tube.x1 + 1;
    const int32_t th = tube.y2 - tube.y1 + 1;
    if (art_.unit != layout_.main.unit || art_.w != tw || art_.h != th) {
        art_.build(layout_.main, tw, th);
    }
    if (layout_.sub_count > 0) {
        const DirtyRect& small = layout_.cards[layout_.card_count];
        const int32_t sw = small.x2 - small.x1 + 1;
        const int32_t sh = small.y2 - small.y1 + 1;
        if (sub_art_.unit != layout_.small.unit || sub_art_.w != sw || sub_art_.h != sh) {
            sub_art_.build(layout_.small, sw, sh);
        }
    }
    for (size_t c = 0; c < layout_.colon_count; c++) {
        const bool main = c < layout_.main_colon_count;
        const DirtyRect& gap = layout_.colons[c];
        const int32_t gw = gap.x2 - gap.x1 + 1;
        const int32_t gh = gap.y2 - gap.y1 + 1;
        NixieColonArt& colon = main ? colon_art_ : sub_colon_art_;
        const NixieTubeArt& beside = main ? art_ : sub_art_;
        if (colon.unit != (main ? layout_.main : layout_.small).unit || colon.w != gw ||
            colon.h != gh) {
            colon.build(main ? layout_.main : layout_.small, gw, gh, beside.socket_y);
        }
    }
    for (size_t i = 0; i < layout_.card_count; i++) {
        art_.ensure(face.cards[i].digit);
    }
    for (size_t i = 0; i < layout_.sub_count; i++) {
        sub_art_.ensure(face.sub[i].digit);
    }
}

void NixieClockSim::redraw_tube(FrameTarget& frame, size_t i, std::vector<DirtyRect>& dirty) const {
    if (i >= tube_count()) {
        return;
    }
    draw_tube(frame, i);
    dirty.push_back(layout_.cards[i]);
}

void NixieClockSim::request_level(size_t level) {
    level_ = std::min(level, kNixieClockLevelCount - 1);
}

DirtyRect NixieClockSim::meridiem_box() const {
    if (layout_.card_count == 0 || art_.w <= 0) {
        return {};
    }
    const DirtyRect& tube = layout_.cards[0];
    const int32_t pad = std::max<int32_t>(1, art_.inset * 2);
    return {tube.x1 + pad, tube.y1 + art_.socket_y + 2, tube.x2 - pad, tube.y2 - 1};
}

DirtyRect NixieClockSim::tip_box(size_t i) const {
    if (i >= tube_count()) {
        return {};
    }
    const NixieTubeArt& art = art_of(i);
    const DirtyRect& tube = layout_.cards[i];
    return {tube.x1 + art.tip.x1, tube.y1 + art.tip.y1, tube.x1 + art.tip.x2, tube.y1 + art.tip.y2};
}

DirtyRect NixieClockSim::colon_tip_box(size_t c) const {
    if (c >= layout_.colon_count) {
        return {};
    }
    const NixieColonArt& art = c < layout_.main_colon_count ? colon_art_ : sub_colon_art_;
    const DirtyRect& gap = layout_.colons[c];
    return {gap.x1 + art.tip.x1, gap.y1 + art.tip.y1, gap.x1 + art.tip.x2, gap.y1 + art.tip.y2};
}

DirtyRect NixieClockSim::glow_box(size_t i) const {
    if (i >= tube_count()) {
        return {};
    }
    const NixieTubeArt& art = art_of(i);
    const DirtyRect& tube = layout_.cards[i];
    const int32_t x1 = tube.x1 + std::max<int32_t>(0, art.glow_x);
    const int32_t y1 = tube.y1 + std::max<int32_t>(0, art.glow_y);
    return DirtyRect{x1, y1, std::min(tube.x2, tube.x1 + art.glow_x + art.glow_w - 1),
                     std::min(tube.y2, tube.y1 + art.glow_y + art.glow_h - 1)}
        .clipped(frame_w_, frame_h_);
}

bool NixieClockSim::fading() const {
    for (size_t i = 0; i < tube_count(); i++) {
        if (anims_[i].elapsed_ms < kNixieFadeMs) {
            return true;
        }
    }
    return false;
}

float NixieClockSim::fade_progress(size_t i) const {
    if (i >= anims_.size()) {
        return 1.0f;
    }
    return static_cast<float>(std::min(anims_[i].elapsed_ms, kNixieFadeMs)) /
           static_cast<float>(kNixieFadeMs);
}

char NixieClockSim::lit(size_t i) const {
    return i < anims_.size() ? anims_[i].to : '\0';
}

size_t NixieClockSim::heap_bytes() const {
    return art_.heap_bytes() + sub_art_.heap_bytes() + colon_art_.heap_bytes() +
           sub_colon_art_.heap_bytes();
}

void NixieClockSim::clear() {
    art_.clear();
    sub_art_.clear();
    colon_art_.clear();
    sub_colon_art_.clear();
    layout_ = {};
}

bool NixieClockSim::step_cycle(uint32_t dt_ms, const FlipFace& face) {
    if (!kNixieClockLevels[level_].fade || face.mode != ClockMode::CLOCK) {
        const bool was = cycling();
        cycle_ms_ = kNixieCycleMs;
        since_cycle_ms_ = 0;
        return was;
    }
    if (cycling()) {
        cycle_ms_ = std::min(cycle_ms_ + dt_ms, kNixieCycleMs);
        return true;
    }
    since_cycle_ms_ += dt_ms;
    if (since_cycle_ms_ >= kNixieCycleEveryMs && !fading()) {
        since_cycle_ms_ = 0;
        cycle_ms_ = 0;
        return true;
    }
    return false;
}

void NixieClockSim::step(uint32_t dt_ms, FrameTarget& frame, const FlipFace& face,
                         std::vector<DirtyRect>& dirty) {
    dirty.clear();
    const bool fade = kNixieClockLevels[level_].fade;

    if (face.shape != drift_shape_ || face.sub_shape != drift_sub_shape_) {
        drift_shape_ = face.shape;
        drift_sub_shape_ = face.sub_shape;
        drift_.set_range(flip_centred_bounds(frame_w_, frame_h_, drift_shape_, drift_sub_shape_,
                                             face.sub_colons, kNixieTubeGeometry),
                         frame_w_, frame_h_);
    }
    const bool moved = drift_.step(dt_ms);
    const Rgb ink = print_state_color(face.mode, kNeon);

    if (shape_differs(face, layout_) || moved || !(ink == ink_)) {
        // A new layout shows the face as it stands: a running fade or cycle ends here.
        cycle_ms_ = kNixieCycleMs;
        for (size_t i = 0; i < anims_.size(); i++) {
            const char c = face_char(face, i);
            anims_[i] = {c, c, kNixieFadeMs};
        }
        if (!(ink == ink_)) {
            ink_ = ink;
            build_ramp();
        }
        relayout(frame, face, dirty);
        return;
    }

    const bool cycling_now = step_cycle(dt_ms, face) && cycling();
    for (size_t i = 0; i < tube_count(); i++) {
        TubeAnim& anim = anims_[i];
        char want = face_char(face, i);
        if (cycling_now && i < layout_.card_count) {
            const uint32_t turn = cycle_ms_ / kNixieCycleStepMs + static_cast<uint32_t>(i);
            want = static_cast<char>('0' + turn % 10);
        }
        bool redraw = false;
        if (want != anim.to) {
            (i < layout_.card_count ? art_ : sub_art_).ensure(want);
            anim.from = anim.to;
            anim.to = want;
            // The cycle flashes each numeral at once; a real change fades.
            anim.elapsed_ms = fade && !cycling_now ? 0 : kNixieFadeMs;
            redraw = true;
        } else if (anim.elapsed_ms < kNixieFadeMs) {
            anim.elapsed_ms = fade ? std::min(anim.elapsed_ms + dt_ms, kNixieFadeMs) : kNixieFadeMs;
            redraw = true;
        }
        if (redraw) {
            draw_glow(frame, i);
            const DirtyRect box = glow_box(i);
            if (!box.empty()) {
                dirty.push_back(box);
            }
        }
    }
}

void NixieClockSim::relayout(FrameTarget& frame, const FlipFace& face,
                             std::vector<DirtyRect>& dirty) {
    DirtyRect area = layout_.bounds();
    PixelWriter(frame).fill_rect(area, kBackground);
    layout_ = layout_flip_cards(frame_w_, frame_h_, face.shape, face.sub_shape, drift_.dx,
                                drift_.dy, face.sub_colons, kNixieTubeGeometry);
    build_art(face);
    for (size_t i = 0; i < tube_count(); i++) {
        draw_tube(frame, i);
    }
    draw_colons(frame);
    area.add(layout_.bounds());
    area = area.clipped(frame_w_, frame_h_);
    if (!area.empty()) {
        dirty.push_back(area);
    }
}

void NixieClockSim::draw_tube(FrameTarget& frame, size_t i) const {
    const NixieTubeArt& art = art_of(i);
    const DirtyRect& tube = layout_.cards[i];
    if (art.base.empty()) {
        return;
    }
    PixelWriter writer(frame);
    const DirtyRect glow = glow_box(i);
    for (int32_t y = 0; y < art.h; y++) {
        for (int32_t x = 0; x < art.w; x++) {
            const int32_t fx = tube.x1 + x;
            const int32_t fy = tube.y1 + y;
            if (fx >= glow.x1 && fx <= glow.x2 && fy >= glow.y1 && fy <= glow.y2) {
                continue;
            }
            if (writer.contains(fx, fy)) {
                writer.put_dithered(fx, fy, art.base[static_cast<size_t>(y) * art.w + x]);
            }
        }
    }
    draw_glow(frame, i);
}

void NixieClockSim::draw_glow(FrameTarget& frame, size_t i) const {
    const NixieTubeArt& art = art_of(i);
    const DirtyRect box = glow_box(i);
    if (art.base.empty() || box.empty()) {
        return;
    }
    const DirtyRect& tube = layout_.cards[i];
    const TubeAnim& anim = anims_[i];
    const float p = fade_progress(i);
    const uint8_t* to = art.glow_of(anim.to);
    const uint8_t* from = p < 1.0f && anim.from != anim.to ? art.glow_of(anim.from) : nullptr;
    const NixieFadeAmounts amounts = nixie_fade_amounts(p);
    const uint32_t to_amount_v = to_amount(amounts.to);
    const uint32_t from_amount_v = to_amount(amounts.from);
    PixelWriter writer(frame);
    for (int32_t fy = box.y1; fy <= box.y2; fy++) {
        const int32_t ty = fy - tube.y1;
        const int32_t gy = ty - art.glow_y;
        for (int32_t fx = box.x1; fx <= box.x2; fx++) {
            const int32_t tx = fx - tube.x1;
            const int32_t gx = tx - art.glow_x;
            Rgb c = art.base[static_cast<size_t>(ty) * art.w + tx];
            const size_t g = static_cast<size_t>(gy) * art.glow_w + gx;
            if (to != nullptr && to[g] != 0) {
                c = add_glow(c, ramp_[to[g]], to_amount_v);
            }
            if (from != nullptr && from[g] != 0) {
                c = add_glow(c, ramp_[from[g]], from_amount_v);
            }
            writer.put_dithered(fx, fy, c);
        }
    }
}

void NixieClockSim::draw_colons(FrameTarget& frame) const {
    PixelWriter writer(frame);
    for (size_t c = 0; c < layout_.colon_count; c++) {
        const NixieColonArt& art = c < layout_.main_colon_count ? colon_art_ : sub_colon_art_;
        const DirtyRect& gap = layout_.colons[c];
        if (art.base.empty()) {
            continue;
        }
        for (int32_t y = 0; y < art.h; y++) {
            for (int32_t x = 0; x < art.w; x++) {
                if (!writer.contains(gap.x1 + x, gap.y1 + y)) {
                    continue;
                }
                const size_t at = static_cast<size_t>(y) * art.w + x;
                const uint8_t g = art.glow[at];
                const Rgb c0 = art.base[at];
                writer.put_dithered(gap.x1 + x, gap.y1 + y,
                                    g == 0 ? c0 : add_glow(c0, ramp_[g], 256));
            }
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
