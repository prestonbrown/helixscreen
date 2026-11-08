// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_hyperspace_sim.h
 * @brief A flight through glowing rainbow space, after Terence Welsh's Hyperspace
 *
 * Star flares stream past a gently drifting camera; for stretches a shimmering wormhole sweeps
 * in around the flight path and fades back out; now and then a shell of flares bursts from a
 * point ahead; and a slow plasma of iridescent goo glows behind it all.
 *
 * A print shows as a light at the vanishing point, the end of the tunnel, growing as the print
 * goes. A paused print slows the flight; a finished one drops out of hyperspace into still
 * stars; a failed or cancelled one dims the light and closes the tunnel.
 *
 * No LVGL, no clock and no shared random sequence: the saver passes the frame time, its own
 * random sequence, the print reading and the frame, so a seed replays exactly. Buffers are sized in
 * init() for the frame; only a level change resizes the tunnel's, and a frame allocates nothing.
 */

namespace helix::ui {

/// One level of the hyperspace ladder. Every tunable a level changes is here.
struct HyperspaceLevel {
    uint32_t period_ms; ///< frame period in ms
    /// Side, in px, of the square blocks the tunnel and goo are drawn in; 0 draws no tunnel.
    uint8_t block;
    bool goo;
    uint16_t stars;
};

/// The ladder, most expensive level first. A requested level applies at the next frame.
inline constexpr HyperspaceLevel kHyperspaceLevels[] = {
    {kSaverFastPeriod, 1, true, 160},
    {33, 1, true, 160},
    {33, 2, true, 160},
    {33, 2, false, 160},
    {33, 4, false, 160},
    {33, 0, false, 96},
};

inline constexpr size_t kHyperspaceLevelCount = std::size(kHyperspaceLevels);

/// The light's radius at the start of a print, px, and at its end, as a share of the frame's
/// shorter side; the level without a tunnel draws it per pixel, so it ends smaller there.
inline constexpr float kHyperspaceLightStartPx = 4.0f;
inline constexpr float kHyperspaceLightEndShare = 0.18f;
inline constexpr float kHyperspaceCheapLightEndShare = 0.09f;
/// Camera speed while a print is paused, as a share of the flight's own.
inline constexpr float kHyperspacePausedSpeed = 0.25f;
/// How long a finished print takes to drop out of hyperspace to a standstill.
inline constexpr uint32_t kHyperspaceDropOutMs = 3000;

/// Largest roll either way, in radians.
inline constexpr float kHyperspaceMaxRoll = 0.3f;

/// Where the camera is and how it moves.
struct HyperspaceCamera {
    float speed = 0.0f;   ///< forward, in depth units per second
    float roll = 0.0f;    ///< radians, within kHyperspaceMaxRoll either way
    float drift_x = 0.0f; ///< vanishing point's offset from the frame centre, px
    float drift_y = 0.0f;
};

class HyperspaceSim {
  public:
    static constexpr const auto& kLevels = kHyperspaceLevels;

    /// Most star flares at any level.
    static constexpr size_t kMaxStars = 160;
    /// Flares one starburst throws.
    static constexpr size_t kBurstFlares = 48;

    /**
     * @brief Sizes every buffer for `frame` and starts the flight at `level`
     *
     * The tunnel's tables are built for the level the run starts at, on its first frame, and a
     * coarser level reuses them; a level without a tunnel frees them. The flight starts as
     * `reading` leaves it: an ended print's already at its end.
     */
    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const ClockReading& reading);

    /**
     * @brief Advances the flight by `dt_ms`, following `reading`, and draws the whole frame
     *
     * Every pixel is written, opaque, so `dirty` is replaced with one box over the frame; once
     * a finished print has come to a standstill and been drawn, a frame draws nothing and
     * `dirty` is left empty.
     */
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
              const ClockReading& reading, std::vector<DirtyRect>& dirty);

    /// Runs at `level` (clamped to the ladder) from the next frame on.
    void request_level(size_t level);

    /// Level the flight runs at now.
    size_t level() const {
        return level_;
    }

    /// How far the tunnel has swept in as drawn, 0 (open space) to 255 (fully in the tunnel).
    uint8_t tunnel_amount() const {
        return shown_tunnel_;
    }

    /// Radius, px, of the light at the vanishing point at the level the flight runs at; 0
    /// with none.
    float light_radius() const;

    /// How bright the light burns, 0 (out) to 1.
    float light_gain() const {
        return light_gain_;
    }

    /// Share of the flight's speed the camera flies at now.
    float speed_scale() const {
        return speed_scale_;
    }

    const HyperspaceCamera& camera() const {
        return camera_;
    }

    /// Starbursts thrown since init().
    uint32_t bursts_started() const {
        return bursts_started_;
    }

    /// Burst flares still burning.
    size_t burst_flares_alive() const;

    /// Hue the palettes are centred on now, 0 to 255 around the colour wheel.
    uint8_t hue() const;

    /// Heap bytes the sim holds. Constant from frame to frame; a level change may resize the
    /// tunnel's share once.
    size_t heap_bytes() const;

    /// Heap bytes of the tunnel's lookup tables, texture and palette: sized for the finest block
    /// size the run has drawn, and none at a level without a tunnel.
    size_t tunnel_bytes() const;

    /// Frees every buffer.
    void clear();

  private:
    struct Star {
        float x = 0.0f; ///< camera space: right, down and forward
        float y = 0.0f;
        float z = 0.0f;
        uint8_t hue_offset = 0;
    };

    struct Flare {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float vx = 0.0f; ///< per second
        float vy = 0.0f;
        float vz = 0.0f;
        uint16_t age_ms = 0;
        uint16_t life_ms = 0;
        uint8_t hue_offset = 0;
        bool alive = false;
    };

    enum class Phase : uint8_t { OPEN, SWEEP_IN, TUNNEL, SWEEP_OUT };

    /// Moves the speed, tunnel and light toward where `reading` puts them after `dt_ms`, or
    /// there at once when `snap` is set.
    void follow_print(const ClockReading& reading, uint32_t dt_ms, bool snap);
    /// True when a finished print has stopped the flight: nothing left moves.
    bool standing_still() const;
    void advance(uint32_t dt_ms, std::minstd_rand& rng);
    void advance_phase(uint32_t dt_ms, std::minstd_rand& rng);
    void throw_starburst(std::minstd_rand& rng);
    void place_star(Star& star, std::minstd_rand& rng, bool anywhere);
    void build_texture();
    void build_tables();
    void build_palettes();

    template <typename Px> void draw_background(FrameTarget& frame);
    /// Draws a flare at camera-space (x, y, z), its sprite radius `size` of the frame height at
    /// z = 1, at `brightness` out of 256.
    /// Adds the light's glow around the vanishing point, in blocks of the level's size.
    template <typename Px> void draw_light(FrameTarget& frame);
    template <typename Px>
    void draw_flare(FrameTarget& frame, float x, float y, float z, float size, uint8_t hue_offset,
                    uint32_t brightness);

    size_t level_ = 0;
    size_t pending_level_ = 0;
    bool tables_stale_ = true;
    int32_t w_ = 0;
    int32_t h_ = 0;
    PixelFormat format_ = PixelFormat::XRGB8888;
    float scale_ = 1.0f; ///< frame height over the 480 px the distances are tuned for

    uint64_t elapsed_ms_ = 0; ///< wide enough never to wrap
    HyperspaceCamera camera_{};
    float scroll_ = 0.0f; ///< distance flown, in texture rows

    Phase phase_ = Phase::OPEN;
    int32_t phase_left_ms_ = 0;
    uint8_t tunnel_amount_ = 0;
    int32_t burst_in_ms_ = 0;
    uint32_t bursts_started_ = 0;

    ClockMode mode_ = ClockMode::CLOCK;
    float speed_scale_ = 1.0f;
    float tunnel_fade_ = 1.0f;    ///< share of the swept-in tunnel drawn
    uint8_t shown_tunnel_ = 0;    ///< tunnel_amount_ by tunnel_fade_
    float light_progress_ = 0.0f; ///< 0 to 1, the light's radius between start and end
    float light_gain_ = 0.0f;
    bool still_drawn_ = false; ///< the standstill has been drawn, so frames draw nothing

    // Tunnel lookup for one quadrant around the vanishing point, at the level's block size and
    // wide enough for the drift: a pixel's distance into the tunnel and its angle within the
    // quadrant.
    int32_t quad_w_ = 0;
    int32_t quad_h_ = 0;
    std::vector<uint16_t> depth_;
    std::vector<uint16_t> angle_;
    std::vector<uint8_t> shade_of_depth_;
    std::vector<uint8_t> texture_; ///< 256 x 256 rippling bands
    // Palettes packed in the frame's format, rebuilt each frame. Tunnel and goo colours keep
    // within complementary channel ranges, so adding two packed entries never carries.
    std::vector<uint32_t> tunnel_palette_; ///< shade level x texel
    std::vector<uint32_t> goo_palette_;
    // Goo plasma terms per column, row and diagonal of the block grid.
    std::vector<uint8_t> goo_col_;
    std::vector<uint8_t> goo_row_;
    std::vector<uint8_t> goo_diag_;
    // Row of packed colours at the block size, copied down the block's rows.
    std::vector<uint32_t> row_;
    std::vector<uint8_t> sprites_;    ///< radial falloff per radius, packed back to back
    std::vector<uint32_t> sprite_at_; ///< offset of each radius in sprites_

    std::vector<Star> stars_;
    std::vector<Flare> flares_; ///< kBurstFlares, reused by every starburst
};

} // namespace helix::ui
