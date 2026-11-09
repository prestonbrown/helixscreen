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
 * @file screensaver_fireworks_sim.h
 * @brief A fireworks show over hills under a night sky, stepped one frame at a time
 *
 * No LVGL, no clock, no shared random sequence and no allocation after init(): the saver
 * passes the frame time, its own random sequence and the frame, so a seed replays exactly and
 * the show runs in tests without a display.
 */

namespace helix::ui {

class PixelWriter;

/// One level of the fireworks ladder. Every tunable a level changes is here.
struct FireworksLevel {
    uint32_t period_ms; ///< frame period in ms
    uint16_t sparks_per_burst;
    uint8_t trail;          ///< trail points behind a trailing spark's head
    uint8_t bursts_at_once; ///< shells in flight plus bursts still burning
};

/// The ladder, most expensive level first. A requested level applies when the next shell launches.
inline constexpr FireworksLevel kFireworksLevels[] = {
    {kSaverFastPeriod, 150, 5, 6},
    {33, 150, 5, 6},
    {33, 90, 3, 4},
    {50, 50, 0, 3},
};

inline constexpr size_t kFireworksLevelCount = std::size(kFireworksLevels);

/// When shells launch. The defaults are the show's pacing; tests shorten them.
struct FireworksPacing {
    uint32_t first_launch_ms = 300;
    uint32_t min_gap_ms = 800;
    uint32_t max_gap_ms = 2500;
    uint32_t finale_every_ms = 180000;
    uint8_t finale_min_shells = 6;
    uint8_t finale_max_shells = 10;
    uint32_t finale_span_ms = 4000;
};

/// A running print's longest launch gap at 100%: the show quickens toward it as the print goes.
inline constexpr uint32_t kFireworksPrintEndMaxGapMs = 900;

class FireworksSim {
  public:
    static constexpr const auto& kLevels = kFireworksLevels;

    /// Most shells in flight plus bursts burning, at any level.
    static constexpr size_t kMaxBursts = 6;
    /// Faint fixed stars in the sky.
    static constexpr int kStarCount = 80;

    /// PALM throws a few heavy gold comets that arc and droop like fronds; CROSSETTE's stars
    /// each split into a cross of four; STROBE's stars hang and blink white.
    enum class BurstKind : uint8_t {
        PEONY,
        CHRYSANTHEMUM,
        WILLOW,
        RING,
        PALM,
        CROSSETTE,
        STROBE,
    };
    static constexpr uint8_t kBurstKindCount = 7;
    static constexpr uint8_t kAllKinds = (1u << kBurstKindCount) - 1;

    /**
     * @brief Lays out the sky for `frame`, sizes the spark pool for `level`, and paints the sky
     *
     * The pool holds sparks_per_burst * bursts_at_once sparks of `level` and never grows; a
     * burst that finds it full drops the sparks that do not fit.
     */
    void init(FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const FireworksPacing& pacing = {});

    /**
     * @brief Advances the show by `dt_ms` for the print `print` reads and draws it into `frame`
     *
     * Replaces `dirty` with one box per rocket or burst whose pixels changed: what it erased
     * from the previous frame and what it drew in this one.
     *
     * With no print the show runs on its pacing. A running print shortens the longest launch
     * gap toward kFireworksPrintEndMaxGapMs and grows each shell from half the level's sparks to
     * all of them as it goes, with no periodic finale; a paused one launches nothing and its
     * clocks stand still. A finished print runs the finale, again each time its shells have
     * burnt out, until the print reading changes; a failed or cancelled one sends up one
     * small red willow, then nothing.
     */
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng, const ClockReading& print,
              std::vector<DirtyRect>& dirty);

    /// Runs at `level` (clamped to the ladder) from the next shell launch on.
    void request_level(size_t level);

    /// Shells launch only as kinds whose bit (1 << BurstKind) is set in `mask`; an empty mask
    /// means every kind.
    void set_kinds(uint8_t mask) {
        kinds_ = (mask & kAllKinds) != 0 ? static_cast<uint8_t>(mask & kAllKinds) : kAllKinds;
    }

    /// Level the show runs at now.
    size_t level() const {
        return level_;
    }

    /// Sparks the pool holds, fixed from init() to clear().
    size_t spark_capacity() const {
        return sparks_.size();
    }

    /// Sparks alive now.
    size_t sparks_in_use() const;

    /// Shells launched since init().
    uint32_t shells_launched() const {
        return launches_;
    }
    /// True while finale shells are still to launch.
    bool finale_running() const {
        return finale_left_ > 0;
    }

    /// Frees the spark pool and the sky layout.
    void clear();
    /// Bytes the spark pool and the sky layout hold.
    size_t heap_bytes() const;

    /// Background at (x, y) inside the frame: the hills, a star, or the sky gradient.
    Rgb sky_at(int32_t x, int32_t y) const;

  private:
    enum SparkFlag : uint8_t {
        SPARK_ALIVE = 1u << 0,
        SPARK_DRAWN = 1u << 1,     ///< drawn in the last frame, so the next frame erases it
        SPARK_DRAWN_BIG = 1u << 2, ///< drawn 2x2 in the last frame
        SPARK_SPLIT = 1u << 3,     ///< a crossette fragment, which never splits again
    };

    struct Spark {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f; ///< px per second
        float vy = 0.0f;
        uint16_t age_ms = 0;
        uint16_t life_ms = 0;
        Rgb color{};
        uint8_t flags = 0;
        int16_t drawn_x = 0; ///< head drawn in the last frame
        int16_t drawn_y = 0;
        int8_t drawn_dx = 0; ///< unit direction the trail runs in, times 100
        int8_t drawn_dy = 0;
        uint8_t burst = 0; ///< owning burst slot
    };
    // 900 sparks at level 0 keep the pool near 30 KB.
    static_assert(sizeof(Spark) <= 32, "a spark must stay within 32 bytes");

    struct Burst {
        bool active = false;
        BurstKind kind = BurstKind::PEONY;
        uint8_t trail = 0;
        uint16_t alive = 0;
        float drag = 0.0f;    ///< share of velocity lost per second
        float gravity = 0.0f; ///< px/s^2
    };

    struct Rocket {
        bool active = false;
        bool drawn = false;
        BurstKind kind = BurstKind::PEONY;
        uint8_t trail = 0;
        uint16_t sparks = 0;
        Rgb color{};
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        int16_t head_x = 0; ///< line drawn in the last frame
        int16_t head_y = 0;
        int16_t tail_x = 0;
        int16_t tail_y = 0;
    };

    /// How the print shapes the show.
    enum class Show : uint8_t { NO_PRINT, RUNNING, PAUSED, DONE, FAILED };

    /// A shell whose kind, colour and size are set rather than drawn.
    struct FixedShell {
        BurstKind kind;
        Rgb color;
        uint16_t sparks;
    };

    void follow_print(const ClockReading& print, std::minstd_rand& rng);
    void arm_finale(std::minstd_rand& rng);
    void advance(uint32_t step_ms, std::minstd_rand& rng);
    /// Launches a shell, `fixed` or drawn at random; false when every rocket slot is taken.
    bool launch(std::minstd_rand& rng, const FixedShell* fixed = nullptr);
    /// Sparks a drawn shell bursts into at the level `params`.
    uint16_t shell_sparks(const FireworksLevel& params) const;
    void burst(const Rocket& rocket, std::minstd_rand& rng);
    void split(Spark& spark, std::minstd_rand& rng);
    BurstKind pick_kind(std::minstd_rand& rng) const;
    Spark* take_spark();
    size_t in_flight() const;
    int32_t next_gap_ms(std::minstd_rand& rng) const;

    void draw_spark(PixelWriter& writer, Spark& spark, std::minstd_rand& rng, DirtyRect& box);
    void erase_spark(PixelWriter& writer, const Spark& spark, DirtyRect& box);
    void draw_rocket(PixelWriter& writer, Rocket& rocket, DirtyRect& box);
    void erase_rocket(PixelWriter& writer, const Rocket& rocket, DirtyRect& box);
    void light_point(PixelWriter& writer, int32_t x, int32_t y, Rgb color, DirtyRect& box);
    void erase_point(PixelWriter& writer, int32_t x, int32_t y, DirtyRect& box);

    /// Calls visit(x, y, brightness 0-255) for the head, the 2x2 block when drawn big, and the
    /// trail of `spark` as it was last drawn.
    template <typename Visit>
    void visit_spark_points(const Spark& spark, uint8_t trail, Visit&& visit) const;

    FireworksPacing pacing_{};
    size_t level_ = 0;
    size_t pending_level_ = 0;
    uint8_t kinds_ = kAllKinds;
    int32_t w_ = 0;
    int32_t h_ = 0;
    float scale_ = 1.0f;

    // The sky as layout, never as pixels: a colour per row, a hill top and at most one star per
    // column.
    std::vector<Rgb> sky_rows_;
    std::vector<int16_t> hill_top_;
    std::vector<int16_t> star_y_; ///< -1 where a column has no star
    std::vector<uint8_t> star_level_;

    std::vector<Spark> sparks_;
    size_t next_spark_ = 0;
    Burst bursts_[kMaxBursts]{};
    Rocket rockets_[kMaxBursts]{};

    int32_t launch_in_ms_ = 0;
    int32_t finale_in_ms_ = 0;
    uint8_t finale_left_ = 0;
    int32_t finale_gap_ms_ = 0;

    Show show_ = Show::NO_PRINT;
    float progress_ = 0.0f; ///< the running print's progress, 0 to 1
    bool fizzle_pending_ = false;
    uint32_t launches_ = 0;
};

} // namespace helix::ui
