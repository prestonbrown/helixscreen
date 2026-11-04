// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_aqua_sim.h"

#include "screensaver_blur.h"
#include "screensaver_buffer.h"
#include "screensaver_fast_math.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace helix::ui {

using screensaver::random_below;
using screensaver::random_between;
using screensaver::random_normal;
using screensaver::smoothstep01;
using screensaver::unit_random;
using screensaver::wrap_index;

namespace {

constexpr float kGravity = 9.81f;
/// Tank depth and length, for the dispersion relation and the sloshing modes.
constexpr float kTankDepth = 1.2f;
constexpr float kTankL = 7.0f;
constexpr float kCamH = 0.80f;
/// Focal length and horizon offset as tuned at a 400 x 240 internal frame.
constexpr float kFocalAt400 = 500.0f;
constexpr float kPitchAt240 = -92.0f;
constexpr float kDzGrow = 1.015f;
constexpr float F0 = 0.02f;
constexpr float kWrinkleAmp = 0.7f;
constexpr int32_t kWrinkleN = 64;
constexpr int32_t kEnvN = 512;
constexpr size_t kEnvLevels = 6;
constexpr float kEnvSigmas[kEnvLevels] = {0.0f, 0.008f, 0.016f, 0.032f, 0.064f, 0.128f};
/// Sun direction, normalised from (-0.3, 0.75, -0.6).
constexpr float kSunX = -0.29814f;
constexpr float kSunY = 0.74536f;
constexpr float kSunZ = -0.59628f;
constexpr uint32_t kEmitStepMs = 50;
constexpr uint32_t kEmitStepsMax = 4;
constexpr uint16_t kGlintMs = 250;
constexpr size_t kGlintCap = 64;
constexpr size_t kBubbleCount = 300;
constexpr size_t kImpulseCap = 8;
constexpr float kImpulseLife = 12.0f;
/// Crest score a grid cell needs to throw droplets, and how far behind the near edge.
constexpr float kCrestScore = 2.6f;
constexpr float kCrestDepth = 3.0f;
constexpr Rgb kTintNone{};
constexpr Rgb kRingTrack{40, 42, 46};
constexpr Rgb kRingFill{220, 226, 232};
constexpr Rgb kDigitColor{230, 236, 242};
constexpr float kDigitOpacity = 0.9f;
constexpr float kDigitStroke = 0.06f;

float omega_for(float k) {
    return std::sqrt(kGravity * k * std::tanh(k * kTankDepth));
}

float sign(float v) {
    return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
}

constexpr float kModeAmps[3] = {0.34f, 0.20f, 0.10f};
struct WaveShape {
    float lambda, amp, angle, steep;
};
constexpr WaveShape kWaveShapes[4] = {
    {2.6f, 0.13f, 0.15f, 0.9f},
    {1.7f, 0.08f, -0.5f, 0.9f},
    {1.1f, 0.045f, 0.9f, 0.85f},
    {0.75f, 0.02f, -1.2f, 0.8f},
};

/// Splits `value` over the four pixels around (fx, fy) and max-blends each share.
void splat_max(PixelWriter& w, const std::vector<Rgb>& tint, float fx, float fy, float value) {
    const float x0f = std::floor(fx);
    const float y0f = std::floor(fy);
    const float ax = fx - x0f;
    const float ay = fy - y0f;
    const auto x0 = static_cast<int32_t>(x0f);
    const auto y0 = static_cast<int32_t>(y0f);
    const float shares[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
    const int32_t dx[4] = {0, 1, 0, 1};
    const int32_t dy[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; i++) {
        const int32_t x = x0 + dx[i];
        const int32_t y = y0 + dy[i];
        const float v = value * shares[i];
        if (v < 0.004f || !w.contains(x, y)) {
            continue;
        }
        w.blend_max(x, y, tint[unit_to_u8(v)]);
    }
}

/// Rim brightness profile across the near edge, by rows from it: a bright line with a soft
/// glow trailing down into the water.
float rim_profile(float d) {
    constexpr float XS[] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    constexpr float kYs[] = {0.0f, 0.4f, 1.0f, 0.7f, 0.4f, 0.2f, 0.1f, 0.0f};
    if (d <= XS[0] || d >= XS[7]) {
        return 0.0f;
    }
    const auto i = static_cast<size_t>(std::floor(d - XS[0]));
    const float f = d - XS[i];
    return kYs[i] + (kYs[i + 1] - kYs[i]) * f;
}

} // namespace

// ---------- AquaWater ----------

namespace {

/// `omega t + phase` reduced to one turn in double, so a long run keeps its precision.
float phase_at(double omega, double t, double phase) {
    return static_cast<float>(std::fmod(omega * t + phase, 2.0 * 3.14159265358979323846));
}

/// One Gerstner wave at (x, z) with `phase` = phi - w t, already reduced.
float gerstner_at_phase(const AquaWave& w, float x, float z, float phase) {
    // p0 = p - Q A d cos(k d.p - w t + phi), and since d is a unit vector,
    // k d.p0 - w t + phi = theta - k Q A cos(theta).
    const float theta = w.k * (w.dir_x * x + w.dir_z * z) + phase;
    return w.amp * fast_sin(theta - w.k * w.steep * w.amp * fast_cos(theta));
}

} // namespace

float aqua_gerstner_height(const AquaWave& w, float x, float z, double t) {
    return gerstner_at_phase(w, x, z, phase_at(-w.omega, t, w.phase));
}

void AquaWater::init(std::minstd_rand& rng, int32_t gx, int32_t gz, float u_max) {
    u_max_ = u_max;
    t_ = 0.0f;
    dt_ = 0.0f;
    next_impulse_ = 0.6;
    fresh_ = true;
    for (size_t n = 0; n < modes_.size(); n++) {
        const float k = static_cast<float>(n + 1) * kSaverPi / kTankL;
        modes_[n] = {kModeAmps[n], k, omega_for(k), random_between(rng, 0.0f, kSaverTwoPi)};
    }
    for (size_t i = 0; i < waves_.size(); i++) {
        const WaveShape& s = kWaveShapes[i];
        const float k = kSaverTwoPi / s.lambda;
        waves_[i] = {s.amp,
                     k,
                     omega_for(k),
                     std::cos(s.angle),
                     std::sin(s.angle),
                     s.steep,
                     random_between(rng, 0.0f, kSaverTwoPi)};
    }
    impulses_.clear();
    impulses_.reserve(kImpulseCap);
    set_grid(gx, gz);
}

void AquaWater::set_grid(int32_t gx, int32_t gz) {
    gx_ = std::max(2, gx);
    gz_ = std::max(2, gz);
    h_.assign(static_cast<size_t>(gx_) * gz_, 0.0f);
    prev_h_.assign(h_.size(), 0.0f);
    fresh_ = true;
}

float AquaWater::cell_u(int32_t i) const {
    return -u_max_ + 2.0f * u_max_ * static_cast<float>(i) / static_cast<float>(gx_ - 1);
}

float AquaWater::cell_z(int32_t k) const {
    return kAquaZNear + kAquaDepth * static_cast<float>(k) / static_cast<float>(gz_ - 1);
}

float AquaWater::height(float x, float z, double t) const {
    float h = 0.0f;
    for (const AquaMode& m : modes_) {
        h += m.amp * fast_cos(m.k * (x + kTankL / 2.0f)) * fast_cos(phase_at(m.omega, t, m.phase));
    }
    for (const AquaWave& w : waves_) {
        h += aqua_gerstner_height(w, x, z, t);
    }
    for (const AquaImpulse& e : impulses_) {
        const auto age = static_cast<float>(t - e.t0);
        if (age < 0.0f) {
            continue;
        }
        const float c = 1.3f * age;
        const float env = e.amp * std::exp(-age / 3.0f) * std::min(1.0f, age / 0.6f) * 0.5f;
        const float sigma = 0.22f + 0.18f * age;
        h += env * fast_gauss(z - e.z) *
             (fast_gauss((x - e.x - c) / sigma) + fast_gauss((x - e.x + c) / sigma));
    }
    return h;
}

void AquaWater::evaluate(double t, std::minstd_rand& rng) {
    impulses_.erase(std::remove_if(impulses_.begin(), impulses_.end(),
                                   [t](const AquaImpulse& e) { return t - e.t0 >= kImpulseLife; }),
                    impulses_.end());
    while (t >= next_impulse_) {
        if (impulses_.size() >= kImpulseCap) {
            impulses_.erase(impulses_.begin());
        }
        impulses_.push_back({next_impulse_, random_between(rng, -2.5f, 2.5f),
                             kAquaZNear + random_between(rng, 1.2f, 2.4f),
                             random_between(rng, 0.5f, 0.9f)});
        next_impulse_ += random_between(rng, 2.0f, 4.0f);
    }

    h_.swap(prev_h_);
    dt_ = fresh_ ? 0.0f : static_cast<float>(t - t_);
    t_ = t;

    float mode_time[3];
    for (size_t m = 0; m < modes_.size(); m++) {
        mode_time[m] = modes_[m].amp * fast_cos(phase_at(modes_[m].omega, t, modes_[m].phase));
    }
    float wave_phase[4];
    for (size_t i = 0; i < waves_.size(); i++) {
        wave_phase[i] = phase_at(-waves_[i].omega, t, waves_[i].phase);
    }
    struct Live {
        float x, z, c, sigma, env;
    };
    Live live[kImpulseCap];
    size_t live_n = 0;
    for (const AquaImpulse& e : impulses_) {
        const auto age = static_cast<float>(t - e.t0);
        if (age < 0.0f) {
            continue;
        }
        live[live_n++] = {e.x, e.z, 1.3f * age, 0.22f + 0.18f * age,
                          e.amp * std::exp(-age / 3.0f) * std::min(1.0f, age / 0.6f) * 0.5f};
    }

    for (int32_t k = 0; k < gz_; k++) {
        const float z = cell_z(k);
        float row_env[kImpulseCap];
        for (size_t n = 0; n < live_n; n++) {
            row_env[n] = live[n].env * fast_gauss(z - live[n].z);
        }
        float* row = &h_[static_cast<size_t>(k) * gx_];
        for (int32_t i = 0; i < gx_; i++) {
            const float x = cell_u(i) * z;
            float h = 0.0f;
            for (size_t m = 0; m < modes_.size(); m++) {
                h += mode_time[m] * fast_cos(modes_[m].k * (x + kTankL / 2.0f));
            }
            for (size_t w = 0; w < waves_.size(); w++) {
                h += gerstner_at_phase(waves_[w], x, z, wave_phase[w]);
            }
            for (size_t n = 0; n < live_n; n++) {
                if (row_env[n] < 1e-4f) {
                    continue;
                }
                const Live& e = live[n];
                h += row_env[n] * (fast_gauss((x - e.x - e.c) / e.sigma) +
                                   fast_gauss((x - e.x + e.c) / e.sigma));
            }
            row[i] = h;
        }
    }
    if (fresh_) {
        prev_h_ = h_;
        fresh_ = false;
    }
}

float AquaWater::height_at(float x, float z) const {
    const float u = x / std::max(z, 0.01f);
    const float fi = (u + u_max_) / (2.0f * u_max_) * static_cast<float>(gx_ - 1);
    const float fk = (z - kAquaZNear) / kAquaDepth * static_cast<float>(gz_ - 1);
    return bilinear_sample(h_.data(), gx_, gz_, fi, fk, false);
}

float AquaWater::exact_height(float x, float z, double t) const {
    return height(x, z, t);
}

float AquaWater::rise_at(int32_t i, int32_t k) const {
    if (dt_ <= 0.0f) {
        return 0.0f;
    }
    const size_t idx = static_cast<size_t>(k) * gx_ + i;
    return (h_[idx] - prev_h_[idx]) / dt_;
}

size_t AquaWater::heap_bytes() const {
    return buffer_bytes(impulses_, h_, prev_h_);
}

void AquaWater::clear() {
    release_buffers(impulses_, h_, prev_h_);
    gx_ = 0;
    gz_ = 0;
}

// ---------- AquaSplashes ----------

void AquaSplashes::init(size_t capacity) {
    capacity_ = capacity;
    droplets_.clear();
    droplets_.reserve(capacity);
    emitted_ = 0;
}

bool AquaSplashes::emit(const AquaDroplet& drop) {
    if (droplets_.size() >= capacity_) {
        return false;
    }
    droplets_.push_back(drop);
    emitted_++;
    return true;
}

void AquaSplashes::emit_crests(const AquaWater& water, std::minstd_rand& rng) {
    if (droplets_.size() >= capacity_) {
        return;
    }
    const int32_t gx = water.grid_x();
    const int32_t gz = water.grid_z();
    const auto is_crest = [&water](int32_t i, int32_t k) {
        return water.rise_at(i, k) + 2.0f * std::max(water.cell_height(i, k), 0.0f) > kCrestScore;
    };
    int32_t rows = 0;
    while (rows < gz && water.cell_z(rows) < kAquaZNear + kCrestDepth) {
        rows++;
    }
    int32_t candidates = 0;
    for (int32_t k = 0; k < rows; k++) {
        for (int32_t i = 0; i < gx; i++) {
            candidates += is_crest(i, k) ? 1 : 0;
        }
    }
    if (candidates == 0) {
        return;
    }
    const int32_t picks = std::min(candidates, 3);
    for (int32_t p = 0; p < picks; p++) {
        int32_t target = random_below(rng, candidates);
        int32_t ci = 0;
        int32_t ck = 0;
        for (int32_t k = 0; k < rows && target >= 0; k++) {
            for (int32_t i = 0; i < gx; i++) {
                if (is_crest(i, k) && target-- == 0) {
                    ci = i;
                    ck = k;
                    break;
                }
            }
        }
        if (unit_random(rng) >= 0.15f) {
            continue;
        }
        const int32_t count = 1 + random_below(rng, 3);
        const float z = water.cell_z(ck);
        const float u = water.cell_u(ci);
        const float h = water.cell_height(ci, ck);
        const float rise = std::max(water.rise_at(ci, ck), 0.0f);
        const int32_t il = std::max(0, ci - 1);
        const int32_t ir = std::min(gx - 1, ci + 1);
        const float du = water.cell_u(ir) - water.cell_u(il);
        const float slope_x =
            du > 0.0f ? (water.cell_height(ir, ck) - water.cell_height(il, ck)) / du / z : 0.0f;
        // The droplets peel off toward the downslope side of the crest.
        const float lean = -sign(slope_x) * 0.35f;
        for (int32_t c = 0; c < count; c++) {
            AquaDroplet d;
            d.x = u * z;
            d.y = h + 0.02f;
            d.z = z;
            d.vx = 1.5f * lean + random_normal(rng, 0.0f, 0.3f);
            d.vy = 0.6f * rise + random_between(rng, 0.8f, 1.5f);
            d.vz = random_normal(rng, -0.1f, 0.1f);
            if (!emit(d)) {
                return;
            }
        }
    }
}

void AquaSplashes::move(float dt, const AquaWater& water, std::vector<AquaLanding>* landed) {
    const float g = kAquaDropletGravity * kGravity;
    const float damp = 1.0f - 0.3f * dt;
    const float z_far = kAquaZNear + kAquaDepth;
    for (size_t i = 0; i < droplets_.size();) {
        AquaDroplet& d = droplets_[i];
        d.x += d.vx * dt;
        d.y += d.vy * dt;
        d.z += d.vz * dt;
        d.vy -= g * dt;
        d.vx *= damp;
        d.vy *= damp;
        d.vz *= damp;
        // The tank's walls: a droplet stays inside the water's depth, reflected back in.
        if (d.z < kAquaZNear) {
            d.z = std::min(2.0f * kAquaZNear - d.z, z_far);
            d.vz = -d.vz;
        } else if (d.z > z_far) {
            d.z = std::max(2.0f * z_far - d.z, kAquaZNear);
            d.vz = -d.vz;
        }
        const float surface = water.height_at(d.x, d.z);
        if (d.y < surface) {
            if (landed != nullptr) {
                landed->push_back({d.x, d.y, surface, d.z});
            }
            droplets_[i] = droplets_.back();
            droplets_.pop_back();
            continue;
        }
        i++;
    }
}

void AquaSplashes::step(float dt, const AquaWater& water, std::minstd_rand& rng,
                        std::vector<AquaLanding>* landed) {
    emit_crests(water, rng);
    move(dt, water, landed);
}

size_t AquaSplashes::heap_bytes() const {
    return buffer_bytes(droplets_);
}

void AquaSplashes::clear() {
    release_buffer(droplets_);
    capacity_ = 0;
    emitted_ = 0;
}

// ---------- Clock ----------

AquaRings aqua_rings(const ClockReading& reading) {
    const ClockTime& t = reading.time;
    const float seconds = static_cast<float>(t.second);
    const float minutes = static_cast<float>(t.minute) + seconds / 60.0f;
    AquaRings rings;
    if (reading.use_24h) {
        rings.hour = (static_cast<float>(t.hour_of_day) + minutes / 60.0f) / 24.0f;
    } else {
        rings.hour = (static_cast<float>(t.hour_of_day % 12) + minutes / 60.0f) / 12.0f;
    }
    rings.minute = minutes / 60.0f;
    switch (reading.mode) {
    case ClockMode::CLOCK:
        rings.top = seconds / 60.0f;
        break;
    case ClockMode::COUNTDOWN:
    case ClockMode::PAUSED:
        rings.top = static_cast<float>(std::clamp(reading.progress_pct, 0, 100)) / 100.0f;
        break;
    case ClockMode::DONE:
        rings.top = 1.0f;
        break;
    }
    return rings;
}

AquaClockLayout layout_aqua_clock(int32_t w, int32_t h, int32_t dx, int32_t dy) {
    const float s = static_cast<float>(std::min(w, h)) / 480.0f;
    const auto px = [s](float v) { return static_cast<int32_t>(std::lround(v * s)); };
    AquaClockLayout l;
    l.ring_r = std::max(6, px(36.0f));
    l.top_r = std::max(4, px(22.0f));
    l.ring_width = std::max(1, px(2.0f));
    const int32_t cy = static_cast<int32_t>(std::lround(0.80f * static_cast<float>(h))) + dy;
    const int32_t cx = w / 2 + dx;
    l.hour_cx = cx - px(45.0f);
    l.minute_cx = cx + px(45.0f);
    l.hour_cy = cy;
    l.minute_cy = cy;
    l.top_cx = cx;
    l.top_cy = cy - px(68.0f);
    l.digit_unit =
        std::max(3, static_cast<int32_t>(std::lround(0.8f * static_cast<float>(l.ring_r) /
                                                     (kStrokeDigitPathH + kDigitStroke))));
    l.text = {cx - px(220.0f), cy + px(48.0f), cx + px(220.0f), cy + px(70.0f)};
    const int32_t pad = l.ring_width + 1;
    l.bounds = {};
    l.bounds.add(l.hour_cx - l.ring_r - pad, l.hour_cy - l.ring_r - pad, l.hour_cx + l.ring_r + pad,
                 l.hour_cy + l.ring_r + pad);
    l.bounds.add(l.minute_cx - l.ring_r - pad, l.minute_cy - l.ring_r - pad,
                 l.minute_cx + l.ring_r + pad, l.minute_cy + l.ring_r + pad);
    l.bounds.add(l.top_cx - l.top_r - pad, l.top_cy - l.top_r - pad, l.top_cx + l.top_r + pad,
                 l.top_cy + l.top_r + pad);
    l.bounds.add(l.text);
    return l;
}

// ---------- AquaSim ----------

/// Shades one slice of water for the renderer: fresnel reflection of the softbox bands, a sun
/// glint, and light through thin crests, from the height grid plus one octave of wrinkle.
class AquaSim::Scene : public HeightfieldScene {
  public:
    explicit Scene(AquaSim& sim) : sim_(sim) {}

    void sample_slice(size_t index, const HeightfieldSlice& slice, float* heights,
                      Rgb* colors) override {
        AquaSim& s = sim_;
        const AquaWater& water = s.water_;
        const int32_t gx = water.grid_x();
        const int32_t gz = water.grid_z();
        // The wrinkle scrolls by whole table turns, reduced in double.
        const auto shift_u = static_cast<float>(std::fmod(s.t_sim_ * 0.4, kWrinkleN));
        const auto shift_v = static_cast<float>(std::fmod(s.t_sim_ * 0.6, kWrinkleN));
        const float z = slice.z;
        const float fk = std::clamp((z - kAquaZNear) / kAquaDepth * static_cast<float>(gz - 1),
                                    0.0f, static_cast<float>(gz - 1));
        const int32_t k0 = std::min(static_cast<int32_t>(fk), gz - 2);
        const float fz = fk - static_cast<float>(k0);
        const float amp =
            kWrinkleAmp * std::clamp(1.4f - 1.6f * (z - kAquaZNear) / kAquaDepth, 0.1f, 1.0f);
        const float wv = z * 14.0f - shift_v;
        const float wv_floor = std::floor(wv);
        const float av = wv - wv_floor;
        const int32_t v0 = static_cast<int32_t>(static_cast<int64_t>(wv_floor) & (kWrinkleN - 1));
        const int32_t v1 = (v0 + 1) & (kWrinkleN - 1);
        const float* hg = water.heights().data();
        const float* gxg = s.grid_gx_.data();
        const float* gzg = s.grid_gz_.data();
        const float* thg = s.grid_thin_.data();
        const float* wr = s.wrinkle_.data();
        float* scratch = s.scratch_.data();
        const int32_t iw = s.iw_;

        for (int32_t c = 0; c < iw; c++) {
            const int32_t i0 = s.col_i_[c];
            const float fx = s.col_f_[c];
            const size_t a = static_cast<size_t>(k0) * gx + i0;
            const size_t b = a + static_cast<size_t>(gx);
            const auto bil = [&](const float* g) {
                const float top = g[a] + (g[a + 1] - g[a]) * fx;
                const float bot = g[b] + (g[b + 1] - g[b]) * fx;
                return top + (bot - top) * fz;
            };
            const float h = bil(hg);
            const float x = slice.x_left + static_cast<float>(c) * slice.x_step;

            const float wu = x * 4.0f + shift_u;
            const float wu_floor = std::floor(wu);
            const float au = wu - wu_floor;
            const int32_t u0 =
                static_cast<int32_t>(static_cast<int64_t>(wu_floor) & (kWrinkleN - 1));
            const int32_t u1 = (u0 + 1) & (kWrinkleN - 1);
            const auto wbil = [&](int32_t ch) {
                const float p = wr[(v0 * kWrinkleN + u0) * 2 + ch];
                const float q = wr[(v0 * kWrinkleN + u1) * 2 + ch];
                const float r = wr[(v1 * kWrinkleN + u0) * 2 + ch];
                const float sv = wr[(v1 * kWrinkleN + u1) * 2 + ch];
                return (p + (q - p) * au) * (1.0f - av) + (r + (sv - r) * au) * av;
            };
            const float hx = bil(gxg) + amp * wbil(0);
            const float hz = bil(gzg) + amp * wbil(1);

            const float n_len = 1.0f / std::sqrt(hx * hx + 1.0f + hz * hz);
            const float nx = -hx * n_len;
            const float ny = n_len;
            const float nz = -hz * n_len;
            const float vy0 = h - kCamH;
            const float v_len = 1.0f / std::sqrt(x * x + vy0 * vy0 + z * z);
            const float vx = x * v_len;
            const float vy = vy0 * v_len;
            const float vz = z * v_len;
            const float cosi = std::clamp(-(vx * nx + vy * ny + vz * nz), 0.0f, 1.0f);
            const float rx = vx + 2.0f * cosi * nx;
            const float ry = vy + 2.0f * cosi * ny;
            const float rz = vz + 2.0f * cosi * nz;
            const float m = 1.0f - cosi;
            const float m2 = m * m;
            const float fres = std::max(F0 + (1.0f - F0) * m2 * m2 * m, kAquaFresnelFloor);
            float sp = std::clamp(rx * kSunX + ry * kSunY + rz * kSunZ, 0.0f, 1.0f);
            const float s2 = sp * sp;
            const float s4 = s2 * s2;
            const float s8 = s4 * s4;
            const float s16 = s8 * s8;
            const float s32 = s16 * s16;
            const float s64 = s32 * s32;
            sp = s64 * s32 * s16 * s8;

            heights[c] = h;
            s.el_[c] = std::clamp(ry, -1.0f, 1.0f);
            scratch[c * 4 + 0] = fres;
            scratch[c * 4 + 1] = sp;
            scratch[c * 4 + 2] = bil(thg);
        }

        // Reflection: the environment pre-blurred as much as the reflected elevation moves
        // across this pixel, so a crisp band neither aliases nor shimmers.
        const float* env = s.env_.data();
        for (int32_t c = 0; c < iw; c++) {
            const float el = s.el_[c];
            float foot = std::fabs(s.el_[std::min(c + 1, iw - 1)] - el);
            if (index > 0) {
                foot = std::max(foot, std::fabs(el - s.prev_el_[c]));
            }
            const float lvl = std::clamp(std::log2(std::max(foot, 1e-4f) / 0.02f) + 1.0f, 0.0f,
                                         static_cast<float>(kEnvLevels - 1));
            const auto l0 = static_cast<size_t>(lvl);
            const size_t l1 = std::min(l0 + 1, kEnvLevels - 1);
            const float lf = lvl - static_cast<float>(l0);
            const auto ei = static_cast<size_t>(std::clamp((el + 1.0f) * 255.5f, 0.0f, 511.0f));
            const float refl =
                env[l0 * kEnvN + ei] + (env[l1 * kEnvN + ei] - env[l0 * kEnvN + ei]) * lf;
            const float fres = scratch[c * 4 + 0];
            const float sp = scratch[c * 4 + 1];
            const float thin = scratch[c * 4 + 2];
            const float trans = (1.0f - fres) * (0.05f + 0.22f * thin);
            const float shade = (fres * refl * 4.0f + 1.5f * sp + trans) * slice.fog;
            if (index == 0) {
                s.front_shade_[c] = shade;
            }
            colors[c] = s.tint_[unit_to_u8(shade)];
        }
        s.prev_el_.swap(s.el_);
    }

  private:
    AquaSim& sim_;
};

namespace {

void build_env(std::vector<float>& env) {
    env.assign(kEnvLevels * kEnvN, 0.0f);
    std::vector<float> base(kEnvN);
    const auto box = [](float e, float c, float w, float a, float soft) {
        return a * smoothstep01((w - std::fabs(e - c)) / soft);
    };
    const float step = 2.0f / static_cast<float>(kEnvN - 1);
    for (int32_t i = 0; i < kEnvN; i++) {
        const float e = -1.0f + step * static_cast<float>(i);
        base[i] = box(e, 0.07f, 0.022f, 0.9f, 0.025f) + box(e, 0.22f, 0.026f, 1.0f, 0.025f) +
                  box(e, 0.45f, 0.035f, 0.9f, 0.03f) + box(e, 0.70f, 0.03f, 0.7f, 0.03f) +
                  0.05f * std::clamp(e, 0.0f, 1.0f);
    }
    for (size_t l = 0; l < kEnvLevels; l++) {
        float* out = &env[l * kEnvN];
        const float sigma = kEnvSigmas[l];
        if (sigma <= 0.0f) {
            std::copy(base.begin(), base.end(), out);
            continue;
        }
        const int32_t reach = static_cast<int32_t>(4.0f * sigma / step) + 1;
        float norm = 0.0f;
        for (int32_t k = -reach; k <= reach; k++) {
            const float d = static_cast<float>(k) * step / sigma;
            norm += std::exp(-0.5f * d * d);
        }
        for (int32_t i = 0; i < kEnvN; i++) {
            float sum = 0.0f;
            for (int32_t k = -reach; k <= reach; k++) {
                const int32_t j = i + k;
                if (j < 0 || j >= kEnvN) {
                    continue;
                }
                const float d = static_cast<float>(k) * step / sigma;
                sum += base[j] * std::exp(-0.5f * d * d);
            }
            out[i] = sum / norm;
        }
    }
}

/// One gentle octave of surface wrinkle as slopes across and deep: band-limited noise with a
/// k^-2 spectrum, stretched across, normalised so the steepest slope is 1.
void build_wrinkle(std::vector<float>& out) {
    constexpr int32_t N = kWrinkleN;
    std::minstd_rand rng(7);
    std::vector<float> h(static_cast<size_t>(N) * N, 0.0f);
    std::vector<float> cos_x(N);
    std::vector<float> sin_x(N);
    for (int32_t fz = -N / 2; fz < N / 2; fz++) {
        for (int32_t fx = -N / 2; fx < N / 2; fx++) {
            const float kx = static_cast<float>(fx) / N * 6.0f;
            const float kz = static_cast<float>(fz) / N;
            const float k = std::sqrt(kx * kx + kz * kz);
            if (!(k > 0.03f && k < 0.3f)) {
                continue;
            }
            const float re = random_normal(rng, 0.0f, 1.0f);
            const float im = random_normal(rng, 0.0f, 1.0f);
            const float amp = std::sqrt(re * re + im * im) / (k * k);
            const float phase = std::atan2(im, re);
            const float cp = std::cos(phase);
            const float spp = std::sin(phase);
            for (int32_t z = 0; z < N; z++) {
                for (int32_t x = 0; x < N; x++) {
                    const int32_t idx = wrap_index(fx * x + fz * z, N);
                    const float ca =
                        kFastSinTable.v[(idx * (kFastSinN / N) + kFastSinN / 4) & (kFastSinN - 1)];
                    const float sa = kFastSinTable.v[idx * (kFastSinN / N)];
                    h[static_cast<size_t>(z) * N + x] += amp * (ca * cp - sa * spp);
                }
            }
        }
    }
    out.assign(static_cast<size_t>(N) * N * 2, 0.0f);
    float steepest = 1e-6f;
    for (int32_t z = 0; z < N; z++) {
        for (int32_t x = 0; x < N; x++) {
            const float dx = (h[z * N + (x + 1) % N] - h[z * N + (x + N - 1) % N]) * 0.5f;
            const float dz = (h[((z + 1) % N) * N + x] - h[((z + N - 1) % N) * N + x]) * 0.5f;
            out[(z * N + x) * 2] = dx;
            out[(z * N + x) * 2 + 1] = dz;
            steepest = std::max({steepest, std::fabs(dx), std::fabs(dz)});
        }
    }
    for (float& v : out) {
        v /= steepest;
    }
}

void build_tint(std::vector<Rgb>& tint) {
    tint.resize(256);
    for (int32_t i = 0; i < 256; i++) {
        const float g = std::pow(static_cast<float>(i) / 255.0f, 1.05f) * 255.0f;
        tint[i] = {static_cast<uint8_t>(std::min(255.0f, g * 0.94f + 0.5f)),
                   static_cast<uint8_t>(std::min(255.0f, g * 0.98f + 0.5f)),
                   static_cast<uint8_t>(std::min(255.0f, g * 1.03f + 0.5f))};
    }
}

} // namespace

void AquaSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
                   const ClockReading& reading) {
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    t_sim_ = 0.0f;
    drift_.reset();
    emit_steps_.reset();
    const int32_t iw0 = (w_ + 1) / 2;
    const float focal0 = kFocalAt400 * static_cast<float>(iw0) / 400.0f;
    water_.init(rng, kAquaGridX, kAquaGridZ, (static_cast<float>(iw0) / 2.0f + 8.0f) / focal0);
    build_scene(rng);

    // Size every buffer for the finest rung any later step down reaches (the still rung renders
    // at the finest scale), so a rung change allocates nothing.
    level_ = 0;
    apply_view();
    level_ = std::min(level, kAquaLevelCount - 1);
    apply_view();

    update_clock_layout();
    drift_.set_range(clock_.bounds, w_, h_);
    build_stroke_digits(clock_.digit_unit, kDigitStroke, digits_);
    const DirtyRect& b = clock_.bounds;
    clock_save_.assign(
        static_cast<size_t>(b.x2 - b.x1 + 1) * (b.y2 - b.y1 + 1) * pixel_bytes(frame.format), 0);
    clock_saved_box_ = {};

    band_top_ = 0;
    painted_ = false;
    water_.evaluate(0.0f, rng);
    FrameTarget out = frame;
    paint_all(out, reading);
    if (kAquaLevels[level_].still) {
        release_scene();
    }
    shown_ = reading;
}

void AquaSim::build_scene(std::minstd_rand& rng) {
    build_env(env_);
    build_wrinkle(wrinkle_);
    build_tint(tint_);
    splashes_.init(kAquaMaxDroplets);
    landed_.clear();
    landed_.reserve(kAquaMaxDroplets);
    glints_.clear();
    glints_.reserve(kGlintCap);
    spray_.clear();
    spray_.reserve(kAquaMaxDroplets + kGlintCap);
    last_spray_.clear();
    last_spray_.reserve(kAquaMaxDroplets + kGlintCap);
    bubbles_.resize(kBubbleCount);
    for (Bubble& b : bubbles_) {
        b.x = random_between(rng, 0.0f, static_cast<float>(w_));
        b.y = random_between(rng, 0.0f, static_cast<float>(h_));
        b.speed = random_between(rng, 0.15f, 0.6f);
        b.phase = random_between(rng, 0.0f, kSaverTwoPi);
    }
    edge_rows_.assign(static_cast<size_t>(w_), static_cast<float>(h_));
    rim_.assign(static_cast<size_t>(w_), kAquaRimFloor);
    scene_released_ = false;
}

void AquaSim::release_scene() {
    water_.clear();
    splashes_.clear();
    renderer_.clear();
    release_buffers(env_, wrinkle_, tint_, frame_, rim_, landed_, glints_, bubbles_, last_spray_,
                    spray_, grid_gx_, grid_gz_, grid_thin_, grid_mean_, col_i_, col_f_, el_,
                    prev_el_, front_shade_, scratch_, edge_rows_);
    scene_released_ = true;
}

void AquaSim::apply_view() {
    const AquaLevel& lv = kAquaLevels[level_];
    scale_ = std::max<int32_t>(1, lv.scale);
    iw_ = scaled_extent(w_, scale_);
    ih_ = scaled_extent(h_, scale_);
    focal_ = kFocalAt400 * static_cast<float>(iw_) / 400.0f;
    horizon_ = static_cast<float>(ih_) / 2.0f + kPitchAt240 * static_cast<float>(ih_) / 240.0f;

    HeightfieldView v;
    v.w = iw_;
    v.h = ih_;
    v.horizon_row = horizon_;
    v.focal_x = focal_;
    v.focal_y = focal_;
    v.cam_h = kCamH;
    v.z_near = kAquaZNear;
    v.z_far = kAquaZNear + kAquaDepth;
    v.dz_grow = kDzGrow;
    v.slices = lv.slices;
    v.sink_depth = 0.0f;
    v.edge = HeightfieldEdge::FEATHERED_SILHOUETTE;
    v.span_gradient = true;
    v.first_slice_fills = false;
    v.fog = {kAquaZNear, kAquaZNear + kAquaDepth, 0.6f, true};
    renderer_.set_view(v);

    const int32_t div = std::max<int32_t>(1, lv.grid_div);
    if (water_.grid_x() != kAquaGridX / div || water_.grid_z() != kAquaGridZ / div) {
        water_.set_grid(kAquaGridX / div, kAquaGridZ / div);
        // The new grid starts flat: evaluate it at the current time so a frozen rung shows
        // water, not a blank surface.
        std::minstd_rand none(1);
        if (t_sim_ > 0.0f) {
            water_.evaluate(t_sim_, none);
        }
    }
    const size_t gcells = static_cast<size_t>(water_.grid_x()) * water_.grid_z();
    grid_gx_.resize(gcells);
    grid_gz_.resize(gcells);
    grid_thin_.resize(gcells);
    grid_mean_.resize(static_cast<size_t>(water_.grid_x()) * 2);
    frame_.resize(static_cast<size_t>(iw_) * ih_);
    col_i_.resize(static_cast<size_t>(iw_));
    col_f_.resize(static_cast<size_t>(iw_));
    el_.resize(static_cast<size_t>(iw_));
    prev_el_.resize(static_cast<size_t>(iw_));
    front_shade_.resize(static_cast<size_t>(iw_));
    scratch_.resize(static_cast<size_t>(iw_) * 4);
    const int32_t gx = water_.grid_x();
    const float u_max = water_.u_max();
    for (int32_t c = 0; c < iw_; c++) {
        const float u = (static_cast<float>(c) - static_cast<float>(iw_) / 2.0f + 0.5f) / focal_;
        const float fi = std::clamp((u + u_max) / (2.0f * u_max) * static_cast<float>(gx - 1), 0.0f,
                                    static_cast<float>(gx - 1));
        col_i_[c] = std::min(static_cast<int32_t>(fi), gx - 2);
        col_f_[c] = fi - static_cast<float>(col_i_[c]);
    }
}

void AquaSim::update_clock_layout() {
    clock_ = layout_aqua_clock(w_, h_, drift_.dx, drift_.dy);
}

void AquaSim::derive_grids() {
    const int32_t gx = water_.grid_x();
    const int32_t gz = water_.grid_z();
    const float du = 2.0f * water_.u_max() / static_cast<float>(gx - 1);
    const float dzz = kAquaDepth / static_cast<float>(gz - 1);
    const float* h = water_.heights().data();
    for (int32_t k = 0; k < gz; k++) {
        const float z = water_.cell_z(k);
        const int32_t kl = std::max(0, k - 1);
        const int32_t kr = std::min(gz - 1, k + 1);
        const float zspan = static_cast<float>(kr - kl) * dzz;
        // Each cell's mean over the nine cells round it along the row, the row's ends held.
        float* mean = grid_mean_.data();
        std::copy(h + static_cast<size_t>(k) * gx, h + static_cast<size_t>(k + 1) * gx, mean);
        box_blur(mean, gx, 1, 4, false, false, mean + gx, 1);
        for (int32_t i = 0; i < gx; i++) {
            const int32_t il = std::max(0, i - 1);
            const int32_t ir = std::min(gx - 1, i + 1);
            const size_t idx = static_cast<size_t>(k) * gx + i;
            const float hu = (h[k * gx + ir] - h[k * gx + il]) / (static_cast<float>(ir - il) * du);
            const float hz = (h[kr * gx + i] - h[kl * gx + i]) / zspan;
            const float slope_x = hu / z;
            grid_gx_[idx] = slope_x;
            grid_gz_[idx] = hz - water_.cell_u(i) * slope_x;
            grid_thin_[idx] = std::clamp((h[idx] - mean[i]) / 0.12f, 0.0f, 1.0f);
        }
    }
}

DirtyRect AquaSim::paint_water(FrameTarget& frame) {
    derive_grids();
    Scene scene(*this);
    renderer_.render(scene);

    // Rows from the highest silhouette (less the feathering) down are redrawn, along with
    // whatever the last frame's water covered and the clock, which sits in the water.
    float highest = static_cast<float>(ih_);
    for (int32_t x = 0; x < iw_; x++) {
        highest = std::min(highest, renderer_.top_at(x));
    }
    const int32_t water_top = std::max(0, static_cast<int32_t>(std::floor(highest)) - 2);
    int32_t from = std::min(water_top, band_top_ / scale_);
    from = std::min(from, std::max(0, clock_.bounds.y1 / scale_ - 1));
    from = std::max(0, from);

    // Below the near edge: near-black water lit faintly by the edge above it.
    float* smear = scratch_.data();
    float* rim = scratch_.data() + iw_;
    float* tmp = scratch_.data() + 2 * iw_;
    std::copy(front_shade_.begin(), front_shade_.end(), smear);
    std::copy(front_shade_.begin(), front_shade_.end(), rim);
    box_blur(smear, iw_, 1, 7, false, false, tmp, 1);
    box_blur(rim, iw_, 1, 2, false, false, tmp, 1);
    for (int32_t x = 0; x < iw_; x++) {
        const float edge = renderer_.front_top_at(x);
        const float base = smear[x] * 0.25f;
        for (int32_t y = from; y < ih_; y++) {
            const float d = static_cast<float>(y) - edge;
            Rgb c{};
            if (d >= 0.0f) {
                const float v = base * std::exp(-d / 18.0f) + 0.008f;
                c = tint_[unit_to_u8(v)];
            }
            frame_[static_cast<size_t>(y) * iw_ + x] = c;
        }
    }
    renderer_.resolve(frame_.data(), kTintNone, from, ih_, frame_);
    const DirtyRect band = upscale_frame(frame_, iw_, ih_, scale_, from, ih_, frame);
    band_top_ = water_top * scale_;

    // The near edge and its brightness at output resolution.
    for (int32_t ox = 0; ox < w_; ox++) {
        const float fx = (static_cast<float>(ox) + 0.5f) / static_cast<float>(scale_) - 0.5f;
        const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(fx)), 0, iw_ - 1);
        const int32_t x1 = std::min(x0 + 1, iw_ - 1);
        const float f = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
        const float e0 = renderer_.front_top_at(x0);
        const float e1 = renderer_.front_top_at(x1);
        edge_rows_[ox] = (e0 + (e1 - e0) * f) * static_cast<float>(scale_);
        const float r = rim[x0] + (rim[x1] - rim[x0]) * f;
        rim_[ox] = std::clamp(0.40f + 1.2f * r, kAquaRimFloor, 1.0f);
    }
    return band;
}

void AquaSim::draw_rim(FrameTarget& frame) const {
    PixelWriter writer(frame);
    const float s = std::max(1.0f, static_cast<float>(h_) / 480.0f);
    for (int32_t ox = 0; ox < w_; ox++) {
        const float e = edge_rows_[ox];
        const float b = rim_[ox];
        const auto y0 = static_cast<int32_t>(std::floor(e - 2.0f * s));
        const auto y1 = static_cast<int32_t>(std::ceil(e + 5.0f * s));
        for (int32_t y = std::max(0, y0); y <= std::min(h_ - 1, y1); y++) {
            const float p = rim_profile((static_cast<float>(y) + 0.5f - e) / s);
            if (p <= 0.0f) {
                continue;
            }
            writer.blend_max(ox, y, tint_[unit_to_u8(b * p)]);
        }
    }
}

void AquaSim::draw_bubbles(FrameTarget& frame) const {
    PixelWriter writer(frame);
    const size_t count = std::min<size_t>(kAquaLevels[level_].bubbles, bubbles_.size());
    const float s = static_cast<float>(h_) / 480.0f;
    const auto wf = static_cast<float>(w_);
    for (size_t i = 0; i < count; i++) {
        const Bubble& b = bubbles_[i];
        float bx = std::fmod(b.x + 3.0f * s * std::sin(b.phase), wf);
        bx += bx < 0.0f ? wf : 0.0f;
        const int32_t col = std::clamp(static_cast<int32_t>(bx), 0, w_ - 1);
        if (b.y <= edge_rows_[col] + 6.0f * s) {
            continue;
        }
        splat_max(writer, tint_, bx, b.y, 0.10f + 0.40f * b.speed);
    }
}

void AquaSim::draw_spray(FrameTarget& frame, std::vector<DirtyRect>& boxes) const {
    PixelWriter writer(frame);
    const auto sc = static_cast<float>(scale_);
    const float f = focal_ * sc;
    const float hz = horizon_ * sc;
    const float cx = static_cast<float>(w_) / 2.0f;
    const auto add_box = [&](float x1, float y1, float x2, float y2) {
        const DirtyRect box = DirtyRect{static_cast<int32_t>(std::floor(x1)) - 2,
                                        static_cast<int32_t>(std::floor(y1)) - 2,
                                        static_cast<int32_t>(std::ceil(x2)) + 2,
                                        static_cast<int32_t>(std::ceil(y2)) + 2}
                                  .clipped(w_, h_);
        if (!box.empty()) {
            boxes.push_back(box);
        }
    };
    for (const AquaDroplet& d : splashes_.droplets()) {
        const float px = d.x * f / d.z + cx;
        const float py = hz + (kCamH - d.y) * f / d.z;
        // A short streak back along the motion, as a slow shutter sees it.
        const float tx = -d.vx * f / d.z * 0.035f;
        const float ty = d.vy * f / d.z * 0.035f;
        const float bright = std::clamp(1.2f - (d.z - kAquaZNear) / kAquaDepth, 0.3f, 1.0f);
        for (int i = 0; i < 10; i++) {
            const float u = static_cast<float>(i) / 9.0f;
            const float taper = std::pow(1.0f - u, 1.5f);
            splat_max(writer, tint_, px + tx * u, py + ty * u, bright * taper);
        }
        add_box(std::min(px, px + tx), std::min(py, py + ty), std::max(px, px + tx),
                std::max(py, py + ty));
    }
    for (const Glint& g : glints_) {
        const float px = g.x * f / g.z + cx;
        const float py = hz + (kCamH - g.y) * f / g.z;
        const float v = 0.8f * (1.0f - static_cast<float>(g.age_ms) / kGlintMs);
        splat_max(writer, tint_, px, py, v);
        splat_max(writer, tint_, px - 1.0f, py, v * 0.5f);
        splat_max(writer, tint_, px + 1.0f, py, v * 0.5f);
        add_box(px - 1.0f, py, px + 1.0f, py);
    }
}

void AquaSim::draw_clock(FrameTarget& frame, const ClockReading& reading) const {
    PixelWriter writer(frame);
    const AquaRings rings = aqua_rings(reading);
    const auto ring = [&](int32_t cx, int32_t cy, int32_t r, float fill) {
        const float half = static_cast<float>(clock_.ring_width) / 2.0f;
        const auto rf = static_cast<float>(r);
        const float end = std::clamp(fill, 0.0f, 1.0f) * kSaverTwoPi;
        const int32_t reach = r + clock_.ring_width + 1;
        for (int32_t y = cy - reach; y <= cy + reach; y++) {
            for (int32_t x = cx - reach; x <= cx + reach; x++) {
                if (!writer.contains(x, y)) {
                    continue;
                }
                const float dx = static_cast<float>(x - cx) + 0.5f;
                const float dy = static_cast<float>(y - cy) + 0.5f;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float cov = std::clamp(half + 0.5f - std::fabs(d - rf), 0.0f, 1.0f);
                if (cov <= 0.0f) {
                    continue;
                }
                // Clockwise from twelve o'clock, with soft ends.
                float a = std::atan2(dx, -dy);
                a += a < 0.0f ? kSaverTwoPi : 0.0f;
                float filled = 0.0f;
                if (end > 0.0f) {
                    filled = edge_coverage((end - a) * rf) * edge_coverage(a * rf);
                    if (end >= kSaverTwoPi - 1e-4f) {
                        filled = 1.0f;
                    }
                }
                const Rgb color = mix_rgb(kRingTrack, kRingFill, filled);
                writer.blend_over(x, y, color, cov);
            }
        }
    };
    ring(clock_.hour_cx, clock_.hour_cy, clock_.ring_r, rings.hour);
    ring(clock_.minute_cx, clock_.minute_cy, clock_.ring_r, rings.minute);
    ring(clock_.top_cx, clock_.top_cy, clock_.top_r, rings.top);

    const auto number = [&](int32_t cx, int32_t cy, int32_t value) {
        const int32_t gap =
            std::max(1, static_cast<int32_t>(std::lround(0.15f * clock_.digit_unit)));
        const int32_t total = 2 * digits_.w + gap;
        const int32_t x0 = cx - total / 2;
        const int32_t y0 = cy - digits_.h / 2;
        const std::array<int, 2> digit = two_digits(value);
        for (int n = 0; n < 2; n++) {
            blit_stroke_digit(writer, digits_, digit[n], x0 + n * (digits_.w + gap), y0,
                              kDigitColor, kDigitOpacity);
        }
    };
    number(clock_.hour_cx, clock_.hour_cy, reading.time.hour);
    number(clock_.minute_cx, clock_.minute_cy, reading.time.minute);
}

void AquaSim::save_clock_region(const FrameTarget& frame) {
    const DirtyRect& b = clock_.bounds;
    const DirtyRect clip = b.clipped(w_, h_);
    clock_saved_box_ = clip;
    if (clip.empty()) {
        return;
    }
    const uint32_t bpp = pixel_bytes(frame.format);
    const size_t row_bytes = static_cast<size_t>(clip.x2 - clip.x1 + 1) * bpp;
    if (clock_save_.size() < row_bytes * (clip.y2 - clip.y1 + 1)) {
        clock_saved_box_ = {};
        return;
    }
    for (int32_t y = clip.y1; y <= clip.y2; y++) {
        std::memcpy(&clock_save_[static_cast<size_t>(y - clip.y1) * row_bytes],
                    frame.data + static_cast<size_t>(y) * frame.stride + clip.x1 * bpp, row_bytes);
    }
}

void AquaSim::restore(FrameTarget& frame, const DirtyRect& box,
                      std::vector<DirtyRect>& dirty) const {
    if (!kAquaLevels[level_].still || clock_saved_box_.empty() ||
        !box.intersects(clock_saved_box_)) {
        return;
    }
    const DirtyRect& s = clock_saved_box_;
    const DirtyRect clip{std::max(box.x1, s.x1), std::max(box.y1, s.y1), std::min(box.x2, s.x2),
                         std::min(box.y2, s.y2)};
    const uint32_t bpp = pixel_bytes(frame.format);
    const size_t saved_row = static_cast<size_t>(s.x2 - s.x1 + 1) * bpp;
    const size_t n = static_cast<size_t>(clip.x2 - clip.x1 + 1) * bpp;
    for (int32_t y = clip.y1; y <= clip.y2; y++) {
        std::memcpy(
            frame.data + static_cast<size_t>(y) * frame.stride + clip.x1 * bpp,
            &clock_save_[static_cast<size_t>(y - s.y1) * saved_row + (clip.x1 - s.x1) * bpp], n);
    }
    dirty.push_back(clip);
}

void AquaSim::paint_all(FrameTarget& frame, const ClockReading& reading) {
    PixelWriter(frame).fill({});
    band_top_ = 0;
    paint_water(frame);
    draw_rim(frame);
    draw_bubbles(frame);
    spray_.clear();
    draw_spray(frame, spray_);
    last_spray_.swap(spray_);
    spray_.clear();
    save_clock_region(frame);
    draw_clock(frame, reading);
    painted_ = true;
}

void AquaSim::request_level(size_t level) {
    const size_t next = std::min(level, kAquaLevelCount - 1);
    if (next == level_) {
        return;
    }
    level_ = next;
    painted_ = false;
    if (!scene_released_) {
        // A released scene is built again, at this rung, by the next step.
        apply_view();
    }
}

float AquaSim::rim_brightness(int32_t x) const {
    if (rim_.empty()) {
        return kAquaRimFloor;
    }
    return rim_[static_cast<size_t>(std::clamp(x, 0, w_ - 1))];
}

float AquaSim::rim_row(int32_t x) const {
    if (edge_rows_.empty()) {
        return static_cast<float>(h_);
    }
    return edge_rows_[static_cast<size_t>(std::clamp(x, 0, w_ - 1))];
}

void AquaSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                   const ClockReading& reading, std::vector<DirtyRect>& dirty) {
    dirty.clear();
    const AquaLevel& lv = kAquaLevels[level_];
    if (scene_released_ && (!lv.still || !painted_)) {
        // A rung change after the still one: the water comes back before it is drawn.
        build_scene(rng);
        apply_view();
        water_.evaluate(t_sim_, rng);
    }

    const bool moved = drift_.step(dt_ms);
    if (moved) {
        update_clock_layout();
    }
    const DirtyRect whole = DirtyRect::whole(w_, h_);

    if (lv.still) {
        // The water stands still: draw it once, then only the clock when what it shows changes.
        if (!painted_) {
            paint_all(frame, reading);
            dirty.push_back(whole);
            release_scene();
        } else if (moved) {
            // The clock steps aside: put the water back under where it was, keep what is under
            // where it goes, and draw it there. The water itself is not rendered again.
            const DirtyRect old = clock_saved_box_;
            dirty.clear();
            restore(frame, old, dirty);
            save_clock_region(frame);
            draw_clock(frame, reading);
            dirty.push_back(clock_saved_box_);
        } else if (!clock_face_same(reading, shown_, true)) {
            restore(frame, clock_.bounds, dirty);
            draw_clock(frame, reading);
            dirty.clear();
            dirty.push_back(clock_saved_box_);
        }
        shown_ = reading;
        return;
    }

    const float dt_sim = static_cast<float>(dt_ms) / 1000.0f * kAquaTimeScale;
    t_sim_ += dt_sim;
    water_.evaluate(t_sim_, rng);
    const uint32_t emits = emit_steps_.steps_due(dt_ms, kEmitStepMs, kEmitStepsMax);
    if (lv.splashes) {
        for (uint32_t i = 0; i < emits; i++) {
            splashes_.emit_crests(water_, rng);
        }
    }
    // Droplets in flight keep flying at every rung until they land.
    landed_.clear();
    splashes_.move(dt_sim, water_, &landed_);
    for (Glint& g : glints_) {
        g.age_ms = static_cast<uint16_t>(std::min<uint32_t>(kGlintMs, g.age_ms + dt_ms));
    }
    glints_.erase(std::remove_if(glints_.begin(), glints_.end(),
                                 [](const Glint& g) { return g.age_ms >= kGlintMs; }),
                  glints_.end());
    for (const AquaLanding& l : landed_) {
        if (glints_.size() < kGlintCap) {
            glints_.push_back({l.x, l.surface, l.z, 0});
        }
    }
    const float dt_s = static_cast<float>(dt_ms) / 1000.0f;
    const float rise = 24.0f * static_cast<float>(h_) / 480.0f;
    for (Bubble& b : bubbles_) {
        b.y -= b.speed * rise * dt_s;
        b.phase += 2.0f * dt_s;
        if (b.phase > kSaverTwoPi) {
            b.phase -= kSaverTwoPi;
        }
        if (b.y < 0.0f) {
            b.y = static_cast<float>(h_) + random_between(rng, 0.0f, 40.0f);
        }
    }

    const bool full = !painted_;
    if (full) {
        PixelWriter(frame).fill({});
        band_top_ = 0;
    }
    // What the last frame flung above the water goes back to black.
    const int32_t band_before = band_top_;
    PixelWriter writer(frame);
    for (const DirtyRect& box : last_spray_) {
        for (int32_t y = box.y1; y <= std::min(box.y2, band_before - 1); y++) {
            for (int32_t x = box.x1; x <= box.x2; x++) {
                writer.put(x, y, {});
            }
        }
    }
    const DirtyRect band = paint_water(frame);
    draw_rim(frame);
    draw_bubbles(frame);
    spray_.clear();
    draw_spray(frame, spray_);
    draw_clock(frame, reading);
    painted_ = true;

    if (full) {
        dirty.push_back(whole);
    } else {
        dirty.push_back(band);
        for (const DirtyRect& b : last_spray_) {
            if (b.y1 < band.y1) {
                dirty.push_back(b);
            }
        }
        for (const DirtyRect& b : spray_) {
            if (b.y1 < band.y1) {
                dirty.push_back(b);
            }
        }
    }
    last_spray_.swap(spray_);
    spray_.clear();
    reduce_dirty(dirty, kSaverMaxDirtyAreas, w_, h_);
    shown_ = reading;
}

size_t AquaSim::heap_bytes() const {
    return water_.heap_bytes() + splashes_.heap_bytes() + renderer_.heap_bytes() +
           digits_.heap_bytes() +
           buffer_bytes(env_, wrinkle_, tint_, frame_, rim_, landed_, glints_, bubbles_,
                        last_spray_, spray_, grid_gx_, grid_gz_, grid_thin_, grid_mean_, col_i_,
                        col_f_, el_, prev_el_, front_shade_, scratch_, edge_rows_, clock_save_);
}

void AquaSim::clear() {
    release_scene();
    digits_.clear();
    release_buffer(clock_save_);
    clock_saved_box_ = {};
    painted_ = false;
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
