// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_fireworks_sim.h"

#include "screensaver_buffer.h"
#include "screensaver_fast_math.h"
#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace helix::ui {

namespace {

using screensaver::random_below;
using screensaver::random_int;
using screensaver::unit_random;

constexpr Rgb kSkyTop = {2, 3, 10};       // near black overhead
constexpr Rgb kSkyHorizon = {12, 20, 52}; // deep navy where the sky meets the hills
constexpr Rgb kHills = {1, 2, 4};
constexpr Rgb WHITE = {255, 255, 255};
constexpr Rgb kEmber = {255, 110, 30};
constexpr Rgb kWillowGold = {255, 190, 90};
constexpr Rgb kPalmGold = {255, 165, 55};
constexpr Rgb kStrobeWhite = {235, 240, 255};
constexpr Rgb kRocket = {255, 214, 150};
constexpr Rgb kShellColors[] = {
    {255, 60, 60},  {80, 255, 90},  {90, 140, 255}, {255, 230, 80},
    {255, 90, 230}, {90, 240, 255}, {255, 160, 60},
};
constexpr int kShellColorCount = static_cast<int>(std::size(kShellColors));

// Physics runs in steps no longer than this, so a late frame moves everything the same way.
constexpr uint32_t kPhysicsStepMs = 20;

// Distances and accelerations are for a 480 px tall frame and scale with the frame's height.
constexpr float kReferenceHeight = 480.0f;
constexpr float kRocketGravity = 110.0f; // px/s^2
constexpr float kBurstSpeed = 30.0f;     // a shell bursts once it climbs slower than this, px/s
constexpr float kRocketTailPx = 9.0f;
constexpr float kPalmTailPx = 22.0f;        // a palm shell climbs on a long burning tail
constexpr float kPalmCometRisePx = 135.0f;  // a palm's comets climb this far above its burst
constexpr int32_t kTrailSpacingTenths = 16; // trail points 1.6 px apart

// Where each colour phase of a spark's life ends, as a share of the life.
constexpr float kFlashEnd = 0.08f;
constexpr float kColorEnd = 0.55f;
constexpr float kEmberEnd = 0.80f;
constexpr float kFlickerFrom = 0.70f;
constexpr float kBigUntil = 0.25f;

// A crossette star flies this long, then splits into four fragments thrown across its path.
constexpr int kCrossetteSplitMinMs = 550;
constexpr int kCrossetteSplitMaxMs = 800;
constexpr int kCrossetteFragmentMinMs = 700;
constexpr int kCrossetteFragmentMaxMs = 1050;
constexpr float kCrossetteFragmentSpeed = 65.0f; // px/s, added to what the star kept
constexpr int kCrossetteFragments = 4;

// A strobe star shows its shell colour, then hangs blinking white: lit for kStrobeOnMs of
// every kStrobePeriodMs, each star on its own phase.
constexpr float kStrobeFrom = 0.30f;
constexpr uint32_t kStrobePeriodMs = 190;
constexpr uint32_t kStrobeOnMs = 55;

constexpr int kStarMinLevel = 40;
constexpr int kStarMaxLevel = 110;

struct KindParams {
    float speed_min; // px/s
    float speed_max;
    int life_min_ms;
    int life_max_ms;
    float drag;    // share of velocity lost per second
    float gravity; // px/s^2
    bool full_trail;
    float big_until; // share of the life a spark is drawn 2x2
};

// Indexed by FireworksSim::BurstKind.
constexpr KindParams kKinds[] = {
    {70.0f, 110.0f, 1100, 1600, 1.1f, 40.0f, false, kBigUntil}, // peony: an even ball
    {80.0f, 120.0f, 1400, 1900, 0.9f, 36.0f, true, kBigUntil},  // chrysanthemum: sparks trail
    {55.0f, 85.0f, 2600, 3400, 1.8f, 60.0f, true, kBigUntil},   // willow: gold, heavy drag, droops
    {95.0f, 115.0f, 1200, 1500, 1.0f, 38.0f, false, kBigUntil}, // ring: a tilted flat circle
    {125.0f, 150.0f, 2000, 2600, 0.8f, 115.0f, true, 0.65f},    // palm: few heavy comets
    // crossette: the life is the star's flight before it splits
    {85.0f, 115.0f, kCrossetteSplitMinMs, kCrossetteSplitMaxMs, 0.9f, 30.0f, true, 1.0f},
    {60.0f, 90.0f, 2300, 3000, 1.7f, 22.0f, false, 0.15f}, // strobe: slow, hangs, blinks
};
static_assert(std::size(kKinds) == FireworksSim::kBurstKindCount, "one entry per burst kind");

// Palm comets and crossette stars are few and large; the rest of the level's sparks are unused.
constexpr int kPalmMinComets = 7;
constexpr int kPalmMaxComets = 12;
constexpr int kCrossetteMinStars = 6;
constexpr int kCrossetteMaxStars = 18;

int16_t to_pixel(float v) {
    return static_cast<int16_t>(std::clamp(round_px(v), -16000, 16000));
}

} // namespace

void FireworksSim::init(FrameTarget& frame, std::minstd_rand& rng, size_t level,
                        const FireworksPacing& pacing) {
    pacing_ = pacing;
    level_ = std::min(level, kFireworksLevelCount - 1);
    pending_level_ = level_;
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    scale_ = std::max(0.4f, static_cast<float>(h_) / kReferenceHeight);

    sky_rows_.resize(static_cast<size_t>(h_));
    for (int32_t y = 0; y < h_; y++) {
        const float t = h_ > 1 ? static_cast<float>(y) / static_cast<float>(h_ - 1) : 0.0f;
        sky_rows_[static_cast<size_t>(y)] = mix_rgb(kSkyTop, kSkyHorizon, t);
    }

    // Rolling hills: three waves with random phases give one height per column.
    const float phase_a = unit_random(rng) * 2.0f * kSaverPi;
    const float phase_b = unit_random(rng) * 2.0f * kSaverPi;
    const float phase_c = unit_random(rng) * 2.0f * kSaverPi;
    hill_top_.resize(static_cast<size_t>(w_));
    for (int32_t x = 0; x < w_; x++) {
        const float fx = static_cast<float>(x) / static_cast<float>(std::max(w_, 1));
        const float share = 0.14f + 0.05f * std::sin(fx * 2.0f * kSaverPi * 1.1f + phase_a) +
                            0.025f * std::sin(fx * 2.0f * kSaverPi * 3.0f + phase_b) +
                            0.01f * std::sin(fx * 2.0f * kSaverPi * 9.0f + phase_c);
        const int32_t top = h_ - static_cast<int32_t>(share * static_cast<float>(h_));
        hill_top_[static_cast<size_t>(x)] = static_cast<int16_t>(std::clamp<int32_t>(top, 0, h_));
    }

    // Faint fixed stars, at most one per column, in the upper 70% of the frame.
    star_y_.assign(static_cast<size_t>(w_), -1);
    star_level_.assign(static_cast<size_t>(w_), 0);
    for (int i = 0; i < kStarCount && w_ > 0 && h_ > 0; i++) {
        int32_t x = random_below(rng, w_);
        const int32_t y = random_below(rng, std::max(1, h_ * 7 / 10));
        const auto star_level = static_cast<uint8_t>(random_int(rng, kStarMinLevel, kStarMaxLevel));
        for (int32_t tries = 0; tries < w_ && star_y_[static_cast<size_t>(x)] >= 0; tries++) {
            x = (x + 1) % w_;
        }
        if (star_y_[static_cast<size_t>(x)] >= 0 || y >= hill_top_[static_cast<size_t>(x)]) {
            continue;
        }
        star_y_[static_cast<size_t>(x)] = static_cast<int16_t>(y);
        star_level_[static_cast<size_t>(x)] = star_level;
    }

    const FireworksLevel& params = kFireworksLevels[level_];
    sparks_.assign(static_cast<size_t>(params.sparks_per_burst) * params.bursts_at_once, Spark{});
    next_spark_ = 0;
    for (Burst& b : bursts_) {
        b = Burst{};
    }
    for (Rocket& r : rockets_) {
        r = Rocket{};
    }
    launch_in_ms_ = static_cast<int32_t>(pacing_.first_launch_ms);
    finale_in_ms_ = static_cast<int32_t>(std::max<uint32_t>(pacing_.finale_every_ms, 1));
    finale_left_ = 0;
    finale_gap_ms_ = 0;

    PixelWriter writer(frame);
    for (int32_t y = 0; y < h_; y++) {
        for (int32_t x = 0; x < w_; x++) {
            writer.put_dithered(x, y, sky_at(x, y));
        }
    }
}

Rgb FireworksSim::sky_at(int32_t x, int32_t y) const {
    const auto column = static_cast<size_t>(x);
    if (y >= hill_top_[column]) {
        return kHills;
    }
    if (star_y_[column] == y) {
        const uint8_t v = star_level_[column];
        return {v, v, static_cast<uint8_t>(std::min(255, v + 25))};
    }
    return sky_rows_[static_cast<size_t>(y)];
}

void FireworksSim::request_level(size_t level) {
    pending_level_ = std::min(level, kFireworksLevelCount - 1);
}

size_t FireworksSim::sparks_in_use() const {
    return static_cast<size_t>(std::count_if(sparks_.begin(), sparks_.end(), [](const Spark& s) {
        return (s.flags & SPARK_ALIVE) != 0;
    }));
}

size_t FireworksSim::heap_bytes() const {
    return buffer_bytes(sparks_, sky_rows_, hill_top_, star_y_, star_level_);
}

void FireworksSim::clear() {
    release_buffers(sparks_, sky_rows_, hill_top_, star_y_, star_level_);
    for (Burst& b : bursts_) {
        b = Burst{};
    }
    for (Rocket& r : rockets_) {
        r = Rocket{};
    }
    w_ = 0;
    h_ = 0;
}

FireworksSim::Spark* FireworksSim::take_spark() {
    const size_t count = sparks_.size();
    for (size_t k = 0; k < count; k++) {
        const size_t i = (next_spark_ + k) % count;
        if (sparks_[i].flags == 0) {
            next_spark_ = (i + 1) % count;
            return &sparks_[i];
        }
    }
    return nullptr;
}

size_t FireworksSim::in_flight() const {
    size_t count = 0;
    for (const Rocket& r : rockets_) {
        count += r.active ? 1 : 0;
    }
    for (const Burst& b : bursts_) {
        count += b.active ? 1 : 0;
    }
    return count;
}

int32_t FireworksSim::next_gap_ms(std::minstd_rand& rng) const {
    const uint32_t min_gap = pacing_.min_gap_ms;
    const uint32_t max_gap = std::max(pacing_.max_gap_ms, min_gap);
    return random_int(rng, static_cast<int>(min_gap), static_cast<int>(max_gap));
}

void FireworksSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                        std::vector<DirtyRect>& dirty) {
    dirty.clear();
    if (w_ <= 0 || h_ <= 0) {
        return;
    }
    PixelWriter writer(frame);
    DirtyRect rocket_boxes[kMaxBursts];
    DirtyRect burst_boxes[kMaxBursts];

    // Everything the previous frame drew goes back to sky first, so the frame shows only what
    // it draws itself.
    for (size_t i = 0; i < kMaxBursts; i++) {
        if (rockets_[i].drawn) {
            erase_rocket(writer, rockets_[i], rocket_boxes[i]);
            rockets_[i].drawn = false;
        }
    }
    for (Spark& spark : sparks_) {
        if ((spark.flags & SPARK_DRAWN) != 0) {
            erase_spark(writer, spark, burst_boxes[spark.burst]);
            spark.flags = static_cast<uint8_t>(spark.flags & ~(SPARK_DRAWN | SPARK_DRAWN_BIG));
        }
    }

    for (uint32_t remaining = dt_ms; remaining > 0;) {
        const uint32_t step_ms = std::min(remaining, kPhysicsStepMs);
        advance(step_ms, rng);
        remaining -= step_ms;
    }

    for (size_t i = 0; i < kMaxBursts; i++) {
        if (rockets_[i].active) {
            draw_rocket(writer, rockets_[i], rocket_boxes[i]);
        }
    }
    for (Spark& spark : sparks_) {
        if ((spark.flags & SPARK_ALIVE) != 0) {
            draw_spark(writer, spark, rng, burst_boxes[spark.burst]);
        }
    }
    // A burst with no sparks left drew nothing this frame, and its last pixels are erased.
    for (Burst& b : bursts_) {
        if (b.active && b.alive == 0) {
            b.active = false;
        }
    }

    for (size_t i = 0; i < kMaxBursts; i++) {
        if (!rocket_boxes[i].empty()) {
            dirty.push_back(rocket_boxes[i]);
        }
        if (!burst_boxes[i].empty()) {
            dirty.push_back(burst_boxes[i]);
        }
    }
}

void FireworksSim::advance(uint32_t step_ms, std::minstd_rand& rng) {
    const float dt = static_cast<float>(step_ms) / 1000.0f;
    const auto elapsed = static_cast<int32_t>(step_ms);

    finale_in_ms_ -= elapsed;
    if (finale_in_ms_ <= 0) {
        finale_in_ms_ += static_cast<int32_t>(std::max<uint32_t>(pacing_.finale_every_ms, 1));
        if (finale_left_ == 0) {
            const int extra = std::max(0, static_cast<int>(pacing_.finale_max_shells) -
                                              pacing_.finale_min_shells);
            finale_left_ = static_cast<uint8_t>(std::max(
                1, random_int(rng, pacing_.finale_min_shells, pacing_.finale_min_shells + extra)));
            finale_gap_ms_ = static_cast<int32_t>(pacing_.finale_span_ms / finale_left_);
            launch_in_ms_ = 0;
        }
    }

    launch_in_ms_ -= elapsed;
    if (launch_in_ms_ <= 0) {
        if (in_flight() < kFireworksLevels[pending_level_].bursts_at_once) {
            launch(rng);
            if (finale_left_ > 0) {
                finale_left_--;
            }
            launch_in_ms_ = finale_left_ > 0 ? finale_gap_ms_ : next_gap_ms(rng);
        } else {
            launch_in_ms_ = 0; // the sky is full; try again next step
        }
    }

    const float rocket_gravity = kRocketGravity * scale_;
    for (Rocket& rocket : rockets_) {
        if (!rocket.active) {
            continue;
        }
        rocket.vy += rocket_gravity * dt;
        rocket.x += rocket.vx * dt;
        rocket.y += rocket.vy * dt;
        if (rocket.vy >= -kBurstSpeed * scale_) {
            burst(rocket, rng);
            rocket.active = false;
        }
    }

    for (Spark& spark : sparks_) {
        if ((spark.flags & SPARK_ALIVE) == 0) {
            continue;
        }
        Burst& owner = bursts_[spark.burst];
        const uint32_t age = static_cast<uint32_t>(spark.age_ms) + step_ms;
        if (age >= spark.life_ms) {
            if (owner.kind == BurstKind::CROSSETTE && (spark.flags & SPARK_SPLIT) == 0) {
                split(spark, rng);
                continue;
            }
            // Only the erase-pending bits survive: take_spark() reuses a spark whose flags are
            // all clear.
            spark.flags = static_cast<uint8_t>(spark.flags & (SPARK_DRAWN | SPARK_DRAWN_BIG));
            if (owner.alive > 0) {
                owner.alive--;
            }
            continue;
        }
        spark.age_ms = static_cast<uint16_t>(age);
        const float keep = std::max(0.0f, 1.0f - owner.drag * dt);
        spark.vx *= keep;
        spark.vy = spark.vy * keep + owner.gravity * dt;
        spark.x += spark.vx * dt;
        spark.y += spark.vy * dt;
    }
}

void FireworksSim::launch(std::minstd_rand& rng) {
    level_ = pending_level_;
    Rocket* slot = nullptr;
    for (Rocket& r : rockets_) {
        if (!r.active) {
            slot = &r;
            break;
        }
    }
    if (slot == nullptr) {
        return;
    }
    const FireworksLevel& params = kFireworksLevels[level_];
    // Launched from anywhere along the hills, to burst in the upper 60% of the frame.
    const int32_t x = std::clamp<int32_t>(
        static_cast<int32_t>(static_cast<float>(w_) * (0.1f + 0.8f * unit_random(rng))), 0, w_ - 1);
    const float ground = static_cast<float>(hill_top_[static_cast<size_t>(x)]);
    const float apex_pick = unit_random(rng);

    Rocket& rocket = *slot;
    rocket = Rocket{};
    rocket.active = true;
    rocket.kind = pick_kind(rng);
    // A palm bursts low enough that its comets' climb stays inside the frame.
    float highest = static_cast<float>(h_) * 0.08f;
    if (rocket.kind == BurstKind::PALM) {
        highest = std::max(highest, kPalmCometRisePx * scale_);
    }
    const float lowest = std::max(highest, static_cast<float>(h_) * 0.60f);
    const float apex = highest + (lowest - highest) * apex_pick;
    const float rise = std::max(ground - apex, 20.0f * scale_);
    switch (rocket.kind) {
    case BurstKind::WILLOW:
        rocket.color = kWillowGold;
        break;
    case BurstKind::PALM:
        rocket.color = kPalmGold;
        break;
    default:
        rocket.color = kShellColors[random_below(rng, kShellColorCount)];
        break;
    }
    rocket.sparks = params.sparks_per_burst;
    rocket.trail = params.trail;
    rocket.x = static_cast<float>(x);
    rocket.y = ground;
    rocket.vx = (unit_random(rng) - 0.5f) * 30.0f * scale_;
    rocket.vy = -std::sqrt(2.0f * kRocketGravity * scale_ * rise);
}

FireworksSim::BurstKind FireworksSim::pick_kind(std::minstd_rand& rng) const {
    int allowed = 0;
    for (uint8_t k = 0; k < kBurstKindCount; k++) {
        allowed += (kinds_ >> k) & 1u;
    }
    int pick = random_below(rng, std::max(allowed, 1));
    for (uint8_t k = 0; k < kBurstKindCount; k++) {
        if (((kinds_ >> k) & 1u) != 0 && pick-- == 0) {
            return static_cast<BurstKind>(k);
        }
    }
    return BurstKind::PEONY;
}

void FireworksSim::burst(const Rocket& rocket, std::minstd_rand& rng) {
    size_t slot = kMaxBursts;
    for (size_t i = 0; i < kMaxBursts; i++) {
        if (!bursts_[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxBursts) {
        return;
    }
    const KindParams& kind = kKinds[static_cast<size_t>(rocket.kind)];
    Burst& owner = bursts_[slot];
    owner = Burst{};
    owner.active = true;
    owner.kind = rocket.kind;
    owner.trail = kind.full_trail ? rocket.trail : static_cast<uint8_t>(rocket.trail / 2);
    owner.drag = kind.drag;
    owner.gravity = kind.gravity * scale_;

    int count = rocket.sparks;
    switch (rocket.kind) {
    case BurstKind::PALM:
        count = std::clamp(count / 12, kPalmMinComets, kPalmMaxComets);
        // Few comets afford long tails, even at the level that gives other shells none.
        owner.trail = static_cast<uint8_t>(std::max(rocket.trail * 3, 6));
        break;
    case BurstKind::CROSSETTE:
        // Each star becomes four fragments, so the burst ends near half the level's sparks.
        count = std::clamp(count / 8, kCrossetteMinStars, kCrossetteMaxStars);
        break;
    case BurstKind::STROBE:
        count = count * 2 / 3;
        owner.trail = 0;
        break;
    default:
        break;
    }

    const float speed =
        (kind.speed_min + (kind.speed_max - kind.speed_min) * unit_random(rng)) * scale_;
    const float tilt = std::cos(unit_random(rng) * 0.45f * kSaverPi);
    const float spin = unit_random(rng) * 2.0f * kSaverPi;
    for (int i = 0; i < count; i++) {
        Spark* spark = take_spark();
        if (spark == nullptr) {
            break; // the pool is full: the rest of this burst is dropped
        }
        float dx = 0.0f;
        float dy = 0.0f;
        float spark_speed = speed;
        if (rocket.kind == BurstKind::PALM) {
            // Comets spread evenly round the burst, thrown slightly upward, so each arcs over
            // and falls like a frond.
            const float theta = 2.0f * kSaverPi *
                                (static_cast<float>(i) + 0.4f * (unit_random(rng) - 0.5f)) /
                                static_cast<float>(count);
            dx = std::cos(theta);
            dy = std::sin(theta) - 0.55f;
            spark_speed = speed * (0.9f + 0.1f * unit_random(rng));
        } else if (rocket.kind == BurstKind::RING) {
            const float theta = 2.0f * kSaverPi * static_cast<float>(i) / static_cast<float>(count);
            const float cx = std::cos(theta);
            const float cy = std::sin(theta) * tilt;
            dx = cx * std::cos(spin) - cy * std::sin(spin);
            dy = cx * std::sin(spin) + cy * std::cos(spin);
        } else {
            // A direction uniform over a sphere, seen face on: an evenly filled ball.
            const float z = 2.0f * unit_random(rng) - 1.0f;
            const float phi = 2.0f * kSaverPi * unit_random(rng);
            const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
            dx = radius * std::cos(phi);
            dy = radius * std::sin(phi);
            spark_speed = speed * (0.85f + 0.15f * unit_random(rng));
        }
        *spark = Spark{};
        spark->x = rocket.x;
        spark->y = rocket.y;
        spark->vx = dx * spark_speed + rocket.vx * 0.3f;
        spark->vy = dy * spark_speed + rocket.vy * 0.3f;
        spark->life_ms = static_cast<uint16_t>(random_int(rng, kind.life_min_ms, kind.life_max_ms));
        spark->color = rocket.color;
        spark->flags = SPARK_ALIVE;
        spark->burst = static_cast<uint8_t>(slot);
        owner.alive++;
    }
    if (owner.alive == 0) {
        owner.active = false;
    }
}

void FireworksSim::split(Spark& spark, std::minstd_rand& rng) {
    const Spark star = spark;
    Burst& owner = bursts_[star.burst];
    // The four fragments fly out across the star's path, as an X over its direction of travel.
    const float base = std::atan2(star.vy, star.vx) + 0.25f * kSaverPi;
    const float push = kCrossetteFragmentSpeed * scale_;
    for (int k = 0; k < kCrossetteFragments; k++) {
        Spark* fragment = k == 0 ? &spark : take_spark();
        if (fragment == nullptr) {
            break; // the pool is full: the star splits into fewer fragments
        }
        const float theta = base + 0.5f * kSaverPi * static_cast<float>(k);
        *fragment = Spark{};
        fragment->x = star.x;
        fragment->y = star.y;
        fragment->vx = star.vx * 0.35f + std::cos(theta) * push;
        fragment->vy = star.vy * 0.35f + std::sin(theta) * push;
        fragment->life_ms = static_cast<uint16_t>(
            random_int(rng, kCrossetteFragmentMinMs, kCrossetteFragmentMaxMs));
        fragment->color = star.color;
        fragment->flags = SPARK_ALIVE | SPARK_SPLIT;
        fragment->burst = star.burst;
        if (k > 0) {
            owner.alive++;
        }
    }
}

template <typename Visit>
void FireworksSim::visit_spark_points(const Spark& spark, uint8_t trail, Visit&& visit) const {
    visit(spark.drawn_x, spark.drawn_y, 255u);
    if ((spark.flags & SPARK_DRAWN_BIG) != 0) {
        visit(spark.drawn_x + 1, spark.drawn_y, 255u);
        visit(spark.drawn_x, spark.drawn_y + 1, 255u);
        visit(spark.drawn_x + 1, spark.drawn_y + 1, 255u);
    }
    for (int32_t k = 1; k <= trail; k++) {
        const int32_t tx = spark.drawn_x + spark.drawn_dx * k * kTrailSpacingTenths / 1000;
        const int32_t ty = spark.drawn_y + spark.drawn_dy * k * kTrailSpacingTenths / 1000;
        visit(tx, ty, static_cast<uint32_t>(220 - 200 * k / (trail + 1)));
    }
}

void FireworksSim::draw_spark(PixelWriter& writer, Spark& spark, std::minstd_rand& rng,
                              DirtyRect& box) {
    const BurstKind kind = bursts_[spark.burst].kind;
    const float life = static_cast<float>(std::max<uint16_t>(spark.life_ms, 1));
    float f = static_cast<float>(spark.age_ms) / life;
    float big_until = kKinds[static_cast<size_t>(kind)].big_until;
    if (kind == BurstKind::CROSSETTE) {
        if ((spark.flags & SPARK_SPLIT) == 0) {
            f *= 0.45f; // a star still in flight keeps its colour until it splits
        } else {
            big_until = kBigUntil;
        }
    }

    Rgb color = spark.color;
    if (kind == BurstKind::STROBE && f >= kStrobeFrom) {
        const auto phase = static_cast<uint32_t>(&spark - sparks_.data()) * 53u;
        if ((spark.age_ms + phase) % kStrobePeriodMs >= kStrobeOnMs) {
            return;
        }
        color = f < kEmberEnd
                    ? kStrobeWhite
                    : scale_rgb(kStrobeWhite,
                                static_cast<uint32_t>(
                                    255.0f * (1.0f - (f - kEmberEnd) / (1.0f - kEmberEnd))));
    } else if (f > kFlickerFrom && random_below(rng, 4) == 0) {
        // Late in its life a spark flickers: on some frames it is not drawn.
        return;
    } else if (f < kFlashEnd) {
        // White flash, the shell colour, ember orange, then a fade into the sky.
        color = mix_rgb(WHITE, spark.color, f / kFlashEnd);
    } else if (f < kColorEnd) {
        color = spark.color;
    } else if (f < kEmberEnd) {
        color = mix_rgb(spark.color, kEmber, (f - kColorEnd) / (kEmberEnd - kColorEnd));
    } else {
        color = scale_rgb(
            kEmber, static_cast<uint32_t>(255.0f * (1.0f - (f - kEmberEnd) / (1.0f - kEmberEnd))));
    }

    const float speed = std::sqrt(spark.vx * spark.vx + spark.vy * spark.vy);
    spark.drawn_x = to_pixel(spark.x);
    spark.drawn_y = to_pixel(spark.y);
    spark.drawn_dx =
        speed > 1.0f ? static_cast<int8_t>(std::lround(-spark.vx / speed * 100.0f)) : 0;
    spark.drawn_dy =
        speed > 1.0f ? static_cast<int8_t>(std::lround(-spark.vy / speed * 100.0f)) : 0;
    spark.flags =
        static_cast<uint8_t>(spark.flags | SPARK_DRAWN | (f < big_until ? SPARK_DRAWN_BIG : 0));
    visit_spark_points(spark, bursts_[spark.burst].trail,
                       [&](int32_t x, int32_t y, uint32_t amount) {
                           light_point(writer, x, y, scale_rgb(color, amount), box);
                       });
}

void FireworksSim::erase_spark(PixelWriter& writer, const Spark& spark, DirtyRect& box) {
    visit_spark_points(spark, bursts_[spark.burst].trail,
                       [&](int32_t x, int32_t y, uint32_t) { erase_point(writer, x, y, box); });
}

void FireworksSim::draw_rocket(PixelWriter& writer, Rocket& rocket, DirtyRect& box) {
    const float speed = std::sqrt(rocket.vx * rocket.vx + rocket.vy * rocket.vy);
    const float ux = speed > 1.0f ? rocket.vx / speed : 0.0f;
    const float uy = speed > 1.0f ? rocket.vy / speed : -1.0f;
    const float tail = (rocket.kind == BurstKind::PALM ? kPalmTailPx : kRocketTailPx) * scale_;
    rocket.head_x = to_pixel(rocket.x);
    rocket.head_y = to_pixel(rocket.y);
    rocket.tail_x = to_pixel(rocket.x - ux * tail);
    rocket.tail_y = to_pixel(rocket.y - uy * tail);
    rocket.drawn = true;

    // A short spark trail, brightest at the shell.
    const int32_t points =
        std::max(std::abs(rocket.tail_x - rocket.head_x), std::abs(rocket.tail_y - rocket.head_y)) +
        1;
    int32_t index = 0;
    PixelWriter::line(
        rocket.head_x, rocket.head_y, rocket.tail_x, rocket.tail_y, [&](int32_t x, int32_t y) {
            light_point(writer, x, y,
                        scale_rgb(kRocket, static_cast<uint32_t>(255 - 195 * index / points)), box);
            index++;
        });
}

void FireworksSim::erase_rocket(PixelWriter& writer, const Rocket& rocket, DirtyRect& box) {
    PixelWriter::line(rocket.head_x, rocket.head_y, rocket.tail_x, rocket.tail_y,
                      [&](int32_t x, int32_t y) { erase_point(writer, x, y, box); });
}

void FireworksSim::light_point(PixelWriter& writer, int32_t x, int32_t y, Rgb color,
                               DirtyRect& box) {
    if (!writer.contains(x, y)) {
        return;
    }
    writer.blend_max(x, y, color);
    box.add(x, y, x, y);
}

void FireworksSim::erase_point(PixelWriter& writer, int32_t x, int32_t y, DirtyRect& box) {
    if (!writer.contains(x, y)) {
        return;
    }
    // Erasing recomputes the sky at the pixel, so no background buffer exists.
    writer.put_dithered(x, y, sky_at(x, y));
    box.add(x, y, x, y);
}

} // namespace helix::ui
