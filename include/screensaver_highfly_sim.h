// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_heightfield.h"
#include "screensaver_stroke_digits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_highfly_sim.h
 * @brief High Fly: flying just above a sunlit cloud deck under a deep blue sky
 *
 * Back to front: a static sky with a glow and four soft shafts, a far billow band drifting
 * sideways so the skyline is never flat, the cloud deck (the shared column renderer over a
 * baked tileable heightfield) scrolling toward the camera, translucent full-width layers and
 * now and then a towering cumulus, each depth-tested against the deck, and about once a
 * minute or two a flight through a cloud. A thin clock sits at the cloud line.
 *
 * No LVGL, no clock: the saver passes the frame time, its random sequence, the clock reading
 * and the frame. Textures are baked in init() from the seed; a frame allocates nothing.
 */

namespace helix::ui {

struct HighFlyLevel {
    uint32_t period_ms;
    uint8_t scale;   ///< the scene renders at canvas / scale
    uint16_t slices; ///< deck depth slices
    uint8_t layers;  ///< translucent layers drawn, 0 to kHighflyLayerCount
    bool towers;
    bool still; ///< draw the scene once, then only the clock
};

/// Most expensive first: layers go before the deck coarsens, the coarse deck slows to a slow
/// drift before it stops, and the last rung stands still.
inline constexpr HighFlyLevel kHighflyLevels[] = {
    {kSaverFastPeriod, 2, 117, 3, true, false},
    {33, 2, 117, 3, true, false},
    {33, 2, 117, 0, true, false},
    {50, 4, 70, 0, false, false},
    {125, 4, 70, 0, false, false},
    {250, 2, 117, 3, true, true},
};
inline constexpr size_t kHighflyLevelCount = std::size(kHighflyLevels);

inline constexpr size_t kHighflyLayerCount = 3;
inline constexpr size_t kHighflyTowerVariants = 3;
/// Deck heightfield side, texels; tileable both ways.
inline constexpr int32_t kHighflyField = 256;
/// Deck scroll toward the camera, world units per second.
inline constexpr float kHighflySpeed = 10.0f;
/// Fly-throughs start this many seconds apart, uniformly.
inline constexpr float kHighflyFlyMinS = 60.0f;
inline constexpr float kHighflyFlyMaxS = 120.0f;
/// Fly-through envelope: ease in, hold, clear, seconds.
inline constexpr float kHighflyFlyInS = 1.0f;
inline constexpr float kHighflyFlyHoldS = 0.5f;
inline constexpr float kHighflyFlyOutS = 1.3f;
/// Interior colour's opacity at the depth of the fly-through.
inline constexpr float kHighflyWhiteoutMax = 0.62f;
/// Towers enter this far apart in travel time, uniformly.
inline constexpr float kHighflyTowerMinS = 13.0f;
inline constexpr float kHighflyTowerMaxS = 14.0f;

/// A premultiplied RGBA texture, row-major.
struct HighFlyTexture {
    int32_t w = 0;
    int32_t h = 0;
    std::vector<uint8_t> rgba; ///< premultiplied, 4 bytes per texel
    size_t heap_bytes() const;
};

/// Field texels per world unit: a world height is the stored height / kHighflyTexelsPerUnit.
inline constexpr float kHighflyTexelsPerUnit = 2.0f;

/// The baked deck: height (texel units) and shade (0-1) per texel, kHighflyField^2 row-major.
struct HighFlyField {
    std::vector<float> height;
    std::vector<float> shade;
};

/// Builds the deck at kHighflyField^2 from `rng`: puffs at three scales, noise, a talus pass
/// that limits each texel to kHighflyTalus above its lowest neighbour's limit, a 1-texel blur,
/// then Lambert + ambient occlusion + noise shading.
void build_highfly_field(std::minstd_rand& rng, HighFlyField& out);
/// Slope limit of the talus pass, height units per texel.
inline constexpr float kHighflyTalus = 2.0f;

/// Fly-through amount, 0 to 1, `since_start_s` after one began; 0 before and after.
float highfly_whiteout(float since_start_s);

/// How much of a billboard at depth `layer_z` shows over a deck pixel that reached half
/// coverage at `deck_z`: clamp((deck_z - layer_z) / (0.25 layer_z) + 0.5, 0, 1). 1 where no
/// deck is there (deck_z infinite).
float highfly_layer_visibility(float deck_z, float layer_z);

/// Most the fly-through interior and its wisps together may cover the scene over the clock,
/// so the pale numerals stay readable.
inline constexpr float kHighflyClockCoverMax = 0.70f;

/// `scene` seen from inside the fly-through cloud `amt` (0 to 1) of the way in: eased toward
/// the blue-grey interior, never more than kHighflyWhiteoutMax of the way, then lit toward
/// white by a wisp of strength `wisp`, held so the two together cover at most `most` of the
/// scene (where the interior alone already covers more, the wisp adds nothing).
Rgb highfly_inside(Rgb scene, float amt, float wisp = 0.0f, float most = 1.0f);

/// True when the big numerals show the print's time left rather than the time of day:
/// COUNTDOWN, and PAUSED once an estimate has been seen.
constexpr bool highfly_counts_down(const ClockReading& r) {
    return r.mode == ClockMode::COUNTDOWN || (r.mode == ClockMode::PAUSED && r.has_countdown);
}

/// The six numeral slots for `r`: {H, H, M, M} big and {S, S} small. The time of day for CLOCK,
/// PROGRESS and ended prints; for COUNTDOWN, and PAUSED with an estimate, the time left with
/// hours clamped to 99 and a leading zero hour blank (-1).
std::array<int, 6> highfly_numerals(const ClockReading& r);

/// The sky's colours for one moment of a print: the sky's top, its bottom and the sun's glow,
/// then the deck's sunlit tops, its fog, its shadows and its midtones.
struct HighflyPalette {
    Rgb top, bottom, glow, lit, fog, shadow, midtone;

    bool operator==(const HighflyPalette& o) const {
        return top == o.top && bottom == o.bottom && glow == o.glow && lit == o.lit &&
               fog == o.fog && shadow == o.shadow && midtone == o.midtone;
    }
};
/// A clear midday: the sky with no print.
inline constexpr HighflyPalette kHighflyDay{{40, 128, 180},  {12, 78, 124},  {215, 242, 252},
                                            {252, 254, 255}, {30, 110, 162}, {45, 130, 182},
                                            {150, 212, 235}};
/// Golden hour, which a print reaches at 100%: amber sky and glow over warm-lit tops.
inline constexpr HighflyPalette kHighflyGolden{{196, 126, 64},  {92, 66, 84},   {255, 222, 150},
                                               {255, 238, 204}, {128, 96, 100}, {128, 102, 126},
                                               {236, 186, 146}};
/// Sunset, for a finished print: violet above, a deep orange sun.
inline constexpr HighflyPalette kHighflySunset{{70, 38, 110},   {150, 58, 74}, {255, 116, 40},
                                               {255, 196, 150}, {104, 54, 96}, {96, 58, 112},
                                               {222, 132, 120}};
/// Overcast, for a failed or cancelled print: blue-grey, the sun hidden.
inline constexpr HighflyPalette kHighflyOvercast{{92, 104, 116},  {58, 66, 78}, {164, 172, 180},
                                                 {196, 200, 206}, {78, 86, 98}, {98, 106, 118},
                                                 {148, 154, 164}};
/// A print warms the sky in this many steps, so it rebakes at most this often per print.
inline constexpr int kHighflyPaletteSteps = 10;
/// kHighflyDay with no print; blended toward kHighflyGolden with progress, in
/// kHighflyPaletteSteps steps; kHighflySunset for DONE, kHighflyOvercast for FAILED and
/// CANCELLED.
HighflyPalette highfly_palette(const ClockReading& r);
/// Forward speed while a print is paused, against 1 otherwise.
inline constexpr float kHighflyPausedSpeed = 0.25f;
/// How far the paused view sways to either side, field texels, and its rate, radians a second.
inline constexpr float kHighflySwayTexels = 24.0f;
inline constexpr float kHighflySwayRate = 0.15f;

/// Where the clock sits on a w x h canvas, moved by the drift (dx, dy).
struct HighFlyClockLayout {
    int32_t digit_unit = 0;            ///< big numerals' glyph unit
    int32_t second_unit = 0;           ///< seconds' glyph unit
    std::array<DirtyRect, 5> digits{}; ///< H H : M M, the hours with a leading zero
    std::array<DirtyRect, 2> seconds{};
    int32_t halo = 0; ///< soft shadow round the numerals, px either side
    int32_t second_halo = 0;
    int32_t text_halo = 0; ///< soft shadow round the date block and AM/PM, px either side
    DirtyRect meridiem;    ///< under the seconds
    DirtyRect date_line;   ///< left block, first line
    DirtyRect day_line;    ///< left block, second line
    DirtyRect bounds;
};
HighFlyClockLayout layout_highfly_clock(int32_t w, int32_t h, int32_t dx, int32_t dy);

class HighFlySim {
  public:
    friend class HighFlySimTestAccess;
    static constexpr const auto& kLevels = kHighflyLevels;

    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const ClockReading& reading);
    /// Advances by `dt_ms` and draws the cloud band (the whole frame during a fly-through)
    /// and the numerals into `frame`, replacing `dirty` with the boxes written.
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
              const ClockReading& reading, std::vector<DirtyRect>& dirty);
    void request_level(size_t level);
    size_t level() const {
        return level_;
    }

    const HeightfieldRenderer& renderer() const {
        return renderer_;
    }
    const HighFlyClockLayout& clock_layout() const {
        return clock_;
    }
    /// Seconds of flight since init() (run time, slowed while a print is paused), and since the
    /// current fly-through's whiteout began (negative when none runs; the cloud it flies into
    /// shows for a while before, see next_fly_s()).
    double elapsed_s() const {
        return t_;
    }
    float fly_since_s() const;
    /// Seconds until the next fly-through and the next tower.
    float next_fly_s() const {
        return next_fly_s_;
    }
    float next_tower_s() const {
        return next_tower_s_;
    }
    /// Towers on screen now.
    size_t towers_visible() const;
    /// Top internal row of the band of rows the last frame repainted whole: the far billow
    /// band and the deck. Above it only the clock, towers and a fly-through repaint the sky.
    int32_t band_top() const {
        return band_top_;
    }
    /// Internal frame scale at the current rung.
    int32_t scale() const {
        return scale_;
    }
    /// Shape (0 to kHighflyTowerVariants - 1) of tower slot `i`, or -1 when the slot is empty.
    int tower_variant(size_t i) const;
    /// Internal-frame box tower slot `i` covers, empty when the slot is empty or off screen.
    DirtyRect tower_box(size_t i) const;
    /// Seconds into the run each tower so far entered.
    const std::vector<double>& tower_entries() const {
        return tower_entries_;
    }
    /// Seconds into the run each fly-through so far began.
    const std::vector<double>& fly_starts() const {
        return fly_starts_;
    }
    const HighFlyTexture& tower_texture(size_t i) const {
        return towers_tex_[i];
    }
    size_t heap_bytes() const;
    void clear();

  private:
    struct Tower {
        float z = 0.0f;
        double entered_s = 0.0; ///< run time it entered at kTowerZEnter
        float x = 0.0f;         ///< world x of its centre when it entered
        float drift = 0.0f;     ///< wind, world units per second sideways
        float size = 1.0f;
        uint8_t variant = 0;
        bool live = false;
    };
    /// A billboard to composite: a texture at depth z, its left world x and base height.
    struct Billboard {
        const HighFlyTexture* tex = nullptr;
        float z = 0.0f;
        float xw0 = 0.0f;
        float hb = 0.0f;
        float q = 1.0f;
        bool wrap = false;
        float alpha = 1.0f;
        float fogmul = 1.0f;
        bool depth_test = true;
    };
    class Scene;

    /// Sizes the renderer and internal buffers for level_, and with `bake` builds the static
    /// sky and the fly-through's polar table when the internal size changed.
    void apply_view(bool bake = true);
    /// The baked deck, textures, wisps and the tower and fly-through history. Draws from `rng`.
    void build_scene(std::minstd_rand& rng);
    /// Frees all of the scene but the composed frame and the numerals, once the still rung has
    /// painted it: from then on a frame only redraws the clock over that frame.
    void release_scene();
    void build_sky();
    /// Per deck slice: 256 shades through the palette's near white and fog.
    void bake_slice_lut();
    /// Follows the print over `dt_s` seconds: forward speed and sway ease toward paused or not,
    /// the step into DONE starts a fly-through, and the palette moves to highfly_palette()
    /// (DONE's once a fly-through's whiteout covers the frame), asking for a repaint.
    void follow_print(const ClockReading& reading, float dt_s);
    /// World units the view sits sideways of its centre line.
    float sway() const;
    void build_polar();
    void update_schedule(std::minstd_rand& rng);
    /// Starts a tower in `slot` that entered at run time `entered_s`.
    void spawn_tower(Tower& slot, double entered_s, std::minstd_rand& rng);
    /// The billboard tower `t` draws as.
    Billboard tower_billboard(const Tower& t) const;
    /// Composes the internal frame: rows from `from` down, plus the boxes in `boxes_`.
    void compose(int32_t from);
    void draw_billboard(const Billboard& b);
    /// Internal box a billboard covers, empty when off screen.
    DirtyRect billboard_box(const Billboard& b) const;
    /// Where a billboard lands at the internal size: texel scale across and down, top row and
    /// left column.
    struct BillboardScreen {
        float sx = 0.0f;
        float sy = 0.0f;
        float top = 0.0f;
        float xl = 0.0f;
    };
    BillboardScreen billboard_screen(const Billboard& b) const;
    size_t collect_billboards(Billboard* out, size_t cap) const;
    /// The numerals, drawn in two groups so the seconds ticking redraw only their own.
    enum class NumeralGroup : uint8_t { HOURS_MINUTES, SECONDS };
    /// One group's numerals and shadows over the scene, baked once per digit change: per pixel
    /// of `box` (the canvas box at no drift, halos included), what of the scene still shows
    /// (top byte, 0 to 255) and their colour over black (low 24 bits, R G B).
    struct NumeralLayer {
        DirtyRect box;
        std::vector<uint32_t> over;
        std::array<int, 4> digits{-1, -1, -1, -1}; ///< what `over` shows, -1 before a bake
    };
    class LayerWriter;
    /// Draws group `g` of `reading` at layout `l` into `writer`.
    void draw_numeral_group(LayerWriter& writer, const ClockReading& reading, NumeralGroup g,
                            const HighFlyClockLayout& l) const;
    /// Sizes the two layers for the current clock size.
    void size_numerals();
    /// Bakes `layer` for group `g` showing `digits` from `reading`.
    void bake_numerals(NumeralLayer& layer, NumeralGroup g, const ClockReading& reading,
                       const std::array<int, 4>& digits);
    /// Composes the numerals over `frame` where the clock is now, baking a group first when
    /// its digits changed.
    void draw_numerals(FrameTarget& frame, const ClockReading& reading);
    void update_clock_layout();
    /// The clock bounds as an internal box.
    DirtyRect clock_box_internal() const;
    /// Paints everything once and draws the numerals.
    void paint_all(FrameTarget& frame, const ClockReading& reading);

    size_t level_ = 0;
    int32_t w_ = 0;
    int32_t h_ = 0;
    int32_t scale_ = 2;
    int32_t iw_ = 0;
    int32_t ih_ = 0;
    float horizon_ = 0.0f;
    float focal_x_ = 1.0f;
    float focal_y_ = 1.0f;
    double t_ = 0.0;           ///< seconds since init(); phases are reduced from it in double
    float deck_texels_ = 0.0f; ///< the deck's scroll, reduced to one field tile
    int32_t baked_w_ = 0;      ///< internal size the sky and polar table were built for
    int32_t baked_h_ = 0;
    ClockDrift drift_;
    double fly_start_s_ = -1.0; ///< when the scheduled fly-through's whiteout begins
    float next_fly_s_ = 0.0f;
    float next_tower_s_ = 0.0f;
    double next_tower_at_ = 0.0;
    int32_t band_top_ = 0;
    int32_t band_tex_top_ = 0; ///< the far band's top row, fixed for the view
    uint8_t next_variant_ = 0;
    bool next_left_ = true;
    bool painted_ = false;
    bool last_full_ = false;
    bool scene_released_ = false;
    std::vector<int16_t> deck_height_; ///< the baked deck, texels x 128, kHighflyField^2
    std::vector<uint8_t> deck_shade_;  ///< and its shade, 0 to 255
    HeightfieldRenderer renderer_;
    std::vector<Rgb> sky_;       ///< internal row-major, static
    std::vector<Rgb> frame_;     ///< internal row-major
    std::vector<Rgb> slice_lut_; ///< per slice: 256 deck shades through near-white and fog
    HighFlyTexture band_;
    std::array<HighFlyTexture, kHighflyLayerCount> layers_{};
    std::array<HighFlyTexture, kHighflyTowerVariants> towers_tex_{};
    std::vector<float> wisp_;         ///< polar wisp noise, 64 log-radius rows x 128 angles
    std::vector<uint16_t> polar_;     ///< per internal pixel: log-radius, angle, radial weight
    std::vector<int32_t> col_u0_;     ///< billboard sampling, per internal column
    std::vector<uint16_t> col_fu_;    ///< and its weight toward the next texel, 0 to 256
    std::vector<uint16_t> slice_vis_; ///< per deck slice: how much of a billboard shows, 0 to 256
    std::array<Tower, 2> towers_{};
    std::vector<double> tower_entries_;
    std::vector<double> fly_starts_;
    std::vector<DirtyRect> boxes_;      ///< internal boxes above the band repainted this frame
    std::vector<DirtyRect> last_boxes_; ///< and last frame
    HighFlyClockLayout clock_;
    HighflyPalette palette_ = kHighflyDay; ///< what sky_ and slice_lut_ were baked with
    float speed_ = 1.0f;                   ///< forward speed, 1 or easing toward paused
    float sway_amp_ = 0.0f;                ///< 0 to 1: how much of the paused sway shows
    ClockMode last_mode_ = ClockMode::CLOCK;
    std::minstd_rand scene_rng_; ///< the rng as the scene was last built from, to build it again
    StrokeDigitSet digits_;
    StrokeDigitSet second_digits_;
    StrokeDigitSet digit_halo_; ///< digits_ widened and blurred, drawn under them
    StrokeDigitSet second_halo_;
    std::array<NumeralLayer, 2> numerals_{}; ///< by NumeralGroup
    ClockReading shown_;
};

} // namespace helix::ui
