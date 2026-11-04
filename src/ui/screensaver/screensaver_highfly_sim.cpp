// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_highfly_sim.h"

#include "screensaver_blur.h"
#include "screensaver_buffer.h"
#include "screensaver_fast_math.h"
#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace helix::ui {

using screensaver::random_below;
using screensaver::random_between;
using screensaver::random_normal;
using screensaver::smoothstep01;
using screensaver::unit_random;
using screensaver::wrap_index;

namespace {

constexpr int32_t N = kHighflyField;

// Camera and deck, as tuned at a 400 x 240 internal frame.
constexpr float kHorizon = 0.56f;
constexpr float kCamH = 15.0f;
constexpr float kFocalYAt240 = 150.0f;
constexpr float kFov = 1.0f; ///< half-width tan
constexpr float kZNear = 10.0f;
constexpr float kZFar = 350.0f;
constexpr float kDzGrow = 1.025f;
constexpr float kFeatherAt240 = 10.0f;
constexpr float kSink = 40.0f;
constexpr float kNearWhite = 40.0f;
constexpr float kNearK = 0.7f;
constexpr float kHaze = 0.22f;
/// World x of the field under the screen's centre.
constexpr float kFieldX0 = 64.0f;

constexpr Rgb kCShadow{45, 130, 182};
constexpr Rgb kCMidtone{150, 212, 235};
constexpr Rgb kCLit{252, 254, 255};
constexpr Rgb kCFog{30, 110, 162};
constexpr Rgb INSIDE{200, 216, 230};
constexpr Rgb kNumeral{226, 238, 250};
constexpr float kNumeralOpacity = 0.85f;
/// The numerals' soft shadow: the sky's deep blue, barely there over sky, enough to lift the
/// pale strokes off white cloud.
constexpr Rgb kHalo{10, 50, 92};
constexpr float kHaloOpacity = 0.45f;
constexpr float kDigitStroke = 0.06f;

const HeightfieldFog kFog{25.0f, 300.0f, 0.8f, false};

// Billboards.
constexpr float kBandZ = 260.0f;
constexpr float kBandQ = 0.55f;
constexpr float kBandHb = -6.0f;
constexpr float kBandDrift = 1.5f; ///< world units per second sideways
constexpr float kLayerQ = 0.16f;
constexpr float kLayerZfar = 170.0f;
constexpr float kLayerZnear = 8.0f;
constexpr float kLayerHb = -7.0f;
constexpr float kTowerQ = 0.22f;
constexpr float kTowerHb = -8.0f;
constexpr float kTowerZEnter = 330.0f;
/// A tower enters this far to the side, as a share of the half-width at its entry depth; it
/// keeps its world x, so it slides outward as it nears, and drifts on the wind besides.
constexpr float kTowerSideMin = 0.30f;
constexpr float kTowerSideMax = 0.50f;
constexpr float kTowerWindMin = 1.0f;
constexpr float kTowerWindMax = 2.5f;
/// Towers fade out from kTowerFadeFrom to kTowerZLeave as they come near, then go, so no
/// more than two are ever up with one entering every 13 to 14 s.
constexpr float kTowerFadeFrom = 120.0f;
constexpr float kTowerZLeave = 70.0f;
constexpr float kTowerFogmul = 0.25f;
constexpr float kBodyQ = 0.09f;
/// The fly-through cloud is this far ahead when its whiteout begins.
constexpr float kFlyFace = 10.0f;

/// The deck's stored heights are texels times this, rounded.
constexpr float kDeckHeightSteps = 128.0f;

constexpr size_t kHistoryCap = 64;
/// Rows the deck may rise in a frame: the band repaints this far above last frame's top.
constexpr int32_t kDeckRise = 3;
/// Boxes above the band a frame repaints: the clock where it is and was, two towers and the
/// cloud ahead, this frame and last, with room to spare. Past it boxes merge, never drop.
constexpr size_t kBoxCap = 12;

/// Cloud shade 0-1 to colour: shadow blue to midtone, then midtone to sunlit white.
Rgb shade_to_rgb(float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    if (s < 0.55f) {
        return mix_rgb(kCShadow, kCMidtone, s / 0.55f);
    }
    return mix_rgb(kCMidtone, kCLit, (s - 0.55f) / 0.45f);
}

// ---------- Grid helpers ----------

float stddev(const std::vector<float>& a) {
    double sum = 0.0;
    double sq = 0.0;
    for (const float v : a) {
        sum += v;
        sq += static_cast<double>(v) * v;
    }
    const double n = static_cast<double>(a.size());
    const double mean = sum / n;
    return static_cast<float>(std::sqrt(std::max(sq / n - mean * mean, 1e-12)));
}

/// Fractal value noise: smoothstep-interpolated normal grids at a few cell counts, summed and
/// normalised to unit deviation. Wraps across x when asked.
void fbm(std::minstd_rand& rng, int32_t h, int32_t w, bool wrap_x,
         std::initializer_list<std::pair<int32_t, float>> octaves, std::vector<float>& out) {
    out.assign(static_cast<size_t>(w) * h, 0.0f);
    std::vector<float> g;
    for (const auto& [cells, amp] : octaves) {
        const int32_t gw = cells;
        const int32_t gh = std::max(2, cells * h / w + 2);
        g.resize(static_cast<size_t>(gh) * (gw + 1));
        for (float& v : g) {
            v = random_normal(rng);
        }
        if (wrap_x) {
            for (int32_t y = 0; y < gh; y++) {
                g[static_cast<size_t>(y) * (gw + 1) + gw] = g[static_cast<size_t>(y) * (gw + 1)];
            }
        }
        for (int32_t y = 0; y < h; y++) {
            const float fy = static_cast<float>(gh - 1.001) * static_cast<float>(y) /
                             static_cast<float>(std::max(1, h - 1));
            const auto y0 = static_cast<int32_t>(fy);
            const float ty = smoothstep01(fy - static_cast<float>(y0));
            for (int32_t x = 0; x < w; x++) {
                const float fx = wrap_x ? static_cast<float>(gw) * static_cast<float>(x) / w
                                        : static_cast<float>(gw) * static_cast<float>(x) /
                                              static_cast<float>(std::max(1, w - 1));
                const auto x0 = std::min(static_cast<int32_t>(fx), gw - 1);
                const float tx = smoothstep01(fx - static_cast<float>(x0));
                const size_t r0 = static_cast<size_t>(y0) * (gw + 1);
                const size_t r1 = r0 + static_cast<size_t>(gw + 1);
                const float a = g[r0 + x0] + (g[r0 + x0 + 1] - g[r0 + x0]) * tx;
                const float b = g[r1 + x0] + (g[r1 + x0 + 1] - g[r1 + x0]) * tx;
                out[static_cast<size_t>(y) * w + x] += amp * (a + (b - a) * ty);
            }
        }
    }
    const float sd = stddev(out);
    for (float& v : out) {
        v /= sd;
    }
}

/// Adds a Gaussian blob exp(-d^2 / r^2) * amp, wrapping across x when asked.
void add_blob(std::vector<float>& d, int32_t w, int32_t h, float cx, float cy, float r, bool wrap_x,
              float amp) {
    const float reach = 3.0f * r;
    const auto y0 = static_cast<int32_t>(std::max(0.0f, std::floor(cy - reach)));
    const auto y1 =
        static_cast<int32_t>(std::min(static_cast<float>(h - 1), std::ceil(cy + reach)));
    const auto x0 = static_cast<int32_t>(std::floor(cx - reach));
    const auto x1 = static_cast<int32_t>(std::ceil(cx + reach));
    const float inv_r = 1.0f / r;
    for (int32_t y = y0; y <= y1; y++) {
        const float dy = (static_cast<float>(y) - cy) * inv_r;
        for (int32_t xx = x0; xx <= x1; xx++) {
            int32_t x = xx;
            if (wrap_x) {
                x = wrap_index(x, w);
            } else if (x < 0 || x >= w) {
                continue;
            }
            const float dx = (static_cast<float>(xx) - cx) * inv_r;
            d[static_cast<size_t>(y) * w + x] += amp * fast_exp_neg(dx * dx + dy * dy);
        }
    }
}

/// `c` at coverage `alpha` (0 to 1) as premultiplied RGBA bytes at `out`.
void premultiplied_rgba(Rgb c, float alpha, uint8_t* out) {
    out[0] = static_cast<uint8_t>(c.r * alpha + 0.5f);
    out[1] = static_cast<uint8_t>(c.g * alpha + 0.5f);
    out[2] = static_cast<uint8_t>(c.b * alpha + 0.5f);
    out[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
}

/// The premultiplied RGBA texel at column `u` of `row`, as one little-endian word.
inline uint32_t texel_at(const uint8_t* row, int32_t u) {
    uint32_t v = 0;
    std::memcpy(&v, row + static_cast<size_t>(u) * 4, sizeof(v));
    return v;
}

/// `p` moved `f` (0 to 256) 256ths of the way to `q`, each byte truncated, two at a time.
inline uint32_t lerp_rgba(uint32_t p, uint32_t q, int32_t f) {
    const auto t = static_cast<uint32_t>(f);
    const uint32_t keep = 256u - t;
    const uint32_t even = ((p & 0x00FF00FFu) * keep + (q & 0x00FF00FFu) * t) >> 8 & 0x00FF00FFu;
    const uint32_t odd = ((p >> 8 & 0x00FF00FFu) * keep + (q >> 8 & 0x00FF00FFu) * t) & 0xFF00FF00u;
    return even | odd;
}

/// Density to premultiplied RGBA, lit from above by the light the cloud lets through.
void shade_cloud(std::vector<float>& d, const std::vector<float>& wisp, float wisp_scale, int32_t w,
                 int32_t h, bool wrap_x, float dens_k, float absorb, int32_t smooth, float bias,
                 bool crown, HighFlyTexture& out) {
    std::vector<float> tmp;
    for (size_t i = 0; i < d.size(); i++) {
        const float dc = std::clamp(d[i], 0.0f, 1.0f);
        d[i] = std::max(d[i] + 0.45f * wisp[i] * wisp_scale * dc * (1.0f - dc) * 4.0f, 0.0f);
    }
    box_blur(d, w, h, smooth, wrap_x, false, tmp);
    // Optical depth from the top, the light that reaches each texel.
    std::vector<float> od(d.size());
    for (int32_t x = 0; x < w; x++) {
        float acc = 0.0f;
        for (int32_t y = 0; y < h; y++) {
            acc += d[static_cast<size_t>(y) * w + x];
            od[static_cast<size_t>(y) * w + x] = acc / static_cast<float>(h) * 18.0f;
        }
    }
    box_blur(od, w, h, 2, wrap_x, false, tmp);
    std::vector<float> soft = d;
    box_blur(soft, w, h, 3, wrap_x, false, tmp);

    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int32_t y = 0; y < h; y++) {
        const int32_t yu = std::max(0, y - 1);
        const int32_t yd = std::min(h - 1, y + 1);
        for (int32_t x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const float alpha = 1.0f - std::exp(-dens_k * d[i]);
            const float lit = std::exp(-absorb * od[i]);
            const float gy =
                (soft[static_cast<size_t>(yd) * w + x] - soft[static_cast<size_t>(yu) * w + x]) /
                static_cast<float>(std::max(1, yd - yu));
            const float relief = std::clamp(gy * 6.0f, -1.0f, 1.0f);
            const float edge = std::clamp(1.0f - alpha, 0.0f, 1.0f);
            float s = bias + 0.50f + 0.55f * lit + 0.30f * edge + 0.12f * relief +
                      0.03f * wisp[i] * wisp_scale - 0.25f * (1.0f - lit) * (1.0f - lit);
            if (crown) {
                // Sunlit tops read white, not bluish.
                s += 0.25f * std::clamp(1.0f - od[i] / 2.0f, 0.0f, 1.0f);
            }
            const Rgb c = shade_to_rgb(s);
            premultiplied_rgba(c, alpha, &out.rgba[i * 4]);
        }
    }
}

void make_band(std::minstd_rand& rng, HighFlyTexture& out) {
    const int32_t w = 1024;
    const int32_t h = 90;
    std::vector<float> d(static_cast<size_t>(w) * h, 0.0f);
    float x = 0.0f;
    while (x < static_cast<float>(w)) {
        const float r = random_between(rng, 14.0f, 30.0f);
        const float top = random_between(rng, 10.0f, 50.0f);
        add_blob(d, w, h, x, top + r, r, true, 1.0f);
        for (int i = 0; i < 4; i++) {
            const float sx = x + random_between(rng, -r, r);
            const float sy = top + random_between(rng, 0.0f, r);
            add_blob(d, w, h, sx, sy, r * random_between(rng, 0.3f, 0.55f), true, 0.8f);
        }
        x += random_between(rng, 18.0f, 40.0f);
    }
    for (int32_t y = 55; y < h; y++) {
        const float add = 2.5f * static_cast<float>(y - 55) / static_cast<float>(h - 56);
        for (int32_t xx = 0; xx < w; xx++) {
            d[static_cast<size_t>(y) * w + xx] += add;
        }
    }
    std::vector<float> wisp;
    fbm(rng, h, w, true, {{8, 1.0f}, {16, 0.6f}, {32, 0.35f}, {64, 0.2f}}, wisp);
    shade_cloud(d, wisp, 1.0f, w, h, true, 2.2f, 0.9f, 1, 0.0f, false, out);
}

void make_layer(std::minstd_rand& rng, HighFlyTexture& out) {
    const int32_t w = 512;
    const int32_t h = 128;
    std::vector<float> d(static_cast<size_t>(w) * h, 0.0f);
    for (int i = 0; i < 70; i++) {
        const float r = random_between(rng, 8.0f, 26.0f);
        const float cx = random_between(rng, 0.0f, static_cast<float>(w));
        const float cy = random_between(rng, 30.0f, 115.0f);
        add_blob(d, w, h, cx, cy, r, true, random_between(rng, 0.5f, 1.0f));
    }
    for (int32_t y = 80; y < h; y++) {
        const float add = 2.0f * static_cast<float>(y - 80) / static_cast<float>(h - 81);
        for (int32_t x = 0; x < w; x++) {
            d[static_cast<size_t>(y) * w + x] += add;
        }
    }
    std::vector<float> wisp;
    fbm(rng, h, w, true, {{8, 1.0f}, {16, 0.6f}, {32, 0.35f}, {64, 0.2f}}, wisp);
    for (size_t i = 0; i < d.size(); i++) {
        d[i] *= std::clamp(0.8f + 0.25f * wisp[i], 0.0f, 2.0f);
    }
    shade_cloud(d, wisp, 1.0f, w, h, true, 1.8f, 0.9f, 1, 0.0f, false, out);
}

/// Cumulus colour by shade 0-1: blue-grey under the lobes to sunlit white on top.
Rgb tower_rgb(float s) {
    constexpr Rgb kShade{118, 152, 184};
    constexpr Rgb kMid{196, 218, 236};
    s = std::clamp(s, 0.0f, 1.0f);
    if (s < 0.5f) {
        return mix_rgb(kShade, kMid, s / 0.5f);
    }
    return mix_rgb(kMid, kCLit, (s - 0.5f) / 0.5f);
}

/// A towering cumulus lit from above and a little in front: each lobe takes Lambert light on a
/// normal from the smoothed density, so the crown and the tops of the lobes read sunlit white
/// and their undersides blue-grey, darkening toward the base.
void shade_tower(std::vector<float>& d, int32_t w, int32_t h, HighFlyTexture& out) {
    std::vector<float> tmp;
    box_blur(d, w, h, 2, false, false, tmp);
    std::vector<float> soft = d;
    box_blur(soft, w, h, 6, false, false, tmp);
    std::vector<float> od(d.size());
    for (int32_t x = 0; x < w; x++) {
        float acc = 0.0f;
        for (int32_t y = 0; y < h; y++) {
            acc += d[static_cast<size_t>(y) * w + x];
            od[static_cast<size_t>(y) * w + x] = acc / static_cast<float>(h) * 18.0f;
        }
    }
    box_blur(od, w, h, 3, false, false, tmp);
    // Light from above, a little to the left and toward the camera; y grows downward.
    const float lx = -0.35f;
    const float ly = -0.80f;
    const float lz = 0.48f;
    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int32_t y = 0; y < h; y++) {
        const int32_t yu = std::max(0, y - 1);
        const int32_t yd = std::min(h - 1, y + 1);
        for (int32_t x = 0; x < w; x++) {
            const int32_t xl = std::max(0, x - 1);
            const int32_t xr = std::min(w - 1, x + 1);
            const size_t i = static_cast<size_t>(y) * w + x;
            const float alpha = 1.0f - std::exp(-2.2f * d[i]);
            if (alpha < 0.004f) {
                continue;
            }
            const float gx =
                (soft[static_cast<size_t>(y) * w + xr] - soft[static_cast<size_t>(y) * w + xl]) *
                0.5f;
            const float gy =
                (soft[static_cast<size_t>(yd) * w + x] - soft[static_cast<size_t>(yu) * w + x]) *
                0.5f;
            // The surface faces away from where the density grows.
            const float nx = -gx * 5.0f;
            const float ny = -gy * 5.0f;
            const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
            const float diffuse = std::clamp((nx * lx + ny * ly + lz) * inv, 0.0f, 1.0f);
            const float lit = std::exp(-0.35f * od[i]);
            const float base = static_cast<float>(y) / static_cast<float>(h);
            const float s = 0.30f + 0.50f * diffuse + 0.30f * lit - 0.25f * base * base +
                            0.25f * std::clamp(1.0f - od[i] / 2.0f, 0.0f, 1.0f);
            const Rgb c = tower_rgb(s);
            premultiplied_rgba(c, alpha, &out.rgba[i * 4]);
        }
    }
}

void make_tower(std::minstd_rand& rng, HighFlyTexture& out) {
    const int32_t w = 256;
    const int32_t h = 300;
    std::vector<float> d(static_cast<size_t>(w) * h, 0.0f);
    const float cx = static_cast<float>(w) / 2.0f;
    // A column of billows narrowing upward to a domed crown, each tier a main blob with
    // smaller lobes bulging out round its upper half.
    float drift = 0.0f;
    for (int i = 0; i < 12; i++) {
        const float t = static_cast<float>(i) / 11.0f; // 0 base .. 1 crown
        const float y = static_cast<float>(h) - 45.0f - t * static_cast<float>(h - 110);
        const float rad = 60.0f * (1.0f - 0.35f * t) * random_between(rng, 0.85f, 1.1f);
        drift += random_between(rng, -8.0f, 8.0f);
        const float xo = cx + std::clamp(drift, -20.0f, 20.0f);
        add_blob(d, w, h, xo, y, rad * 0.75f, false, 1.2f);
        const int lobes = 4 + random_below(rng, 3);
        for (int k = 0; k < lobes; k++) {
            const float a = random_between(rng, kSaverPi * 1.0f, kSaverPi * 2.0f);
            add_blob(d, w, h, xo + std::cos(a) * rad * 0.7f, y + std::sin(a) * rad * 0.5f,
                     rad * random_between(rng, 0.30f, 0.45f), false, 0.9f);
        }
    }
    // The crown: a dome with a few lobes of its own.
    const float crown_y = 70.0f;
    add_blob(d, w, h, cx + drift * 0.5f, crown_y, 34.0f, false, 1.1f);
    for (int k = 0; k < 5; k++) {
        const float a = random_between(rng, kSaverPi * 1.05f, kSaverPi * 1.95f);
        add_blob(d, w, h, cx + drift * 0.5f + std::cos(a) * 30.0f, crown_y + std::sin(a) * 18.0f,
                 random_between(rng, 12.0f, 18.0f), false, 0.9f);
    }
    for (int32_t y = h - 50; y < h; y++) {
        const float add = 2.0f * static_cast<float>(y - (h - 50)) / 49.0f;
        for (int32_t x = 0; x < w; x++) {
            d[static_cast<size_t>(y) * w + x] += add;
        }
    }
    std::vector<float> wisp;
    fbm(rng, h, w, false, {{8, 1.0f}, {16, 0.6f}, {32, 0.35f}, {64, 0.2f}}, wisp);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const float xn = std::fabs(static_cast<float>(x) - cx) / cx;
            // Torn edges, and sides that fade so the base melts into the deck.
            d[i] *= std::clamp(0.85f + 0.2f * wisp[i], 0.0f, 2.0f) *
                    std::clamp(1.6f - xn * xn * 1.6f, 0.0f, 1.0f);
            const float dc = std::clamp(d[i], 0.0f, 1.0f);
            d[i] = std::max(d[i] + 0.25f * wisp[i] * dc * (1.0f - dc), 0.0f);
        }
    }
    shade_tower(d, w, h, out);
}

} // namespace

size_t HighFlyTexture::heap_bytes() const {
    return buffer_bytes(rgba);
}

// ---------- Field ----------

void build_highfly_field(std::minstd_rand& rng, HighFlyField& out) {
    std::vector<float> hf(static_cast<size_t>(N) * N, 0.0f);
    std::vector<float> tmp;
    const auto splat = [&hf](float cx, float cy, float r, float base) {
        const auto x0 = static_cast<int32_t>(std::floor(cx - r));
        const auto x1 = static_cast<int32_t>(std::ceil(cx + r));
        const auto y0 = static_cast<int32_t>(std::floor(cy - r));
        const auto y1 = static_cast<int32_t>(std::ceil(cy + r));
        const float r2 = r * r;
        for (int32_t yy = y0; yy <= y1; yy++) {
            const float dy = static_cast<float>(yy) - cy;
            const int32_t y = wrap_index(yy, N);
            for (int32_t xx = x0; xx <= x1; xx++) {
                const float dx = static_cast<float>(xx) - cx;
                const float d2 = dx * dx + dy * dy;
                if (d2 >= r2) {
                    continue;
                }
                const int32_t x = wrap_index(xx, N);
                float& h = hf[static_cast<size_t>(y) * N + x];
                h = std::max(h, base + std::sqrt(r2 - d2));
            }
        }
    };
    const auto height_at = [&hf](float cx, float cy) {
        const int32_t x = static_cast<int32_t>(cx) % N;
        const int32_t y = static_cast<int32_t>(cy) % N;
        return hf[static_cast<size_t>(y) * N + x];
    };
    const auto fN = static_cast<float>(N);
    // Big billows: sphere centres below the floor, so the caps are wide domes.
    for (int i = 0; i < 30; i++) {
        const float cx = random_between(rng, 0.0f, fN);
        const float cy = random_between(rng, 0.0f, fN);
        const float r = random_between(rng, 18.0f, 34.0f);
        splat(cx, cy, r, random_between(rng, -16.0f, 8.0f));
    }
    // Lumps riding on the billows.
    for (int i = 0; i < 170; i++) {
        const float cx = random_between(rng, 0.0f, fN);
        const float cy = random_between(rng, 0.0f, fN);
        const float r = random_between(rng, 8.0f, 16.0f);
        splat(cx, cy, r, height_at(cx, cy) - r * random_between(rng, 0.55f, 0.85f));
    }
    // Cauliflower.
    for (int i = 0; i < 1500; i++) {
        const float cx = random_between(rng, 0.0f, fN);
        const float cy = random_between(rng, 0.0f, fN);
        const float r = random_between(rng, 2.5f, 5.5f);
        splat(cx, cy, r, height_at(cx, cy) - r * random_between(rng, 0.5f, 0.8f));
    }
    // Noise, stronger where the cloud is thicker.
    std::vector<float> fb(hf.size(), 0.0f);
    std::vector<float> g(hf.size());
    for (const auto& [cells, amp] :
         {std::pair<int32_t, float>{16, 1.0f}, {32, 0.5f}, {64, 0.25f}}) {
        std::vector<float> coarse(static_cast<size_t>(cells) * cells);
        for (float& v : coarse) {
            v = random_normal(rng);
        }
        const int32_t block = N / cells;
        for (int32_t y = 0; y < N; y++) {
            for (int32_t x = 0; x < N; x++) {
                g[static_cast<size_t>(y) * N + x] =
                    coarse[static_cast<size_t>(y / block) * cells + x / block];
            }
        }
        box_blur(g, N, N, std::max(1, N / cells / 2), true, true, tmp);
        for (size_t i = 0; i < fb.size(); i++) {
            fb[i] += amp * g[i];
        }
    }
    const float fb_sd = stddev(fb);
    for (size_t i = 0; i < hf.size(); i++) {
        hf[i] += 1.5f * fb[i] / fb_sd * std::clamp(hf[i] / 10.0f, 0.0f, 1.0f);
    }
    // Talus: sphere caps leave vertical walls the renderer would draw as curtains, so every
    // texel rises to within kHighflyTalus of its highest neighbour.
    for (int it = 0; it < 20; it++) {
        g = hf;
        for (int32_t y = 0; y < N; y++) {
            const int32_t yu = (y + N - 1) % N;
            const int32_t yd = (y + 1) % N;
            for (int32_t x = 0; x < N; x++) {
                const int32_t xl = (x + N - 1) % N;
                const int32_t xr = (x + 1) % N;
                const float nb = std::max(
                    {g[static_cast<size_t>(yu) * N + x], g[static_cast<size_t>(yd) * N + x],
                     g[static_cast<size_t>(y) * N + xl], g[static_cast<size_t>(y) * N + xr]});
                float& h = hf[static_cast<size_t>(y) * N + x];
                h = std::max(h, nb - kHighflyTalus);
            }
        }
    }
    box_blur(hf, N, N, 1, true, true, tmp);

    // Shade: Lambert from a sun above and a little beyond, ambient occlusion at two scales,
    // height, and a little noise.
    std::vector<float> blur6 = hf;
    box_blur(blur6, N, N, 6, true, true, tmp);
    std::vector<float> blur2 = hf;
    box_blur(blur2, N, N, 2, true, true, tmp);
    std::vector<float> noise(hf.size());
    for (float& v : noise) {
        v = random_normal(rng);
    }
    box_blur(noise, N, N, 1, true, true, tmp);
    const float noise_sd = stddev(noise);
    const auto [lo_it, hi_it] = std::minmax_element(hf.begin(), hf.end());
    const float lo = *lo_it;
    const float range = std::max(*hi_it - lo, 1e-6f);
    const float sun_len = std::sqrt(0.3f * 0.3f + 0.3f * 0.3f + 1.0f);
    const float sx = 0.3f / sun_len;
    const float sy = 0.3f / sun_len;
    const float sz = 1.0f / sun_len;
    out.height = hf;
    out.shade.assign(hf.size(), 0.0f);
    for (int32_t y = 0; y < N; y++) {
        const int32_t yu = (y + N - 1) % N;
        const int32_t yd = (y + 1) % N;
        for (int32_t x = 0; x < N; x++) {
            const size_t i = static_cast<size_t>(y) * N + x;
            const float gx = (hf[static_cast<size_t>(y) * N + (x + 1) % N] -
                              hf[static_cast<size_t>(y) * N + (x + N - 1) % N]) *
                             0.5f;
            const float gy =
                (hf[static_cast<size_t>(yd) * N + x] - hf[static_cast<size_t>(yu) * N + x]) * 0.5f;
            const float inv = 1.0f / std::sqrt(gx * gx + gy * gy + 1.0f);
            const float lam = std::clamp((-gx * sx - gy * sy + sz) * inv, 0.0f, 1.0f);
            const float ao = std::clamp(0.5f + (hf[i] - blur6[i]) / 10.0f, 0.0f, 1.0f);
            const float ao2 = std::clamp(0.5f + (hf[i] - blur2[i]) / 3.0f, 0.0f, 1.0f);
            const float hnorm = (hf[i] - lo) / range;
            const float s = -0.05f + 0.40f * lam + 0.75f * ao + 0.15f * hnorm +
                            0.03f * noise[i] / noise_sd + 0.5f * (ao2 - 0.5f);
            out.shade[i] = std::clamp(s, 0.0f, 1.0f);
        }
    }
}

float highfly_whiteout(float since_start_s) {
    if (since_start_s <= 0.0f) {
        return 0.0f;
    }
    if (since_start_s < kHighflyFlyInS) {
        return smoothstep01(since_start_s / kHighflyFlyInS);
    }
    const float hold_end = kHighflyFlyInS + kHighflyFlyHoldS;
    if (since_start_s < hold_end) {
        return 1.0f;
    }
    return 1.0f - smoothstep01((since_start_s - hold_end) / kHighflyFlyOutS);
}

Rgb highfly_inside(Rgb scene, float amt, float wisp, float most) {
    const float k = std::clamp(amt, 0.0f, 1.0f) * kHighflyWhiteoutMax;
    // What shows of the scene after both: (1 - k)(1 - wisp), held at 1 - most or more.
    const float wisp_most = std::max(0.0f, 1.0f - (1.0f - most) / (1.0f - k));
    const float a = std::clamp(wisp, 0.0f, std::min(1.0f, wisp_most));
    return mix_rgb(mix_rgb(scene, INSIDE, k), kCLit, a);
}

float highfly_layer_visibility(float deck_z, float layer_z) {
    if (!std::isfinite(deck_z)) {
        return 1.0f;
    }
    return std::clamp((deck_z - layer_z) / (0.25f * layer_z) + 0.5f, 0.0f, 1.0f);
}

// ---------- Clock layout ----------

HighFlyClockLayout layout_highfly_clock(int32_t w, int32_t h, int32_t dx, int32_t dy) {
    const float s = std::min(static_cast<float>(h) / 480.0f, static_cast<float>(w) / 800.0f);
    HighFlyClockLayout l;
    const float path_h = kStrokeDigitPathH + kDigitStroke;
    l.digit_unit = std::max(4, static_cast<int32_t>(std::lround(110.0f * s / path_h)));
    const auto u = static_cast<float>(l.digit_unit);
    const auto px = [](float v) { return static_cast<int32_t>(std::lround(v)); };
    const int32_t gw = px((1.0f + kDigitStroke) * u);
    const int32_t gh = px(path_h * u);
    const int32_t gap = std::max(1, px(0.15f * u));
    const int32_t colon_w = std::max(2, px(0.45f * u));
    const int32_t total = 4 * gw + 4 * gap + colon_w;
    const int32_t y2 = px(0.60f * static_cast<float>(h)) + dy;
    const int32_t y1 = y2 - gh + 1;
    int32_t x = w / 2 - total / 2 + dx;
    for (size_t i = 0; i < 5; i++) {
        const int32_t width = i == 2 ? colon_w : gw;
        l.digits[i] = {x, y1, x + width - 1, y2};
        x += width + gap;
    }
    l.second_unit = std::max(2, px(0.35f * u));
    const auto su = static_cast<float>(l.second_unit);
    const int32_t sgw = px((1.0f + kDigitStroke) * su);
    const int32_t sgh = px(path_h * su);
    const int32_t sgap = std::max(1, px(0.15f * su));
    const int32_t sx = l.digits[4].x2 + 1 + px(0.15f * u);
    l.seconds[0] = {sx, y1, sx + sgw - 1, y1 + sgh - 1};
    l.seconds[1] = {sx + sgw + sgap, y1, sx + 2 * sgw + sgap - 1, y1 + sgh - 1};
    const int32_t my = l.seconds[0].y2 + 1 + px(0.1f * u);
    l.meridiem = {sx, my, std::max(l.seconds[1].x2, sx + px(1.2f * u)), my + px(0.3f * gh) - 1};
    // The date block sits in the sky beside the numerals' upper half, clear of the clouds.
    const int32_t line_h = std::max(10, px(0.055f * static_cast<float>(h)));
    const int32_t lx1 = px(0.05f * static_cast<float>(w)) + dx;
    const int32_t lx2 =
        std::min(px(0.30f * static_cast<float>(w)) + dx, l.digits[0].x1 - px(0.1f * u));
    const int32_t date_y = y1 + px(0.10f * static_cast<float>(gh));
    l.date_line = {lx1, date_y, lx2, date_y + line_h - 1};
    l.day_line = {lx1, date_y + line_h, lx2, date_y + 2 * line_h - 1};
    l.halo = std::max(3, px(0.12f * u));
    l.second_halo = std::max(2, px(0.12f * u * 0.6f));
    l.bounds = {};
    for (const DirtyRect& d : l.digits) {
        l.bounds.add(d.x1 - l.halo, d.y1 - l.halo, d.x2 + l.halo, d.y2 + l.halo);
    }
    for (const DirtyRect& d : l.seconds) {
        l.bounds.add(d.x1 - l.second_halo, d.y1 - l.second_halo, d.x2 + l.second_halo,
                     d.y2 + l.second_halo);
    }
    // The text lines' soft shadow reaches this far round their boxes.
    l.text_halo = std::max(2, px(0.04f * u));
    for (const DirtyRect& d : {l.meridiem, l.date_line, l.day_line}) {
        l.bounds.add(d.x1 - l.text_halo, d.y1 - l.text_halo, d.x2 + l.text_halo,
                     d.y2 + l.text_halo);
    }
    return l;
}

// ---------- HighFlySim ----------

/// The deck: the baked field scrolling toward the camera, shaded through per-slice tables.
class HighFlySim::Scene : public HeightfieldScene {
  public:
    explicit Scene(const HighFlySim& sim) : sim_(sim) {}

    void sample_slice(size_t index, const HeightfieldSlice& slice, float* heights,
                      Rgb* colors) override {
        const HighFlySim& s = sim_;
        const int16_t* hf = s.deck_height_.data();
        const uint8_t* sh = s.deck_shade_.data();
        const Rgb* lut = &s.slice_lut_[index * 256];
        const float wy = -s.deck_texels_ - slice.z * kHighflyTexelsPerUnit;
        const float wy_floor = std::floor(wy);
        const float fv = wy - wy_floor;
        const int32_t v0 = static_cast<int32_t>(static_cast<int64_t>(wy_floor) & (N - 1));
        const int32_t v1 = (v0 + 1) & (N - 1);
        const int16_t* row0h = hf + static_cast<size_t>(v0) * N;
        const int16_t* row1h = hf + static_cast<size_t>(v1) * N;
        const uint8_t* row0s = sh + static_cast<size_t>(v0) * N;
        const uint8_t* row1s = sh + static_cast<size_t>(v1) * N;
        const float to_world = 1.0f / (kHighflyTexelsPerUnit * kDeckHeightSteps);
        for (int32_t c = 0; c < s.iw_; c++) {
            const float wx = (kFieldX0 + slice.x_left + static_cast<float>(c) * slice.x_step) *
                             kHighflyTexelsPerUnit;
            const float wx_floor = std::floor(wx);
            const float fu = wx - wx_floor;
            const int32_t u0 = static_cast<int32_t>(static_cast<int64_t>(wx_floor) & (N - 1));
            const int32_t u1 = (u0 + 1) & (N - 1);
            const float h0 =
                static_cast<float>(row0h[u0]) + static_cast<float>(row0h[u1] - row0h[u0]) * fu;
            const float h1 =
                static_cast<float>(row1h[u0]) + static_cast<float>(row1h[u1] - row1h[u0]) * fu;
            heights[c] = (h0 + (h1 - h0) * fv) * to_world;
            const float s0 =
                static_cast<float>(row0s[u0]) + static_cast<float>(row0s[u1] - row0s[u0]) * fu;
            const float s1 =
                static_cast<float>(row1s[u0]) + static_cast<float>(row1s[u1] - row1s[u0]) * fu;
            colors[c] = lut[static_cast<uint8_t>(s0 + (s1 - s0) * fv + 0.5f)];
        }
    }

  private:
    const HighFlySim& sim_;
};

void HighFlySim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
                      const ClockReading& reading) {
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    t_ = 0.0;
    deck_texels_ = 0.0f;
    drift_.reset();
    build_scene(rng);

    towers_ = {};
    next_variant_ = 0;
    next_left_ = unit_random(rng) < 0.5f;
    // One tower already up when the run starts, part way in.
    spawn_tower(towers_[0], -random_between(rng, 7.0f, 17.0f), rng);
    next_tower_at_ = random_between(rng, 5.0f, 9.0f);
    next_tower_s_ = next_tower_at_;
    fly_start_s_ = random_between(rng, kHighflyFlyMinS, kHighflyFlyMaxS);
    next_fly_s_ = fly_start_s_;

    level_ = 0;
    baked_w_ = 0;
    baked_h_ = 0;
    apply_view(false);
    level_ = std::min(level, kHighflyLevelCount - 1);
    apply_view();
    update_clock_layout();
    drift_.set_range(clock_.bounds, w_, h_);
    build_stroke_digits(clock_.digit_unit, kDigitStroke, digits_);
    build_stroke_digits(clock_.second_unit, kDigitStroke, second_digits_);
    build_stroke_halo(digits_, clock_.halo, digit_halo_);
    build_stroke_halo(second_digits_, clock_.second_halo, second_halo_);
    size_numerals();

    FrameTarget out = frame;
    paint_all(out, reading);
    if (kHighflyLevels[level_].still) {
        release_scene();
    }
    shown_ = reading;
}

void HighFlySim::build_scene(std::minstd_rand& rng) {
    {
        HighFlyField field;
        build_highfly_field(rng, field);
        deck_height_.resize(field.height.size());
        deck_shade_.resize(field.shade.size());
        for (size_t i = 0; i < field.height.size(); i++) {
            deck_height_[i] = static_cast<int16_t>(
                std::clamp(std::lround(field.height[i] * kDeckHeightSteps), -32767L, 32767L));
            deck_shade_[i] = unit_to_u8(field.shade[i]);
        }
    }
    make_band(rng, band_);
    for (HighFlyTexture& layer : layers_) {
        make_layer(rng, layer);
    }
    for (HighFlyTexture& tower : towers_tex_) {
        make_tower(rng, tower);
    }
    std::vector<float> wisp;
    fbm(rng, 64, 128, true, {{8, 1.0f}, {32, 0.6f}}, wisp);
    for (float& v : wisp) {
        v = std::clamp(v * 0.6f, 0.0f, 1.0f);
    }
    std::vector<float> tmp;
    box_blur(wisp, 128, 64, 3, false, true, tmp);
    wisp_ = wisp;

    tower_entries_.clear();
    tower_entries_.reserve(kHistoryCap);
    fly_starts_.clear();
    fly_starts_.reserve(kHistoryCap);
    boxes_.clear();
    boxes_.reserve(kBoxCap);
    last_boxes_.clear();
    last_boxes_.reserve(kBoxCap);
    scene_released_ = false;
}

void HighFlySim::release_scene() {
    release_buffers(deck_height_, deck_shade_, sky_, slice_lut_, band_.rgba, wisp_, polar_, col_u0_,
                    col_fu_, slice_vis_, tower_entries_, fly_starts_, boxes_, last_boxes_);
    renderer_.clear();
    for (HighFlyTexture& t : layers_) {
        release_buffer(t.rgba);
    }
    for (HighFlyTexture& t : towers_tex_) {
        release_buffer(t.rgba);
    }
    baked_w_ = 0;
    baked_h_ = 0;
    scene_released_ = true;
}

void HighFlySim::apply_view(bool bake) {
    const HighFlyLevel& lv = kHighflyLevels[level_];
    scale_ = std::max<int32_t>(1, lv.scale);
    iw_ = scaled_extent(w_, scale_);
    ih_ = scaled_extent(h_, scale_);
    horizon_ = kHorizon * static_cast<float>(ih_);
    focal_x_ = (static_cast<float>(iw_) / 2.0f) / kFov;
    focal_y_ = kFocalYAt240 * static_cast<float>(ih_) / 240.0f;

    HeightfieldView v;
    v.w = iw_;
    v.h = ih_;
    v.horizon_row = horizon_;
    v.focal_x = focal_x_;
    v.focal_y = focal_y_;
    v.cam_h = kCamH;
    v.z_near = kZNear;
    v.z_far = kZFar;
    v.dz_grow = kDzGrow;
    v.slices = lv.slices;
    v.sink_depth = kSink;
    v.edge = HeightfieldEdge::SOFT_UNDER;
    v.feather_px = kFeatherAt240 * static_cast<float>(ih_) / 240.0f;
    v.span_gradient = true;
    v.first_slice_fills = true;
    v.below_color = kCLit;
    v.fog = kFog;
    renderer_.set_view(v);

    slice_lut_.resize(renderer_.slices().size() * 256);
    for (size_t s = 0; s < renderer_.slices().size(); s++) {
        const HeightfieldSlice& slice = renderer_.slices()[s];
        const float near = kNearK * std::clamp(1.0f - slice.z / kNearWhite, 0.0f, 1.0f);
        for (int32_t i = 0; i < 256; i++) {
            Rgb c = shade_to_rgb(static_cast<float>(i) / 255.0f);
            c = mix_rgb(c, kCLit, near);
            c = mix_rgb(c, kCFog, slice.fog);
            slice_lut_[s * 256 + i] = c;
        }
    }
    sky_.resize(static_cast<size_t>(iw_) * ih_);
    frame_.resize(static_cast<size_t>(iw_) * ih_);
    polar_.resize(static_cast<size_t>(iw_) * ih_ * 3);
    col_u0_.resize(static_cast<size_t>(iw_));
    col_fu_.resize(static_cast<size_t>(iw_));
    slice_vis_.resize(renderer_.slices().size());
    if (bake && (baked_w_ != iw_ || baked_h_ != ih_)) {
        build_sky();
        build_polar();
        baked_w_ = iw_;
        baked_h_ = ih_;
    }

    // The far band's top: the first row any of its texels reaches with real cover.
    Billboard band;
    band.tex = &band_;
    band.z = kBandZ;
    band.q = kBandQ;
    band.hb = kBandHb;
    band.wrap = true;
    int32_t first_row = band_.h;
    for (int32_t y = 0; y < band_.h && first_row == band_.h; y++) {
        for (int32_t x = 0; x < band_.w; x++) {
            if (band_.rgba[(static_cast<size_t>(y) * band_.w + x) * 4 + 3] > 2) {
                first_row = y;
                break;
            }
        }
    }
    const float top =
        horizon_ + (kCamH - (kBandHb + static_cast<float>(band_.h) * kBandQ)) * focal_y_ / kBandZ;
    const float sy = kBandQ * focal_y_ / kBandZ;
    band_tex_top_ = std::max(0, static_cast<int32_t>(std::floor(top + first_row * sy)) - 1);
    band_top_ = band_tex_top_;
    painted_ = false;
}

void HighFlySim::build_sky() {
    const auto W = static_cast<float>(iw_);
    const auto H = static_cast<float>(ih_);
    const float cx = W * 0.5f;
    const float cy = -H * 0.12f;
    constexpr Rgb TOP{40, 128, 180};
    constexpr Rgb BOTTOM{12, 78, 124};
    constexpr Rgb GLOW{215, 242, 252};
    struct Shaft {
        float a0, width, strength;
    };
    constexpr Shaft kShafts[] = {{-0.30f, 0.17f, 0.085f},
                                 {-0.02f, 0.22f, 0.10f},
                                 {0.27f, 0.19f, 0.085f},
                                 {0.55f, 0.16f, 0.05f}};
    for (int32_t y = 0; y < ih_; y++) {
        const float fy = static_cast<float>(y);
        const float t = std::pow(std::clamp(fy / (H * kHorizon), 0.0f, 1.0f), 0.8f);
        const Rgb base = mix_rgb(TOP, BOTTOM, t);
        const float fall = std::pow(std::clamp(1.0f - (fy - cy) / (H * 0.8f), 0.0f, 1.0f), 1.6f);
        for (int32_t x = 0; x < iw_; x++) {
            const float fx = static_cast<float>(x);
            const float dx = (fx - cx) / W;
            const float dy = (fy - cy) / H;
            const float d = std::sqrt(dx * dx + 1.6f * dy * dy);
            const float glow = 0.8f * std::exp(-(d / 0.22f) * (d / 0.22f)) +
                               0.3f * std::exp(-(d / 0.55f) * (d / 0.55f));
            Rgb c = mix_rgb(base, GLOW, std::min(glow, 1.0f));
            const float ang = std::atan2(fx - cx, fy - cy);
            float shafts = 0.0f;
            for (const Shaft& sh : kShafts) {
                const float q = (ang - sh.a0) / sh.width;
                shafts += sh.strength * std::exp(-q * q);
            }
            c = mix_rgb(c, GLOW, std::min(shafts * fall, 1.0f));
            sky_[static_cast<size_t>(y) * iw_ + x] = c;
        }
    }
}

void HighFlySim::build_polar() {
    const auto W = static_cast<float>(iw_);
    const auto H = static_cast<float>(ih_);
    for (int32_t y = 0; y < ih_; y++) {
        for (int32_t x = 0; x < iw_; x++) {
            const float dx = (static_cast<float>(x) - W / 2.0f) / (W / 2.0f);
            const float dy = (static_cast<float>(y) - H * 0.5f) / (H / 2.0f);
            const float r = std::sqrt(dx * dx + dy * dy) + 1e-3f;
            const float lr = std::log(r) * 40.0f; // about -276 to 14
            const float ang = (std::atan2(dy, dx) / kSaverTwoPi + 0.5f) * 128.0f; // 0 to 128
            const size_t i = (static_cast<size_t>(y) * iw_ + x) * 3;
            polar_[i] = static_cast<uint16_t>(std::clamp((lr + 300.0f) * 64.0f, 0.0f, 65535.0f));
            polar_[i + 1] = static_cast<uint16_t>(std::clamp(ang * 256.0f, 0.0f, 65535.0f));
            polar_[i + 2] = static_cast<uint16_t>(std::clamp(r * 1.2f, 0.0f, 1.0f) * 65535.0f);
        }
    }
}

HighFlySim::Billboard HighFlySim::tower_billboard(const Tower& t) const {
    const HighFlyTexture& tex = towers_tex_[t.variant];
    Billboard b;
    b.tex = &tex;
    b.z = t.z;
    b.q = kTowerQ * t.size;
    b.xw0 = t.x + t.drift * static_cast<float>(t_ - t.entered_s) -
            static_cast<float>(tex.w) * b.q / 2.0f;
    b.hb = kTowerHb;
    b.alpha = std::clamp((kTowerZEnter - t.z) / 50.0f, 0.0f, 1.0f) *
              std::clamp((t.z - kTowerZLeave) / (kTowerFadeFrom - kTowerZLeave), 0.0f, 1.0f);
    b.fogmul = kTowerFogmul;
    return b;
}

DirtyRect HighFlySim::tower_box(size_t i) const {
    if (i >= towers_.size() || !towers_[i].live) {
        return {};
    }
    return billboard_box(tower_billboard(towers_[i]));
}

int HighFlySim::tower_variant(size_t i) const {
    if (i >= towers_.size() || !towers_[i].live) {
        return -1;
    }
    return towers_[i].variant;
}

float HighFlySim::fly_since_s() const {
    const auto since = static_cast<float>(t_ - fly_start_s_);
    if (since < 0.0f || since > kHighflyFlyInS + kHighflyFlyHoldS + kHighflyFlyOutS) {
        return -1.0f;
    }
    return since;
}

size_t HighFlySim::towers_visible() const {
    size_t n = 0;
    for (const Tower& t : towers_) {
        n += t.live ? 1 : 0;
    }
    return n;
}

void HighFlySim::spawn_tower(Tower& slot, double entered_s, std::minstd_rand& rng) {
    const float side = next_left_ ? -1.0f : 1.0f;
    slot.entered_s = entered_s;
    slot.z = kTowerZEnter - static_cast<float>(t_ - entered_s) * kHighflySpeed;
    slot.x = side * random_between(rng, kTowerSideMin, kTowerSideMax) * kTowerZEnter * kFov;
    slot.drift = side * random_between(rng, kTowerWindMin, kTowerWindMax);
    slot.size = random_between(rng, 0.95f, 1.2f);
    slot.variant = next_variant_;
    slot.live = true;
    next_variant_ = static_cast<uint8_t>((next_variant_ + 1) % kHighflyTowerVariants);
    next_left_ = !next_left_;
}

void HighFlySim::update_schedule(std::minstd_rand& rng) {
    // Towers come on at the deck's speed and leave once faded near the camera or gone off
    // the side.
    for (Tower& t : towers_) {
        if (!t.live) {
            continue;
        }
        t.z = kTowerZEnter - static_cast<float>(t_ - t.entered_s) * kHighflySpeed;
        Billboard b = tower_billboard(t);
        b.alpha = 1.0f; // off screen is a matter of where it is, not how faded
        if (t.z < kTowerZLeave || billboard_box(b).empty()) {
            t.live = false;
        }
    }
    while (t_ >= next_tower_at_) {
        Tower* slot = nullptr;
        for (Tower& t : towers_) {
            if (!t.live) {
                slot = &t;
                break;
            }
        }
        if (slot == nullptr) {
            // Both slots up: the nearer one goes to make room.
            slot = towers_[0].z < towers_[1].z ? &towers_[0] : &towers_[1];
        }
        spawn_tower(*slot, next_tower_at_, rng);
        if (tower_entries_.size() < tower_entries_.capacity()) {
            tower_entries_.push_back(next_tower_at_);
        }
        next_tower_at_ += random_between(rng, kHighflyTowerMinS, kHighflyTowerMaxS);
    }
    next_tower_s_ = static_cast<float>(next_tower_at_ - t_);

    const float fly_end = kHighflyFlyInS + kHighflyFlyHoldS + kHighflyFlyOutS;
    if (t_ - fly_start_s_ > fly_end) {
        if (fly_starts_.size() < fly_starts_.capacity()) {
            fly_starts_.push_back(fly_start_s_);
        }
        fly_start_s_ += random_between(rng, kHighflyFlyMinS, kHighflyFlyMaxS);
    }
    next_fly_s_ = static_cast<float>(fly_start_s_ - t_);
}

size_t HighFlySim::collect_billboards(Billboard* out, size_t cap) const {
    const HighFlyLevel& lv = kHighflyLevels[level_];
    size_t n = 0;
    const float span = kLayerZfar - kLayerZnear;
    const size_t layers = std::min<size_t>(lv.layers, kHighflyLayerCount);
    for (size_t i = 0; i < layers && n < cap; i++) {
        const double travel =
            t_ * kHighflySpeed + static_cast<double>(i) * span / kHighflyLayerCount;
        const auto z =
            static_cast<float>(kLayerZfar - std::fmod(travel, static_cast<double>(span)));
        const auto cycle = static_cast<int64_t>(travel / span);
        const float fin = std::clamp((kLayerZfar - z) / 30.0f, 0.0f, 1.0f);
        const float fout = std::clamp((z - kLayerZnear) / 22.0f, 0.0f, 1.0f);
        Billboard& b = out[n++];
        b.tex =
            &layers_[static_cast<size_t>((static_cast<int64_t>(i) + cycle) % kHighflyLayerCount)];
        b.z = z;
        b.xw0 = static_cast<float>((static_cast<int64_t>(i) * 97 + cycle * 61) % 300) - 150.0f;
        b.hb = kLayerHb;
        b.q = kLayerQ;
        b.wrap = true;
        b.alpha = 0.85f * fin * fout;
    }
    if (lv.towers) {
        for (const Tower& t : towers_) {
            if (!t.live || n >= cap) {
                continue;
            }
            out[n++] = tower_billboard(t);
        }
    }
    // The cloud the camera is about to fly into.
    const auto since = static_cast<float>(t_ - fly_start_s_);
    const float body_z = kFlyFace - since * kHighflySpeed;
    if (body_z > 0.8f && body_z < 330.0f && n < cap) {
        const HighFlyTexture& tex = towers_tex_[0];
        Billboard& b = out[n++];
        b.tex = &tex;
        b.z = body_z;
        b.q = kBodyQ;
        b.xw0 = -static_cast<float>(tex.w) * kBodyQ / 2.0f;
        b.hb = kCamH - static_cast<float>(tex.h) * kBodyQ * 0.6f;
        b.alpha = std::clamp((330.0f - body_z) / 50.0f, 0.0f, 1.0f) *
                  (1.0f - highfly_whiteout(since)) * 0.9f;
    }
    std::sort(out, out + n, [](const Billboard& a, const Billboard& b) { return a.z > b.z; });
    return n;
}

HighFlySim::BillboardScreen HighFlySim::billboard_screen(const Billboard& b) const {
    BillboardScreen s;
    s.sx = b.q * focal_x_ / b.z;
    s.sy = b.q * focal_y_ / b.z;
    s.top = horizon_ + (kCamH - (b.hb + static_cast<float>(b.tex->h) * b.q)) * focal_y_ / b.z;
    s.xl = static_cast<float>(iw_) / 2.0f + b.xw0 * focal_x_ / b.z;
    return s;
}

DirtyRect HighFlySim::billboard_box(const Billboard& b) const {
    if (b.tex == nullptr || b.z <= 0.5f || b.alpha <= 0.003f) {
        return {};
    }
    const auto [sx, sy, top, xl] = billboard_screen(b);
    DirtyRect box;
    box.y1 = std::max(0, static_cast<int32_t>(std::floor(top)));
    box.y2 =
        std::min(ih_, static_cast<int32_t>(std::ceil(top + static_cast<float>(b.tex->h) * sy))) - 1;
    if (b.wrap) {
        box.x1 = 0;
        box.x2 = iw_ - 1;
    } else {
        box.x1 = std::max(0, static_cast<int32_t>(std::floor(xl)));
        box.x2 =
            std::min(iw_, static_cast<int32_t>(std::ceil(xl + static_cast<float>(b.tex->w) * sx))) -
            1;
    }
    return box;
}

void HighFlySim::draw_billboard(const Billboard& b) {
    const DirtyRect box = billboard_box(b);
    if (box.empty()) {
        return;
    }
    const HighFlyTexture& tex = *b.tex;
    const auto [sx, sy, top, xl] = billboard_screen(b);
    const float nw = kNearK * std::clamp(1.0f - b.z / kNearWhite, 0.0f, 1.0f);
    const float f = kFog.at(b.z) * b.fogmul;
    // Columns: texel and weight, or -1 outside the texture.
    for (int32_t x = box.x1; x <= box.x2; x++) {
        const float xs = (static_cast<float>(x) + 0.5f - xl) / sx - 0.5f;
        if (b.wrap) {
            const float fl = std::floor(xs);
            col_u0_[x] = wrap_index(static_cast<int64_t>(fl), tex.w);
            col_fu_[x] = static_cast<uint16_t>((xs - fl) * 256.0f + 0.5f);
        } else if (xs < -0.5f || xs > static_cast<float>(tex.w) - 0.5f) {
            col_u0_[x] = -1;
        } else {
            const float ux = std::clamp(xs, 0.0f, static_cast<float>(tex.w) - 1.001f);
            col_u0_[x] = static_cast<int32_t>(ux);
            col_fu_[x] =
                static_cast<uint16_t>((ux - static_cast<float>(col_u0_[x])) * 256.0f + 0.5f);
        }
    }
    // Fixed point from here: weights and coverage in 1/256ths. The near white and the fog are
    // both linear in the premultiplied texel, so together they are a scale of its colour plus
    // a colour times its coverage.
    const auto u8_of = [](float v) { return static_cast<int32_t>(v * 256.0f + 0.5f); };
    const int32_t keep_own = u8_of((1.0f - nw) * (1.0f - f));
    const int32_t add_r = u8_of(kCLit.r * nw * (1.0f - f) + kCFog.r * f);
    const int32_t add_g = u8_of(kCLit.g * nw * (1.0f - f) + kCFog.g * f);
    const int32_t add_b = u8_of(kCLit.b * nw * (1.0f - f) + kCFog.b * f);
    const int32_t alpha = u8_of(b.alpha);
    if (b.depth_test) {
        const std::vector<HeightfieldSlice>& slices = renderer_.slices();
        for (size_t i = 0; i < slices.size(); i++) {
            slice_vis_[i] =
                static_cast<uint16_t>(u8_of(highfly_layer_visibility(slices[i].z, b.z)));
        }
    }
    const uint16_t* depth = renderer_.depth_data();
    const auto ih = static_cast<size_t>(ih_);
    const uint8_t* rgba = tex.rgba.data();
    for (int32_t y = box.y1; y <= box.y2; y++) {
        const float ys = (static_cast<float>(y) + 0.5f - top) / sy - 0.5f;
        if (!b.wrap && (ys < -0.5f || ys > static_cast<float>(tex.h) - 0.5f)) {
            continue;
        }
        const float vy = std::clamp(ys, 0.0f, static_cast<float>(tex.h) - 1.001f);
        const auto v0 = static_cast<int32_t>(vy);
        const int32_t fv = u8_of(vy - static_cast<float>(v0));
        const uint8_t* r0 = rgba + static_cast<size_t>(v0) * tex.w * 4;
        const uint8_t* r1 = r0 + static_cast<size_t>(tex.w) * 4;
        Rgb* out = &frame_[static_cast<size_t>(y) * iw_];
        for (int32_t x = box.x1; x <= box.x2; x++) {
            const int32_t u0 = col_u0_[x];
            if (u0 < 0) {
                continue;
            }
            int32_t vis = 256;
            if (b.depth_test) {
                const uint16_t d = depth[static_cast<size_t>(x) * ih + y];
                if (d != HeightfieldRenderer::kNoDepth) {
                    vis = slice_vis_[d];
                    if (vis == 0) {
                        continue;
                    }
                }
            }
            const int32_t u1 = b.wrap ? (u0 + 1 == tex.w ? 0 : u0 + 1) : u0 + 1;
            const int32_t fu = col_fu_[x];
            const uint32_t texel = lerp_rgba(lerp_rgba(texel_at(r0, u0), texel_at(r0, u1), fu),
                                             lerp_rgba(texel_at(r1, u0), texel_at(r1, u1), fu), fv);
            // Coverage x 256, after the billboard's alpha and the deck in front.
            const int32_t a = ((static_cast<int32_t>(texel >> 24) * alpha + 127) / 255) * vis >> 8;
            if (a == 0) {
                continue;
            }
            // Each channel x 256: the texel's colour at the billboard's alpha, kept by the near
            // white and the fog, plus what they add at its coverage.
            const int32_t scale = alpha * vis >> 8;
            const int32_t rr =
                ((static_cast<int32_t>(texel & 0xFF) * scale * keep_own) >> 8) + (a * add_r >> 8);
            const int32_t gg = ((static_cast<int32_t>(texel >> 8 & 0xFF) * scale * keep_own) >> 8) +
                               (a * add_g >> 8);
            const int32_t bb =
                ((static_cast<int32_t>(texel >> 16 & 0xFF) * scale * keep_own) >> 8) +
                (a * add_b >> 8);
            Rgb& o = out[x];
            const int32_t keep = 256 - a;
            o = {static_cast<uint8_t>(std::clamp((rr + o.r * keep) >> 8, 0, 255)),
                 static_cast<uint8_t>(std::clamp((gg + o.g * keep) >> 8, 0, 255)),
                 static_cast<uint8_t>(std::clamp((bb + o.b * keep) >> 8, 0, 255))};
        }
    }
}

DirtyRect HighFlySim::clock_box_internal() const {
    const DirtyRect& b = clock_.bounds;
    return DirtyRect{b.x1 / scale_ - 1, b.y1 / scale_ - 1, b.x2 / scale_ + 1, b.y2 / scale_ + 1}
        .clipped(iw_, ih_);
}

void HighFlySim::compose(int32_t from) {
    const auto copy_sky = [this](const DirtyRect& r) {
        for (int32_t y = std::max(0, r.y1); y <= std::min(ih_ - 1, r.y2); y++) {
            const size_t o = static_cast<size_t>(y) * iw_;
            std::copy(sky_.begin() + static_cast<ptrdiff_t>(o + std::max(0, r.x1)),
                      sky_.begin() + static_cast<ptrdiff_t>(o + std::min(iw_ - 1, r.x2) + 1),
                      frame_.begin() + static_cast<ptrdiff_t>(o + std::max(0, r.x1)));
        }
    };
    copy_sky({0, from, iw_ - 1, ih_ - 1});
    for (const DirtyRect& r : boxes_) {
        copy_sky({r.x1, r.y1, r.x2, std::min(r.y2, from - 1)});
    }

    Billboard band;
    band.tex = &band_;
    band.z = kBandZ;
    band.q = kBandQ;
    band.hb = kBandHb;
    band.wrap = true;
    // The band repeats every texture width, so its drift is reduced to one width.
    const double band_period = static_cast<double>(band_.w) * kBandQ / kBandDrift;
    band.xw0 = -kBandDrift * static_cast<float>(std::fmod(t_, band_period));
    band.depth_test = false;
    draw_billboard(band);

    Scene scene(*this);
    renderer_.render(scene);
    renderer_.resolve(frame_.data(), Rgb{}, from, ih_, frame_);

    Billboard boards[kHighflyLayerCount + 3];
    const size_t n = collect_billboards(boards, std::size(boards));
    for (size_t i = 0; i < n; i++) {
        draw_billboard(boards[i]);
    }

    // A static white haze over the bottom rows: the reference's glowing foreground.
    const auto H = static_cast<float>(ih_);
    for (int32_t y = std::max(from, static_cast<int32_t>((1.0f - kHaze) * H)); y < ih_; y++) {
        const float t = std::pow(
            std::clamp((static_cast<float>(y) - H * (1.0f - kHaze)) / (H * kHaze), 0.0f, 1.0f),
            1.5f);
        Rgb* row = &frame_[static_cast<size_t>(y) * iw_];
        for (int32_t x = 0; x < iw_; x++) {
            row[x] = mix_rgb(row[x], kCLit, t);
        }
    }

    const auto since = static_cast<float>(t_ - fly_start_s_);
    const float amt = highfly_whiteout(since);
    if (amt > 0.0f) {
        // The wisp table repeats every 64 rows.
        const auto shift = static_cast<float>(std::fmod(t_ * 120.0, 64.0));
        // Over the clock the interior and the wisps together stay translucent enough to read
        // the numerals through, easing to no limit over a tenth of the screen round it, so the
        // limit never shows as an edge.
        const DirtyRect clock = clock_box_internal();
        const float feather = 0.1f * static_cast<float>(ih_);
        for (int32_t y = 0; y < ih_; y++) {
            Rgb* row = &frame_[static_cast<size_t>(y) * iw_];
            for (int32_t x = 0; x < iw_; x++) {
                const size_t pi = (static_cast<size_t>(y) * iw_ + x) * 3;
                const float lr = static_cast<float>(polar_[pi]) / 64.0f - 300.0f - shift;
                const float ang = static_cast<float>(polar_[pi + 1]) / 256.0f;
                const float rf = static_cast<float>(polar_[pi + 2]) / 65535.0f;
                const float n_wisp = bilinear_sample(wisp_.data(), 128, 64, ang, lr, true);
                const float a = std::clamp(1.5f * n_wisp, 0.0f, 1.0f) * rf * amt * 0.7f;
                const float out_x = static_cast<float>(std::max({clock.x1 - x, x - clock.x2, 0}));
                const float out_y = static_cast<float>(std::max({clock.y1 - y, y - clock.y2, 0}));
                const float near = smoothstep01(1.0f - std::hypot(out_x, out_y) / feather);
                const float most = 1.0f - near * (1.0f - kHighflyClockCoverMax);
                row[x] = highfly_inside(row[x], amt, a, most);
            }
        }
    }
}

/// Blends layers over a scene not yet known, into NumeralLayer::over: per pixel the colour they
/// leave over black and what of the scene still shows through them. A PixelWriter's contains()
/// and blend_over(), so blit_stroke_digit() draws into it.
class HighFlySim::LayerWriter {
  public:
    LayerWriter(std::vector<uint32_t>& over, int32_t w, int32_t h) : over_(over), w_(w), h_(h) {
        std::fill(over_.begin(), over_.end(), 0xFF000000u);
    }
    bool contains(int32_t x, int32_t y) const {
        return x >= 0 && y >= 0 && x < w_ && y < h_;
    }
    void blend_over(int32_t x, int32_t y, Rgb c, float alpha) {
        uint32_t& v = over_[static_cast<size_t>(y) * w_ + x];
        const Rgb own =
            mix_rgb({static_cast<uint8_t>(v >> 16 & 0xFF), static_cast<uint8_t>(v >> 8 & 0xFF),
                     static_cast<uint8_t>(v & 0xFF)},
                    c, alpha);
        const uint32_t shows = to_u8(static_cast<float>(v >> 24) * (1.0f - alpha));
        v = shows << 24 | static_cast<uint32_t>(own.r) << 16 | static_cast<uint32_t>(own.g) << 8 |
            own.b;
    }

  private:
    std::vector<uint32_t>& over_;
    int32_t w_;
    int32_t h_;
};

void HighFlySim::draw_numeral_group(LayerWriter& writer, const ClockReading& reading,
                                    NumeralGroup g, const HighFlyClockLayout& l) const {
    const std::array<int, 2> hour = two_digits(reading.time.hour);
    const std::array<int, 2> minute = two_digits(reading.time.minute);
    const std::array<int, 2> second = two_digits(reading.time.second);
    struct Place {
        const StrokeDigitSet& set;
        const StrokeDigitSet& halo;
        int32_t pad;
        const DirtyRect& box;
        int digit;
    };
    const bool seconds = g == NumeralGroup::SECONDS;
    const Place big[] = {
        {digits_, digit_halo_, l.halo, l.digits[0], hour[0]},
        {digits_, digit_halo_, l.halo, l.digits[1], hour[1]},
        {digits_, digit_halo_, l.halo, l.digits[3], minute[0]},
        {digits_, digit_halo_, l.halo, l.digits[4], minute[1]},
    };
    const Place small[] = {
        {second_digits_, second_halo_, l.second_halo, l.seconds[0], second[0]},
        {second_digits_, second_halo_, l.second_halo, l.seconds[1], second[1]},
    };
    const Place* places = seconds ? small : big;
    const size_t count = seconds ? std::size(small) : std::size(big);
    // Every shadow first, so none falls across a neighbour's strokes.
    for (size_t i = 0; i < count; i++) {
        const Place& p = places[i];
        blit_stroke_digit(writer, p.halo, p.digit, p.box.x1 - p.pad, p.box.y1 - p.pad, kHalo,
                          kHaloOpacity);
    }
    const auto strokes = [&] {
        for (size_t i = 0; i < count; i++) {
            const Place& p = places[i];
            blit_stroke_digit(writer, p.set, p.digit, p.box.x1, p.box.y1, kNumeral,
                              kNumeralOpacity);
        }
    };
    if (seconds) {
        strokes();
        return;
    }

    // The colon: two round dots, each with a soft shadow of its own.
    const DirtyRect& c = l.digits[2];
    const float r = std::max(1.0f, 0.05f * static_cast<float>(l.digit_unit));
    const auto halo = static_cast<float>(l.halo);
    const float cx = static_cast<float>(c.x1 + c.x2 + 1) / 2.0f;
    for (const bool shadow : {true, false}) {
        if (!shadow) {
            strokes();
        }
        const float reach = shadow ? r + halo : r;
        for (const float share : {0.35f, 0.75f}) {
            const float cy = static_cast<float>(c.y1) + static_cast<float>(c.y2 - c.y1 + 1) * share;
            for (int32_t y = static_cast<int32_t>(cy - reach - 1.0f);
                 y <= static_cast<int32_t>(cy + reach + 1.0f); y++) {
                for (int32_t x = static_cast<int32_t>(cx - reach - 1.0f);
                     x <= static_cast<int32_t>(cx + reach + 1.0f); x++) {
                    if (!writer.contains(x, y)) {
                        continue;
                    }
                    const float d = std::hypot(static_cast<float>(x) + 0.5f - cx,
                                               static_cast<float>(y) + 0.5f - cy);
                    if (shadow) {
                        const float fall =
                            std::clamp((reach - d) / std::max(halo, 1.0f), 0.0f, 1.0f);
                        if (fall > 0.0f) {
                            writer.blend_over(x, y, kHalo, kHaloOpacity * fall * fall);
                        }
                        continue;
                    }
                    const float cov = edge_coverage(r - d);
                    if (cov > 0.0f) {
                        writer.blend_over(x, y, kNumeral, kNumeralOpacity * cov);
                    }
                }
            }
        }
    }
}

void HighFlySim::size_numerals() {
    const HighFlyClockLayout l = layout_highfly_clock(w_, h_, 0, 0);
    DirtyRect big;
    for (const DirtyRect& d : l.digits) {
        big.add(d.x1 - l.halo, d.y1 - l.halo, d.x2 + l.halo, d.y2 + l.halo);
    }
    DirtyRect small;
    for (const DirtyRect& d : l.seconds) {
        small.add(d.x1 - l.second_halo, d.y1 - l.second_halo, d.x2 + l.second_halo,
                  d.y2 + l.second_halo);
    }
    numerals_[0].box = big;
    numerals_[1].box = small;
    for (NumeralLayer& layer : numerals_) {
        layer.over.assign(static_cast<size_t>(layer.box.area()), 0);
        layer.digits = {-1, -1, -1, -1};
    }
}

void HighFlySim::bake_numerals(NumeralLayer& layer, NumeralGroup g, const ClockReading& reading,
                               const std::array<int, 4>& digits) {
    LayerWriter writer(layer.over, layer.box.x2 - layer.box.x1 + 1,
                       layer.box.y2 - layer.box.y1 + 1);
    // The layout moved so the box's corner is the layer's.
    draw_numeral_group(writer, reading, g,
                       layout_highfly_clock(w_, h_, -layer.box.x1, -layer.box.y1));
    layer.digits = digits;
}

namespace {

/// `layer` composed over `frame` with its box moved by (dx, dy), in output format `F`.
template <PixelFormat F>
void compose_numeral_layer(const std::vector<uint32_t>& over, const DirtyRect& box, int32_t dx,
                           int32_t dy, FrameTarget& frame) {
    const int32_t bw = box.x2 - box.x1 + 1;
    const DirtyRect at = DirtyRect{box.x1 + dx, box.y1 + dy, box.x2 + dx, box.y2 + dy}.clipped(
        static_cast<int32_t>(frame.w), static_cast<int32_t>(frame.h));
    const PixelWriter writer(frame);
    for (int32_t y = at.y1; y <= at.y2; y++) {
        uint8_t* row = writer.row(y);
        const uint32_t* src = over.data() + static_cast<size_t>(y - dy - box.y1) * bw;
        for (int32_t x = at.x1; x <= at.x2; x++) {
            const uint32_t v = src[x - dx - box.x1];
            const uint32_t shows = v >> 24;
            if (shows == 255) {
                continue;
            }
            Rgb bg;
            if constexpr (F == PixelFormat::RGB565) {
                const uint8_t* p = row + static_cast<size_t>(x) * 2;
                bg = PixelWriter::unpack565(static_cast<uint16_t>(p[0] | p[1] << 8));
            } else {
                const uint8_t* p = row + static_cast<size_t>(x) * 4;
                bg = {p[2], p[1], p[0]};
            }
            const auto ch = [shows](uint32_t own, uint32_t under) {
                return static_cast<uint8_t>(std::min(255u, own + (under * shows + 127u) / 255u));
            };
            PixelWriter::put_dithered_in_row<F>(
                row, x, y, {ch(v >> 16 & 0xFF, bg.r), ch(v >> 8 & 0xFF, bg.g), ch(v & 0xFF, bg.b)});
        }
    }
}

} // namespace

void HighFlySim::draw_numerals(FrameTarget& frame, const ClockReading& reading) {
    const std::array<int, 2> hour = two_digits(reading.time.hour);
    const std::array<int, 2> minute = two_digits(reading.time.minute);
    const std::array<int, 2> second = two_digits(reading.time.second);
    const std::array<int, 4> wants[] = {{hour[0], hour[1], minute[0], minute[1]},
                                        {second[0], second[1], -1, -1}};
    for (size_t i = 0; i < numerals_.size(); i++) {
        NumeralLayer& layer = numerals_[i];
        if (layer.over.empty()) {
            continue;
        }
        if (layer.digits != wants[i]) {
            bake_numerals(layer, static_cast<NumeralGroup>(i), reading, wants[i]);
        }
        if (frame.format == PixelFormat::RGB565) {
            compose_numeral_layer<PixelFormat::RGB565>(layer.over, layer.box, drift_.dx, drift_.dy,
                                                       frame);
        } else {
            compose_numeral_layer<PixelFormat::XRGB8888>(layer.over, layer.box, drift_.dx,
                                                         drift_.dy, frame);
        }
    }
}

void HighFlySim::update_clock_layout() {
    clock_ = layout_highfly_clock(w_, h_, drift_.dx, drift_.dy);
}

void HighFlySim::paint_all(FrameTarget& frame, const ClockReading& reading) {
    boxes_.clear();
    last_boxes_.clear();
    compose(0);
    upscale_frame(frame_, iw_, ih_, scale_, 0, ih_, frame);
    draw_numerals(frame, reading);
    painted_ = true;
    last_full_ = false;
}

void HighFlySim::request_level(size_t level) {
    const size_t next = std::min(level, kHighflyLevelCount - 1);
    if (next == level_) {
        return;
    }
    level_ = next;
    if (!scene_released_) {
        // A released scene is built again, at this rung, by the next step.
        apply_view();
    }
}

void HighFlySim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                      const ClockReading& reading, std::vector<DirtyRect>& dirty) {
    dirty.clear();
    const HighFlyLevel& lv = kHighflyLevels[level_];
    const DirtyRect whole = DirtyRect::whole(w_, h_);
    if (scene_released_ && (!lv.still || !painted_)) {
        // A rung change after the still one: the sky comes back before it is drawn.
        build_scene(rng);
        apply_view();
    }

    const DirtyRect old_clock = clock_box_internal();
    const bool moved = drift_.step(dt_ms);
    if (moved) {
        update_clock_layout();
    }

    if (lv.still) {
        // The scene stands still: paint it once, then only the clock when it changes.
        if (!painted_) {
            paint_all(frame, reading);
            dirty.push_back(whole);
            release_scene();
        } else if (moved || !clock_face_same(reading, shown_, false)) {
            DirtyRect box = clock_box_internal();
            box.add(old_clock);
            const DirtyRect out = upscale_frame(frame_, iw_, ih_, scale_, box.y1, box.y2 + 1, frame,
                                                box.x1, box.x2 + 1);
            draw_numerals(frame, reading);
            if (!out.empty()) {
                dirty.push_back(out);
            }
        }
        shown_ = reading;
        return;
    }

    t_ += static_cast<double>(dt_ms) / 1000.0;
    // The field repeats every kHighflyField texels, so the deck's scroll is reduced to one tile.
    deck_texels_ = static_cast<float>(
        std::fmod(t_ * kHighflySpeed * kHighflyTexelsPerUnit, static_cast<double>(N)));
    update_schedule(rng);
    const auto since = static_cast<float>(t_ - fly_start_s_);
    const bool whiteout = highfly_whiteout(since) > 0.0f;
    const bool full = !painted_ || whiteout || last_full_;

    // Boxes above the band: the clock (where it is and where it was), the towers and the
    // approaching cloud, this frame and last.
    last_boxes_.swap(boxes_);
    boxes_.clear();
    const auto add_box = [this](const DirtyRect& b) {
        if (boxes_.size() < kBoxCap) {
            boxes_.push_back(b);
        } else {
            boxes_.back().add(b);
        }
    };
    add_box(clock_box_internal());
    if (moved) {
        add_box(old_clock);
    }
    Billboard boards[kHighflyLayerCount + 3];
    const size_t n = collect_billboards(boards, std::size(boards));
    for (size_t i = 0; i < n; i++) {
        if (!boards[i].wrap) {
            const DirtyRect b = billboard_box(boards[i]);
            if (!b.empty()) {
                add_box(b);
            }
        }
    }
    const size_t mine = boxes_.size();
    for (const DirtyRect& b : last_boxes_) {
        add_box(b);
    }

    // The band: from the far billows' top or the deck's highest point last frame, less a few
    // rows for the deck rising, and down from wherever last frame's band began.
    int32_t top = band_tex_top_;
    if (painted_) {
        for (int32_t x = 0; x < iw_; x++) {
            top = std::min(top, static_cast<int32_t>(std::floor(renderer_.top_at(x))) - kDeckRise);
        }
    }
    top = std::max(0, top);
    int32_t from = full ? 0 : std::min(top, band_top_);
    compose(from);

    const DirtyRect band = upscale_frame(frame_, iw_, ih_, scale_, from, ih_, frame);
    if (!band.empty()) {
        dirty.push_back(band);
    }
    if (!full) {
        for (const DirtyRect& b : boxes_) {
            if (b.y1 >= from) {
                continue;
            }
            const DirtyRect out = upscale_frame(frame_, iw_, ih_, scale_, b.y1,
                                                std::min(b.y2 + 1, from), frame, b.x1, b.x2 + 1);
            if (!out.empty()) {
                dirty.push_back(out);
            }
        }
    }
    draw_numerals(frame, reading);
    band_top_ = top;
    boxes_.resize(mine);
    painted_ = true;
    last_full_ = whiteout;
    if (full) {
        dirty.assign(1, whole);
    } else {
        reduce_dirty(dirty, kSaverMaxDirtyAreas, w_, h_);
    }
    shown_ = reading;
}

size_t HighFlySim::heap_bytes() const {
    size_t n = buffer_bytes(deck_height_, deck_shade_) + renderer_.heap_bytes() +
               buffer_bytes(sky_, frame_, slice_lut_) + band_.heap_bytes() +
               buffer_bytes(wisp_, polar_, col_u0_, col_fu_, slice_vis_, tower_entries_,
                            fly_starts_, boxes_, last_boxes_) +
               digits_.heap_bytes() + second_digits_.heap_bytes() + digit_halo_.heap_bytes() +
               second_halo_.heap_bytes();
    for (const NumeralLayer& layer : numerals_) {
        n += buffer_bytes(layer.over);
    }
    for (const HighFlyTexture& t : layers_) {
        n += t.heap_bytes();
    }
    for (const HighFlyTexture& t : towers_tex_) {
        n += t.heap_bytes();
    }
    return n;
}

void HighFlySim::clear() {
    release_scene();
    release_buffer(frame_);
    digits_.clear();
    second_digits_.clear();
    digit_halo_.clear();
    second_halo_.clear();
    for (NumeralLayer& layer : numerals_) {
        release_buffer(layer.over);
        layer.digits = {-1, -1, -1, -1};
    }
    towers_ = {};
    painted_ = false;
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
