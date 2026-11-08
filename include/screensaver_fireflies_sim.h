// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_motion.h"
#include "screensaver_vec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_fireflies_sim.h
 * @brief Swarms of glowing flies chasing invisible baits, after Matt Perry's Fireflies
 *
 * A clean reimplementation from the original's described behaviour and published numbers. On
 * black, 2 to 5 invisible baits wander a box 80 units tall, aspect-scaled wide and 50 deep,
 * seen through a fixed perspective camera; 100 to 175 flies chase them in coloured swarms,
 * each trailing a tail bent by a slowly changing wind, all drawn with additive light.
 *
 * No LVGL, no clock and no shared random sequence: the saver passes the frame time, its own
 * random sequence and the frame. The world advances in fixed kFirefliesStepMs steps, and the
 * random sequence is read only inside a step, so the motion is identical whatever the frame
 * period or level. Buffers are sized in init(); a frame allocates nothing.
 *
 * The sim reads the print each frame. During a running print the swarm gathers for
 * kFirefliesGatherMs every kFirefliesGatherEveryMs into the percent; a finished print holds the
 * done text, glowing; a failed or cancelled one turns the flies kPrintFailed and sends the baits
 * to new waypoints. Without a print nothing of this runs, and the random sequence is read exactly
 * as it would be without the reading.
 */

namespace helix::ui {

/// One level of the fireflies ladder. Levels change only what is drawn, never the motion.
struct FirefliesLevel {
    uint32_t period_ms;    ///< frame period in ms
    uint16_t max_flies;    ///< flies drawn, the first of the population
    uint16_t tail_ms;      ///< age of the oldest tail point drawn
    uint8_t tail_width_px; ///< widest tail span at 480 px tall; 1 draws a single-pixel line
    bool halo;             ///< soft halo round each body while GLOW runs
};

/// The ladder, most expensive level first. A requested level applies at the next frame.
inline constexpr FirefliesLevel kFirefliesLevels[] = {
    {kSaverFastPeriod, 175, 2250, 3, true},
    {33, 175, 2250, 3, true},
    {33, 175, 2250, 1, false},
    {50, 120, 1500, 1, false},
    {100, 48, 750, 1, false},
};

inline constexpr size_t kFirefliesLevelCount = std::size(kFirefliesLevels);

/// Fixed simulation step, ms.
inline constexpr uint32_t kFirefliesStepMs = 10;
/// Tail points are recorded every this many ms.
inline constexpr uint32_t kFirefliesTailSampleMs = 50;
/// Age of the oldest tail point at level 0, ms.
inline constexpr uint32_t kFirefliesTailMs = 2250;
/// Tail points kept per fly: enough for kFirefliesTailMs.
inline constexpr size_t kFirefliesTailPoints = kFirefliesTailMs / kFirefliesTailSampleMs + 1;

/// World box, in world units: height and depth; the width is height times the frame's aspect.
inline constexpr float kFirefliesWorldH = 80.0f;
inline constexpr float kFirefliesWorldD = 50.0f;

/// Top speed (units/s) and acceleration (units/s^2) of baits and flies.
inline constexpr float kFirefliesBaitSpeed = 50.0f;
inline constexpr float kFirefliesBaitAccel = 600.0f;
inline constexpr float kFirefliesFlySpeed = 100.0f;
inline constexpr float kFirefliesFlyAccel = 300.0f;

/// Swarm hue drift, degrees per second, and a fly's hue swing with its speed, degrees either way.
inline constexpr float kFirefliesHueRateDps = 15.0f;
inline constexpr float kFirefliesHueSwingDeg = 20.0f;

inline constexpr size_t kFirefliesMinBaits = 2;
inline constexpr size_t kFirefliesMaxBaits = 5;
inline constexpr size_t kFirefliesMinFlies = 100;
inline constexpr size_t kFirefliesMaxFlies = 175;

/// The wind takes a new heading after 4 to 8 tail lengths.
inline constexpr uint32_t kFirefliesWindMinMs = 4 * kFirefliesTailMs;
inline constexpr uint32_t kFirefliesWindMaxMs = 8 * kFirefliesTailMs;

/// What the swarms are doing. NORMAL runs between the others; SPLIT and MERGE are instant.
enum class FirefliesMode : uint8_t {
    NORMAL,
    STOP,       ///< every bait brakes to a halt and holds, so the swarms close in on them
    LOOP,       ///< every bait circles the point where the mode found it
    GLOW,       ///< tails and bodies burn brighter
    HYPERSPEED, ///< baits and flies go three times as fast, accelerating three times as hard
    SPLIT,      ///< a bait divides: a new bait takes every other fly of its swarm
    MERGE,      ///< a bait's swarm joins the nearest other bait and the bait goes
    WIND_GUST,  ///< the wind blows four times as hard
};
inline constexpr size_t kFirefliesModeCount = 8;

/// Side, in px, of the tiles the sim tracks what it drew in.
inline constexpr int32_t kFirefliesTilePx = 16;
/// Most areas one frame reports dirty. An area costs a board about what 3600 of its pixels do,
/// so the tiles are joined into a few boxes rather than reported one by one.
inline constexpr size_t kFirefliesMaxDirtyAreas = 24;

/**
 * @brief Brightness of a tail point `u` of the way along the drawn tail (0 head, 1 oldest)
 *
 * sin(pi u) * (1 - 0.5 u): zero at both ends, brightest near the middle, and dimmer toward the
 * old end than the new. 0 outside [0, 1].
 */
float fireflies_tail_brightness(float u);

/// Tail span, in px, at `u` for a tail `max_px` wide at its widest: 1 + (max_px - 1) * sin(pi u),
/// rounded, at least 1 inside [0, 1].
int32_t fireflies_tail_width(float u, float max_px);

/// During a running print the swarm gathers into the percent for kFirefliesGatherMs at the start
/// of every kFirefliesGatherEveryMs, counted from when the sim first read the print running.
inline constexpr uint32_t kFirefliesGatherEveryMs = 60000;
inline constexpr uint32_t kFirefliesGatherMs = 6000;
/// Stroke width of the gathered text, in glyph units (screensaver_stroke_digits.h).
inline constexpr float kFirefliesTextStroke = 0.15f;
/// Most characters the gathered text holds: "100%" and "DONE".
inline constexpr size_t kFirefliesTextMaxChars = 4;
/// Where a gathered text of `chars` characters sits on a `w` x `h` frame: glyph boxes `unit_px`
/// per glyph unit, the first box's top left at (x0, y0), each next box `advance_px` to the right.
/// The text spans 70% of the width, or less where 60% of the height bounds it, centred.
struct FirefliesTextLayout {
    float unit_px;
    float x0;
    float y0;
    float advance_px;
};
FirefliesTextLayout fireflies_text_layout(int32_t w, int32_t h, size_t chars);

/// Positions and directions are SaverVec3s in world units: x right, y down, z away from the
/// camera.
class FirefliesSim {
  public:
    friend class FirefliesSimTestAccess;
    static constexpr const auto& kLevels = kFirefliesLevels;

    struct Bait {
        SaverVec3 pos;
        SaverVec3 vel;
        SaverVec3 target;      ///< waypoint the bait steers for
        SaverVec3 loop_centre; ///< centre LOOP circles
        float hue_deg = 0.0f;
        int32_t retarget_ms = 0; ///< left before a new waypoint
        bool alive = false;
    };

    struct Fly {
        SaverVec3 pos;
        SaverVec3 vel;
        float pace = 1.0f; ///< this fly's share of the top speed and acceleration, 0.75 to 1
        uint8_t bait = 0;  ///< index into baits()
    };

    /// Sizes every buffer for `frame` and places the population: 2 to 5 baits and 100 to 175
    /// flies spread through the world box, every tail starting collapsed on its fly. A finished
    /// print in `print` places the flies on the done text at once; a running one starts the
    /// first gather.
    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const ClockReading& print);

    /**
     * @brief Advances the world by `dt_ms` and redraws the swarms
     *
     * The world moves in whole kFirefliesStepMs steps; the remainder carries to the next call.
     * Every kFirefliesTilePx tile the previous frame drew in is cleared to black, then every
     * drawn fly adds its light. `dirty` is replaced with at most kFirefliesMaxDirtyAreas boxes
     * covering every tile drawn in either frame: what the last frame drew (and this frame
     * cleared) and what this one drew. Tails are thin, so the tiles cover far less than the
     * boxes round whole swarms would. `print` is the print now; see the file comment.
     */
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng, const ClockReading& print,
              std::vector<DirtyRect>& dirty);

    /// True while the flies steer for their places in the gathered text.
    bool gathering() const {
        return gathering_;
    }
    /// The text gathered now or last: "63%", the done text, or empty before the first gather.
    const char* gather_text() const {
        return gather_text_;
    }

    /// Draws at `level` (clamped to the ladder) from the next frame on.
    void request_level(size_t level);
    size_t level() const {
        return level_;
    }

    /// Starts `mode` now for `duration_ms` (ignored for SPLIT and MERGE, which act at once and
    /// return to NORMAL). A SPLIT with kFirefliesMaxBaits alive, or a MERGE with
    /// kFirefliesMinBaits alive, is refused and returns false.
    bool begin_mode(FirefliesMode mode, uint32_t duration_ms, std::minstd_rand& rng);
    FirefliesMode mode() const {
        return mode_;
    }
    /// Times each mode has begun since init(), indexed by FirefliesMode.
    uint32_t modes_begun(FirefliesMode mode) const;

    const std::vector<Bait>& baits() const {
        return baits_;
    }
    size_t baits_alive() const;
    const std::vector<Fly>& flies() const {
        return flies_;
    }

    /// Hue, in degrees [0, 360), fly `i` is drawn in now: its swarm's hue, shifted from
    /// -kFirefliesHueSwingDeg at rest to +kFirefliesHueSwingDeg at kFirefliesFlySpeed.
    float fly_hue_deg(size_t i) const;

    /// Age, in ms, of the oldest recorded tail point, the same for every fly: they are sampled
    /// together. Grows to kFirefliesTailMs (within one kFirefliesTailSampleMs) and stays there.
    uint32_t tail_age_ms() const;

    /// Wind now, world units per second; a tail point of age a is drawn displaced by wind * a.
    const SaverVec3& wind() const {
        return wind_;
    }
    /// Times the wind has taken a new heading since init().
    uint32_t wind_changes() const {
        return wind_changes_;
    }
    /// Sim time, ms since init(), of the latest new heading; 0 before the first.
    uint64_t last_wind_change_ms() const {
        return last_wind_change_ms_;
    }

    /// Simulated ms since init(): a whole number of kFirefliesStepMs.
    uint64_t elapsed_ms() const {
        return elapsed_ms_;
    }

    /// Heap bytes the sim holds; constant from frame to frame and across level changes.
    size_t heap_bytes() const;

    /// Frees every buffer.
    void clear();

  private:
    /// Most steps one frame advances: a stall moves the swarms half a second, never more.
    static constexpr uint32_t kMaxStepsPerFrame = 50;

    void advance_one(std::minstd_rand& rng);
    SaverVec3 random_waypoint(std::minstd_rand& rng) const;
    float speed_mult() const;
    bool split(std::minstd_rand& rng);
    bool merge();
    std::array<size_t, kFirefliesMaxBaits> swarm_sizes() const;
    void new_wind_heading(std::minstd_rand& rng);
    void update_wind();
    void schedule_next_mode(std::minstd_rand& rng);
    template <typename Px> void draw(FrameTarget& frame, std::vector<DirtyRect>& dirty);
    void collect_dirty(std::vector<DirtyRect>& dirty);
    void reset_tails();
    void read_print(const ClockReading& print, std::minstd_rand& rng, bool at_init);
    void update_gather();
    void place_targets(const char* text);

    size_t level_ = 0;
    size_t pending_level_ = 0;
    int32_t w_ = 0;
    int32_t h_ = 0;
    float world_w_ = 0.0f;
    float focal_ = 0.0f; ///< px per world unit at the box's middle depth

    screensaver::StepAccumulator steps_{};
    uint64_t elapsed_ms_ = 0;

    FirefliesMode mode_ = FirefliesMode::NORMAL;
    int32_t mode_left_ms_ = 0;
    int32_t mode_total_ms_ = 0;
    int32_t next_mode_ms_ = 0;
    uint32_t modes_begun_[kFirefliesModeCount]{};

    SaverVec3 wind_{};
    /// The wind before any gust: a new heading eases from this, never from the gusted wind_.
    SaverVec3 base_wind_{};
    SaverVec3 wind_from_{};
    SaverVec3 wind_to_{};
    int32_t wind_left_ms_ = 0;
    uint32_t wind_changes_ = 0;
    uint64_t last_wind_change_ms_ = 0;

    std::vector<Bait> baits_; ///< kFirefliesMaxBaits slots
    /// Per bait slot, the hue at elapsed 0 that its hue_deg turns from.
    float hue_origin_[kFirefliesMaxBaits]{};
    /// Per bait slot, where on LOOP's circle the bait started.
    float loop_phase_[kFirefliesMaxBaits]{};
    std::vector<Fly> flies_;
    /// kFirefliesTailPoints per fly, newest first from tail_head_, in fixed point (1/64 unit).
    std::vector<int16_t> tail_;
    size_t tail_head_ = 0;
    size_t tail_count_ = 0;

    /// Per kFirefliesTilePx tile, nonzero where the last frame drew; the tiles this frame draws.
    std::vector<uint8_t> drawn_tiles_;
    std::vector<uint8_t> now_tiles_;
    int32_t tiles_x_ = 0;
    int32_t tiles_y_ = 0;

    /// The print as the last frame read it.
    ClockMode print_mode_ = ClockMode::CLOCK;
    int32_t print_pct_ = 0;
    /// Sim time the print was first read running; the gathers count from it.
    uint64_t print_start_ms_ = 0;
    /// The print's spell_states: a finished print gathers "DONE", or kGlyphCheck where false.
    bool spell_states_ = true;
    bool gathering_ = false;
    bool gather_done_ = false; ///< the gathered text is the done text
    char gather_text_[kFirefliesTextMaxChars + 1]{};
    /// Per fly, its place in the gathered text, on the box's middle depth plane.
    std::vector<SaverVec3> fly_target_;
    /// Scratch for place_targets(): the covered cells of the text's coarse glyph grid, room
    /// for kFirefliesTextMaxChars glyphs.
    std::vector<uint16_t> text_cells_;
};

} // namespace helix::ui
