// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "ui_widget_ref.h"

#include "screensaver_base.h"
#include "screensaver_motion.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <lvgl.h>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace helix::screensaver_bounce {

/// Room the sprite must leave on each axis for the bounce to read as a bounce.
inline constexpr int kMinRangePx = 48;

/// Sprite edge as a fraction of the screen's narrow axis.
inline constexpr float kSpriteFraction = 0.28f;

/**
 * @brief Longest sprite edge for a screen, or 0 when the screen cannot host a bounce
 *
 * Sized off the narrow axis, then held below what would leave less than
 * kMinRangePx of travel. 480x320 yields a 90px box over a 390x230 range.
 */
inline int sprite_size_for(int screen_w, int screen_h) {
    const int narrow = (screen_w < screen_h) ? screen_w : screen_h;
    int target = static_cast<int>(std::lround(kSpriteFraction * static_cast<float>(narrow)));
    if (target > 320) {
        target = 320;
    }
    const int ceiling = narrow - kMinRangePx;
    if (target > ceiling) {
        target = ceiling;
    }
    return (target < 48) ? 0 : target;
}

/**
 * @brief Fit a source image's aspect ratio inside a square box of @p box pixels
 *
 * The walls are the edges of the sprite's own rectangle, so that rectangle has
 * to be the shape of the artwork. A square box around a tall printer render
 * would hold a column of empty pixels on each side and the printer would turn
 * before reaching the screen edge.
 */
inline void fit_sprite(int box, int32_t src_w, int32_t src_h, int& out_w, int& out_h) {
    if (src_w <= 0 || src_h <= 0) {
        out_w = box;
        out_h = box;
        return;
    }
    const double scale =
        static_cast<double>(box) / static_cast<double>((src_w >= src_h) ? src_w : src_h);
    out_w = static_cast<int>(std::lround(static_cast<double>(src_w) * scale));
    out_h = static_cast<int>(std::lround(static_cast<double>(src_h) * scale));
    if (out_w < 1) {
        out_w = 1;
    }
    if (out_h < 1) {
        out_h = 1;
    }
}

/**
 * @brief Rows of an `src_h`-row sprite still grey at `pct` percent printed
 *
 * The picture fills with colour from the bottom: 0% is all grey, 100% none. `pct` is clamped
 * to 0 to 100.
 */
inline int32_t grey_rows_for(int32_t src_h, int32_t pct) {
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    return src_h * (100 - pct) / 100;
}

} // namespace helix::screensaver_bounce

namespace helix {

/**
 * @brief Bouncing Printer screensaver
 *
 * The printer this display is attached to drifts across a black field and
 * reflects off the edges, changing tint on every wall. Landing a true corner
 * earns a celebration: the backdrop flashes and, above the ladder's lowest
 * level on a board that animates, a confetti burst.
 *
 * While a print runs the picture starts grey and fills with colour from the bottom as the
 * print goes, and the wall tints pause. A finished print throws confetti on every bounce,
 * one burst at a time (a green flash on the cheapest rung); a failed or cancelled one tints
 * the sprite red.
 *
 * Position is computed from elapsed time rather than integrated per frame,
 * matching the rest of the subsystem, so a level that changes the frame period
 * changes nothing on screen but the frame rate.
 */
class BouncingPrinterScreensaver : public helix::ui::SaverBase {
  public:
    BouncingPrinterScreensaver() = default;
    BouncingPrinterScreensaver(const BouncingPrinterScreensaver&) = delete;
    BouncingPrinterScreensaver& operator=(const BouncingPrinterScreensaver&) = delete;

    ScreensaverType type() const override {
        return ScreensaverType::BOUNCING_PRINTER;
    }

    /// The bouncing image, or nullptr while stopped.
    lv_obj_t* sprite() const {
        return img_.get();
    }
    /// The art in its own colours, or nullptr while stopped.
    const lv_draw_buf_t* sprite_art() const {
        return decoded_;
    }
    /// The copy of the art a print fills with colour, or nullptr while stopped or when the
    /// art's format cannot be rewritten.
    const lv_draw_buf_t* fill_art() const {
        return fill_;
    }
    /// The confetti burst still alive, or nullptr.
    lv_obj_t* confetti() const {
        return confetti_.get();
    }

  protected:
    /// LVGL path of the image that bounces on a screen `screen_w` pixels wide
    virtual std::string sprite_path(int screen_w) const;
    /// What bounces, for log lines
    virtual const char* subject_name() const {
        return "printer";
    }
    /// Whether, with no print, walls change the tint and a corner flashes and throws confetti
    virtual bool decorated() const {
        return true;
    }

    /// The sprite is an LVGL image on the overlay; there is no canvas.
    std::optional<lv_color_format_t> canvas_format() const override {
        return std::nullopt;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return std::size(kLevels);
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return helix::ui::ladder_period(kLevels, level);
    }

  private:
    friend class BounceTestAccess;

    struct Level {
        uint32_t period_ms; ///< frame period in ms
        bool confetti;      ///< a corner throws confetti as well as flashing
    };
    /// The ladder, most expensive level first.
    static constexpr Level kLevels[] = {
        {helix::ui::kSaverFastPeriod, true},
        {33, true},
        {33, false},
    };

    /// Decode the active printer image into a persistent draw buffer
    bool decode_sprite();
    void free_sprite();
    /// Pick a velocity whose path is not a short repeating loop
    void seed_motion();
    /// Size the sprite to `box` and derive travel ranges and speed from the current screen
    void apply_geometry(int box);
    /// Recompute travel ranges after a resolution change without teleporting
    void rebase(int screen_w, int screen_h);
    /// Sets the recolor the print state and, with no print, the wall tint call for.
    void apply_tint();
    /// Shows the art for the print state, rewriting the grey rows of fill_ when the line moved.
    void apply_fill();
    /// A celebration: the backdrop flashes `flash`, and confetti flies when the level and the
    /// board allow it and no earlier burst is still alive.
    void celebrate(lv_color_t flash);
    /// Confetti particles a corner releases at `level`: full bursts, none on the flash-only rung.
    int confetti_count(size_t level) const;

    helix::ui::WidgetRef img_;
    lv_draw_buf_t* decoded_ = nullptr;
    // decoded_ with its top grey_rows_ rows in grey; nullptr when the format cannot be rewritten
    lv_draw_buf_t* fill_ = nullptr;
    int32_t grey_rows_ = 0;
    helix::ui::WidgetRef confetti_;
    bool animates_ = false;
    helix::ui::ClockMode shown_mode_ = helix::ui::ClockMode::CLOCK;

    uint32_t elapsed_ms_ = 0;

    int screen_w_ = 0;
    int screen_h_ = 0;
    int sprite_w_ = 0;
    int sprite_h_ = 0;
    int32_t src_w_ = 0;
    int32_t src_h_ = 0;

    // Speed (px/s) the path travels, shared by both axes
    float speed_ = 0.0f;

    // Travel range per axis: screen extent less the sprite footprint
    float range_x_ = 0.0f;
    float range_y_ = 0.0f;

    // Phase offset and velocity (px/s) of the unbounded path
    float x0_ = 0.0f;
    float y0_ = 0.0f;
    float vx_ = 0.0f;
    float vy_ = 0.0f;

    int prev_fold_x_ = 0;
    int prev_fold_y_ = 0;
    int32_t prev_x_ = INT32_MIN;
    int32_t prev_y_ = INT32_MIN;

    int color_index_ = 0;
    int corner_flash_ticks_ = 0;
};

/**
 * @brief Bouncing Logo screensaver
 *
 * The bouncing printer's motion with the HelixScreen logo as the sprite, in its own
 * colors on plain black: with no print, no tint, no corner flash, no confetti. A print shows
 * on it as it does on the printer.
 */
class BouncingLogoScreensaver : public BouncingPrinterScreensaver {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::BOUNCING_LOGO;
    }

  protected:
    std::string sprite_path(int screen_w) const override;
    const char* subject_name() const override {
        return "logo";
    }
    bool decorated() const override {
        return false;
    }
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
