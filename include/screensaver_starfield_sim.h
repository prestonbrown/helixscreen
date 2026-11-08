// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

namespace helix::ui {

class PixelWriter;

/// Stars in the starfield's population.
inline constexpr int kStarfieldNumStars = 150;

/// One level of the starfield ladder.
struct StarfieldLevel {
    uint32_t period_ms; ///< frame period in ms
    int stars;          ///< stars flown, the first of the population
};

/**
 * @brief The ladder, most expensive level first. A requested level applies at once.
 *
 * Past full rate the rungs thin the star population instead of slowing further, because a
 * slower frame that still flies every star saves little of a scattered sky's redraw.
 */
inline constexpr StarfieldLevel kStarfieldLevels[] = {
    {kSaverFastPeriod, kStarfieldNumStars},
    {33, 96},
    {33, 64},
    {33, 32},
};

/// The planet's radius, as a share of the frame's shorter side, at the start and the end of a
/// print.
inline constexpr float kStarfieldPlanetStartShare = 0.02f;
inline constexpr float kStarfieldPlanetEndShare = 0.22f;
/// How long a finished print takes to arrive at the planet, which then fills the view.
inline constexpr uint32_t kStarfieldArriveMs = 4000;
/// How long the planet of a failed or cancelled print takes to shrink and drift off the view.
inline constexpr uint32_t kStarfieldDepartMs = 6000;
/// Star speed while a print is paused, as a share of the flight's own.
inline constexpr float kStarfieldPausedSpeed = 0.15f;

/// The planet as the frame shows it: centre and radius in px, radius 0 when none is drawn.
struct StarfieldPlanet {
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t r = 0;

    bool operator==(const StarfieldPlanet& o) const {
        return cx == o.cx && cy == o.cy && r == o.r;
    }
    bool operator!=(const StarfieldPlanet& o) const {
        return !(*this == o);
    }
    /// True when (x, y) lies on the disc.
    bool covers(int32_t x, int32_t y) const {
        const int32_t dx = x - cx;
        const int32_t dy = y - cy;
        return r > 0 && dx * dx + dy * dy <= r * r;
    }
};

/// Stars flown at `level`, clamped to the ladder.
constexpr int starfield_star_count(size_t level) {
    const size_t last = std::size(kStarfieldLevels) - 1;
    return kStarfieldLevels[level < last ? level : last].stars;
}

/**
 * @brief The starfield's stars, and how one frame of them is drawn
 *
 * No LVGL, no clock, no shared random sequence and no allocation after init(), so it can
 * be stepped and checked without a display.
 *
 * A print shows as a planet at the centre that grows as the print goes, the flight
 * approaching it: it fills the view when the print finishes, and shrinks and drifts away when
 * it fails or is cancelled. With no print there is no planet.
 */
class StarfieldSim {
  public:
    static constexpr int kNumStars = kStarfieldNumStars;
    static constexpr const auto& kLevels = kStarfieldLevels;

    struct Star {
        float x;        // normalized position (-1..1)
        float y;        // normalized position (-1..1)
        float z;        // depth (0..1, 1=far, approaches 0)
        float speed;    // z decrement per 33 ms
        uint8_t tint_r; // color tint (assigned at birth, visible when close)
        uint8_t tint_g;
        uint8_t tint_b;
        // Where the star was drawn in the frame, for the next step's erase
        int16_t prev_sx;
        int16_t prev_sy;
        uint8_t prev_size; // 0 = not drawn
    };

    /// Places kNumStars stars for `frame`, drawing from `rng`, and flies the count of `level`.
    /// The planet starts where `reading` puts it, an ended print's already at its end.
    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const ClockReading& reading);

    /// Flies the star count of `level` from the next step on.
    void request_level(size_t level) {
        set_active_count(starfield_star_count(level));
    }

    /// Frees the population, capacity included.
    void clear();
    /// Bytes the population holds.
    size_t heap_bytes() const;

    /**
     * @brief Stars moved and drawn per frame: the first `count` of the population
     *
     * `count` is clamped to [1, kNumStars]. The population keeps its allocation, and a
     * star past the count is erased on the next step and then parked, so the ladder that
     * changes it reallocates nothing per frame.
     */
    void set_active_count(int count);

    /// The count set by set_active_count(), kNumStars until one is set.
    int active_count() const {
        return active_count_;
    }

    /**
     * @brief Draws the next frame into `target`
     *
     * Moves the planet toward where `reading` puts it and redraws it when its centre or
     * whole-pixel radius changes; erases the stars where the previous step drew them, moves
     * each active star by `dt_ms` of flight and draws it, recycling a star that reaches the
     * camera or leaves the frame. A star neither draws nor erases a pixel of the planet.
     *
     * @param dirty Replaced with one box over the planet's old and new discs when it was
     * redrawn, then one box per star that erased or drew anything: the union of that star's
     * erase box and its draw box, clipped to the frame
     */
    void step(uint32_t dt_ms, FrameTarget& target, std::minstd_rand& rng,
              const ClockReading& reading, std::vector<DirtyRect>& dirty);

    std::vector<Star>& stars() {
        return stars_;
    }

    const std::vector<Star>& stars() const {
        return stars_;
    }

    /// The planet as the last step drew it.
    const StarfieldPlanet& planet() const {
        return planet_;
    }

    /// Share of the flight's speed the stars fly at now.
    float speed_scale() const {
        return speed_scale_;
    }

  private:
    void recycle(Star& star, std::minstd_rand& rng);
    /// Moves the planet and the star speed toward where `reading` puts them after `dt_ms`, or
    /// there at once when `snap` is set.
    void follow_print(const ClockReading& reading, uint32_t dt_ms, bool snap);
    /// Redraws the planet when the disc follow_print() placed differs from the drawn one.
    void draw_planet(PixelWriter& writer, const FrameTarget& target, std::vector<DirtyRect>& dirty);

    std::vector<Star> stars_;
    /// Per-star dirty box of the step in flight, sized by init().
    std::vector<DirtyRect> star_dirty_;
    int active_count_ = kNumStars;
    float cx_ = 0;    // frame center X
    float cy_ = 0;    // frame center Y
    float focal_ = 0; // projection focal length

    ClockMode mode_ = ClockMode::CLOCK;
    float planet_r_ = 0;  ///< radius follow_print() placed, px
    float planet_dx_ = 0; ///< centre's offset from the frame centre, px
    float planet_dy_ = 0;
    /// Where the planet was when the print ended, and how long ago that was.
    float end_r_ = 0;
    float end_dx_ = 0;
    float end_dy_ = 0;
    uint32_t end_ms_ = 0;
    float speed_scale_ = 1.0f;
    StarfieldPlanet planet_; ///< as drawn in the frame
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
