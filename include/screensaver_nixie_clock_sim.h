// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

/**
 * @file screensaver_nixie_clock_sim.h
 * @brief A clock of imitation nixie tubes that counts a running print down, stepped one frame
 * at a time
 *
 * The tubes show what the flip clock's cards show (flip_face()): the same readings, the same
 * small row while a print runs; only the look differs. No LVGL, no clock: the saver passes the
 * frame time and the face, so a run replays exactly and runs in tests without a display. The
 * AM/PM marker is text the saver draws on the first tube's socket (meridiem_box()).
 *
 * Cost: every part of a tube that never changes (glass, anode mesh, the dark stack of unlit
 * numerals, socket) is drawn once per tube size into a cached layer, and every numeral's glow
 * once into a byte map. A digit change composes the cached layer with one or two glow maps over
 * the numeral's box for the length of the cross-fade; between changes a frame writes nothing.
 */

namespace helix::ui {

/// One level of the nixie clock ladder.
struct NixieClockLevel {
    uint32_t period_ms; ///< frame period in ms
    bool fade;          ///< cross-fade a changed numeral and cycle the cathodes, or swap at once
};

/// The ladder, most expensive level first. Nothing is drawn between changes at any level.
inline constexpr NixieClockLevel kNixieClockLevels[] = {
    {kSaverFastPeriod, true},
    {33, true},
    {250, false},
};

inline constexpr size_t kNixieClockLevelCount = std::size(kNixieClockLevels);

/// How long a change of numeral takes: the new cathode lights almost at once, and the old one's
/// afterglow dies over the whole of it (nixie_fade_amounts()).
inline constexpr uint32_t kNixieFadeMs = 120;
/// Share of the fade by which the new numeral reaches full brightness.
inline constexpr float kNixieWarmShare = 0.3f;

/// How brightly the old and the new numeral glow `p` (0 to 1) into a fade, each 0 to 1.
struct NixieFadeAmounts {
    float from;
    float to;
};
NixieFadeAmounts nixie_fade_amounts(float p);
/// How often the tubes run through every numeral to keep the cathodes healthy, while the time of
/// day shows.
inline constexpr uint32_t kNixieCycleEveryMs = 10 * 60 * 1000;
/// How long each numeral stays lit during that run; ten of them make one cycle.
inline constexpr uint32_t kNixieCycleStepMs = 70;
inline constexpr uint32_t kNixieCycleMs = 10 * kNixieCycleStepMs;

/// The tubes' proportions in glyph units: thin wire numerals; above them the top mica disc, the
/// glass's shoulders and crown and the sealed tip; below them the bottom disc, the stem and the
/// socket. The gap between groups holds the narrow colon tube.
inline constexpr CardGeometry kNixieTubeGeometry{
    0.07f, // stroke
    0.36f, // pad_x
    0.62f, // pad_top
    0.72f, // pad_bottom
    0.16f, // card_gap
    0.74f, // group_gap
    0.3f,  // row_gap
    0.22f, // sub_gap
    1.4f,  // sub_spacer
    0.34f, // sub_scale
};

/// What the tubes show: the flip clock's reading of ClockState, card for tube.
using NixieClockState = FlipClockState;

/**
 * @brief The tubes' pixels at one size, built once and composed per frame
 *
 * `base` is the unlit tube, `w` x `h`, in Rgb. Each glow map covers a numeral's box widened by
 * `halo` px on every side, `glow_w` x `glow_h`, placed with its top left at (`glow_x`,
 * `glow_y`) in the tube; a byte is how brightly the wire glows there, which the ink's ramp turns
 * into a colour added over the base.
 */
struct NixieTubeArt {
    int32_t unit = 0;
    int32_t w = 0;
    int32_t h = 0;
    int32_t halo = 0;
    int32_t glow_x = 0;
    int32_t glow_y = 0;
    int32_t glow_w = 0;
    int32_t glow_h = 0;
    int32_t inset = 0;    ///< the glass and socket's inset from the card's sides
    int32_t socket_y = 0; ///< the socket's first row
    /// The sealed glass tip on top of the crown, in the tube's pixels.
    DirtyRect tip;
    std::vector<Rgb> base;
    /// One map per character of kStrokeGlyphChars; digits are built with the art, the rest on
    /// first use.
    std::array<std::vector<uint8_t>, kStrokeGlyphCount> glow{};

    /// Builds the tube for cards of `w` x `h` px in a row of `row`'s metrics, and the ten
    /// digits' glow.
    void build(const FlipRowMetrics& row, int32_t w, int32_t h);
    /// Builds `c`'s glow once; a character with no glyph, or NUL, is ignored.
    void ensure(char c);
    /// `c`'s glow map, or nullptr when it is not built.
    const uint8_t* glow_of(char c) const;
    size_t heap_bytes() const;
    void clear();
};

/**
 * @brief The colon tube between two groups at one size: a narrow glass tube with two lit neon
 * bulbs, its tip on top and its socket level with the digit tubes'
 *
 * `base` is the tube unlit and `glow` the bulbs' glow, both `w` x `h`; the colon never changes,
 * so the ink's ramp adds the glow when the layout is drawn.
 */
struct NixieColonArt {
    int32_t unit = 0;
    int32_t w = 0;
    int32_t h = 0;
    DirtyRect tip;
    std::vector<Rgb> base;
    std::vector<uint8_t> glow;

    /// Builds the colon for a gap of `w` x `h` px beside tubes of `row`'s metrics whose socket
    /// starts at `socket_y`.
    void build(const FlipRowMetrics& row, int32_t w, int32_t h, int32_t socket_y);
    size_t heap_bytes() const;
    void clear();
};

/**
 * @brief Draws the tubes into a frame, redrawing only the numerals that change
 *
 * A tube whose character changes cross-fades over kNixieFadeMs at a fading level, or swaps at
 * once otherwise; tubes that change together fade together, and the rest are not touched. Every
 * kNixieCycleEveryMs of the time of day the main tubes run through their ten numerals and fade
 * back. A changed shape, ink or drift step repaints the whole layout.
 */
class NixieClockSim {
  public:
    static constexpr const auto& kLevels = kNixieClockLevels;

    /// Lays out `face` for `frame`, paints the screen and draws the tubes.
    void init(FrameTarget& frame, size_t level, const FlipFace& face);

    /// Advances by `dt_ms` towards showing `face` and draws into `frame`, replacing `dirty` with
    /// one box per numeral redrawn, or one box over the old and new layout after a repaint.
    void step(uint32_t dt_ms, FrameTarget& frame, const FlipFace& face,
              std::vector<DirtyRect>& dirty);

    /// Draws tube `i` again as it stands and adds its box to `dirty`, for text drawn over it
    /// that has to go.
    void redraw_tube(FrameTarget& frame, size_t i, std::vector<DirtyRect>& dirty) const;

    /// Runs at `level` (clamped) from now on; a fade or cathode cycle in progress finishes at
    /// once when the new level does not fade.
    void request_level(size_t level);

    size_t level() const {
        return level_;
    }
    const FlipLayout& layout() const {
        return layout_;
    }
    /// Where the AM/PM marker goes: the first tube's socket.
    DirtyRect meridiem_box() const;
    /// The box of tube `i`'s numeral and its halo, which a fade redraws.
    DirtyRect glow_box(size_t i) const;
    /// The box of tube `i`'s sealed glass tip, on top of its crown.
    DirtyRect tip_box(size_t i) const;
    /// The box of colon tube `c`'s tip.
    DirtyRect colon_tip_box(size_t c) const;
    /// True while any tube is mid-fade.
    bool fading() const;
    /// Share of the fade done for tube `i`, 1 when it is still.
    float fade_progress(size_t i) const;
    /// True while the cathodes cycle.
    bool cycling() const {
        return cycle_ms_ < kNixieCycleMs;
    }
    /// The character tube `i` glows with now (the newer one mid-fade).
    char lit(size_t i) const;

    /// Frees the cached art.
    void clear();
    /// Bytes the cached art holds.
    size_t heap_bytes() const;

    static constexpr Rgb kBackground{0, 0, 0};
    /// The neon glow of a running clock.
    static constexpr Rgb kNeon{255, 106, 18};

  private:
    struct TubeAnim {
        char from = '\0';
        char to = '\0';
        uint32_t elapsed_ms = kNixieFadeMs;
    };

    void relayout(FrameTarget& frame, const FlipFace& face, std::vector<DirtyRect>& dirty);
    void build_art(const FlipFace& face);
    void build_ramp();
    void draw_tube(FrameTarget& frame, size_t i) const;
    void draw_glow(FrameTarget& frame, size_t i) const;
    void draw_colons(FrameTarget& frame) const;
    const NixieTubeArt& art_of(size_t i) const {
        return i < layout_.card_count ? art_ : sub_art_;
    }
    size_t tube_count() const {
        return static_cast<size_t>(layout_.card_count + layout_.sub_count);
    }
    /// Moves the cathode cycle on by `dt_ms`; true when the numerals it shows changed.
    bool step_cycle(uint32_t dt_ms, const FlipFace& face);

    size_t level_ = 0;
    int32_t frame_w_ = 0;
    int32_t frame_h_ = 0;
    ClockDrift drift_;
    FlipShape drift_shape_;
    FlipShape drift_sub_shape_;
    Rgb ink_ = kNeon;
    /// Glow byte to added colour for the ink, 0 black.
    std::array<Rgb, 256> ramp_{};
    FlipLayout layout_;
    std::array<TubeAnim, kFlipMaxCards + kFlipMaxSubCards> anims_{};
    /// Time of day shown since the last cathode cycle, and how far into a cycle the tubes are
    /// (kNixieCycleMs when none runs).
    uint32_t since_cycle_ms_ = 0;
    uint32_t cycle_ms_ = kNixieCycleMs;
    NixieTubeArt art_;
    NixieTubeArt sub_art_;
    NixieColonArt colon_art_;
    NixieColonArt sub_colon_art_;
};

} // namespace helix::ui
