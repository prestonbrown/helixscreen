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
