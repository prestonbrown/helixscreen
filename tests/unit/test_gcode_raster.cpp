// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/gcode_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

namespace {

/// A test surface with a guard band around it, so a rasterizer that writes out
/// of bounds is caught rather than silently corrupting a neighbour allocation.
/// The current blend_pixel/blend_pixel_solid pair bounds-check on every write;
/// the extracted rasterizer must keep doing so.
struct Surface {
    static constexpr int kGuard = 4;
    int w, h;
    std::vector<uint8_t> mem;

    Surface(int width, int height) : w(width), h(height) {
        mem.assign(static_cast<size_t>((w + 2 * kGuard) * (h + 2 * kGuard) * 4), 0);
    }

    size_t stride() const {
        return static_cast<size_t>(w + 2 * kGuard) * 4;
    }

    RasterTarget target() {
        // Point the target at the inner region; the guard band surrounds it.
        uint8_t* origin = mem.data() + kGuard * stride() + static_cast<size_t>(kGuard) * 4;
        return RasterTarget{origin, stride(), w, h};
    }

    uint8_t channel(int x, int y, int c) const {
        return *(mem.data() + static_cast<size_t>(y + kGuard) * stride() +
                 static_cast<size_t>(x + kGuard) * 4 + c);
    }

    bool pixel_set(int x, int y) const {
        const uint8_t* p = mem.data() + static_cast<size_t>(y + kGuard) * stride() +
                           static_cast<size_t>(x + kGuard) * 4;
        return p[0] || p[1] || p[2] || p[3];
    }

    bool guard_band_clean() const {
        const int fw = w + 2 * kGuard;
        for (int y = 0; y < h + 2 * kGuard; ++y) {
            for (int x = 0; x < fw; ++x) {
                const bool inside =
                    (x >= kGuard && x < kGuard + w && y >= kGuard && y < kGuard + h);
                if (inside) {
                    continue;
                }
                const uint8_t* p =
                    mem.data() + static_cast<size_t>(y) * stride() + static_cast<size_t>(x) * 4;
                if (p[0] || p[1] || p[2] || p[3]) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Rows touched in a given column, and whether they form one contiguous run.
    void column_coverage(int x, int& count, bool& contiguous) const {
        int first = -1, last = -1;
        count = 0;
        for (int y = 0; y < h; ++y) {
            if (pixel_set(x, y)) {
                ++count;
                if (first < 0) {
                    first = y;
                }
                last = y;
            }
        }
        contiguous = (count == 0) || (last - first + 1 == count);
    }
};

constexpr uint32_t kOpaqueWhite = 0xFFFFFFFFu;

} // namespace

// ---------------------------------------------------------------------------
// The reason this unit exists.
//
// gcode_layer_renderer.cpp had four near-identical thick-line rasterizers whose
// width formulas disagreed:
//
//   bresenham: half = (width - 1) * 0.5f;  for i in [0, width)   -> width lines
//   aa:        half = width / 2;           for i in [-half, half] -> 2*(width/2)+1
//
// and the isometric cache path picks between them on ssao_enabled_ (line ~850),
// so turning SSAO on changed how thick every extrusion drew -- but only at even
// widths, which is why it went unnoticed. Extrusion pixel width is
// clamp(round(width_mm * scale), 1, 8), so even widths are routine.
//
// Separately, the bresenham offsets for an even width land on +/-0.5, and
// std::round takes halves AWAY from zero, so width 2 draws at -1 and +1 and
// skips 0 -- a hollow line.
//
// These cases are the specification: one width formula, N contiguous rows for
// width N, identical between AA and non-AA.
// ---------------------------------------------------------------------------

TEST_CASE("thick_line_offsets yields exactly `width` distinct integer offsets", "[gcode_raster]") {
    // Tests the width FORMULA directly rather than rasterized coverage, because
    // the AA path legitimately writes partial-coverage edge pixels and counting
    // "any non-zero pixel" would not distinguish a real extra line from
    // antialiasing spill.
    //
    // Today's two formulas disagree here:
    //   bresenham: half = (width - 1) * 0.5f; i in [0, width)    -> width values
    //   aa:        half = width / 2;          i in [-half, half]  -> 2*(width/2)+1
    // At width 4 that is 4 offsets versus 5.
    for (int width = 1; width <= 8; ++width) {
        float offs[kMaxThickLineOffsets];
        const int n = thick_line_offsets(width, offs);
        CAPTURE(width);
        REQUIRE(n == width);
    }
}

TEST_CASE("thick_line_offsets is symmetric about the core line", "[gcode_raster]") {
    // An asymmetric offset set reads as a drop shadow rather than a thick line.
    for (int width = 1; width <= 8; ++width) {
        float offs[kMaxThickLineOffsets];
        const int n = thick_line_offsets(width, offs);
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) {
            sum += offs[i];
        }
        CAPTURE(width, sum);
        REQUIRE(sum == Catch::Approx(0.0f).margin(1e-4));
    }
}

TEST_CASE("thick_line offsets leave no gap once mapped to pixels", "[gcode_raster]") {
    // The old code applied std::round, which takes halves AWAY from zero, so an
    // even width's +/-0.5 offsets became -1 and +1 and skipped 0 -- a hollow
    // line. round_offset uses floor(x + 0.5), which maps them to 0 and 1.
    //
    // Note an even count cannot be both contiguous and symmetric about zero in
    // integer pixels; the float offsets carry the symmetry, the pixel mapping
    // carries the contiguity. Both properties are asserted, on their own domain.
    for (int width = 1; width <= 12; ++width) {
        float offs[kMaxThickLineOffsets];
        const int n = thick_line_offsets(width, offs);
        std::vector<int> rounded;
        rounded.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            rounded.push_back(round_offset(offs[i]));
        }
        std::sort(rounded.begin(), rounded.end());
        CAPTURE(width, n);
        REQUIRE(static_cast<int>(rounded.size()) == width);
        for (size_t i = 1; i < rounded.size(); ++i) {
            REQUIRE(rounded[i] - rounded[i - 1] == 1);
        }
    }
}

TEST_CASE("AA and non-AA agree on rasterized thickness", "[gcode_raster]") {
    // The regression that shipped: SSAO on drew every even-width extrusion a
    // pixel wider than SSAO off. Compares the two paths against each other, so
    // it holds regardless of how AA distributes coverage.
    for (int width = 1; width <= 8; ++width) {
        Surface off(24, 24), on(24, 24);
        auto t_off = off.target();
        auto t_on = on.target();
        thick_line(t_off, 2, 12, 21, 12, kOpaqueWhite, width, Aa::Off);
        thick_line(t_on, 2, 12, 21, 12, kOpaqueWhite, width, Aa::On);

        int c_off = 0, c_on = 0;
        bool k_off = false, k_on = false;
        off.column_coverage(12, c_off, k_off);
        on.column_coverage(12, c_on, k_on);

        CAPTURE(width, c_off, c_on);
        REQUIRE(c_off == c_on);
        REQUIRE(k_off);
        REQUIRE(k_on);
    }
}

TEST_CASE("a vertical thick line covers exactly `width` contiguous columns", "[gcode_raster]") {
    for (int width = 1; width <= 8; ++width) {
        Surface s(24, 24);
        auto t = s.target();
        thick_line(t, 12, 2, 12, 21, kOpaqueWhite, width, Aa::Off);

        int first = -1, last = -1, count = 0;
        for (int x = 0; x < 24; ++x) {
            if (s.pixel_set(x, 12)) {
                ++count;
                if (first < 0) {
                    first = x;
                }
                last = x;
            }
        }
        CAPTURE(width, count);
        REQUIRE(count == width);
        REQUIRE(last - first + 1 == count);
    }
}

TEST_CASE("a degenerate zero-length line still marks its own pixel", "[gcode_raster]") {
    // The old code funnelled sub-threshold lines to the single-pixel path, with
    // the threshold itself inconsistent: MIN_LINE_LENGTH was 0.001f at file
    // scope but shadowed by a local 0.5f inside the AA variant, so only the AA
    // path applied the coarser cutoff.
    for (Aa aa : {Aa::Off, Aa::On}) {
        Surface s(16, 16);
        auto t = s.target();
        thick_line(t, 8, 8, 8, 8, kOpaqueWhite, 3, aa);
        REQUIRE(s.pixel_set(8, 8));
    }
}

TEST_CASE("clipping: a line running off-surface writes nothing outside it", "[gcode_raster]") {
    for (Aa aa : {Aa::Off, Aa::On}) {
        Surface s(16, 16);
        auto t = s.target();
        thick_line(t, -40, 8, 60, 9, kOpaqueWhite, 6, aa);
        REQUIRE(s.guard_band_clean());
    }
}

TEST_CASE("clipping: a line entirely off-surface is a no-op", "[gcode_raster]") {
    Surface s(16, 16);
    auto t = s.target();
    thick_line(t, -50, -50, -40, -40, kOpaqueWhite, 4, Aa::Off);
    REQUIRE(s.guard_band_clean());
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            REQUIRE_FALSE(s.pixel_set(x, y));
        }
    }
}

TEST_CASE("blend overwrites all four channels", "[gcode_raster]") {
    // Deliberately an overwrite, not a composite. The original blend_pixel and
    // blend_pixel_solid both wrote B,G,R,A unconditionally -- the name is
    // historical. A refactor must not quietly turn this into alpha compositing.
    Surface s(8, 8);
    auto t = s.target();
    blend(t, 4, 4, 0xFF112233u);
    REQUIRE(s.channel(4, 4, 0) == 0x33); // B
    REQUIRE(s.channel(4, 4, 1) == 0x22); // G
    REQUIRE(s.channel(4, 4, 2) == 0x11); // R
    REQUIRE(s.channel(4, 4, 3) == 0xFF); // A
    blend(t, 4, 4, 0x00000000u);
    REQUIRE(s.channel(4, 4, 3) == 0x00);
}

TEST_CASE("blend_coverage treats zero coverage as a no-op", "[gcode_raster]") {
    Surface s(8, 8);
    auto t = s.target();
    blend_coverage(t, 4, 4, 0xFFFFFFu, 0);
    REQUIRE_FALSE(s.pixel_set(4, 4));
}

TEST_CASE("blend_coverage writes straight through at full coverage", "[gcode_raster]") {
    Surface s(8, 8);
    auto t = s.target();
    blend_coverage(t, 4, 4, 0x112233u, 255);
    REQUIRE(s.channel(4, 4, 0) == 0x33);
    REQUIRE(s.channel(4, 4, 3) == 255);
}

TEST_CASE("blend_coverage composites over an existing pixel", "[gcode_raster]") {
    Surface s(8, 8);
    auto t = s.target();
    blend_coverage(t, 4, 4, 0x000000u, 255); // opaque black
    blend_coverage(t, 4, 4, 0xFFFFFFu, 128); // half-covered white over it
    const uint8_t b = s.channel(4, 4, 0);
    REQUIRE(b > 0);
    REQUIRE(b < 255);
}

TEST_CASE("both blends bounds-check every write", "[gcode_raster]") {
    Surface s(8, 8);
    auto t = s.target();
    for (auto xy : {std::pair<int, int>{-1, 4}, {8, 4}, {4, -1}, {4, 8}}) {
        blend(t, xy.first, xy.second, 0xFFFFFFFFu);
        blend_coverage(t, xy.first, xy.second, 0xFFFFFFu, 255);
    }
    REQUIRE(s.guard_band_clean());
}

// ===========================================================================
// stroke_selection_rim(): the white silhouette, derived from the alpha tag.
//
// This replaced a dilate-and-overpaint halo, which was not a silhouette
// algorithm at all: it drew the selected object wide in white, drew it again
// narrower on top, and kept whatever survived. That holds up on a vertical wall,
// where consecutive layers land on each other, and floods on a sloped one, where
// each layer's white sticks out past the layer above it. A test cone went solid
// white by layer 120. The cases below pin the properties that failure lacked.
// ===========================================================================

namespace {

/// Fill a rectangle with the tag, at an arbitrary opaque colour.
void fill_tagged(Surface& s, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            blend(s.target(), x, y, (static_cast<uint32_t>(kSelectedAlpha) << 24) | 0x336699);
        }
    }
}

bool is_white(const Surface& s, int x, int y) {
    return s.channel(x, y, 0) == 0xFF && s.channel(x, y, 1) == 0xFF && s.channel(x, y, 2) == 0xFF;
}

constexpr uint32_t kWhite = 0xFFFFFF;

} // namespace

TEST_CASE("the rim traces the boundary of the tagged region", "[gcode_raster]") {
    Surface s(20, 20);
    fill_tagged(s, 5, 5, 14, 14);
    stroke_selection_rim(s.target(), 1, 1, kWhite, ChannelOrder::Bgra);

    // Boundary is white.
    CHECK(is_white(s, 5, 5));
    CHECK(is_white(s, 14, 14));
    CHECK(is_white(s, 5, 10));
    CHECK(is_white(s, 10, 14));

    // One pixel in is not: a 1px rim is a 1px rim.
    CHECK_FALSE(is_white(s, 6, 6));
    CHECK_FALSE(is_white(s, 10, 10));

    // Nothing outside the tagged region was touched.
    CHECK_FALSE(s.pixel_set(4, 4));
    CHECK_FALSE(s.pixel_set(15, 15));
    CHECK(s.guard_band_clean());
}

TEST_CASE("rim thickness follows the requested pixel width", "[gcode_raster]") {
    Surface s(20, 20);
    fill_tagged(s, 4, 4, 15, 15);
    stroke_selection_rim(s.target(), 2, 1, kWhite, ChannelOrder::Bgra);

    CHECK(is_white(s, 4, 9));       // boundary
    CHECK(is_white(s, 5, 9));       // one in, still rim at width 2
    CHECK_FALSE(is_white(s, 6, 9)); // two in, interior
}

// The property dilate-and-overpaint did not have. A sloped wall is a stack of
// offset strokes; the old halo left each layer's white sticking out past the one
// above it, so the face filled in from the bottom up. Deriving the rim from the
// tagged region's boundary cannot do that, whatever shape the region is.
TEST_CASE("a staircase edge gets a rim, not a flood", "[gcode_raster]") {
    Surface s(40, 40);
    // Each row shifted one pixel right of the row below: a 45-degree wall.
    for (int y = 0; y < 30; ++y) {
        fill_tagged(s, 5 + y / 2, 5 + y, 30, 5 + y);
    }
    stroke_selection_rim(s.target(), 1, 1, kWhite, ChannelOrder::Bgra);

    int white = 0, tagged_total = 0;
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 40; ++x) {
            if (s.channel(x, y, 3) == kSelectedAlpha) {
                ++tagged_total;
                if (is_white(s, x, y)) {
                    ++white;
                }
            }
        }
    }
    REQUIRE(tagged_total > 0);
    REQUIRE(white > 0);
    // A rim is a boundary, so it is a small minority of the area. The old halo
    // put this above 90% on the same shape.
    CHECK(white * 2 < tagged_total);
}

TEST_CASE("a one-pixel seam inside the object is not outlined", "[gcode_raster]") {
    // Sparse infill leaves pinholes between strokes. With gap_px = 1 every one of
    // them reads as the outside and the interior fills with speckle.
    Surface s(20, 20);
    fill_tagged(s, 4, 4, 15, 15);
    // Punch a single untagged pixel in the middle.
    blend(s.target(), 10, 10, 0xFF000000u | 0x112233);

    stroke_selection_rim(s.target(), 2, 2, kWhite, ChannelOrder::Bgra);

    CHECK_FALSE(is_white(s, 9, 10));
    CHECK_FALSE(is_white(s, 11, 10));
    CHECK_FALSE(is_white(s, 10, 9));
    CHECK_FALSE(is_white(s, 10, 11));
    // The real boundary still gets its rim.
    CHECK(is_white(s, 4, 10));
}

TEST_CASE("a hole deeper than the gap threshold IS outlined", "[gcode_raster]") {
    // The counterpart to the seam case: a genuine hole through the object is a
    // real contour and Orca outlines it too. Suppressing every interior boundary
    // would be the wrong cure.
    Surface s(24, 24);
    fill_tagged(s, 4, 4, 19, 19);
    for (int y = 10; y <= 13; ++y) {
        for (int x = 10; x <= 13; ++x) {
            blend(s.target(), x, y, 0xFF000000u | 0x112233);
        }
    }
    stroke_selection_rim(s.target(), 2, 2, kWhite, ChannelOrder::Bgra);
    CHECK(is_white(s, 9, 11));
    CHECK(is_white(s, 14, 11));
}

TEST_CASE("untagged pixels are never recoloured", "[gcode_raster]") {
    Surface s(20, 20);
    // A fully opaque neighbour object butted right up against the tagged one.
    for (int y = 4; y <= 15; ++y) {
        for (int x = 4; x <= 9; ++x) {
            blend(s.target(), x, y, 0xFF000000u | 0x112233);
        }
    }
    fill_tagged(s, 10, 4, 15, 15);
    stroke_selection_rim(s.target(), 2, 2, kWhite, ChannelOrder::Bgra);

    for (int y = 4; y <= 15; ++y) {
        for (int x = 4; x <= 9; ++x) {
            CHECK(s.channel(x, y, 2) == 0x11);
            CHECK(s.channel(x, y, 1) == 0x22);
            CHECK(s.channel(x, y, 0) == 0x33);
        }
    }
    // And the tagged object still gets a rim along the shared border, because a
    // neighbour occluding it is exactly where its visible contour ends.
    CHECK(is_white(s, 10, 10));
}

// The rim writes RGB and leaves alpha alone. That is what lets the pass run more
// than once over the same buffer without the second run reading its own output
// as a boundary and eating inward a pixel at a time.
TEST_CASE("the rim pass is idempotent", "[gcode_raster]") {
    Surface a(20, 20);
    fill_tagged(a, 5, 5, 14, 14);
    stroke_selection_rim(a.target(), 2, 2, kWhite, ChannelOrder::Bgra);
    std::vector<uint8_t> once = a.mem;

    stroke_selection_rim(a.target(), 2, 2, kWhite, ChannelOrder::Bgra);
    CHECK(a.mem == once);
}

TEST_CASE("an object running off the canvas is still outlined along the edge", "[gcode_raster]") {
    Surface s(16, 16);
    fill_tagged(s, 0, 0, 7, 15);
    stroke_selection_rim(s.target(), 1, 1, kWhite, ChannelOrder::Bgra);
    // Off-canvas counts as outside, so the left column is a boundary. Losing this
    // would silently drop the rim on any object the user has scrolled or zoomed
    // partly out of view.
    CHECK(is_white(s, 0, 8));
    CHECK(is_white(s, 7, 8));
    CHECK_FALSE(is_white(s, 4, 8));
    CHECK(s.guard_band_clean());
}

TEST_CASE("a buffer with no tagged pixels is left completely alone", "[gcode_raster]") {
    Surface s(16, 16);
    for (int y = 2; y <= 13; ++y) {
        for (int x = 2; x <= 13; ++x) {
            blend(s.target(), x, y, 0xFF000000u | 0x445566);
        }
    }
    std::vector<uint8_t> before = s.mem;
    stroke_selection_rim(s.target(), 2, 2, kWhite, ChannelOrder::Bgra);
    CHECK(s.mem == before);
}

TEST_CASE("degenerate rim parameters are a no-op, not a crash", "[gcode_raster]") {
    Surface s(16, 16);
    fill_tagged(s, 4, 4, 11, 11);
    std::vector<uint8_t> before = s.mem;

    stroke_selection_rim(s.target(), 0, 2, kWhite, ChannelOrder::Bgra);
    CHECK(s.mem == before);
    stroke_selection_rim(s.target(), 2, 0, kWhite, ChannelOrder::Bgra);
    CHECK(s.mem == before);

    RasterTarget null_target{nullptr, 0, 16, 16};
    stroke_selection_rim(null_target, 2, 2, kWhite, ChannelOrder::Bgra); // must not dereference
}

// blend_coverage() accumulates alpha, so an antialiased edge on an UNSELECTED
// object can land on the tag value by chance and pick up a stray white pixel.
TEST_CASE("accumulated coverage never lands on the reserved tag value", "[gcode_raster]") {
    Surface s(4, 4);
    // Drive the accumulator across its whole range looking for the tag.
    for (int first = 1; first < 255; ++first) {
        for (int second = 1; second < 255; ++second) {
            Surface t(4, 4);
            blend_coverage(t.target(), 1, 1, 0x336699, static_cast<uint8_t>(first));
            blend_coverage(t.target(), 1, 1, 0x336699, static_cast<uint8_t>(second));
            REQUIRE(t.channel(1, 1, 3) != kSelectedAlpha);
        }
    }
}

// The two-call case above cannot see the FIRST write. blend_coverage takes an
// unaccumulated fast path whenever the destination is still transparent, and
// there is no accumulator there to launder a bad value; when the first call
// produces 254 the second re-blends through the guarded branch and the final
// state reads clean either way.
TEST_CASE("a single coverage write never lands on the reserved tag value", "[gcode_raster]") {
    for (int coverage = 1; coverage < 256; ++coverage) {
        Surface t(4, 4);
        blend_coverage(t.target(), 1, 1, 0x336699, static_cast<uint8_t>(coverage));
        CAPTURE(coverage);
        // Painted at all, and not carrying the tag. Both halves matter: an alpha
        // of 0 would satisfy "not tagged" while erasing the stroke.
        REQUIRE(t.channel(1, 1, 3) != 0);
        REQUIRE(t.channel(1, 1, 3) != kSelectedAlpha);
    }
}

TEST_CASE("an antialiased line never tags a pixel as selected", "[gcode_raster]") {
    // The reachable shape of the same bug, through the rasterizer rather than the
    // pixel op. line_wu's low coverage is (1 - frac) * 255, which truncates to
    // exactly 254 for any frac in (0, 1/255]; a 300px span rising one pixel makes
    // frac = 1/300 on the first interior column, and that column's pixel is still
    // transparent, so the write takes the fast path.
    //
    // Every pixel this leaves tagged is one stroke_selection_rim() then paints
    // white in the middle of an object nobody selected: an isolated tagged pixel
    // has no tagged neighbour in any of the four directions, so it reads as being
    // on the rim from all of them.
    Surface s(302, 8);
    line(s.target(), 0, 0, 300, 1, 0x336699, Aa::On);

    int painted = 0;
    int tagged = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 302; ++x) {
            const uint8_t a = s.channel(x, y, 3);
            painted += (a != 0);
            tagged += (a == kSelectedAlpha);
        }
    }
    INFO("painted=" << painted << " tagged=" << tagged);
    REQUIRE(painted > 0); // the line drew: a no-op rasterizer must not pass
    REQUIRE(tagged == 0);
}

// ===========================================================================
// Channel order.
//
// The 2D solid cache and the ghost buffer are ARGB8888, which is B, G, R, A in
// memory on little-endian. The 3D path hands this same routine the buffer from a
// glReadPixels(..., GL_RGBA, ...), where red and blue are the other way round.
// The swap is invisible for as long as the outline token stays white, which is
// exactly how it survived: gcode_selection_outline is #FFFFFF today, so the two
// orders produce identical bytes and only a non-grey token separates them.
// ===========================================================================

namespace {
/// Deliberately not grey, and with three distinct channel values, so a swapped
/// or dropped channel cannot alias onto a correct answer.
constexpr uint32_t kOutlineToken = 0xFF6B35;
} // namespace

TEST_CASE("the rim colour lands in ARGB8888 byte order", "[gcode_raster]") {
    Surface s(20, 20);
    fill_tagged(s, 5, 5, 14, 14);
    stroke_selection_rim(s.target(), 1, 1, kOutlineToken, ChannelOrder::Bgra);

    CHECK(s.channel(5, 5, 0) == 0x35); // B
    CHECK(s.channel(5, 5, 1) == 0x6B); // G
    CHECK(s.channel(5, 5, 2) == 0xFF); // R
    // Alpha is byte 3 in both layouts and the pass never writes it, so the tag
    // survives and a second pass over the same buffer is still idempotent.
    CHECK(s.channel(5, 5, 3) == kSelectedAlpha);
}

TEST_CASE("the rim colour lands in GL readback byte order", "[gcode_raster]") {
    Surface s(20, 20);
    fill_tagged(s, 5, 5, 14, 14);
    stroke_selection_rim(s.target(), 1, 1, kOutlineToken, ChannelOrder::Rgba);

    CHECK(s.channel(5, 5, 0) == 0xFF); // R
    CHECK(s.channel(5, 5, 1) == 0x6B); // G
    CHECK(s.channel(5, 5, 2) == 0x35); // B
    CHECK(s.channel(5, 5, 3) == kSelectedAlpha);
}

TEST_CASE("the two channel orders disagree on a non-grey rim", "[gcode_raster]") {
    // Guards the pair above against a future implementation that quietly ignores
    // `order`: both cases would then still be checkable one at a time against
    // whichever layout it picked, but the two buffers could not differ.
    Surface bgra(20, 20), rgba(20, 20);
    fill_tagged(bgra, 5, 5, 14, 14);
    fill_tagged(rgba, 5, 5, 14, 14);
    stroke_selection_rim(bgra.target(), 1, 1, kOutlineToken, ChannelOrder::Bgra);
    stroke_selection_rim(rgba.target(), 1, 1, kOutlineToken, ChannelOrder::Rgba);
    REQUIRE(bgra.mem != rgba.mem);
}
