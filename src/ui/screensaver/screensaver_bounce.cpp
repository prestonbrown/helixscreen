// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_bounce.h"

#include "ui_confetti.h"

#include "lv_draw_buf_guard.h"
#include "platform_capabilities.h"
#include "screensaver_fast_math.h"
#include "screensaver_image.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

using helix::screensaver_bounce::fit_sprite;
using helix::screensaver_bounce::kMinRangePx;
using helix::screensaver_bounce::sprite_size_for;
using helix::ui::screensaver::fold;
using helix::ui::screensaver::fold_index;
using helix::ui::screensaver::is_corner_hit;
using helix::ui::screensaver::near_rational;
using helix::ui::screensaver::unit_random;

namespace {

// Travel per second as a fraction of the screen's narrow axis, so a 480x272
// panel and a 1024x600 one read at the same pace.
constexpr float kSpeedFraction = 0.16f;

// Angles this far off both axes keep the path from degenerating into a line.
constexpr float kAngleMinDeg = 18.0f;
constexpr float kAngleSpanDeg = 54.0f;

// Rejection bound for short repeating paths, and how close counts as repeating.
constexpr int kMaxDenominator = 6;
constexpr float kRationalEps = 0.02f;
constexpr int kSeedAttempts = 16;

// Tint strength. At LV_OPA_COVER the recolor replaces every channel and the
// sprite collapses to a flat silhouette; this leaves the render readable.
constexpr lv_opa_t RECOLOR_OPA = 110;

constexpr int kCornerFlashTicks = 3;
// How far the backdrop travels from black toward the tint. The overlay stays
// fully opaque throughout — fading it would show the UI it is covering.
constexpr lv_opa_t kCornerFlashMix = 56;
// Every particle costs a position, a size and a style write each frame, so one is
// roughly 2.5x a flying-toaster sprite. The tightest board that runs animations
// sustains 49 toaster sprites; this keeps the celebration inside that same band
// instead of spiking to twice it.
constexpr int kCornerConfettiParticles = 24;

// Procedural palette for the sprite tint and the corner flash. These are
// animation content rather than UI chrome, so they are not theme tokens —
// the same footing as kStarTints and kConfettiColors.
const lv_color_t kBounceTints[] = {
    LV_COLOR_MAKE(0xFF, 0x6B, 0x6B), // red
    LV_COLOR_MAKE(0xFF, 0xE6, 0x6D), // yellow
    LV_COLOR_MAKE(0x4E, 0xCD, 0xC4), // teal
    LV_COLOR_MAKE(0x45, 0xB7, 0xD1), // blue
    LV_COLOR_MAKE(0x96, 0xE6, 0xA1), // green
    LV_COLOR_MAKE(0xDD, 0xA0, 0xDD), // plum
    LV_COLOR_MAKE(0xFF, 0xA6, 0x5C), // orange
};
constexpr int kNumTints = sizeof(kBounceTints) / sizeof(kBounceTints[0]);

/// Logo art cropped to its opaque bounds, so the walls meet the artwork itself, and
/// sized to the largest box sprite_size_for() hands out.
constexpr const char* kLogoSpritePath = "A:assets/images/helixscreen-logo-bounce.png";

} // namespace

namespace helix {

std::string BouncingPrinterScreensaver::sprite_path(int screen_w) const {
    return helix::ui::saver_printer_image_path(screen_w);
}

std::string BouncingLogoScreensaver::sprite_path(int /*screen_w*/) const {
    return kLogoSpritePath;
}

bool BouncingPrinterScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting bouncing {}", subject_name());

    screen_w_ = screen_w();
    screen_h_ = screen_h();

    const int box = sprite_size_for(screen_w_, screen_h_);
    if (box <= 0) {
        spdlog::warn("[Screensaver] {}x{} leaves under {}px of travel, declining to bounce",
                     screen_w_, screen_h_, kMinRangePx);
        return false;
    }

    if (!decode_sprite()) {
        return false;
    }
    apply_geometry(box);

    spdlog::debug("[Screensaver] Bouncing {} sprite ready ({}x{}, {}x{} sprite)", subject_name(),
                  screen_w_, screen_h_, sprite_w_, sprite_h_);

    elapsed_ms_ = 0;

    img_ = lv_image_create(overlay().obj());
    lv_image_set_src(img_, decoded_);
    // CONTAIN makes the object's box and the sprite's footprint the same
    // rectangle, which is what the bounce arithmetic addresses. Set once here:
    // it forces a layout pass, and this overlay is built fresh on lv_layer_top()
    // rather than inside a live rebuild (#983/#1025).
    lv_image_set_inner_align(img_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_size(img_, sprite_w_, sprite_h_);
    lv_obj_set_pos(img_, 0, 0);

    seed_motion();
    apply_tint();
    return true;
}

void BouncingPrinterScreensaver::on_stop() {
    // The sprite is a child of the overlay the base has already queued for deletion.
    img_ = nullptr;
    free_sprite();
    prev_x_ = INT32_MIN;
    prev_y_ = INT32_MIN;
    corner_flash_ticks_ = 0;
}

bool BouncingPrinterScreensaver::decode_sprite() {
    const std::string path = sprite_path(screen_w_);

    // Decoded once, each frame is a scaled blit over the sprite's own footprint and nothing
    // more. A device reads the pre-rendered variant here; a source tree without
    // `make gen-images` behind it holds the full-size art instead.
    const std::string what = std::string(subject_name()) + " image";
    decoded_ = helix::ui::saver_decode_image(path.c_str(), what.c_str());
    if (!decoded_) {
        return false;
    }
    src_w_ = static_cast<int32_t>(decoded_->header.w);
    src_h_ = static_cast<int32_t>(decoded_->header.h);
    return true;
}

void BouncingPrinterScreensaver::free_sprite() {
    // The sprite is the bounce image's source until the overlay teardown stops
    // the saver; a blend of its last frame may still be in flight.
    helix::safe_draw_buf_destroy(decoded_, "bounce");
}

void BouncingPrinterScreensaver::seed_motion() {
    // Start anywhere on the field so consecutive runs do not trace the same path.
    const float start_x = unit_random(rng()) * range_x_;
    const float start_y = unit_random(rng()) * range_y_;

    for (int attempt = 0; attempt < kSeedAttempts; ++attempt) {
        const float deg = kAngleMinDeg + unit_random(rng()) * kAngleSpanDeg;
        const float theta = deg * (helix::ui::kSaverPi / 180.0f);
        const float sx = (unit_random(rng()) < 0.5f) ? -1.0f : 1.0f;
        const float sy = (unit_random(rng()) < 0.5f) ? -1.0f : 1.0f;

        vx_ = speed_ * std::cos(theta) * sx;
        vy_ = speed_ * std::sin(theta) * sy;

        if (range_x_ <= 0.0f || range_y_ <= 0.0f) {
            break;
        }
        const float ratio = std::fabs((vx_ / range_x_) / (vy_ / range_y_));
        if (!near_rational(ratio, kMaxDenominator, kRationalEps)) {
            break;
        }
    }

    x0_ = start_x;
    y0_ = start_y;
    prev_fold_x_ = fold_index(x0_, range_x_);
    prev_fold_y_ = fold_index(y0_, range_y_);
}

void BouncingPrinterScreensaver::apply_geometry(int box) {
    fit_sprite(box, src_w_, src_h_, sprite_w_, sprite_h_);
    range_x_ = static_cast<float>(screen_w_ - sprite_w_);
    range_y_ = static_cast<float>(screen_h_ - sprite_h_);
    speed_ = kSpeedFraction * static_cast<float>(std::min(screen_w_, screen_h_));
}

void BouncingPrinterScreensaver::rebase(int screen_w, int screen_h) {
    const int box = sprite_size_for(screen_w, screen_h);
    if (box <= 0) {
        return; // keep bouncing in the old geometry rather than stop mid-flight
    }

    const float t = static_cast<float>(elapsed_ms_) / 1000.0f;
    const float cur_x = fold(x0_ + vx_ * t, range_x_);
    const float cur_y = fold(y0_ + vy_ * t, range_y_);

    screen_w_ = screen_w;
    screen_h_ = screen_h;
    apply_geometry(box);

    if (img_) {
        lv_obj_set_size(img_, sprite_w_, sprite_h_);
    }

    // Re-anchor the phase to where the sprite already is, so a rotation moves
    // the walls without teleporting what is between them.
    const float held_x = std::clamp(cur_x, 0.0f, range_x_);
    const float held_y = std::clamp(cur_y, 0.0f, range_y_);
    x0_ = held_x - vx_ * t;
    y0_ = held_y - vy_ * t;

    prev_fold_x_ = fold_index(x0_ + vx_ * t, range_x_);
    prev_fold_y_ = fold_index(y0_ + vy_ * t, range_y_);
    prev_x_ = INT32_MIN;
    prev_y_ = INT32_MIN;

    spdlog::debug("[Screensaver] Bouncing {} rebased to {}x{} ({}x{} sprite)", subject_name(),
                  screen_w_, screen_h_, sprite_w_, sprite_h_);
}

void BouncingPrinterScreensaver::apply_tint() {
    if (!img_ || !decorated()) {
        return;
    }
    lv_obj_set_style_image_recolor(img_, kBounceTints[color_index_], 0);
    lv_obj_set_style_image_recolor_opa(img_, RECOLOR_OPA, 0);
}

int BouncingPrinterScreensaver::confetti_count(size_t level) const {
    return kLevels[std::min(level, std::size(kLevels) - 1)].confetti ? kCornerConfettiParticles : 0;
}

// The sprite is an LVGL image, which invalidates its own old and new areas when
// it moves, so a frame adds nothing to the dirty list.
void BouncingPrinterScreensaver::on_frame(uint32_t dt_ms,
                                          std::vector<helix::ui::DirtyRect>& /*dirty*/) {
    elapsed_ms_ += dt_ms;

    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        const int w = lv_display_get_horizontal_resolution(disp);
        const int h = lv_display_get_vertical_resolution(disp);
        if (w != screen_w_ || h != screen_h_) {
            rebase(w, h);
        }
    }

    const float t = static_cast<float>(elapsed_ms_) / 1000.0f;
    const float ux = x0_ + vx_ * t;
    const float uy = y0_ + vy_ * t;

    const float x = fold(ux, range_x_);
    const float y = fold(uy, range_y_);

    const int fx = fold_index(ux, range_x_);
    const int fy = fold_index(uy, range_y_);
    const bool bounced_x = (fx != prev_fold_x_);
    const bool bounced_y = (fy != prev_fold_y_);
    prev_fold_x_ = fx;
    prev_fold_y_ = fy;

    if (bounced_x || bounced_y) {
        color_index_ = (color_index_ + 1) % kNumTints;
        apply_tint();
    }

    // Half a frame of travel — wide enough that a corner is not missed between
    // frames at whatever period the running level allows, tight enough that a
    // plain wall hit is not mistaken for one.
    const float tol = std::max(4.0f, speed_ * static_cast<float>(dt_ms) / 2000.0f);
    const bool corner =
        decorated() && is_corner_hit(bounced_x, bounced_y, x, y, range_x_, range_y_, tol);
    if (corner) {
        spdlog::info("[Screensaver] Bouncing {} hit the corner", subject_name());
        corner_flash_ticks_ = kCornerFlashTicks;
        lv_obj_set_style_bg_color(
            overlay().obj(),
            lv_color_mix(kBounceTints[color_index_], lv_color_black(), kCornerFlashMix), 0);

        // The particle system is dozens of objects repositioned and resized
        // every frame. That is the cost profile BASIC and EMBEDDED tiers had the
        // screensaver taken away over, so boards that cannot animate and the
        // ladder's lowest rung get the flash alone.
        const int particles = confetti_count(level());
        if (particles > 0 && helix::PlatformCapabilities::detect().supports_animations) {
            // ui_confetti deletes its own container once the last particle dies, so a
            // handle kept across bursts is dangling by the next corner. Each burst gets
            // its own, and the overlay's deferred delete reaps whatever is still alive.
            if (lv_obj_t* confetti = ui_confetti_create(overlay().obj())) {
                ui_confetti_burst(confetti, particles);
            }
        }
    } else if (corner_flash_ticks_ > 0 && --corner_flash_ticks_ == 0) {
        lv_obj_set_style_bg_color(overlay().obj(), lv_color_black(), 0);
    }

    const int32_t px = static_cast<int32_t>(std::lround(x));
    const int32_t py = static_cast<int32_t>(std::lround(y));
    if (px != prev_x_ || py != prev_y_) {
        lv_obj_set_pos(img_, px, py);
        prev_x_ = px;
        prev_y_ = py;
    }
}

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
