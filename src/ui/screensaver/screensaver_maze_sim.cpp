// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_maze_sim.h"

#include "screensaver_buffer.h"
#include "screensaver_fast_math.h"
#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"
#include "thumbnail_downscale.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

using helix::ui::screensaver::random_below;
using helix::ui::screensaver::random_int;
using helix::ui::screensaver::shuffle_in_place;
using helix::ui::screensaver::smoothstep01;

namespace helix::ui {

namespace {

// Texture and sprite slots.
constexpr size_t kTexBrick = 0;
constexpr size_t kTexFloor = 1;
constexpr size_t kTexCeil = 2;
/// The first picture; each picture after it takes the next slot.
constexpr size_t kTexPicture0 = 3;

constexpr size_t kSprLogo = 0;
constexpr size_t kSprStart = 1;
constexpr size_t kSprExit = 2;
constexpr size_t kSprToolhead = 3;
constexpr size_t kSprRock0 = 4;
constexpr size_t kSprCount = kSprRock0 + MazeRenderer::kRockFrames;

constexpr int kTex = MazeRenderer::kTex;

/// Logos turn once every this many ms; rocks show a new rotation frame this often.
constexpr uint32_t kSpinPeriodMs = 3000;
constexpr uint32_t kRockFrameMs = 90;

struct SpriteStyle {
    float size;     ///< height, tiles
    float center_z; ///< height of the sprite's centre above the floor, tiles (eye is 0.5)
    bool spins;
    bool screen_door; ///< every other pixel left out, so it reads as translucent
    /// Nearer than this along the view (tiles) the sprite is not drawn. The walk passes
    /// through logos and rocks, which would otherwise fill the screen as a blown-up mosaic.
    float near;
};

SpriteStyle style_of(MazeItemKind kind) {
    switch (kind) {
    case MazeItemKind::START:
        return {0.7f, 0.5f, false, false, 0.15f};
    case MazeItemKind::EXIT:
        return {0.6f, 0.5f, false, true, 0.15f};
    case MazeItemKind::LOGO:
        return {0.55f, 0.5f, true, false, 0.4f};
    case MazeItemKind::ROCK:
        return {0.4f, 0.5f, false, false, 0.4f};
    case MazeItemKind::TOOLHEAD:
        return {0.35f, 0.175f, false, false, 0.15f};
    }
    return {0.5f, 0.5f, false, false, 0.15f};
}

/// Average colour and coverage of the image over the box [x0, x1) x [y0, y1), in image pixels.
struct Rgba {
    Rgb c;
    float a = 0.0f;
};

Rgba sample_box(const MazeImage& img, float x0, float y0, float x1, float y1) {
    const int ix0 = std::clamp(static_cast<int>(std::floor(x0)), 0, img.w - 1);
    const int iy0 = std::clamp(static_cast<int>(std::floor(y0)), 0, img.h - 1);
    const int ix1 = std::clamp(static_cast<int>(std::ceil(x1)), ix0 + 1, img.w);
    const int iy1 = std::clamp(static_cast<int>(std::ceil(y1)), iy0 + 1, img.h);
    float r = 0, g = 0, b = 0, a = 0;
    int n = 0;
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            const uint32_t p = img.argb[static_cast<size_t>(y) * img.w + x];
            const float pa = static_cast<float>(p >> 24) / 255.0f;
            r += static_cast<float>(p >> 16 & 0xFF) * pa;
            g += static_cast<float>(p >> 8 & 0xFF) * pa;
            b += static_cast<float>(p & 0xFF) * pa;
            a += pa;
            n++;
        }
    }
    Rgba out;
    if (a > 0.0f) {
        out.c = {to_u8(r / a), to_u8(g / a), to_u8(b / a)};
    }
    out.a = n > 0 ? a / static_cast<float>(n) : 0.0f;
    return out;
}

/// A kTex x kTex image being drawn, before packing: colour and whether the texel draws.
struct Canvas64 {
    Rgb c[kTex][kTex]; // [u][v]
    bool on[kTex][kTex];

    void clear() {
        std::memset(on, 0, sizeof(on));
    }
    void set(int u, int v, Rgb col) {
        if (u >= 0 && v >= 0 && u < kTex && v < kTex) {
            c[u][v] = col;
            on[u][v] = true;
        }
    }
};

/// Fits `img` into the box (bx, by, bw, bh) of `dst`, centred with its aspect kept, and
/// draws the texels it covers at least half; `matte` set blends edges over that colour.
void fit_image(Canvas64& dst, const MazeImage& img, int bx, int by, int bw, int bh,
               const Rgb* matte) {
    const float k = std::min(static_cast<float>(bw) / img.w, static_cast<float>(bh) / img.h);
    const float dw = img.w * k;
    const float dh = img.h * k;
    const float ox = bx + (bw - dw) * 0.5f;
    const float oy = by + (bh - dh) * 0.5f;
    for (int v = by; v < by + bh; v++) {
        for (int u = bx; u < bx + bw; u++) {
            const float sx = (u - ox) / k;
            const float sy = (v - oy) / k;
            if (sx + 1.0f / k <= 0.0f || sy + 1.0f / k <= 0.0f || sx >= img.w || sy >= img.h) {
                continue;
            }
            const Rgba s = sample_box(img, sx, sy, sx + 1.0f / k, sy + 1.0f / k);
            if (matte != nullptr) {
                dst.set(u, v, mix_rgb(*matte, s.c, s.a));
            } else if (s.a >= 0.5f) {
                dst.set(u, v, s.c);
            }
        }
    }
}

/// A Helix-like mark for when the icon cannot be decoded: two strands winding round an axis.
void draw_fallback_mark(Canvas64& dst) {
    for (int v = 4; v < 60; v++) {
        const float phase = (v - 4) / 56.0f * 3.0f * kSaverPi;
        const float s = std::sin(phase);
        const float depth = std::cos(phase);
        const int ua = static_cast<int>(32.0f + 18.0f * s);
        const int ub = static_cast<int>(32.0f - 18.0f * s);
        const Rgb a = scale_rgb_by({0, 170, 255}, 0.7f + 0.3f * depth);
        const Rgb b = scale_rgb_by({235, 240, 255}, 0.7f - 0.3f * depth);
        // The strand in front draws last.
        for (int pass = 0; pass < 2; pass++) {
            const bool a_front = depth >= 0.0f;
            const bool draw_a = (pass == 1) == a_front;
            for (int d = -2; d <= 2; d++) {
                dst.set((draw_a ? ua : ub) + d, v, draw_a ? a : b);
            }
        }
    }
}

/// One frame of a slowly tumbling gray icosahedron, flat shaded.
void draw_rock(Canvas64& dst, int frame) {
    static constexpr float T = 1.6180340f;
    static constexpr float kVerts[12][3] = {
        {-1, T, 0},  {1, T, 0},  {-1, -T, 0}, {1, -T, 0}, {0, -1, T},  {0, 1, T},
        {0, -1, -T}, {0, 1, -T}, {T, 0, -1},  {T, 0, 1},  {-T, 0, -1}, {-T, 0, 1},
    };
    static constexpr int kFaces[20][3] = {
        {0, 11, 5},  {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
        {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
        {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1},
    };
    const float theta = 2.0f * kSaverPi * static_cast<float>(frame) / MazeRenderer::kRockFrames;
    const float tilt = 0.5f;
    float p[12][3];
    for (int i = 0; i < 12; i++) {
        const float x = kVerts[i][0] * std::cos(theta) + kVerts[i][2] * std::sin(theta);
        const float z = -kVerts[i][0] * std::sin(theta) + kVerts[i][2] * std::cos(theta);
        const float y = kVerts[i][1];
        p[i][0] = x;
        p[i][1] = y * std::cos(tilt) - z * std::sin(tilt);
        p[i][2] = y * std::sin(tilt) + z * std::cos(tilt);
    }
    const float radius = 28.0f / std::sqrt(1.0f + T * T);
    const float light[3] = {-0.40f, 0.60f, 0.69f};
    for (const auto& f : kFaces) {
        const float* a = p[f[0]];
        const float* b = p[f[1]];
        const float* c = p[f[2]];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
        const float centre[3] = {a[0] + b[0] + c[0], a[1] + b[1] + c[1], a[2] + b[2] + c[2]};
        if (n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] < 0.0f) {
            n[0] = -n[0];
            n[1] = -n[1];
            n[2] = -n[2];
        }
        // The solid is convex, so the faces turned toward the viewer never overlap.
        if (n[2] <= 0.0f) {
            continue;
        }
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float lambert =
            std::max(0.0f, (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / len);
        const Rgb shade = scale_rgb_by({150, 150, 156}, 0.35f + 0.75f * lambert);
        const float ax = 32 + a[0] * radius, ay = 32 - a[1] * radius;
        const float bx = 32 + b[0] * radius, by = 32 - b[1] * radius;
        const float cx = 32 + c[0] * radius, cy = 32 - c[1] * radius;
        const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (std::fabs(area) < 1e-6f) {
            continue;
        }
        const int u0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
        const int u1 = std::min(kTex - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
        const int v0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
        const int v1 = std::min(kTex - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
        for (int v = v0; v <= v1; v++) {
            for (int u = u0; u <= u1; u++) {
                const float px = u + 0.5f;
                const float py = v + 0.5f;
                const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                    dst.set(u, v, shade);
                }
            }
        }
    }
}

void draw_smiley(Canvas64& dst) {
    for (int v = 0; v < kTex; v++) {
        for (int u = 0; u < kTex; u++) {
            const float dx = u + 0.5f - 32.0f;
            const float dy = v + 0.5f - 32.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > 29.0f) {
                continue;
            }
            Rgb c = d > 27.0f ? Rgb{120, 90, 10} : Rgb{250, 222, 40};
            const float ex = std::fabs(dx) - 9.0f;
            const float ey = dy + 7.0f;
            if ((ex * ex) / 9.0f + (ey * ey) / 30.0f <= 1.0f) {
                c = {20, 20, 20};
            }
            const float my = dy + 2.0f;
            const float md = std::sqrt(dx * dx + my * my);
            if (md >= 15.0f && md <= 19.0f && my > 6.0f) {
                c = {20, 20, 20};
            }
            dst.set(u, v, c);
        }
    }
}

void draw_toolhead(Canvas64& dst) {
    for (int v = 4; v < 44; v++) {
        for (int u = 14; u < 50; u++) {
            const bool edge = u == 14 || u == 49 || v == 4 || v == 43;
            dst.set(u, v, edge ? Rgb{60, 62, 66} : Rgb{115, 120, 126});
        }
    }
    for (int v = 10; v < 35; v++) {
        for (int u = 20; u < 45; u++) {
            const float dx = u + 0.5f - 32.0f;
            const float dy = v + 0.5f - 22.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= 12.0f) {
                const bool blade = std::fmod(std::atan2(dy, dx) + kSaverPi, kSaverPi / 3.0f) < 0.5f;
                dst.set(u, v,
                        d <= 3.0f ? Rgb{30, 30, 34}
                        : blade   ? Rgb{40, 90, 160}
                                  : Rgb{25, 55, 105});
            }
        }
    }
    for (int v = 44; v < 54; v++) {
        for (int u = 24; u < 40; u++) {
            dst.set(u, v, Rgb{190, 110, 50});
        }
    }
    for (int v = 54; v < 62; v++) {
        const int half = 4 - (v - 54) / 3;
        for (int u = 32 - half; u < 32 + half; u++) {
            dst.set(u, v, Rgb{220, 180, 60});
        }
    }
}

/// Small integer hash, for per-brick and per-plank variation that needs no random state.
uint32_t hash2(int a, int b) {
    uint32_t h = static_cast<uint32_t>(a) * 374761393u + static_cast<uint32_t>(b) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float hash_unit(int a, int b) {
    return static_cast<float>(hash2(a, b) & 0xFFFF) / 65535.0f;
}

Rgb brick_texel(int u, int v) {
    const int row = v / 16;
    const int shifted = u + (row % 2) * 16;
    const int col = shifted / 32;
    if (v % 16 < 2 || shifted % 32 < 2) {
        return scale_rgb_by({170, 165, 155}, 0.9f + 0.2f * hash_unit(u, v));
    }
    const float brick = 0.8f + 0.35f * hash_unit(col, row);
    const float grit = 0.9f + 0.2f * hash_unit(u * 7, v * 3);
    return scale_rgb_by({150, 58, 40}, brick * grit);
}

Rgb floor_texel(int x, int y) {
    const int plank = y / 16;
    if (y % 16 == 0 || x == static_cast<int>(hash2(plank, 1) % kTex)) {
        return {50, 32, 18};
    }
    const float grain = std::sin((x + plank * 13) * 0.35f + std::sin(y * 0.9f) * 1.5f);
    const float tone = 0.85f + 0.25f * hash_unit(plank, 2) + 0.08f * grain;
    return scale_rgb_by({135, 88, 48}, tone * (0.94f + 0.12f * hash_unit(x, y)));
}

Rgb ceiling_texel(int x, int y) {
    if (x % 32 == 0 || y % 32 == 0) {
        return {140, 140, 134};
    }
    const float speck = hash_unit(x * 5, y * 11);
    return speck > 0.93f ? Rgb{150, 148, 140}
                         : scale_rgb_by({205, 202, 192}, 0.95f + 0.05f * speck);
}

template <typename Px>
void blit_rows(const std::vector<Px>& buf, uint32_t iw, uint32_t ih,
               const std::vector<int>& src_rows, uint32_t scale, Px black, std::vector<Px>& row,
               FrameTarget& out) {
    for (uint32_t iy = 0; iy < ih; iy++) {
        const uint32_t oy0 = iy * scale;
        if (oy0 >= out.h) {
            break;
        }
        const int src = src_rows[iy];
        if (src < 0) {
            std::fill(row.begin(), row.begin() + out.w, black);
        } else {
            const Px* from = buf.data() + src;
            uint32_t ox = 0;
            for (uint32_t ix = 0; ix < iw && ox < out.w; ix++) {
                const Px v = from[static_cast<size_t>(ix) * ih];
                for (uint32_t k = 0; k < scale && ox < out.w; k++) {
                    row[ox++] = v;
                }
            }
        }
        for (uint32_t k = 0; k < scale && oy0 + k < out.h; k++) {
            std::memcpy(out.data + static_cast<size_t>(oy0 + k) * out.stride, row.data(),
                        out.w * sizeof(Px));
        }
    }
}

/// A sprite texel that draws nothing: a value no XRGB8888 pixel takes, and for RGB565 pure
/// magenta, which pack_sprite() nudges any real texel off.
template <typename Px> constexpr Px sprite_key() {
    if constexpr (sizeof(Px) == 2) {
        return 0xF81F;
    } else {
        return 0x00FFFFFFu;
    }
}

template <typename Px> Px pack_sprite(Rgb c, PixelFormat format) {
    const auto v = static_cast<Px>(PixelWriter::pack(c, format));
    return v == sprite_key<Px>() ? static_cast<Px>(v ^ 1u) : v;
}

/// Fog brightness of band `b`: kMazeFogOne down to the farthest fog in even steps.
uint32_t band_k(int b) {
    const auto far_k = static_cast<float>(maze_fog_k(1e9f));
    const float step = (static_cast<float>(kMazeFogOne) - far_k) / (kMazeFogBands - 1);
    return kMazeFogOne - static_cast<uint32_t>(std::lround(static_cast<float>(b) * step));
}

/// The band whose brightness is nearest `k`.
uint8_t nearest_band(uint32_t k) {
    int best = 0;
    for (int b = 1; b < kMazeFogBands; b++) {
        const auto dk = [k](int band) {
            return std::abs(static_cast<int>(band_k(band)) - static_cast<int>(k));
        };
        if (dk(b) < dk(best)) {
            best = b;
        }
    }
    return static_cast<uint8_t>(best);
}

} // namespace

MazeImage reduce_maze_image(const MazeImage& src, int max_side) {
    if (src.empty() || max_side <= 0) {
        return {};
    }
    if (src.w <= max_side && src.h <= max_side) {
        return src;
    }
    const ThumbnailDims fit = fit_thumbnail(src.w, src.h, max_side, max_side);
    MazeImage out;
    out.w = fit.w;
    out.h = fit.h;
    out.argb.resize(static_cast<size_t>(out.w) * static_cast<size_t>(out.h));
    const float sx = static_cast<float>(src.w) / static_cast<float>(out.w);
    const float sy = static_cast<float>(src.h) / static_cast<float>(out.h);
    for (int y = 0; y < out.h; y++) {
        for (int x = 0; x < out.w; x++) {
            const Rgba s = sample_box(src, x * sx, y * sy, (x + 1) * sx, (y + 1) * sy);
            const auto a = static_cast<uint32_t>(std::lround(s.a * 255.0f));
            out.argb[static_cast<size_t>(y) * out.w + x] =
                a << 24 | static_cast<uint32_t>(s.c.r) << 16 | static_cast<uint32_t>(s.c.g) << 8 |
                s.c.b;
        }
    }
    return out;
}

uint32_t maze_fog_k(float depth) {
    const float fall = std::exp(-std::max(0.0f, depth - kMazeFogStart) / kMazeFogScale);
    return static_cast<uint32_t>(std::lround(static_cast<float>(kMazeFogOne) *
                                             (kMazeFogFloor + (1.0f - kMazeFogFloor) * fall)));
}

Rgb maze_fog(Rgb c, uint32_t k) {
    const uint32_t m = kMazeFogOne - k;
    auto ch = [k, m](uint8_t v, uint8_t t) {
        return static_cast<uint8_t>((v * k + t * m + 16) >> 5);
    };
    return {ch(c.r, kMazeFogTint.r), ch(c.g, kMazeFogTint.g), ch(c.b, kMazeFogTint.b)};
}

void MazeMap::generate(int cells, std::minstd_rand& rng) {
    size_ = 2 * cells + 1;
    tiles_.assign(static_cast<size_t>(size_) * size_, MazeTile::BRICK);
    auto open = [this](int x, int y) {
        tiles_[static_cast<size_t>(y) * size_ + x] = MazeTile::OPEN;
    };
    stack_.clear();
    stack_.push_back(0);
    open(1, 1);
    while (!stack_.empty()) {
        const int c = stack_.back();
        const int cx = c % cells;
        const int cy = c / cells;
        int options[4];
        int n = 0;
        for (int d = 0; d < 4; d++) {
            const int nx = cx + kHeadingDx[d];
            const int ny = cy + kHeadingDy[d];
            if (nx >= 0 && ny >= 0 && nx < cells && ny < cells &&
                tile(2 * nx + 1, 2 * ny + 1) != MazeTile::OPEN) {
                options[n++] = d;
            }
        }
        if (n == 0) {
            stack_.pop_back();
            continue;
        }
        const int d = options[random_below(rng, n)];
        open(2 * cx + 1 + kHeadingDx[d], 2 * cy + 1 + kHeadingDy[d]);
        const int nx = cx + kHeadingDx[d];
        const int ny = cy + kHeadingDy[d];
        open(2 * nx + 1, 2 * ny + 1);
        stack_.push_back(ny * cells + nx);
    }

    pictures_.assign(tiles_.size(), 0);
    hang_pictures(rng);
    label_walls();
}

void MazeMap::hang_pictures(std::minstd_rand& rng) {
    bool hung[kPictureWalls] = {};
    for (size_t i = 0; i < tiles_.size(); i++) {
        if (tiles_[i] == MazeTile::PICTURE && pictures_[i] < kPictureWalls) {
            hung[pictures_[i]] = true;
        }
    }
    for (int p = 0; p < kPictureWalls; p++) {
        for (int attempt = 0; attempt < 2000 && !hung[p]; attempt++) {
            const int x = random_int(rng, 1, size_ - 2);
            const int y = random_int(rng, 1, size_ - 2);
            if (tile(x, y) != MazeTile::BRICK) {
                continue;
            }
            const bool faces_open =
                !wall(x - 1, y) || !wall(x + 1, y) || !wall(x, y - 1) || !wall(x, y + 1);
            if (faces_open) {
                tiles_[index(x, y)] = MazeTile::PICTURE;
                pictures_[index(x, y)] = static_cast<uint8_t>(p);
                hung[p] = true;
            }
        }
    }
}

void MazeMap::label_walls() {
    boundary_.assign(tiles_.size(), 0);
    stack_.clear();
    auto push = [this](int x, int y) {
        if (x >= 0 && y >= 0 && x < size_ && y < size_ && wall(x, y) &&
            boundary_[index(x, y)] == 0) {
            boundary_[index(x, y)] = 1;
            stack_.push_back(static_cast<int>(index(x, y)));
        }
    };
    for (int i = 0; i < size_; i++) {
        push(i, 0);
        push(i, size_ - 1);
        push(0, i);
        push(size_ - 1, i);
    }
    while (!stack_.empty()) {
        const int t = stack_.back();
        stack_.pop_back();
        const int x = t % size_;
        const int y = t / size_;
        for (int d = 0; d < 4; d++) {
            push(x + kHeadingDx[d], y + kHeadingDy[d]);
        }
    }
}

int MazeMap::cell_passages(int c) const {
    const int n = cells();
    int count = 0;
    for (int d = 0; d < 4; d++) {
        count += passage(c % n, c / n, d) ? 1 : 0;
    }
    return count;
}

bool MazeMap::walled_by_boundary(int c) const {
    const int n = cells();
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int x = 2 * (c % n) + 1 + dx;
            const int y = 2 * (c / n) + 1 + dy;
            if (wall(x, y) && !boundary_wall(x, y)) {
                return false;
            }
        }
    }
    return true;
}

bool MazeMap::open_wall(int c, int heading, int keep_a, int keep_b) {
    const int n = cells();
    const int cx = c % n;
    const int cy = c / n;
    const int nx = cx + kHeadingDx[heading];
    const int ny = cy + kHeadingDy[heading];
    if (nx < 0 || ny < 0 || nx >= n || ny >= n) {
        return false;
    }
    const int other = ny * n + nx;
    if (c == keep_a || c == keep_b || other == keep_a || other == keep_b) {
        return false;
    }
    const int x = 2 * cx + 1 + kHeadingDx[heading];
    const int y = 2 * cy + 1 + kHeadingDy[heading];
    if (!wall(x, y)) {
        return false;
    }
    const MazeTile was = tiles_[index(x, y)];
    const uint8_t picture = pictures_[index(x, y)];
    tiles_[index(x, y)] = MazeTile::OPEN;
    pictures_[index(x, y)] = 0;
    label_walls();
    if (walled_by_boundary(keep_a) && walled_by_boundary(keep_b)) {
        return true;
    }
    tiles_[index(x, y)] = was;
    pictures_[index(x, y)] = picture;
    label_walls();
    return false;
}

bool MazeMap::open_room(int cx0, int cy0, int cx1, int cy1, int keep_a, int keep_b) {
    // Every tile from the first cell's to the last cell's, pillars included, so the room
    // holds no one-tile island.
    scratch_.clear();
    for (int y = 2 * cy0 + 1; y <= 2 * cy1 + 1; y++) {
        for (int x = 2 * cx0 + 1; x <= 2 * cx1 + 1; x++) {
            const size_t i = index(x, y);
            if (tiles_[i] != MazeTile::OPEN) {
                // Tile, its old content and its picture, to put back.
                scratch_.push_back(static_cast<int>(i));
                scratch_.push_back(static_cast<int>(tiles_[i]));
                scratch_.push_back(pictures_[i]);
                tiles_[i] = MazeTile::OPEN;
                pictures_[i] = 0;
            }
        }
    }
    label_walls();
    if (walled_by_boundary(keep_a) && walled_by_boundary(keep_b)) {
        return true;
    }
    for (size_t j = 0; j + 2 < scratch_.size(); j += 3) {
        const auto i = static_cast<size_t>(scratch_[j]);
        tiles_[i] = static_cast<MazeTile>(scratch_[j + 1]);
        pictures_[i] = static_cast<uint8_t>(scratch_[j + 2]);
    }
    label_walls();
    return false;
}

void MazeMap::open_up(int keep_a, int keep_b, std::minstd_rand& rng) {
    const int n = cells();
    if (n < 2) {
        return;
    }
    auto kept = [keep_a, keep_b](int c) { return c == keep_a || c == keep_b; };

    // Braid: shuffle the dead ends, then open a wall out of a share of them, toward another
    // dead end where one is next door, so one opening can close two.
    scratch_.clear();
    for (int c = 0; c < n * n; c++) {
        if (!kept(c) && cell_passages(c) == 1) {
            scratch_.push_back(c);
        }
    }
    shuffle_in_place(rng, scratch_.begin(), scratch_.end());
    const auto braids = static_cast<int>(std::lround(kBraidShare * scratch_.size()));
    int braided = 0;
    for (size_t i = 0; i < scratch_.size() && braided < braids; i++) {
        const int c = scratch_[i];
        if (cell_passages(c) != 1) {
            continue;
        }
        const int first = random_below(rng, 4);
        bool opened = false;
        // Two passes: toward a neighbouring dead end first, then toward anything.
        for (int pass = 0; pass < 2 && !opened; pass++) {
            for (int j = 0; j < 4 && !opened; j++) {
                const int d = (first + j) & 3;
                const int nx = c % n + kHeadingDx[d];
                const int ny = c / n + kHeadingDy[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) {
                    continue;
                }
                if (pass == 0 && cell_passages(ny * n + nx) != 1) {
                    continue;
                }
                opened = open_wall(c, d, keep_a, keep_b);
            }
        }
        braided += opened ? 1 : 0;
    }

    int extra = 0;
    for (int attempt = 0; attempt < 200 && extra < kExtraOpenings; attempt++) {
        extra += open_wall(random_below(rng, n * n), random_below(rng, 4), keep_a, keep_b) ? 1 : 0;
    }

    // Rooms of 2x2, 2x3 or 3x2 cells, apart from each other and from the kept cells.
    struct Room {
        int x0, y0, x1, y1;
    };
    Room rooms[kRoomsMin + 1] = {};
    int placed = 0;
    const int wanted = random_int(rng, kRoomsMin, kRoomsMin + 1);
    for (int r = 0; r < wanted; r++) {
        for (int attempt = 0; attempt < 50; attempt++) {
            const int shape = random_below(rng, 3);
            const int w = shape == 2 ? 3 : 2;
            const int h = shape == 1 ? 3 : 2;
            if (w > n || h > n) {
                break;
            }
            const Room room{random_below(rng, n - w + 1), random_below(rng, n - h + 1), 0, 0};
            const Room box{room.x0, room.y0, room.x0 + w - 1, room.y0 + h - 1};
            bool clear = true;
            for (int c : {keep_a, keep_b}) {
                if (c % n >= box.x0 && c % n <= box.x1 && c / n >= box.y0 && c / n <= box.y1) {
                    clear = false;
                }
            }
            for (int o = 0; o < placed; o++) {
                if (box.x0 <= rooms[o].x1 && rooms[o].x0 <= box.x1 && box.y0 <= rooms[o].y1 &&
                    rooms[o].y0 <= box.y1) {
                    clear = false;
                }
            }
            if (clear && open_room(box.x0, box.y0, box.x1, box.y1, keep_a, keep_b)) {
                rooms[placed++] = box;
                break;
            }
        }
    }
    hang_pictures(rng);
}

void MazeMap::release() {
    release_buffers(tiles_, stack_, scratch_, pictures_, boundary_);
    size_ = 0;
}

size_t MazeMap::heap_bytes() const {
    return buffer_bytes(tiles_, stack_, scratch_, pictures_, boundary_);
}

void MazeMap::set_tiles(int size, std::vector<MazeTile> tiles, std::vector<uint8_t> pictures) {
    size_ = size;
    tiles_ = std::move(tiles);
    pictures_ = std::move(pictures);
    pictures_.resize(tiles_.size(), 0);
    label_walls();
}

MazeTile MazeMap::tile(int x, int y) const {
    if (x < 0 || y < 0 || x >= size_ || y >= size_) {
        return MazeTile::BRICK;
    }
    return tiles_[index(x, y)];
}

uint8_t MazeMap::picture_index(int x, int y) const {
    if (tile(x, y) != MazeTile::PICTURE || x < 0 || y < 0 || x >= size_ || y >= size_) {
        return 0;
    }
    return pictures_[index(x, y)];
}

bool MazeMap::boundary_wall(int x, int y) const {
    if (x < 0 || y < 0 || x >= size_ || y >= size_) {
        return true;
    }
    return boundary_[index(x, y)] != 0;
}

bool MazeMap::passage(int cx, int cy, int heading) const {
    return !wall(2 * cx + 1 + kHeadingDx[heading], 2 * cy + 1 + kHeadingDy[heading]);
}

namespace {

/// Next heading out of cell (cx, cy) for a walk that came in heading `d_in`: toward the hand
/// when that side is open, else straight on, else away, else back.
int next_heading(const MazeMap& map, int cx, int cy, int d_in, bool left_handed) {
    const int hand = left_handed ? 3 : 1;
    const int order[4] = {(d_in + hand) & 3, d_in, (d_in + 4 - hand) & 3, (d_in + 2) & 3};
    for (int d : order) {
        if (map.passage(cx, cy, d)) {
            return d;
        }
    }
    return (d_in + 2) & 3;
}

/// Fraction of a move covered at time fraction `u` while slowing to rest at the end, from the
/// speed a linear move has.
float ease_out(float u) {
    return u * (2.0f - u);
}

/// Fraction of a segment covered at time fraction `u`: linear, or eased out.
float covered(float u, bool eases_out) {
    return eases_out ? ease_out(u) : u;
}

float cell_centre(int c) {
    return 2.0f * static_cast<float>(c) + 1.5f;
}

float heading_angle(float quarters) {
    return quarters * (kSaverPi * 0.5f);
}

} // namespace

void MazeWalker::reset(int cx, int cy, int heading, bool left_handed, uint32_t move_ms,
                       uint32_t turn_ms) {
    cx_ = cx;
    cy_ = cy;
    d_in_ = heading & 3;
    d_out_ = d_in_;
    left_handed_ = left_handed;
    move_ms_ = std::max<uint32_t>(1, move_ms);
    turn_ms_ = std::max<uint32_t>(1, turn_ms);
    kind_ = Segment::START;
    spin_quarters_ = 0;
    planned_ = false;
    stopped_ = false;
    arrived_ = true;
    ease_out_ = false;
    progress_ms_ = 0;
    duration_ms_ = 1;
    arrive_ms_ = 0;
    moves_ = 0;
    recoveries_ = 0;
    stop_x_ = -1;
    stop_y_ = -1;
    std::fill(seen_.begin(), seen_.end(), 0);
    path_.clear();
    path_pos_ = 0;
}

void MazeWalker::set_left_handed(bool left) {
    left_handed_ = left;
    std::fill(seen_.begin(), seen_.end(), 0);
    path_.clear();
    path_pos_ = 0;
}

void MazeWalker::reserve(int cells) {
    const auto states = static_cast<size_t>(cells) * static_cast<size_t>(cells) * 4;
    seen_.assign(states, 0);
    prev_.assign(states, -1);
    queue_.reserve(states);
    path_.reserve(states);
}

void MazeWalker::release() {
    release_buffers(seen_, prev_, queue_, path_);
    path_pos_ = 0;
}

size_t MazeWalker::heap_bytes() const {
    return buffer_bytes(seen_, prev_, queue_, path_);
}

int MazeWalker::track_state(const MazeMap& map) {
    // Without a stop cell the walk has nowhere to get to, and circling is fine.
    if (stop_x_ < 0) {
        return -1;
    }
    const int n = map.cells();
    const auto states = static_cast<size_t>(n) * static_cast<size_t>(n) * 4;
    if (seen_.size() != states) {
        reserve(n);
    }
    if (path_pos_ < path_.size()) {
        return path_[path_pos_++];
    }
    if (!path_.empty()) {
        // The recovery walk has reached the boundary wall: follow it from here.
        path_.clear();
        path_pos_ = 0;
        std::fill(seen_.begin(), seen_.end(), 0);
    }
    const int s = (cy_ * n + cx_) * 4 + d_in_;
    if (seen_[static_cast<size_t>(s)] == 0) {
        seen_[static_cast<size_t>(s)] = 1;
        return -1;
    }

    // A state seen before: the hand is on an island. Search the states breadth first for the
    // nearest one whose hand-side wall is joined to the outer ring.
    const int hand = left_handed_ ? 3 : 1;
    std::fill(prev_.begin(), prev_.end(), -1);
    queue_.clear();
    queue_.push_back(static_cast<int16_t>(s));
    prev_[static_cast<size_t>(s)] = static_cast<int16_t>(s);
    int goal = -1;
    for (size_t qi = 0; qi < queue_.size() && goal < 0; qi++) {
        const int u = queue_[qi];
        const int c = u / 4;
        for (int e = 0; e < 4 && goal < 0; e++) {
            if (!map.passage(c % n, c / n, e)) {
                continue;
            }
            const int nc = (c / n + kHeadingDy[e]) * n + c % n + kHeadingDx[e];
            const int v = nc * 4 + e;
            if (prev_[static_cast<size_t>(v)] >= 0) {
                continue;
            }
            prev_[static_cast<size_t>(v)] = static_cast<int16_t>(u);
            queue_.push_back(static_cast<int16_t>(v));
            const int side = (e + hand) & 3;
            if (map.boundary_wall(2 * (nc % n) + 1 + kHeadingDx[side],
                                  2 * (nc / n) + 1 + kHeadingDy[side])) {
                goal = v;
            }
        }
    }
    std::fill(seen_.begin(), seen_.end(), 0);
    if (goal < 0) {
        seen_[static_cast<size_t>(s)] = 1;
        return -1;
    }
    path_.clear();
    for (int v = goal; v != s; v = prev_[static_cast<size_t>(v)]) {
        path_.push_back(static_cast<uint8_t>(v & 3));
    }
    std::reverse(path_.begin(), path_.end());
    recoveries_++;
    path_pos_ = 1;
    return path_[0];
}

void MazeWalker::plan_start(const MazeMap& map) {
    // Turn on the spot to the way out, then speed up to walking pace by the doorway.
    const int tracked = track_state(map);
    d_out_ = tracked >= 0 ? tracked : next_heading(map, cx_, cy_, d_in_, left_handed_);
    const int diff = (d_out_ - d_in_ + 4) & 3;
    spin_quarters_ = diff == 3 ? -1 : diff == 2 ? (left_handed_ ? 2 : -2) : diff;
    kind_ = Segment::START;
    duration_ms_ = turn_ms_ * static_cast<uint32_t>(std::abs(spin_quarters_)) + move_ms_;
    arrived_ = true;
    progress_ms_ = 0;
    planned_ = true;
}

void MazeWalker::plan_segment(const MazeMap& map) {
    const int tracked = track_state(map);
    d_out_ = tracked >= 0 ? tracked : next_heading(map, cx_, cy_, d_in_, left_handed_);
    const int diff = (d_out_ - d_in_ + 4) & 3;
    ease_out_ = cx_ + kHeadingDx[d_out_] == stop_x_ && cy_ + kHeadingDy[d_out_] == stop_y_;
    progress_ms_ = 0;
    arrived_ = false;
    if (diff == 2) {
        kind_ = Segment::DEAD_END;
        spin_quarters_ = left_handed_ ? 2 : -2;
        // In and out at walking pace over one tile each, slowing to rest and setting off.
        duration_ms_ = 2 * move_ms_ + 2 * turn_ms_;
        arrive_ms_ = move_ms_;
        return;
    }
    kind_ = diff == 0 ? Segment::STRAIGHT : Segment::TURN;
    // Two tiles straight, or a quarter arc of radius one tile, at the same speed.
    const float length = diff == 0 ? 2.0f : kSaverPi * 0.5f;
    const float base = static_cast<float>(move_ms_) * length * 0.5f;
    duration_ms_ =
        std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(base * (ease_out_ ? 2.0f : 1.0f))));
    // The middle of the path: half of it covered.
    const float u_mid = ease_out_ ? 1.0f - std::sqrt(0.5f) : 0.5f;
    arrive_ms_ = static_cast<uint32_t>(std::lround(u_mid * static_cast<float>(duration_ms_)));
}

uint32_t MazeWalker::advance(const MazeMap& map, uint32_t dt_ms, WalkEvent& event) {
    event = WalkEvent::NONE;
    if (stopped_) {
        return dt_ms;
    }
    if (!planned_) {
        plan_start(map);
    }
    uint32_t used = 0;
    while (used < dt_ms) {
        const uint32_t available = dt_ms - used;
        if (!arrived_) {
            const uint32_t to_middle = arrive_ms_ - std::min(arrive_ms_, progress_ms_);
            if (available >= to_middle) {
                progress_ms_ += to_middle;
                used += to_middle;
                arrived_ = true;
                moves_++;
                event = WalkEvent::ARRIVED;
                return used;
            }
        }
        const uint32_t remaining = duration_ms_ - progress_ms_;
        if (available < remaining) {
            progress_ms_ += available;
            used += available;
            break;
        }
        used += remaining;
        cx_ += kHeadingDx[d_out_];
        cy_ += kHeadingDy[d_out_];
        d_in_ = d_out_;
        event = WalkEvent::ENTERED;
        if (cx_ == stop_x_ && cy_ == stop_y_) {
            stopped_ = true;
            progress_ms_ = 0;
            return used;
        }
        plan_segment(map);
        return used;
    }
    return used;
}

MazePose MazeWalker::pose() const {
    const float cx = cell_centre(cx_);
    const float cy = cell_centre(cy_);
    const float in_x = static_cast<float>(kHeadingDx[d_in_]);
    const float in_y = static_cast<float>(kHeadingDy[d_in_]);
    const float out_x = static_cast<float>(kHeadingDx[d_out_]);
    const float out_y = static_cast<float>(kHeadingDy[d_out_]);
    MazePose p;
    if (stopped_) {
        // At the doorway into this cell, looking in.
        p.x = cx - in_x;
        p.y = cy - in_y;
        p.angle = heading_angle(static_cast<float>(d_in_));
        return p;
    }
    const float t = static_cast<float>(progress_ms_);
    switch (kind_) {
    case Segment::START: {
        const float spin_ms =
            static_cast<float>(turn_ms_) * static_cast<float>(std::abs(spin_quarters_));
        p.x = cx;
        p.y = cy;
        if (!planned_ || t < spin_ms) {
            const float f = spin_ms > 0.0f ? smoothstep01(t / spin_ms) : 0.0f;
            p.angle = heading_angle(static_cast<float>(d_in_) + f * spin_quarters_);
            return p;
        }
        const float u = (t - spin_ms) / static_cast<float>(move_ms_);
        p.x += out_x * u * u;
        p.y += out_y * u * u;
        p.angle = heading_angle(static_cast<float>(d_out_));
        return p;
    }
    case Segment::STRAIGHT: {
        const float f = covered(t / static_cast<float>(duration_ms_), ease_out_);
        p.x = cx + in_x * (2.0f * f - 1.0f);
        p.y = cy + in_y * (2.0f * f - 1.0f);
        p.angle = heading_angle(static_cast<float>(d_in_));
        return p;
    }
    case Segment::TURN: {
        // A quarter circle of radius one tile round the pillar on the inside of the turn,
        // from the doorway in to the doorway out.
        const float f = covered(t / static_cast<float>(duration_ms_), ease_out_);
        const float sign = d_out_ == ((d_in_ + 1) & 3) ? 1.0f : -1.0f;
        const float pillar_x = cx - in_x + out_x;
        const float pillar_y = cy - in_y + out_y;
        const float phi = std::atan2(-out_y, -out_x) + sign * f * (kSaverPi * 0.5f);
        p.x = pillar_x + std::cos(phi);
        p.y = pillar_y + std::sin(phi);
        p.angle = heading_angle(static_cast<float>(d_in_) + sign * f);
        return p;
    }
    case Segment::DEAD_END: {
        const auto in_ms = static_cast<float>(move_ms_);
        const float spin_ms = 2.0f * static_cast<float>(turn_ms_);
        if (t < in_ms) {
            const float u = t / in_ms;
            const float d = ease_out(u);
            p.x = cx - in_x + in_x * d;
            p.y = cy - in_y + in_y * d;
            p.angle = heading_angle(static_cast<float>(d_in_));
        } else if (t < in_ms + spin_ms) {
            p.x = cx;
            p.y = cy;
            p.angle = heading_angle(static_cast<float>(d_in_) +
                                    smoothstep01((t - in_ms) / spin_ms) * spin_quarters_);
        } else {
            const float u = (t - in_ms - spin_ms) / in_ms;
            p.x = cx + out_x * u * u;
            p.y = cy + out_y * u * u;
            p.angle = heading_angle(static_cast<float>(d_out_));
        }
        return p;
    }
    }
    return p;
}

void MazeRenderer::init(uint32_t w, uint32_t h, PixelFormat format, const MazeArt& art,
                        size_t level) {
    release();
    format_ = format;
    w_ = w;
    h_ = h;
    zbuf_.assign(w, 0.0f);
    wall_top_.assign(w, 0);
    wall_bottom_.assign(w, 0);
    row_dist_q16_.assign(h, 0);
    src_rows_.assign(h, -1);
    row_band_.assign(h, 0);
    wall_k_.assign(w, static_cast<uint8_t>(kMazeFogOne));
    fog_k_.resize(kMazeFogSlots);
    for (int i = 0; i < kMazeFogSlots; i++) {
        fog_k_[static_cast<size_t>(i)] = static_cast<uint8_t>(
            maze_fog_k((static_cast<float>(i) + 0.5f) / static_cast<float>(kMazeFogStepsPerTile)));
    }
    sprite_order_.reserve(32);
    with_pixel_type(format, [&](auto px) { build<decltype(px)>(art); });
    set_level(level);
}

void MazeRenderer::release() {
    auto free_store = [](auto& st) {
        release_buffers(st.frame, st.textures, st.sprites, st.ceil_rows, st.floor_rows, st.row,
                        st.fog_bands, st.fog_tint);
    };
    free_store(s16_);
    free_store(s32_);
    release_buffers(zbuf_, wall_top_, wall_bottom_, row_dist_q16_, src_rows_, sprite_order_, fog_k_,
                    row_band_, wall_k_);
    w_ = h_ = iw_ = ih_ = 0;
}

size_t MazeRenderer::heap_bytes() const {
    auto store_bytes = [](const auto& s) {
        return buffer_bytes(s.frame, s.textures, s.sprites, s.ceil_rows, s.floor_rows, s.row,
                            s.fog_bands, s.fog_tint);
    };
    return store_bytes(s16_) + store_bytes(s32_) +
           buffer_bytes(zbuf_, wall_top_, wall_bottom_, row_dist_q16_, src_rows_, sprite_order_,
                        fog_k_, row_band_, wall_k_);
}

uint32_t MazeRenderer::internal_pixel(uint32_t x, uint32_t y) const {
    const size_t i = static_cast<size_t>(x) * ih_ + y;
    return format_ == PixelFormat::RGB565 ? s16_.frame[i] : s32_.frame[i];
}

void MazeRenderer::set_level(size_t level) {
    level_ = std::min(level, kMazeLevelCount - 1);
    if (w_ == 0 || h_ == 0) {
        return;
    }
    const int32_t s = kMazeLevels[level_].scale;
    iw_ = static_cast<uint32_t>(scaled_extent(static_cast<int32_t>(w_), s));
    ih_ = static_cast<uint32_t>(scaled_extent(static_cast<int32_t>(h_), s));
    focal_ = maze_focal(iw_, ih_);
    plane_len_ = 0.5f * static_cast<float>(iw_) / focal_;
    for (uint32_t y = 0; y < ih_; y++) {
        const float dist = row_distance(y);
        row_dist_q16_[y] = static_cast<int32_t>(dist * 65536.0f);
        row_band_[y] = nearest_band(fog_k_at(dist));
    }
    with_pixel_type(format_, [this](auto px) { apply_level<decltype(px)>(); });
}

float MazeRenderer::row_distance(uint32_t y) const {
    // The eye is half a tile above the floor and below the ceiling.
    const float denom = std::fabs(2.0f * static_cast<float>(y) + 1.0f - static_cast<float>(ih_));
    return std::min(focal_ / std::max(denom, 1.0f), 1000.0f);
}

template <typename Px> Px MazeRenderer::shade(Px p, uint32_t k) const {
    const uint32_t* t = store<Px>().fog_tint.data() + 2 * static_cast<size_t>(k);
    if constexpr (sizeof(Px) == 2) {
        // 0x07E0F81F spreads the three fields apart so one multiply scales them all.
        uint32_t x = (p | static_cast<uint32_t>(p) << 16) & 0x07E0F81Fu;
        x = (x * k + t[0]) >> 5 & 0x07E0F81Fu;
        return static_cast<Px>(x | x >> 16);
    } else {
        const uint32_t rb = ((p & 0xFF00FFu) * k + t[0]) >> 5 & 0xFF00FFu;
        const uint32_t g = ((p & 0xFF00u) * k + t[1]) >> 5 & 0xFF00u;
        return 0xFF000000u | rb | g;
    }
}

template <typename Px> void MazeRenderer::build_fog_bands() {
    Store<Px>& st = store<Px>();
    constexpr size_t kTexels = static_cast<size_t>(kTex) * kTex;
    st.fog_bands.resize(2 * kMazeFogBands * kTexels);
    const Px* floor_tex = st.textures.data() + kTexFloor * kTexels;
    const Px* ceil_tex = st.textures.data() + kTexCeil * kTexels;
    for (int b = 0; b < kMazeFogBands; b++) {
        const uint32_t k = band_k(b);
        Px* floor_band = st.fog_bands.data() + static_cast<size_t>(b) * kTexels;
        Px* ceil_band = floor_band + kMazeFogBands * kTexels;
        for (size_t i = 0; i < kTexels; i++) {
            floor_band[i] = shade<Px>(floor_tex[i], k);
            ceil_band[i] = shade<Px>(ceil_tex[i], k);
        }
    }
}

template <typename Px> void MazeRenderer::apply_level() {
    Store<Px>& st = store<Px>();
    // Sized for the level a run starts at. The gate only ever steps down, to a smaller frame,
    // so a running saver never grows it.
    if (st.frame.size() < static_cast<size_t>(iw_) * ih_) {
        st.frame.resize(static_cast<size_t>(iw_) * ih_);
    }
    if (kMazeLevels[level_].textured_floor) {
        if (st.fog_bands.empty()) {
            build_fog_bands<Px>();
        }
        return;
    }
    release_buffer(st.fog_bands);
    for (uint32_t y = 0; y < ih_; y++) {
        const uint32_t k = fog_k_at(row_distance(y));
        st.ceil_rows[y] =
            static_cast<Px>(PixelWriter::pack(maze_fog(kMazeFlatCeiling, k), format_));
        st.floor_rows[y] = static_cast<Px>(PixelWriter::pack(maze_fog(kMazeFlatFloor, k), format_));
    }
}

template <typename Px> void MazeRenderer::build(const MazeArt& art) {
    Store<Px>& st = store<Px>();
    st.ceil_rows.assign(h_, 0);
    st.floor_rows.assign(h_, 0);
    st.row.assign(w_, 0);
    picture_count_ = 0;
    for (const MazeImage& img : art.pictures) {
        picture_count_ += !img.empty() && picture_count_ < kMazeMaxPictures ? 1 : 0;
    }
    st.textures.assign((kTexPicture0 + picture_count_) * kTex * kTex, 0);
    st.sprites.assign(kSprCount * kTex * kTex, 0);
    auto pack = [this](Rgb c) { return static_cast<Px>(PixelWriter::pack(c, format_)); };

    st.fog_tint.assign(2 * (kMazeFogOne + 1), 0);
    for (uint32_t k = 0; k <= kMazeFogOne; k++) {
        // The tint's share plus the rounding half, in the lanes shade() multiplies in.
        const uint32_t m = kMazeFogOne - k;
        uint32_t* t = st.fog_tint.data() + 2 * static_cast<size_t>(k);
        if constexpr (sizeof(Px) == 2) {
            // The tint rounded to 565 levels, not truncated, so far fog matches 8888's.
            auto level = [](uint8_t c, int bits) {
                const uint32_t top = (1u << bits) - 1;
                return std::min<uint32_t>(top, (c * top + 127) / 255);
            };
            t[0] = (level(kMazeFogTint.r, 5) * m + 16) << 11 |
                   (level(kMazeFogTint.g, 6) * m + 16) << 21 | (level(kMazeFogTint.b, 5) * m + 16);
        } else {
            t[0] = (kMazeFogTint.r * m + 16) << 16 | (kMazeFogTint.b * m + 16);
            t[1] = (kMazeFogTint.g * m + 16) << 8;
        }
    }

    auto tex = [&st](size_t slot, int u, int v) -> Px& {
        return st.textures[(slot * kTex + static_cast<size_t>(u)) * kTex + static_cast<size_t>(v)];
    };
    for (int u = 0; u < kTex; u++) {
        for (int v = 0; v < kTex; v++) {
            tex(kTexBrick, u, v) = pack(brick_texel(u, v));
            tex(kTexFloor, u, v) = pack(floor_texel(u, v));
            tex(kTexCeil, u, v) = pack(ceiling_texel(u, v));
        }
    }

    // Heap, not static: the saver holds no memory while it is not running.
    auto pic_owner = std::make_unique<Canvas64>();
    Canvas64& pic = *pic_owner;
    size_t slot = kTexPicture0;
    for (const MazeImage& img : art.pictures) {
        if (img.empty() || slot >= kTexPicture0 + picture_count_) {
            continue;
        }
        // A wooden frame round a matte, the image fitted inside, on the brick.
        pic.clear();
        const Rgb matte{225, 215, 190};
        for (int v = 8; v < 56; v++) {
            for (int u = 10; u < 54; u++) {
                const bool frame = u < 13 || u >= 51 || v < 11 || v >= 53;
                pic.set(u, v, frame ? Rgb{70, 45, 25} : matte);
            }
        }
        fit_image(pic, img, 15, 13, 34, 38, &matte);
        for (int u = 0; u < kTex; u++) {
            for (int v = 0; v < kTex; v++) {
                tex(slot, u, v) = pic.on[u][v] ? pack(pic.c[u][v]) : tex(kTexBrick, u, v);
            }
        }
        slot++;
    }
    pic_owner.reset();

    auto canvas_owner = std::make_unique<Canvas64>();
    auto logo_owner = std::make_unique<Canvas64>();
    Canvas64& canvas = *canvas_owner;
    Canvas64& logo = *logo_owner;
    auto keep = [this, &st, &canvas](size_t slot) {
        Px* dst = st.sprites.data() + slot * kTex * kTex;
        for (int u = 0; u < kTex; u++) {
            for (int v = 0; v < kTex; v++) {
                dst[static_cast<size_t>(u) * kTex + v] =
                    canvas.on[u][v] ? pack_sprite<Px>(canvas.c[u][v], format_) : sprite_key<Px>();
            }
        }
    };

    canvas.clear();
    if (art.logo.empty()) {
        draw_fallback_mark(canvas);
    } else {
        fit_image(canvas, art.logo, 2, 2, kTex - 4, kTex - 4, nullptr);
    }
    logo = canvas;
    keep(kSprLogo);

    canvas.clear();
    for (int v = 10; v < 54; v++) {
        for (int u = 2; u < 62; u++) {
            const bool border = u < 5 || u >= 59 || v < 13 || v >= 51;
            canvas.set(u, v, border ? Rgb{200, 30, 30} : Rgb{20, 30, 70});
        }
    }
    // The logo at half size, centred on the sign.
    for (int v = 0; v < kTex; v += 2) {
        for (int u = 0; u < kTex; u += 2) {
            if (logo.on[u][v]) {
                canvas.set(16 + u / 2, 16 + v / 2, logo.c[u][v]);
            }
        }
    }
    keep(kSprStart);

    canvas.clear();
    draw_smiley(canvas);
    keep(kSprExit);

    canvas.clear();
    draw_toolhead(canvas);
    keep(kSprToolhead);

    for (int f = 0; f < kRockFrames; f++) {
        canvas.clear();
        draw_rock(canvas, f);
        keep(kSprRock0 + static_cast<size_t>(f));
    }
}

template <typename Px> void MazeRenderer::cast_walls(const MazeMap& map, const MazePose& pose) {
    Store<Px>& st = store<Px>();
    const float dir_x = std::cos(pose.angle);
    const float dir_y = std::sin(pose.angle);
    const float plane_x = -dir_y * plane_len_;
    const float plane_y = dir_x * plane_len_;
    const bool textured = kMazeLevels[level_].textured_floor;
    const auto pos_xq = static_cast<int32_t>(pose.x * 65536.0f);
    const auto pos_yq = static_cast<int32_t>(pose.y * 65536.0f);
    // Textured levels hold the floor and ceiling bands; flat ones read neither.
    const Px* floor_bands = st.fog_bands.data();
    const Px* ceil_bands = textured ? floor_bands + kMazeFogBands * kTex * kTex : nullptr;
    const int ih = static_cast<int>(ih_);
    const int max_steps = 4 * std::max(map.size(), 1);

    for (uint32_t x = 0; x < iw_; x++) {
        const float cam = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(iw_) - 1.0f;
        const float rdx = dir_x + plane_x * cam;
        const float rdy = dir_y + plane_y * cam;
        int mx = static_cast<int>(std::floor(pose.x));
        int my = static_cast<int>(std::floor(pose.y));
        const float ddx = rdx == 0.0f ? 1e30f : std::fabs(1.0f / rdx);
        const float ddy = rdy == 0.0f ? 1e30f : std::fabs(1.0f / rdy);
        const int sx = rdx < 0.0f ? -1 : 1;
        const int sy = rdy < 0.0f ? -1 : 1;
        float side_x = rdx < 0.0f ? (pose.x - mx) * ddx : (mx + 1.0f - pose.x) * ddx;
        float side_y = rdy < 0.0f ? (pose.y - my) * ddy : (my + 1.0f - pose.y) * ddy;
        int side = 0;
        for (int i = 0; i < max_steps; i++) {
            if (side_x < side_y) {
                side_x += ddx;
                mx += sx;
                side = 0;
            } else {
                side_y += ddy;
                my += sy;
                side = 1;
            }
            if (map.wall(mx, my)) {
                break;
            }
        }
        const float perp = std::max(side == 0 ? side_x - ddx : side_y - ddy, 1e-3f);
        float wall_x = side == 0 ? pose.y + perp * rdy : pose.x + perp * rdx;
        wall_x -= std::floor(wall_x);
        int tex_u = std::min(static_cast<int>(wall_x * kTex), kTex - 1);
        // The camera plane points right of the view, so the faces whose wall coordinate runs
        // right to left on screen are the ones read backwards.
        if ((side == 0 && rdx < 0.0f) || (side == 1 && rdy > 0.0f)) {
            tex_u = kTex - 1 - tex_u;
        }
        size_t tex = kTexBrick;
        if (picture_count_ > 0 && map.tile(mx, my) == MazeTile::PICTURE) {
            tex = kTexPicture0 + map.picture_index(mx, my) % picture_count_;
        }
        uint32_t k = fog_k_at(perp);
        if (side == 1) {
            k = (k * kMazeSideK + kMazeFogOne / 2) >> 5;
        }
        wall_k_[x] = static_cast<uint8_t>(k);

        const float line_h = focal_ / perp;
        const float top_f = (static_cast<float>(ih) - line_h) * 0.5f;
        const int top = static_cast<int>(std::floor(top_f));
        const int bottom = static_cast<int>(std::floor(top_f + line_h));
        zbuf_[x] = perp;
        wall_top_[x] = top;
        wall_bottom_[x] = bottom;

        Px* col = st.frame.data() + static_cast<size_t>(x) * ih_;
        const int y0 = std::clamp(top, 0, ih);
        const int y1 = std::clamp(bottom, 0, ih);
        const Px* texcol = st.textures.data() + (tex * kTex + static_cast<size_t>(tex_u)) * kTex;
        // 16.16 texture rows sampled at pixel centres; uint32 wraps keep the low bits the mask
        // reads.
        const auto step = static_cast<uint32_t>(static_cast<int64_t>(kTex * 65536.0 / line_h));
        auto tp = static_cast<uint32_t>(static_cast<int64_t>(
            (static_cast<double>(y0) + 0.5 - top_f) * kTex * 65536.0 / line_h));
        if (y1 - y0 > kTex) {
            // Taller than the texture: shade each texel once, not each pixel it covers.
            Px shaded[kTex];
            for (int v = 0; v < kTex; v++) {
                shaded[v] = shade<Px>(texcol[v], k);
            }
            for (int y = y0; y < y1; y++) {
                col[y] = shaded[(tp >> 16) & (kTex - 1)];
                tp += step;
            }
        } else {
            for (int y = y0; y < y1; y++) {
                col[y] = shade<Px>(texcol[(tp >> 16) & (kTex - 1)], k);
                tp += step;
            }
        }

        if (!textured) {
            for (int y = 0; y < y0; y++) {
                col[y] = st.ceil_rows[static_cast<size_t>(y)];
            }
            for (int y = y1; y < ih; y++) {
                col[y] = st.floor_rows[static_cast<size_t>(y)];
            }
            continue;
        }
        const auto rdxq = static_cast<int32_t>(rdx * 65536.0f);
        const auto rdyq = static_cast<int32_t>(rdy * 65536.0f);
        // Each row reads its own fog band: the band's offset plus the texel.
        auto texel = [&](int y) {
            const int64_t d = row_dist_q16_[static_cast<size_t>(y)];
            const auto fx = static_cast<int32_t>(pos_xq + ((d * rdxq) >> 16));
            const auto fy = static_cast<int32_t>(pos_yq + ((d * rdyq) >> 16));
            return static_cast<size_t>(row_band_[static_cast<size_t>(y)]) * kTex * kTex +
                   static_cast<size_t>((fx >> 10) & (kTex - 1)) * kTex +
                   static_cast<size_t>((fy >> 10) & (kTex - 1));
        };
        for (int y = 0; y < y0; y++) {
            col[y] = ceil_bands[texel(y)];
        }
        for (int y = y1; y < ih; y++) {
            col[y] = floor_bands[texel(y)];
        }
    }
}

template <typename Px>
void MazeRenderer::draw_sprites(const std::vector<MazeItem>& items, const MazePose& pose,
                                uint32_t elapsed_ms) {
    Store<Px>& st = store<Px>();
    sprite_order_.clear();
    for (size_t i = 0; i < items.size(); i++) {
        if (!items[i].active) {
            continue;
        }
        const float dx = items[i].x - pose.x;
        const float dy = items[i].y - pose.y;
        sprite_order_.push_back({dx * dx + dy * dy, static_cast<uint16_t>(i)});
    }
    std::sort(sprite_order_.begin(), sprite_order_.end(),
              [](const SpriteDraw& a, const SpriteDraw& b) { return a.depth > b.depth; });

    const float dir_x = std::cos(pose.angle);
    const float dir_y = std::sin(pose.angle);
    const float plane_x = -dir_y * plane_len_;
    const float plane_y = dir_x * plane_len_;
    const float inv_det = 1.0f / (plane_x * dir_y - dir_x * plane_y);
    const float fw = static_cast<float>(iw_);
    const float fh = static_cast<float>(ih_);
    const Px key = sprite_key<Px>();
    // Phase within one turn, from an integer that wraps instead of a float that loses precision.
    const float spin_phase = 2.0f * kSaverPi * static_cast<float>(elapsed_ms % kSpinPeriodMs) /
                             static_cast<float>(kSpinPeriodMs);

    for (const SpriteDraw& s : sprite_order_) {
        const MazeItem& item = items[s.item];
        const SpriteStyle style = style_of(item.kind);
        const float rx = item.x - pose.x;
        const float ry = item.y - pose.y;
        const float tx = inv_det * (dir_y * rx - dir_x * ry);
        const float ty = inv_det * (-plane_y * rx + plane_x * ry);
        if (ty < style.near) {
            continue;
        }
        const float screen_x = fw * 0.5f * (1.0f + tx / ty);
        const float h = style.size * focal_ / ty;
        float width_k = 1.0f;
        bool mirror = false;
        size_t slot = kSprLogo;
        switch (item.kind) {
        case MazeItemKind::START:
            slot = kSprStart;
            break;
        case MazeItemKind::EXIT:
            slot = kSprExit;
            break;
        case MazeItemKind::TOOLHEAD:
            slot = kSprToolhead;
            break;
        case MazeItemKind::ROCK:
            slot = kSprRock0 + (elapsed_ms / kRockFrameMs + s.item) % kRockFrames;
            break;
        case MazeItemKind::LOGO:
            slot = kSprLogo;
            break;
        }
        if (style.spins) {
            const float c = std::cos(spin_phase + 0.7f * static_cast<float>(s.item));
            width_k = std::fabs(c);
            mirror = c < 0.0f;
        }
        const float w = h * width_k;
        if (w < 1.0f) {
            continue;
        }
        const float centre_y = fh * 0.5f + (0.5f - style.center_z) * focal_ / ty;
        const float x0 = screen_x - w * 0.5f;
        const float y0 = centre_y - h * 0.5f;
        const int xs = std::max(0, static_cast<int>(std::ceil(x0 - 0.5f)));
        const int xe = std::min(static_cast<int>(iw_), static_cast<int>(std::ceil(x0 + w - 0.5f)));
        const int ys = std::max(0, static_cast<int>(std::ceil(y0 - 0.5f)));
        const int ye = std::min(static_cast<int>(ih_), static_cast<int>(std::ceil(y0 + h - 0.5f)));
        if (xs >= xe || ys >= ye) {
            continue;
        }
        const auto vstep = static_cast<uint32_t>(kTex * 65536.0f / h);
        const auto vstart =
            static_cast<uint32_t>((static_cast<float>(ys) + 0.5f - y0) * kTex * 65536.0f / h);
        const Px* spr = st.sprites.data() + slot * kTex * kTex;
        const uint32_t k = fog_k_at(ty);
        for (int x = xs; x < xe; x++) {
            if (ty >= zbuf_[static_cast<size_t>(x)]) {
                continue;
            }
            int u = std::clamp(static_cast<int>((static_cast<float>(x) + 0.5f - x0) * kTex / w), 0,
                               kTex - 1);
            if (mirror) {
                u = kTex - 1 - u;
            }
            const Px* scol = spr + static_cast<size_t>(u) * kTex;
            Px* col = st.frame.data() + static_cast<size_t>(x) * ih_;
            uint32_t vp = vstart;
            for (int y = ys; y < ye; y++, vp += vstep) {
                const Px px = scol[std::min<uint32_t>(vp >> 16, kTex - 1)];
                if (px == key || (style.screen_door && ((x + y) & 1) != 0)) {
                    continue;
                }
                col[y] = shade<Px>(px, k);
            }
        }
    }
}

template <typename Px> void MazeRenderer::blit(float flip, FrameTarget& out) {
    Store<Px>& st = store<Px>();
    const int ih = static_cast<int>(ih_);
    if (flip <= 0.0f) {
        for (int y = 0; y < ih; y++) {
            src_rows_[static_cast<size_t>(y)] = y;
        }
    } else if (flip >= 1.0f) {
        for (int y = 0; y < ih; y++) {
            src_rows_[static_cast<size_t>(y)] = ih - 1 - y;
        }
    } else {
        // Rows squash toward the horizon, through zero, and open out mirrored.
        const float s = std::cos(flip * kSaverPi);
        const float c = static_cast<float>(ih) * 0.5f;
        for (int y = 0; y < ih; y++) {
            int src = -1;
            if (std::fabs(s) >= 0.02f) {
                const int r =
                    static_cast<int>(std::floor(c + (static_cast<float>(y) + 0.5f - c) / s));
                src = r >= 0 && r < ih ? r : -1;
            }
            src_rows_[static_cast<size_t>(y)] = src;
        }
    }
    const auto black = static_cast<Px>(PixelWriter::pack({0, 0, 0}, format_));
    blit_rows<Px>(st.frame, iw_, ih_, src_rows_, kMazeLevels[level_].scale, black, st.row, out);
}

template <typename Px>
void MazeRenderer::render_as(const MazeMap& map, const std::vector<MazeItem>& items,
                             const MazePose& pose, float flip, uint32_t elapsed_ms,
                             FrameTarget& out) {
    cast_walls<Px>(map, pose);
    draw_sprites<Px>(items, pose, elapsed_ms);
    blit<Px>(flip, out);
}

void MazeRenderer::render(const MazeMap& map, const std::vector<MazeItem>& items,
                          const MazePose& pose, float flip, uint32_t elapsed_ms, FrameTarget& out) {
    if (iw_ == 0 || ih_ == 0 || out.w != w_ || out.h != h_ || out.format != format_) {
        return;
    }
    with_pixel_type(format_, [&](auto px) {
        render_as<decltype(px)>(map, items, pose, flip, elapsed_ms, out);
    });
}

void MazeSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
                   const MazeArt& art) {
    renderer_.init(frame.w, frame.h, frame.format, art, level);
    items_.clear();
    items_.reserve(4 + kLogoCount + kRockCount);
    const size_t cells = static_cast<size_t>(CELLS) * CELLS;
    taken_.reserve(cells);
    dist_.reserve(cells);
    queue_.reserve(cells);
    elapsed_ms_ = 0;
    walker_.reserve(CELLS);
    new_maze(rng);
    mazes_ = 0;
    exits_reached_ = 0;
    abandoned_ = 0;
}

void MazeSim::clear() {
    renderer_.release();
    map_.release();
    walker_.release();
    toolhead_.release();
    release_buffers(items_, taken_, dist_, queue_);
    flip_left_ms_ = 0;
    exit_left_ms_ = 0;
}

size_t MazeSim::heap_bytes() const {
    return renderer_.heap_bytes() + map_.heap_bytes() + walker_.heap_bytes() +
           toolhead_.heap_bytes() + buffer_bytes(items_, taken_, dist_, queue_);
}

uint32_t MazeSim::step_bound() const {
    // Each hand (the first, and one more per rock) may lap an island until a state repeats,
    // walk the shortest way off it and lap the boundary to the exit: each of the three at
    // most once through all 4 * cells^2 states.
    return (kRockCount + 1) * 12u * CELLS * CELLS;
}

float MazeSim::flip() const {
    if (flip_left_ms_ == 0) {
        return upside_down_ ? 1.0f : 0.0f;
    }
    const float t = 1.0f - static_cast<float>(flip_left_ms_) / kFlipMs;
    return upside_down_ ? t : 1.0f - t;
}

void MazeSim::new_maze(std::minstd_rand& rng) {
    map_.generate(CELLS, rng);
    const int n = CELLS;
    const auto cells = static_cast<size_t>(n * n);
    taken_.assign(cells, 0);
    dist_.assign(cells, -1);
    queue_.clear();
    auto passages = [this](int c) {
        int count = 0;
        for (int d = 0; d < 4; d++) {
            count += map_.passage(c % CELLS, c / CELLS, d) ? 1 : 0;
        }
        return count;
    };

    // Start in a dead end facing out of it, so the walk opens on the start sign ahead.
    int start = random_below(rng, n * n);
    for (int i = 0; i < n * n; i++) {
        const int c = (start + i) % (n * n);
        if (passages(c) == 1) {
            start = c;
            break;
        }
    }
    int heading = 0;
    while (heading < 3 && !map_.passage(start % n, start / n, heading)) {
        heading++;
    }

    // The exit is the cell furthest from the start, which in a tree is always a dead end.
    queue_.push_back(start);
    dist_[static_cast<size_t>(start)] = 0;
    int exit = start;
    for (size_t qi = 0; qi < queue_.size(); qi++) {
        const int c = queue_[qi];
        if (dist_[static_cast<size_t>(c)] > dist_[static_cast<size_t>(exit)]) {
            exit = c;
        }
        for (int d = 0; d < 4; d++) {
            if (!map_.passage(c % n, c / n, d)) {
                continue;
            }
            const int next = (c / n + kHeadingDy[d]) * n + c % n + kHeadingDx[d];
            if (dist_[static_cast<size_t>(next)] < 0) {
                dist_[static_cast<size_t>(next)] = dist_[static_cast<size_t>(c)] + 1;
                queue_.push_back(next);
            }
        }
    }

    map_.open_up(start, exit, rng);

    items_.clear();
    taken_[static_cast<size_t>(start)] = 1;
    taken_[static_cast<size_t>(exit)] = 1;
    items_.push_back({MazeItemKind::START, cell_centre(start % n) + 0.8f * kHeadingDx[heading],
                      cell_centre(start / n) + 0.8f * kHeadingDy[heading], true});
    items_.push_back({MazeItemKind::EXIT, cell_centre(exit % n), cell_centre(exit / n), true});
    auto place = [&](MazeItemKind kind, int count) {
        for (int placed = 0; placed < count;) {
            const int c = random_below(rng, n * n);
            if (taken_[static_cast<size_t>(c)] != 0) {
                continue;
            }
            taken_[static_cast<size_t>(c)] = 1;
            items_.push_back({kind, cell_centre(c % n), cell_centre(c / n), true});
            placed++;
        }
    };
    place(MazeItemKind::LOGO, kLogoCount);
    place(MazeItemKind::ROCK, kRockCount);

    const int tool_cell = random_below(rng, n * n);
    int tool_heading = 0;
    while (tool_heading < 3 && !map_.passage(tool_cell % n, tool_cell / n, tool_heading)) {
        tool_heading++;
    }
    toolhead_.reset(tool_cell % n, tool_cell / n, tool_heading, random_below(rng, 2) == 1, 1100,
                    600);
    toolhead_item_ = items_.size();
    items_.push_back(
        {MazeItemKind::TOOLHEAD, cell_centre(tool_cell % n), cell_centre(tool_cell / n), true});

    walker_.reset(start % n, start / n, heading, false);
    // The walk eases to a stop in the exit's doorway, with the smiley a tile ahead.
    walker_.set_stop_cell(exit % n, exit / n);
    upside_down_ = false;
    flip_left_ms_ = 0;
    exit_left_ms_ = 0;
    mazes_++;
}

void MazeSim::on_arrival(std::minstd_rand& rng) {
    const float x = cell_centre(walker_.cell_x());
    const float y = cell_centre(walker_.cell_y());
    for (MazeItem& item : items_) {
        if (item.active && item.kind == MazeItemKind::ROCK && item.x == x && item.y == y) {
            item.active = false;
            upside_down_ = !upside_down_;
            walker_.set_left_handed(upside_down_);
            flip_left_ms_ = kFlipMs;
        }
    }
    if (walker_.moves() > step_bound()) {
        abandoned_++;
        new_maze(rng);
    }
}

void MazeSim::advance(uint32_t dt_ms, std::minstd_rand& rng) {
    elapsed_ms_ += dt_ms;

    uint32_t left = dt_ms;
    while (left > 0) {
        WalkEvent event = WalkEvent::NONE;
        const uint32_t used = toolhead_.advance(map_, left, event);
        if (used == 0) {
            break;
        }
        left -= used;
    }
    const MazePose tool = toolhead_.pose();
    items_[toolhead_item_].x = tool.x;
    items_[toolhead_item_].y = tool.y;

    left = dt_ms;
    while (left > 0) {
        if (flip_left_ms_ > 0) {
            const uint32_t used = std::min(left, flip_left_ms_);
            flip_left_ms_ -= used;
            left -= used;
            continue;
        }
        if (exit_left_ms_ > 0) {
            const uint32_t used = std::min(left, exit_left_ms_);
            exit_left_ms_ -= used;
            left -= used;
            if (exit_left_ms_ == 0) {
                new_maze(rng);
            }
            continue;
        }
        WalkEvent event = WalkEvent::NONE;
        const uint32_t used = walker_.advance(map_, left, event);
        left -= used;
        if (event == WalkEvent::ARRIVED) {
            on_arrival(rng);
        } else if (event == WalkEvent::ENTERED && walker_.stopped()) {
            exits_reached_++;
            exit_left_ms_ = kExitHoldMs;
        } else if (used == 0) {
            break;
        }
    }
}

void MazeSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                   std::vector<DirtyRect>& dirty) {
    advance(dt_ms, rng);
    renderer_.render(map_, items_, walker_.pose(), flip(), elapsed_ms_, frame);
    dirty.clear();
    dirty.push_back(DirtyRect::whole(static_cast<int32_t>(frame.w), static_cast<int32_t>(frame.h)));
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
