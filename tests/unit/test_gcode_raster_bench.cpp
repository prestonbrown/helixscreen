// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/gcode_raster.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// ---------------------------------------------------------------------------
// Measurements for the rasterizer work in the render audit, phase 3.
//
// Hidden behind the [.] tag: these print numbers rather than asserting
// behaviour, and the timings are machine-dependent. Run explicitly:
//
//   ./build/bin/helix-tests "[raster_bench]"
//
// The claim they exist to check is that thick_line() writes every interior
// pixel of a stroke several times over, because it draws `width` parallel
// Bresenham lines rather than filling a span. That was asserted in the audit
// spec from reading the code; the ratio below is the actual number.
// ---------------------------------------------------------------------------

namespace {

constexpr int kDim = 512;

struct Canvas {
    std::vector<uint8_t> mem;
    Canvas() : mem(static_cast<size_t>(kDim) * kDim * 4, 0) {}

    RasterTarget target() {
        return RasterTarget{mem.data(), static_cast<size_t>(kDim) * 4, kDim, kDim};
    }
    void clear() {
        std::fill(mem.begin(), mem.end(), uint8_t{0});
    }
    size_t painted() const {
        size_t n = 0;
        for (size_t i = 3; i < mem.size(); i += 4) {
            if (mem[i] != 0) {
                ++n;
            }
        }
        return n;
    }
};

/// How many pixel writes thick_line() issues for one segment: it draws `width`
/// parallel Bresenham lines, each of which touches max(|dx|,|dy|)+1 pixels.
size_t expected_writes(int x0, int y0, int x1, int y1, int width) {
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const size_t per_line = static_cast<size_t>(std::max(dx, dy)) + 1;
    return per_line * static_cast<size_t>(std::max(1, width));
}

} // namespace

TEST_CASE("thick_line write amplification by width", "[.][raster_bench]") {
    // A diagonal, which is the worst case for a parallel-offset thick line:
    // neighbouring offset lines overlap most when the segment is at 45 degrees.
    struct Case {
        const char* name;
        int x0, y0, x1, y1;
    };
    const Case cases[] = {
        {"horizontal", 40, 256, 470, 256},
        {"diagonal-45", 40, 40, 470, 470},
        {"steep", 256, 40, 300, 470},
    };

    std::printf("\n  %-12s %5s %10s %10s %8s\n", "segment", "width", "writes", "unique", "ratio");
    std::printf("  %-12s %5s %10s %10s %8s\n", "------------", "-----", "----------", "----------",
                "--------");

    for (const auto& c : cases) {
        for (int w : {1, 2, 4, 6, 8}) {
            Canvas cv;
            thick_line(cv.target(), c.x0, c.y0, c.x1, c.y1, 0xFF3060C0u, w, Aa::Off);
            const size_t unique = cv.painted();
            const size_t writes = expected_writes(c.x0, c.y0, c.x1, c.y1, w);
            const double ratio =
                unique ? static_cast<double>(writes) / static_cast<double>(unique) : 0.0;
            std::printf("  %-12s %5d %10zu %10zu %8.2f\n", c.name, w, writes, unique, ratio);
        }
    }
    std::printf("\n  ratio = writes issued / distinct pixels covered.\n"
                "  1.00 means each pixel is written once; higher is redundant work.\n\n");
    SUCCEED();
}

TEST_CASE("thick_line throughput", "[.][raster_bench]") {
    // Absolute cost, so the audit can say whether a span rewrite is worth doing
    // rather than only that it would be less work in principle.
    Canvas cv;
    constexpr int kSegments = 20000;

    std::printf("\n  %5s %14s %14s\n", "width", "us/1k segs", "M pixels/s");
    std::printf("  %5s %14s %14s\n", "-----", "--------------", "--------------");

    for (int w : {1, 2, 4, 8}) {
        cv.clear();
        // Deterministic pseudo-scatter; no RNG so the numbers are comparable
        // across runs and machines.
        const auto t0 = std::chrono::steady_clock::now();
        size_t pixels = 0;
        for (int i = 0; i < kSegments; ++i) {
            const int x0 = 20 + (i * 37) % 460;
            const int y0 = 20 + (i * 53) % 460;
            const int x1 = 20 + (i * 71) % 460;
            const int y1 = 20 + (i * 97) % 460;
            thick_line(cv.target(), x0, y0, x1, y1, 0xFF3060C0u, w, Aa::Off);
            pixels += expected_writes(x0, y0, x1, y1, w);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        std::printf("  %5d %14.1f %14.1f\n", w, us / (kSegments / 1000.0),
                    static_cast<double>(pixels) / us);
    }
    std::printf("\n  For scale: the exclude-object test plate parses to 135,197 segments.\n\n");
    SUCCEED();
}

TEST_CASE("antialiasing cost relative to aliased", "[.][raster_bench]") {
    // The solid cache draws antialiased whenever enhanced shading is on, so the
    // AA path is the one that runs on a desktop and a Pi.
    //
    // Which row matters depends on the model, not on the plate. auto_fit() frames
    // the model's bounding box, so scale_ is px per mm of MODEL. Measured on the
    // production path: a 48mm Benchy renders at 3.1 px/mm and width 1, a 10mm
    // calibration cube at 15.0 px/mm and width 6, and anything filling a 200mm
    // plate at 1.3-2.5 px/mm and width 1. Width 1 is the common case and the
    // MAX_EXTRUSION_PIXEL_WIDTH clamp of 8 is not reached by these models.
    //
    // Re-measure after any change to framing: these numbers are a property of
    // auto_fit(), not of the rasterizer, and they moved once already when the
    // preview started framing against the real metadata overlap.
    Canvas cv;
    constexpr int kSegments = 20000;

    for (int w : {1, 2, 4}) {
        for (auto aa : {Aa::Off, Aa::On}) {
            cv.clear();
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kSegments; ++i) {
                const int x0 = 20 + (i * 37) % 460;
                const int y0 = 20 + (i * 53) % 460;
                const int x1 = 20 + (i * 71) % 460;
                const int y1 = 20 + (i * 97) % 460;
                thick_line(cv.target(), x0, y0, x1, y1, 0xFF3060C0u, w, aa);
            }
            const auto t1 = std::chrono::steady_clock::now();
            const double us =
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            std::printf("  width %d, Aa::%-3s : %8.1f us/1k segs\n", w, aa == Aa::On ? "On" : "Off",
                        us / (kSegments / 1000.0));
        }
    }
    std::printf("\n");
    SUCCEED();
}
