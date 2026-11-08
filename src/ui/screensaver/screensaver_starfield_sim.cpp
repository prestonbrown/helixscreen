// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_starfield_sim.h"

#include "screensaver_buffer.h"
#include "screensaver_fast_math.h"
#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <cmath>

namespace helix::ui {

namespace {

using screensaver::random_below;
using screensaver::smoothstep01;
using screensaver::unit_random;

// A star's speed is its depth change per this many ms of frame time
constexpr float kSpeedFrameMs = 33.0f;
constexpr float kColorThreshold = 0.35f; // stars closer than this show color
/// Depth a star is recycled at, and the span of depths a live star occupies. A drawn star
/// always sits in [kZNear, 1.0], so size and brightness scale over kZSpan.
constexpr float kZNear = 0.01f;
constexpr float kZSpan = 1.0f - kZNear;

constexpr Rgb kBlack = {0, 0, 0};

// Star color tints: blue dwarfs, red giants, yellow suns, blue-white hot stars
constexpr uint8_t kStarTints[][3] = {
    {255, 255, 255}, // white (most common)
    {255, 255, 255}, // white
    {255, 255, 255}, // white
    {255, 200, 150}, // warm yellow
    {255, 160, 120}, // orange
    {255, 120, 100}, // red giant
    {150, 180, 255}, // blue dwarf
    {200, 220, 255}, // blue-white
};
constexpr int kNumTints = sizeof(kStarTints) / sizeof(kStarTints[0]);

void assign_tint(std::minstd_rand& rng, uint8_t& r, uint8_t& g, uint8_t& b) {
    int idx = random_below(rng, kNumTints);
    r = kStarTints[idx][0];
    g = kStarTints[idx][1];
    b = kStarTints[idx][2];
}

// The planet: a banded world lit from the upper left, its lit limb glowing with atmosphere.
constexpr Rgb kPlanetColor = {72, 116, 176};
constexpr Rgb kPlanetLimbColor = {150, 205, 255};
constexpr float kPlanetAmbient = 0.12f;
constexpr float kLightX = -0.45f; // toward the light, a unit vector
constexpr float kLightY = -0.55f;
constexpr float kLightZ = 0.70f;
/// Time a running print's planet takes to cover most of the way to a new radius.
constexpr float kPlanetEaseMs = 700.0f;
/// Share of the flight's speed a speed change covers per ms.
constexpr float kSpeedEasePerMs = 1.0f / 600.0f;
/// How far a departing planet drifts by the end, as shares of the frame.
constexpr float kDepartDriftX = 0.6f;
constexpr float kDepartDriftY = -0.4f;

/// Paints the size x size square at (sx, sy), clipped to the frame, and returns the box it
/// wrote (empty when the square is entirely outside the frame).
DirtyRect fill_square(PixelWriter& writer, const FrameTarget& target, int sx, int sy, int size,
                      Rgb color) {
    const DirtyRect box = DirtyRect{sx, sy, sx + size - 1, sy + size - 1}.clipped(
        static_cast<int32_t>(target.w), static_cast<int32_t>(target.h));
    writer.fill_rect(box, color);
    return box;
}

/// fill_square() leaving every pixel of `planet` alone; the box covers the pixels it wrote.
DirtyRect fill_square_beside(PixelWriter& writer, const FrameTarget& target, int sx, int sy,
                             int size, Rgb color, const StarfieldPlanet& planet) {
    if (planet.r == 0) {
        return fill_square(writer, target, sx, sy, size, color);
    }
    const DirtyRect box = DirtyRect{sx, sy, sx + size - 1, sy + size - 1}.clipped(
        static_cast<int32_t>(target.w), static_cast<int32_t>(target.h));
    DirtyRect wrote;
    for (int32_t y = box.y1; y <= box.y2; y++) {
        for (int32_t x = box.x1; x <= box.x2; x++) {
            if (!planet.covers(x, y)) {
                writer.put(x, y, color);
                wrote.add(x, y, x, y);
            }
        }
    }
    return wrote;
}

/// The bounding box of `planet`'s disc inside the frame; empty when there is none.
DirtyRect planet_box(const StarfieldPlanet& planet, const FrameTarget& target) {
    if (planet.r == 0) {
        return {};
    }
    return DirtyRect{planet.cx - planet.r, planet.cy - planet.r, planet.cx + planet.r,
                     planet.cy + planet.r}
        .clipped(static_cast<int32_t>(target.w), static_cast<int32_t>(target.h));
}

/// The colour of `planet` at (x, y), a pixel on its disc.
Rgb planet_shade(const StarfieldPlanet& planet, int32_t x, int32_t y) {
    const float inv_r = 1.0f / static_cast<float>(planet.r);
    const float nx = static_cast<float>(x - planet.cx) * inv_r;
    const float ny = static_cast<float>(y - planet.cy) * inv_r;
    const float d2 = std::min(1.0f, nx * nx + ny * ny);
    const float nz = std::sqrt(1.0f - d2);
    const float diffuse = std::max(0.0f, nx * kLightX + ny * kLightY + nz * kLightZ);
    const float bands = 0.88f + 0.12f * std::sin(ny * 9.0f + nx * 1.5f);
    const float body = (kPlanetAmbient + (1.0f - kPlanetAmbient) * diffuse) * bands;
    // The limb facing the light glows: the outer fifth of the disc, brightest at the edge.
    const float rim = d2 > 0.8f ? (d2 - 0.8f) / 0.2f : 0.0f;
    const float facing =
        d2 > 0.0f ? std::max(0.0f, (nx * kLightX + ny * kLightY) / std::sqrt(d2)) : 0.0f;
    const float limb = rim * facing * 0.9f;
    const auto channel = [body, limb](uint8_t base, uint8_t glow) {
        return static_cast<uint8_t>(std::clamp(
            static_cast<float>(base) * body + static_cast<float>(glow) * limb, 1.0f, 255.0f));
    };
    return {channel(kPlanetColor.r, kPlanetLimbColor.r),
            channel(kPlanetColor.g, kPlanetLimbColor.g),
            channel(kPlanetColor.b, kPlanetLimbColor.b)};
}

} // namespace

void StarfieldSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
                        const ClockReading& reading) {
    cx_ = static_cast<float>(frame.w) / 2.0f;
    cy_ = static_cast<float>(frame.h) / 2.0f;
    focal_ = static_cast<float>(frame.w) / 3.0f;

    stars_.resize(kNumStars);
    star_dirty_.resize(kNumStars);
    for (auto& star : stars_) {
        float angle = unit_random(rng) * 2.0f * kSaverPi;
        float radius = 0.1f + unit_random(rng) * 0.9f;
        star.x = radius * std::cos(angle);
        star.y = radius * std::sin(angle);
        star.z = kZNear + unit_random(rng) * kZSpan;
        star.speed = 0.008f + unit_random(rng) * 0.017f;
        assign_tint(rng, star.tint_r, star.tint_g, star.tint_b);
        star.prev_sx = 0;
        star.prev_sy = 0;
        star.prev_size = 0;
    }
    set_active_count(starfield_star_count(level));

    planet_ = {};
    planet_r_ = 0;
    planet_dx_ = 0;
    planet_dy_ = 0;
    mode_ = ClockMode::CLOCK;
    follow_print(reading, 0, true);
}

void StarfieldSim::follow_print(const ClockReading& reading, uint32_t dt_ms, bool snap) {
    const float w = cx_ * 2.0f;
    const float h = cy_ * 2.0f;
    if (reading.mode != mode_) {
        if (clock_mode_is_end(reading.mode)) {
            end_r_ = planet_r_;
            end_dx_ = planet_dx_;
            end_dy_ = planet_dy_;
            end_ms_ = 0;
        }
        mode_ = reading.mode;
    }

    const float speed = reading.mode == ClockMode::PAUSED ? kStarfieldPausedSpeed : 1.0f;
    if (snap) {
        speed_scale_ = speed;
    } else {
        // Lands on the new speed exactly, so the flight's own speed comes back unscaled.
        const float ease = kSpeedEasePerMs * static_cast<float>(dt_ms);
        speed_scale_ = std::fabs(speed - speed_scale_) <= ease
                           ? speed
                           : speed_scale_ + (speed > speed_scale_ ? ease : -ease);
    }

    const float ease =
        snap ? 1.0f : static_cast<float>(dt_ms) / (static_cast<float>(dt_ms) + kPlanetEaseMs);
    switch (reading.mode) {
    case ClockMode::CLOCK:
    case ClockMode::PROGRESS:
    case ClockMode::COUNTDOWN:
    case ClockMode::PAUSED: {
        float target = 0.0f;
        if (reading.mode != ClockMode::CLOCK) {
            const float p = static_cast<float>(std::clamp(reading.progress_pct, 0, 100)) / 100.0f;
            target = (kStarfieldPlanetStartShare +
                      (kStarfieldPlanetEndShare - kStarfieldPlanetStartShare) * p) *
                     std::min(w, h);
        }
        planet_r_ += (target - planet_r_) * ease;
        planet_dx_ -= planet_dx_ * ease;
        planet_dy_ -= planet_dy_ * ease;
        break;
    }
    case ClockMode::DONE: {
        end_ms_ = snap ? kStarfieldArriveMs : std::min(kStarfieldArriveMs, end_ms_ + dt_ms);
        const float t = smoothstep01(static_cast<float>(end_ms_) / kStarfieldArriveMs);
        const float fill = std::ceil(std::hypot(w, h) / 2.0f) + 1.0f;
        planet_r_ = end_r_ + (fill - end_r_) * t;
        planet_dx_ = end_dx_ * (1.0f - t);
        planet_dy_ = end_dy_ * (1.0f - t);
        break;
    }
    case ClockMode::FAILED:
    case ClockMode::CANCELLED: {
        end_ms_ = snap ? kStarfieldDepartMs : std::min(kStarfieldDepartMs, end_ms_ + dt_ms);
        const float t = static_cast<float>(end_ms_) / kStarfieldDepartMs;
        planet_r_ = end_r_ * (1.0f - smoothstep01(t));
        planet_dx_ = end_dx_ + kDepartDriftX * w * t * t;
        planet_dy_ = end_dy_ + kDepartDriftY * h * t * t;
        break;
    }
    }
}

void StarfieldSim::draw_planet(PixelWriter& writer, const FrameTarget& target,
                               std::vector<DirtyRect>& dirty) {
    StarfieldPlanet want;
    const auto r = static_cast<int32_t>(std::lround(planet_r_));
    if (r >= 1) {
        want = {static_cast<int32_t>(std::lround(cx_ + planet_dx_)),
                static_cast<int32_t>(std::lround(cy_ + planet_dy_)), r};
    }
    if (want == planet_) {
        return;
    }
    const StarfieldPlanet old = planet_;
    planet_ = want;

    // What the old disc covered and the new one does not goes back to space. Stars never drew
    // there, so it holds only planet.
    const DirtyRect old_box = planet_box(old, target);
    for (int32_t y = old_box.y1; y <= old_box.y2; y++) {
        for (int32_t x = old_box.x1; x <= old_box.x2; x++) {
            if (old.covers(x, y) && !want.covers(x, y)) {
                writer.put(x, y, kBlack);
            }
        }
    }
    const DirtyRect new_box = planet_box(want, target);
    for (int32_t y = new_box.y1; y <= new_box.y2; y++) {
        for (int32_t x = new_box.x1; x <= new_box.x2; x++) {
            if (want.covers(x, y)) {
                writer.put_dithered(x, y, planet_shade(want, x, y));
            }
        }
    }
    DirtyRect box = old_box;
    box.add(new_box);
    if (!box.empty()) {
        dirty.push_back(box);
    }
}

size_t StarfieldSim::heap_bytes() const {
    return buffer_bytes(stars_, star_dirty_);
}

void StarfieldSim::clear() {
    release_buffers(stars_, star_dirty_);
    active_count_ = kNumStars;
}

void StarfieldSim::recycle(Star& star, std::minstd_rand& rng) {
    // Pick random angle + radius so stars fly uniformly in all directions
    float angle = unit_random(rng) * 2.0f * kSaverPi;
    float radius = 0.3f + unit_random(rng) * 0.7f;
    star.x = radius * std::cos(angle);
    star.y = radius * std::sin(angle);
    star.z = 1.0f;
    star.speed = 0.008f + unit_random(rng) * 0.017f;
    assign_tint(rng, star.tint_r, star.tint_g, star.tint_b);
}

void StarfieldSim::set_active_count(int count) {
    active_count_ = std::clamp(count, 1, kNumStars);
}

void StarfieldSim::step(uint32_t dt_ms, FrameTarget& target, std::minstd_rand& rng,
                        const ClockReading& reading, std::vector<DirtyRect>& dirty) {
    dirty.clear();
    PixelWriter writer(target);
    const int w = static_cast<int>(target.w);
    const int h = static_cast<int>(target.h);
    star_dirty_.resize(stars_.size());

    follow_print(reading, dt_ms, false);
    draw_planet(writer, target, dirty);

    // Erase previous star positions (an incremental clear, which avoids a full-frame fill).
    // Covers every star of the population, so a star parked by set_active_count() leaves
    // the canvas on the step after it stops flying.
    for (size_t i = 0; i < stars_.size(); i++) {
        star_dirty_[i] = DirtyRect{};
        Star& star = stars_[i];
        if (star.prev_size == 0) {
            continue;
        }
        star_dirty_[i].add(fill_square_beside(writer, target, star.prev_sx, star.prev_sy,
                                              star.prev_size, kBlack, planet_));
        star.prev_size = 0;
    }

    const float frames = static_cast<float>(dt_ms) / kSpeedFrameMs * speed_scale_;
    // active_count_ is a ladder setting, so it can outlive the population it was set for.
    const int flying = std::min(active_count_, static_cast<int>(stars_.size()));
    for (int i = 0; i < flying; i++) {
        Star& star = stars_[i];

        // Move star closer by its speed, scaled to the time since the previous frame
        star.z -= star.speed * frames;

        if (star.z <= kZNear) {
            recycle(star, rng);
            continue;
        }

        // Project to frame coordinates
        float sx = cx_ + (star.x / star.z) * focal_;
        float sy = cy_ + (star.y / star.z) * focal_;

        if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
            recycle(star, rng);
            continue;
        }

        int isx = static_cast<int>(sx);
        int isy = static_cast<int>(sy);

        // Size: larger when closer. A drawn star has z above the recycle bound, so the
        // range is scaled to that bound and rounds to span the full 1..3 px.
        int size = 1 + static_cast<int>(std::lround(2.0f * (1.0f - star.z) / kZSpan));

        // Brightness: brighter when closer, with minimum floor
        float bright_f = 80.0f + 175.0f * (1.0f - star.z);

        // Close stars show their color tint; distant stars stay white
        uint8_t r, g, b;
        if (star.z < kColorThreshold) {
            float tint_mix = (kColorThreshold - star.z) / kColorThreshold;
            r = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_r / 255.0f));
            g = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_g / 255.0f));
            b = static_cast<uint8_t>(bright_f *
                                     (1.0f - tint_mix + tint_mix * star.tint_b / 255.0f));
        } else {
            r = g = b = static_cast<uint8_t>(bright_f);
        }

        star_dirty_[i].add(
            fill_square_beside(writer, target, isx, isy, size, Rgb{r, g, b}, planet_));

        // Remember position for next step's erase pass
        star.prev_sx = static_cast<int16_t>(isx);
        star.prev_sy = static_cast<int16_t>(isy);
        star.prev_size = static_cast<uint8_t>(size);
    }

    for (const DirtyRect& r : star_dirty_) {
        if (!r.empty()) {
            dirty.push_back(r);
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
