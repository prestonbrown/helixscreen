// SPDX-License-Identifier: GPL-3.0-or-later
//
// The teapot's control points and patches are freeglut's (src/fg_teapot_data.h), under this
// notice:
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// PAWEL W. OLSZTA BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// The data derives from Martin Newell's teapot, as Juhana Kouhia archived it from Philip
// Schneider in 1990.

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_pipes_sim.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace helix::ui {

namespace {

/// Patches 0-5 (rim, body, lid, bottom) cover one quadrant and repeat in all four; 6-9
/// (handle, spout) cover the half at y <= 0 and mirror across y = 0.
constexpr int kTeapotPatches = 10;
constexpr int kTeapotQuadrantPatches = 6;

constexpr uint8_t kTeapotPatch[kTeapotPatches][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},           // rim
    {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27}, // body
    {24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39},
    {40, 41, 42, 40, 43, 44, 45, 46, 47, 47, 47, 47, 48, 49, 50, 51}, // lid
    {48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63},
    {64, 64, 64, 64, 65, 66, 67, 68, 69, 70, 71, 72, 39, 38, 37, 36}, // bottom
    {73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88}, // handle
    {85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100},
    {101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116}, // spout
    {113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128},
};

constexpr float kTeapotCp[][3] = {
    {1.40000f, 0.00000f, 2.40000f},   {1.40000f, -0.78400f, 2.40000f},
    {0.78400f, -1.40000f, 2.40000f},  {0.00000f, -1.40000f, 2.40000f},
    {1.33750f, 0.00000f, 2.53125f},   {1.33750f, -0.74900f, 2.53125f},
    {0.74900f, -1.33750f, 2.53125f},  {0.00000f, -1.33750f, 2.53125f},
    {1.43750f, 0.00000f, 2.53125f},   {1.43750f, -0.80500f, 2.53125f},
    {0.80500f, -1.43750f, 2.53125f},  {0.00000f, -1.43750f, 2.53125f},
    {1.50000f, 0.00000f, 2.40000f},   {1.50000f, -0.84000f, 2.40000f},
    {0.84000f, -1.50000f, 2.40000f},  {0.00000f, -1.50000f, 2.40000f},
    {1.75000f, 0.00000f, 1.87500f},   {1.75000f, -0.98000f, 1.87500f},
    {0.98000f, -1.75000f, 1.87500f},  {0.00000f, -1.75000f, 1.87500f},
    {2.00000f, 0.00000f, 1.35000f},   {2.00000f, -1.12000f, 1.35000f},
    {1.12000f, -2.00000f, 1.35000f},  {0.00000f, -2.00000f, 1.35000f},
    {2.00000f, 0.00000f, 0.90000f},   {2.00000f, -1.12000f, 0.90000f},
    {1.12000f, -2.00000f, 0.90000f},  {0.00000f, -2.00000f, 0.90000f},
    {2.00000f, 0.00000f, 0.45000f},   {2.00000f, -1.12000f, 0.45000f},
    {1.12000f, -2.00000f, 0.45000f},  {0.00000f, -2.00000f, 0.45000f},
    {1.50000f, 0.00000f, 0.22500f},   {1.50000f, -0.84000f, 0.22500f},
    {0.84000f, -1.50000f, 0.22500f},  {0.00000f, -1.50000f, 0.22500f},
    {1.50000f, 0.00000f, 0.15000f},   {1.50000f, -0.84000f, 0.15000f},
    {0.84000f, -1.50000f, 0.15000f},  {0.00000f, -1.50000f, 0.15000f},
    {0.00000f, 0.00000f, 3.15000f},   {0.00000f, -0.00200f, 3.15000f},
    {0.00200f, 0.00000f, 3.15000f},   {0.80000f, 0.00000f, 3.15000f},
    {0.80000f, -0.45000f, 3.15000f},  {0.45000f, -0.80000f, 3.15000f},
    {0.00000f, -0.80000f, 3.15000f},  {0.00000f, 0.00000f, 2.85000f},
    {0.20000f, 0.00000f, 2.70000f},   {0.20000f, -0.11200f, 2.70000f},
    {0.11200f, -0.20000f, 2.70000f},  {0.00000f, -0.20000f, 2.70000f},
    {0.40000f, 0.00000f, 2.55000f},   {0.40000f, -0.22400f, 2.55000f},
    {0.22400f, -0.40000f, 2.55000f},  {0.00000f, -0.40000f, 2.55000f},
    {1.30000f, 0.00000f, 2.55000f},   {1.30000f, -0.72800f, 2.55000f},
    {0.72800f, -1.30000f, 2.55000f},  {0.00000f, -1.30000f, 2.55000f},
    {1.30000f, 0.00000f, 2.40000f},   {1.30000f, -0.72800f, 2.40000f},
    {0.72800f, -1.30000f, 2.40000f},  {0.00000f, -1.30000f, 2.40000f},
    {0.00000f, 0.00000f, 0.00000f},   {0.00000f, -1.42500f, 0.00000f},
    {0.79800f, -1.42500f, 0.00000f},  {1.42500f, -0.79800f, 0.00000f},
    {1.42500f, 0.00000f, 0.00000f},   {0.00000f, -1.50000f, 0.07500f},
    {0.84000f, -1.50000f, 0.07500f},  {1.50000f, -0.84000f, 0.07500f},
    {1.50000f, 0.00000f, 0.07500f},   {-1.60000f, 0.00000f, 2.02500f},
    {-1.60000f, -0.30000f, 2.02500f}, {-1.50000f, -0.30000f, 2.25000f},
    {-1.50000f, 0.00000f, 2.25000f},  {-2.30000f, 0.00000f, 2.02500f},
    {-2.30000f, -0.30000f, 2.02500f}, {-2.50000f, -0.30000f, 2.25000f},
    {-2.50000f, 0.00000f, 2.25000f},  {-2.70000f, 0.00000f, 2.02500f},
    {-2.70000f, -0.30000f, 2.02500f}, {-3.00000f, -0.30000f, 2.25000f},
    {-3.00000f, 0.00000f, 2.25000f},  {-2.70000f, 0.00000f, 1.80000f},
    {-2.70000f, -0.30000f, 1.80000f}, {-3.00000f, -0.30000f, 1.80000f},
    {-3.00000f, 0.00000f, 1.80000f},  {-2.70000f, 0.00000f, 1.57500f},
    {-2.70000f, -0.30000f, 1.57500f}, {-3.00000f, -0.30000f, 1.35000f},
    {-3.00000f, 0.00000f, 1.35000f},  {-2.50000f, 0.00000f, 1.12500f},
    {-2.50000f, -0.30000f, 1.12500f}, {-2.65000f, -0.30000f, 0.93750f},
    {-2.65000f, 0.00000f, 0.93750f},  {-2.00000f, 0.00000f, 0.90000f},
    {-2.00000f, -0.30000f, 0.90000f}, {-1.90000f, -0.30000f, 0.60000f},
    {-1.90000f, 0.00000f, 0.60000f},  {1.70000f, 0.00000f, 1.42500f},
    {1.70000f, -0.66000f, 1.42500f},  {1.70000f, -0.66000f, 0.60000f},
    {1.70000f, 0.00000f, 0.60000f},   {2.60000f, 0.00000f, 1.42500f},
    {2.60000f, -0.66000f, 1.42500f},  {3.10000f, -0.66000f, 0.82500f},
    {3.10000f, 0.00000f, 0.82500f},   {2.30000f, 0.00000f, 2.10000f},
    {2.30000f, -0.25000f, 2.10000f},  {2.40000f, -0.25000f, 2.02500f},
    {2.40000f, 0.00000f, 2.02500f},   {2.70000f, 0.00000f, 2.40000f},
    {2.70000f, -0.25000f, 2.40000f},  {3.30000f, -0.25000f, 2.40000f},
    {3.30000f, 0.00000f, 2.40000f},   {2.80000f, 0.00000f, 2.47500f},
    {2.80000f, -0.25000f, 2.47500f},  {3.52500f, -0.25000f, 2.49375f},
    {3.52500f, 0.00000f, 2.49375f},   {2.90000f, 0.00000f, 2.47500f},
    {2.90000f, -0.15000f, 2.47500f},  {3.45000f, -0.15000f, 2.51250f},
    {3.45000f, 0.00000f, 2.51250f},   {2.80000f, 0.00000f, 2.40000f},
    {2.80000f, -0.15000f, 2.40000f},  {3.20000f, -0.15000f, 2.40000f},
    {3.20000f, 0.00000f, 2.40000f},
};

/// Samples along each side of a patch.
constexpr int kTeapotGrid = 6;

/// Model units per data unit: auxSolidTeapot(2.5 r) halves its size argument.
constexpr float kTeapotScale = 0.5f * 2.5f * kPipesRadius;

/// Data height the model is dropped by, so the teapot sits about its node.
constexpr float kTeapotDrop = 1.5f;

/// The point outward normals face away from, in data units (on the axis, mid-height).
constexpr PipeVec3 kTeapotInside{0.0f, 0.0f, 1.5f};

struct Cubic {
    float b[4];
    float d[4];
};

Cubic bernstein(float t) {
    const float s = 1.0f - t;
    return {
        {s * s * s, 3.0f * t * s * s, 3.0f * t * t * s, t * t * t},
        {-3.0f * s * s, 3.0f * s * s - 6.0f * t * s, 6.0f * t * s - 3.0f * t * t, 3.0f * t * t}};
}

/// One patch's 16 control points in data units, its x and y signs applied.
using Patch = std::array<PipeVec3, 16>;

Patch patch_points(int patch, float sx, float sy) {
    Patch p{};
    for (size_t i = 0; i < 16; i++) {
        const float* c = kTeapotCp[kTeapotPatch[patch][i]];
        p[i] = {c[0] * sx, c[1] * sy, c[2]};
    }
    return p;
}

/// Surface point and the cross product of its partial derivatives at (u, v).
void evaluate(const Patch& p, float u, float v, PipeVec3& pos, PipeVec3& normal) {
    const Cubic bu = bernstein(u);
    const Cubic bv = bernstein(v);
    PipeVec3 du{};
    PipeVec3 dv{};
    pos = {};
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            const PipeVec3& c = p[i * 4 + j];
            const float w = bu.b[i] * bv.b[j];
            const float wu = bu.d[i] * bv.b[j];
            const float wv = bu.b[i] * bv.d[j];
            pos = {pos.x + w * c.x, pos.y + w * c.y, pos.z + w * c.z};
            du = {du.x + wu * c.x, du.y + wu * c.y, du.z + wu * c.z};
            dv = {dv.x + wv * c.x, dv.y + wv * c.y, dv.z + wv * c.z};
        }
    }
    normal = cross(du, dv);
}

/// Data-space (z up) to model space (y up), scaled and dropped.
PipeVec3 to_model(PipeVec3 d) {
    return {d.x * kTeapotScale, (d.z - kTeapotDrop) * kTeapotScale, -d.y * kTeapotScale};
}

PipeVec3 normal_to_model(PipeVec3 n) {
    return {n.x, n.z, -n.y};
}

void add_patch(const Patch& p, PipeMesh& out) {
    constexpr int N = kTeapotGrid;
    constexpr float STEP = 1.0f / static_cast<float>(N - 1);
    // A collapsed edge (the lid's tip, the bottom's centre) has no tangent plane; the normal
    // there is the limit from just inside the patch.
    constexpr float kInset = 0.01f;
    std::array<PipeVec3, N * N> pos{};
    std::array<PipeVec3, N * N> nrm{};
    float outward = 0.0f;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            const float u = static_cast<float>(i) * STEP;
            const float v = static_cast<float>(j) * STEP;
            PipeVec3 point;
            PipeVec3 n;
            evaluate(p, u, v, point, n);
            if (length(n) < 1e-5f) {
                PipeVec3 unused;
                evaluate(p, std::clamp(u, kInset, 1.0f - kInset),
                         std::clamp(v, kInset, 1.0f - kInset), unused, n);
            }
            n = normalized_or(n, {0, 0, 1});
            const PipeVec3 away = point - kTeapotInside;
            const float away_len = length(away);
            if (away_len > 0.0f) {
                outward += dot(n, away) / away_len;
            }
            pos[static_cast<size_t>(i * N + j)] = point;
            nrm[static_cast<size_t>(i * N + j)] = n;
        }
    }
    // Mirrored copies reverse the parameter orientation; each patch faces the way most of its
    // surface points away from the inside.
    const bool flip = outward < 0.0f;
    const auto base = static_cast<uint16_t>(out.verts.size());
    for (size_t k = 0; k < pos.size(); k++) {
        PipeVec3 n = nrm[k];
        if (flip) {
            n = {-n.x, -n.y, -n.z};
        }
        out.verts.push_back({to_model(pos[k]), normal_to_model(n)});
    }
    for (int i = 0; i + 1 < N; i++) {
        for (int j = 0; j + 1 < N; j++) {
            const auto a = static_cast<uint16_t>(base + i * N + j);
            const auto b = static_cast<uint16_t>(base + (i + 1) * N + j);
            const auto c = static_cast<uint16_t>(base + (i + 1) * N + j + 1);
            const auto d = static_cast<uint16_t>(base + i * N + j + 1);
            // cross(du, dv) faces out when (u, v) runs counter-clockwise: a, b, c then a, c, d.
            if (flip) {
                out.tris.insert(out.tris.end(), {a, c, b, a, d, c});
            } else {
                out.tris.insert(out.tris.end(), {a, b, c, a, c, d});
            }
        }
    }
}

} // namespace

void pipes_build_teapot(PipeMesh& out) {
    out.verts.clear();
    out.tris.clear();
    constexpr std::array<std::array<float, 2>, 4> kQuadrants = {
        {{1.0f, 1.0f}, {-1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, -1.0f}}};
    for (int patch = 0; patch < kTeapotPatches; patch++) {
        const size_t copies = patch < kTeapotQuadrantPatches ? 4 : 2;
        for (size_t q = 0; q < copies; q++) {
            // Handle and spout copies are the first two quadrants: as given, and mirrored in y.
            const auto& signs = patch < kTeapotQuadrantPatches ? kQuadrants[q] : kQuadrants[q * 2];
            add_patch(patch_points(patch, signs[0], signs[1]), out);
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
