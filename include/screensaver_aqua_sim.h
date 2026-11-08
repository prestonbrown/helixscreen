// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_heightfield.h"
#include "screensaver_motion.h"
#include "screensaver_stroke_digits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_aqua_sim.h
 * @brief Aqua Surface: dark glassy water sloshing in slow motion, seen through a tank wall
 *
 * A heightfield of tank modes, steep Gerstner waves and travelling impulse pulses, drawn by
 * the shared column renderer and shaded by fresnel reflection of a few softbox bands; a bright
 * meniscus along the near edge, bubbles rising below it, the odd flung droplet, and a ring
 * clock in the water.
 *
 * No LVGL, no clock: the saver passes the frame time, its random sequence, the clock reading
 * and the frame, so a seed replays exactly. Buffers are sized in init(); a frame allocates
 * nothing.
 */

namespace helix::ui {

/// One level of the Aqua ladder.
struct AquaLevel {
    uint32_t period_ms;
    uint8_t scale;    ///< the water renders at canvas / scale
    uint16_t slices;  ///< renderer depth slices
    uint8_t grid_div; ///< height grid is kAquaGridX / grid_div x kAquaGridZ / grid_div
    uint16_t bubbles;
    bool splashes;
    bool still; ///< draw the water once, then only the clock
};

/// Most expensive first. The last rung draws the scene once and then only the clock.
inline constexpr AquaLevel kAquaLevels[] = {
    {kSaverFastPeriod, 2, 110, 1, 300, true, false},
    {33, 2, 110, 1, 300, true, false},
    {50, 4, 64, 2, 200, true, false},
    {100, 4, 64, 2, 120, false, false},
    {250, 2, 110, 1, 0, false, true},
};
inline constexpr size_t kAquaLevelCount = std::size(kAquaLevels);

/// Sim seconds per real second: one big slosh takes several seconds.
inline constexpr float kAquaTimeScale = 0.25f;
/// Height grid at level 0, across (screen column) by deep.
inline constexpr int32_t kAquaGridX = 160;
inline constexpr int32_t kAquaGridZ = 64;
/// Near edge depth and the depth of water drawn behind it, world units.
inline constexpr float kAquaZNear = 3.5f;
inline constexpr float kAquaDepth = 4.0f;
/// Droplet gravity, as a share of g, in sim time.
inline constexpr float kAquaDropletGravity = 0.35f;
/// Most droplets alive at once. A crest that would exceed it emits fewer: live droplets are
/// never culled.
inline constexpr size_t kAquaMaxDroplets = 48;
/// Rim brightness never falls below this.
inline constexpr float kAquaRimFloor = 0.4f;
/// Least share of light the water reflects, even looking straight down into it: real water's
/// 0.02 reads dark and oily, 0.35 like chrome.
inline constexpr float kAquaFresnelFloor = 0.12f;

/// Tank mode, Gerstner wave and impulse event, in world units and sim seconds.
struct AquaMode {
    float amp, k, omega, phase;
};
struct AquaWave {
    float amp, k, omega, dir_x, dir_z, steep, phase;
};
struct AquaImpulse {
    double t0; ///< sim seconds it began
    float x, z, amp;
};

/// Height of one Gerstner wave at (x, z, t): one fixed-point pass maps (x, z) back to the
/// point p0 whose displaced position it is, then A sin(k (d . p0) - w t + phi). Time is a
/// double, reduced to the wave's phase, so the surface stays exact on a run of any length.
float aqua_gerstner_height(const AquaWave& wave, float x, float z, double t);

/**
 * @brief The water surface: its height over a grid in (u = x / z, z), evaluated per frame
 *
 * Height = sum of tank modes A cos(k (x + L/2)) cos(w t + phi), w^2 = g k tanh(k depth);
 * Gerstner waves A sin(k (d . p0) - w t + phi) at the point p0 a fixed-point pass maps back
 * from p; and impulses, a Gaussian bump splitting into two pulses travelling apart.
 */
class AquaWater {
  public:
    friend class AquaSimTestAccess;
    /// Seeds the modes, waves and the first impulse from `rng`; grid is gx x gz cells over
    /// u in +-u_max and z from kAquaZNear to kAquaZNear + kAquaDepth.
    void init(std::minstd_rand& rng, int32_t gx, int32_t gz, float u_max);
    /// Re-sizes the grid, keeping the waves.
    void set_grid(int32_t gx, int32_t gz);
    /// Advances the impulse schedule to sim time `t` and evaluates the grid there, keeping the
    /// previous grid for the rate of rise.
    void evaluate(double t, std::minstd_rand& rng);

    /// Scales the slosh and the waves (not the passing impulses): 1 is full, kAquaCalm still.
    void set_amplitude(float scale) {
        amp_ = scale;
    }

    /// Height at world (x, z), bilinear over the grid, clamped to it.
    float height_at(float x, float z) const;
    /// Height of the analytic surface at (x, z, t), no grid: the reference the grid samples.
    float exact_height(float x, float z, double t) const;
    /// Rate of rise at grid cell (i, k) over the last evaluate(), world units per sim second.
    float rise_at(int32_t i, int32_t k) const;
    /// Height at grid cell (i, k), and the cell's world position.
    float cell_height(int32_t i, int32_t k) const {
        return h_[static_cast<size_t>(k) * gx_ + i];
    }
    float cell_u(int32_t i) const;
    float cell_z(int32_t k) const;
    float u_max() const {
        return u_max_;
    }

    int32_t grid_x() const {
        return gx_;
    }
    int32_t grid_z() const {
        return gz_;
    }
    const std::vector<float>& heights() const {
        return h_;
    }
    const std::vector<AquaImpulse>& impulses() const {
        return impulses_;
    }
    const std::array<AquaMode, 3>& modes() const {
        return modes_;
    }
    size_t heap_bytes() const;
    void clear();

  private:
    /// The analytic height at (x, z) for sim time t_.
    float height(float x, float z, double t) const;

    int32_t gx_ = 0;
    int32_t gz_ = 0;
    float u_max_ = 0.0f;
    double t_ = 0.0;
    float dt_ = 0.0f;
    double next_impulse_ = 0.0;
    bool fresh_ = true; ///< no grid evaluated yet, so no rate of rise
    float amp_ = 1.0f;
    std::array<AquaMode, 3> modes_{};
    std::array<AquaWave, 4> waves_{};
    std::vector<AquaImpulse> impulses_;
    std::vector<float> h_;
    std::vector<float> prev_h_;
};

/// A flung droplet or short streak, world units and units per sim second.
struct AquaDroplet {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    bool streak = false;
};

/// Where a droplet met the water.
struct AquaLanding {
    float x = 0.0f;
    float y = 0.0f;       ///< the droplet's height on its last step
    float surface = 0.0f; ///< the surface height under it then
    float z = 0.0f;
};

/**
 * @brief Droplets flung from fast-rising crests
 *
 * A droplet lives until it falls below the surface at its own (x, z): no timer ends it and no
 * budget culls it. kAquaMaxDroplets caps how many are emitted.
 */
class AquaSplashes {
  public:
    void init(size_t capacity);
    /// Stores `drop` and counts it as emitted; false, storing nothing, at the cap.
    bool emit(const AquaDroplet& drop);
    /// Throws droplets from up to three fast-rising crests of `water` near the front, each with
    /// a 15% chance of one to three droplets, as many as the cap leaves room for.
    void emit_crests(const AquaWater& water, std::minstd_rand& rng);
    /// Moves every droplet `dt` sim seconds and removes those now below the surface at their
    /// own (x, z), appending each removal to `landed` when it is not null.
    void move(float dt, const AquaWater& water, std::vector<AquaLanding>* landed);
    /// emit_crests() then move().
    void step(float dt, const AquaWater& water, std::minstd_rand& rng,
              std::vector<AquaLanding>* landed);
    const std::vector<AquaDroplet>& droplets() const {
        return droplets_;
    }
    /// Droplets emitted since init().
    uint32_t emitted() const {
        return emitted_;
    }
    size_t heap_bytes() const;
    void clear();

  private:
    std::vector<AquaDroplet> droplets_;
    size_t capacity_ = 0;
    uint32_t emitted_ = 0;
};

/// Fill of the three rings, 0 to 1: top, hour (bottom left), minute (bottom right).
struct AquaRings {
    float top = 0.0f;
    float hour = 0.0f;
    float minute = 0.0f;
};

/**
 * @brief How full each ring is for `reading`
 *
 * Hour: through the 12 h half-day, or the 24 h day in 24-hour mode, minutes and seconds
 * included. Minute: through the hour, seconds included. Top: through the minute, or the
 * print's progress while counting down, paused or done.
 */
AquaRings aqua_rings(const ClockReading& reading);

/// How far the water stands above its rest level at the brim, in water units; well under the
/// camera's height, where the eye would be at the surface.
inline constexpr float kAquaBrimRise = 0.5f;
/// How fast the water level moves, water units per second, up or down.
inline constexpr float kAquaLevelRate = 0.05f;
/// The still rung paints the water again each time the level moves this far.
inline constexpr float kAquaStillLevelStep = 0.05f;
/// Wave and slosh amplitude while a print is paused, against 1 otherwise.
inline constexpr float kAquaCalm = 0.35f;
/// How fast the amplitude eases between calm and full, per second.
inline constexpr float kAquaCalmRate = 0.5f;
/// Bubbles a finished print sends up at once.
inline constexpr size_t kAquaDoneBurst = 120;

/// The water's rise above rest for `reading`: none with no print, rising with progress to
/// kAquaBrimRise at 100% and at DONE; back to rest for a failed or cancelled print.
float aqua_level_target(const ClockReading& reading);

/// Where the clock sits on a w x h canvas, moved by the drift (dx, dy).
struct AquaClockLayout {
    int32_t ring_r = 0;     ///< bottom rings' radius
    int32_t top_r = 0;      ///< top ring's radius
    int32_t ring_width = 0; ///< arc stroke, px
    int32_t hour_cx = 0, hour_cy = 0;
    int32_t minute_cx = 0, minute_cy = 0;
    int32_t top_cx = 0, top_cy = 0;
    int32_t digit_unit = 0; ///< stroke-digit glyph unit
    DirtyRect text;         ///< the date (or print) line under the rings
    DirtyRect bounds;       ///< everything the clock draws
};
AquaClockLayout layout_aqua_clock(int32_t w, int32_t h, int32_t dx, int32_t dy);

class AquaSim {
  public:
    friend class AquaSimTestAccess;
    static constexpr const auto& kLevels = kAquaLevels;

    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const ClockReading& reading);
    /**
     * @brief Advances by `dt_ms` and draws the water band and the clock into `frame`
     *
     * Replaces `dirty` with the boxes written. At the last rung the water stands still and a
     * frame writes only the clock, and only when the reading it shows changes.
     */
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
              const ClockReading& reading, std::vector<DirtyRect>& dirty);
    void request_level(size_t level);
    size_t level() const {
        return level_;
    }

    const AquaWater& water() const {
        return water_;
    }
    const AquaSplashes& splashes() const {
        return splashes_;
    }
    const HeightfieldRenderer& renderer() const {
        return renderer_;
    }
    const AquaClockLayout& clock_layout() const {
        return clock_;
    }
    /// Sim seconds since init().
    double sim_time() const {
        return t_sim_;
    }
    /// Rim brightness, 0 to 1, over output column `x` in the last frame.
    float rim_brightness(int32_t x) const;
    /// The near edge's output row over output column `x` in the last frame.
    float rim_row(int32_t x) const;
    /// First output row the last frame repainted whole: the water band, which takes in the
    /// clock.
    int32_t band_top() const {
        return band_top_;
    }
    /// Puts back the scene under `box` at the still rung, where the water is not redrawn each
    /// frame, and adds what it wrote to `dirty`. Elsewhere the next frame repaints it anyway.
    void restore(FrameTarget& frame, const DirtyRect& box, std::vector<DirtyRect>& dirty) const;
    size_t heap_bytes() const;
    void clear();

  private:
    class Scene;

    /// Sizes the renderer, the grid and the internal frame for `level_`.
    void apply_view();
    /// Everything the moving water needs beyond the water grid's waves: lookup tables, splashes,
    /// bubbles and the near edge. Draws the bubbles' places from `rng`.
    void build_scene(std::minstd_rand& rng);
    /// Frees all of the scene but the clock, once the still rung has painted it: from then on a
    /// frame only redraws the clock over the saved water.
    void release_scene();
    /// Height grid derivatives for the scene: slopes and crest thinness.
    void derive_grids();
    /// Renders the water at the current sim time and writes the band into `frame`.
    DirtyRect paint_water(FrameTarget& frame);
    void draw_rim(FrameTarget& frame) const;
    void draw_bubbles(FrameTarget& frame) const;
    /// Droplets and glints; adds each one's box to `boxes`.
    void draw_spray(FrameTarget& frame, std::vector<DirtyRect>& boxes) const;
    void draw_clock(FrameTarget& frame, const ClockReading& reading) const;
    void save_clock_region(const FrameTarget& frame);
    /// Paints everything once: the still rung's frame, or any rung's first.
    void paint_all(FrameTarget& frame, const ClockReading& reading);
    void update_clock_layout();

    size_t level_ = 0;
    int32_t w_ = 0;
    int32_t h_ = 0;
    double t_sim_ = 0.0;
    ClockDrift drift_;
    screensaver::StepAccumulator emit_steps_;
    AquaWater water_;
    AquaSplashes splashes_;
    HeightfieldRenderer renderer_;
    AquaClockLayout clock_;
    StrokeDigitSet digits_;
    ClockReading shown_;
    /// The water's rise above rest (aqua_level_target, eased), and the rise the view was last
    /// set up for.
    float water_level_ = 0.0f;
    float viewed_level_ = 0.0f;
    /// The still rung's level step the water was last painted at.
    int32_t painted_level_step_ = 0;
    /// Wave amplitude, easing toward kAquaCalm while paused.
    float calm_ = 1.0f;
    ClockMode last_mode_ = ClockMode::CLOCK;
    /// The camera's height over the water's surface.
    float cam_h() const;
    /// Eases the level and calm toward `reading` over `dt_s`, at once when `dt_s` is negative.
    void follow_print(const ClockReading& reading, float dt_s);
    std::vector<float> env_;     ///< AQUA_ENV_LEVELS pre-blurred environment rows
    std::vector<float> wrinkle_; ///< 64 x 64 x 2 slope table
    std::vector<Rgb> tint_;      ///< 256 grey levels through the cool tint
    std::vector<Rgb> frame_;     ///< internal row-major
    std::vector<float> rim_;     ///< per output column
    std::vector<AquaLanding> landed_;
    struct Glint {
        float x, y, z;
        uint16_t age_ms;
    };
    std::vector<Glint> glints_;
    struct Bubble {
        float x, y, speed, phase;
    };
    std::vector<Bubble> bubbles_;
    std::vector<DirtyRect> last_spray_; ///< droplet and glint boxes drawn last frame
    std::vector<DirtyRect> spray_;
    // Per frame, at grid size: slopes across and deep, crest thinness.
    std::vector<float> grid_gx_;
    std::vector<float> grid_gz_;
    std::vector<float> grid_thin_;
    std::vector<float> grid_mean_; ///< one grid row's local mean, then the blur's scratch
    // Per internal column: grid column and its weight, and the last slice's reflection.
    std::vector<int32_t> col_i_;
    std::vector<float> col_f_;
    std::vector<float> el_;
    std::vector<float> prev_el_;
    std::vector<float> front_shade_;  ///< the first slice's shade, per internal column
    std::vector<float> scratch_;      ///< per internal column, four values
    std::vector<float> edge_rows_;    ///< near edge per output column
    std::vector<uint8_t> clock_save_; ///< still rung: scene pixels under the clock bounds
    DirtyRect clock_saved_box_;
    int32_t scale_ = 2;
    int32_t iw_ = 0; ///< internal frame
    int32_t ih_ = 0;
    float focal_ = 0.0f;
    float horizon_ = 0.0f;
    int32_t band_top_ = 0;
    bool painted_ = false;
    bool scene_released_ = false;
};

} // namespace helix::ui
