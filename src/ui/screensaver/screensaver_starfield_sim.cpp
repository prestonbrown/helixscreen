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

/// Paints the size x size square at (sx, sy), clipped to the frame, and returns the box it
/// wrote (empty when the square is entirely outside the frame).
DirtyRect fill_square(PixelWriter& writer, const FrameTarget& target, int sx, int sy, int size,
                      Rgb color) {
    const DirtyRect box = DirtyRect{sx, sy, sx + size - 1, sy + size - 1}.clipped(
        static_cast<int32_t>(target.w), static_cast<int32_t>(target.h));
    writer.fill_rect(box, color);
    return box;
}

} // namespace

void StarfieldSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level) {
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
                        std::vector<DirtyRect>& dirty) {
    dirty.clear();
    PixelWriter writer(target);
    const int w = static_cast<int>(target.w);
    const int h = static_cast<int>(target.h);
    star_dirty_.resize(stars_.size());

    // Erase previous star positions (an incremental clear, which avoids a full-frame fill).
    // Covers every star of the population, so a star parked by set_active_count() leaves
    // the canvas on the step after it stops flying.
    for (size_t i = 0; i < stars_.size(); i++) {
        star_dirty_[i] = DirtyRect{};
        Star& star = stars_[i];
        if (star.prev_size == 0) {
            continue;
        }
        star_dirty_[i].add(
            fill_square(writer, target, star.prev_sx, star.prev_sy, star.prev_size, kBlack));
        star.prev_size = 0;
    }

    const float frames = static_cast<float>(dt_ms) / kSpeedFrameMs;
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

        star_dirty_[i].add(fill_square(writer, target, isx, isy, size, Rgb{r, g, b}));

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
