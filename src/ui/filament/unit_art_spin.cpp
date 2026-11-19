// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The unit art's turning spools: both flanges of a feeding lane redrawn with their vents
// rotated, paced by an LVGL animation that runs only while the art is on screen and the
// display is awake.

#include "ui_unit_art.h"

#include "unit_art_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <image/lv_image_decoder_private.h>
#include <string>
#include <vector>

namespace helix::ui {

namespace unit_art_detail {

namespace {

/// Frees a lane's spin buffers: its frames once the image showing them is gone.
void free_flange_layers(FlangeSpin& flange) {
    for (lv_draw_buf_t** baked : {&flange.face, &flange.vents, &flange.lit, &flange.front}) {
        if (*baked)
            lv_draw_buf_destroy(*baked);
        *baked = nullptr;
    }
}

void spin_anim_cb(void* var, int32_t value);

/// Frees everything of a lane's spin, its animation included: a condemned image's animation
/// would otherwise keep writing into the lane's next spin.
} // namespace

void free_spin(LaneSpin& spin) {
    if (lv_obj_t* image = spin.flange[kNearFlange].image)
        lv_anim_delete(image, spin_anim_cb);
    for (FlangeSpin& flange : spin.flange) {
        for (lv_draw_buf_t*& frame : flange.frame)
            free_with(flange.image, frame);
        free_flange_layers(flange);
        flange.image = nullptr;
    }
}

namespace {

constexpr uint32_t kSpinFrameMs = 66; ///< a turning spool redraws at most this often
constexpr int32_t kSpinTenthsPerTurn = 3600;

/// A baked layer's pixels inside a flange's box, or nullptr when it cannot be decoded.
lv_draw_buf_t* decode_crop(const std::string& path, int32_t x0, int32_t y0, int32_t w, int32_t h) {
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, path.c_str(), nullptr) != LV_RESULT_OK)
        return nullptr;
    const lv_draw_buf_t* src = dsc.decoded;
    lv_draw_buf_t* out = nullptr;
    if (src && src->header.cf == LV_COLOR_FORMAT_ARGB8888)
        out = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (out) {
        lv_draw_buf_clear(out, nullptr);
        for (int32_t y = 0; y < h; ++y) {
            const int32_t sy = y0 + y;
            if (sy < 0 || sy >= (int32_t)src->header.h)
                continue;
            auto* dst = static_cast<uint8_t*>(lv_draw_buf_goto_xy(out, 0, y));
            const uint8_t* row = src->data + static_cast<size_t>(sy) * src->header.stride;
            for (int32_t x = 0; x < w; ++x) {
                const int32_t sx = x0 + x;
                if (sx >= 0 && sx < (int32_t)src->header.w)
                    std::memcpy(dst + x * 4, row + sx * 4, 4);
            }
        }
    }
    lv_image_decoder_close(&dsc);
    return out;
}

/// Half a flange's extent on screen, in shares of the drawing's width.
void flange_half_extent(const tray::ArtLaneGeom& g, float& hx, float& hy) {
    hx = std::hypot(g.ey_x, g.ez_x);
    hy = std::hypot(g.ey_y, g.ez_y);
}

/// A whole baked layer, or nullptr when it cannot be decoded.
lv_draw_buf_t* decode_whole(const std::string& path) {
    lv_image_header_t header{};
    if (lv_image_decoder_get_info(path.c_str(), &header) != LV_RESULT_OK)
        return nullptr;
    return decode_crop(path, 0, 0, header.w, header.h);
}

/// Loads lane @p lane's flange faces, each cropped to its flange, and the drawing's vents.
/// False when the bake has none or they cannot be read; a lane fails once.
bool load_spin_layers(ArtData* data, int lane, LaneSpin& spin) {
    if (spin.flange[kNearFlange].face)
        return true;
    if (spin.load_failed)
        return false;
    spin.load_failed = true;
    const std::string stem = data->base + "_lane" + std::to_string(lane);
    lv_image_header_t header{};
    if (lv_image_decoder_get_info((stem + "_face.png").c_str(), &header) != LV_RESULT_OK ||
        header.w == 0)
        return false;
    const tray::ArtLaneGeom& g = data->geom->lane[lane];
    float hx = 0, hy = 0;
    flange_half_extent(g, hx, hy);
    spin.baked_w = (float)header.w;
    const int32_t w = (int32_t)std::ceil(2 * hx * spin.baked_w) + 3;
    const int32_t h = (int32_t)std::ceil(2 * hy * spin.baked_w) + 3;
    bool loaded = true;
    for (int which : {kNearFlange, kFarFlange}) {
        FlangeSpin& flange = spin.flange[which];
        const bool near = which == kNearFlange;
        flange.crop_x = (int32_t)std::floor(((near ? g.near_x : g.far_x) - hx) * spin.baked_w) - 1;
        flange.crop_y = (int32_t)std::floor(((near ? g.near_y : g.far_y) - hy) * spin.baked_w) - 1;
        flange.face = decode_crop(stem + (near ? "_face.png" : "_far_face.png"), flange.crop_x,
                                  flange.crop_y, w, h);
        flange.vents = decode_whole(data->base + (near ? "_vents.png" : "_far_vents.png"));
        if (near)
            flange.lit = decode_whole(data->base + "_vents_lit.png");
        flange.front = decode_crop(data->base + "_front.png", flange.crop_x, flange.crop_y, w, h);
        loaded = loaded && flange.face && flange.vents && flange.front && (!near || flange.lit);
    }
    if (!loaded) {
        for (FlangeSpin& flange : spin.flange)
            free_flange_layers(flange);
        spdlog::debug("[UnitArt] no turning vents for {}", stem);
        return false;
    }
    // The outlet in the flange's own plane (u toward the back, v up) sets which way paying
    // out turns: mid + ey * u + ez * v = outlet, from the spool's middle, the depth the
    // outlet stands at under it.
    const float det = g.ey_x * g.ez_y - g.ez_x * g.ey_y;
    const float dx = g.outlet_x - (g.far_x + g.near_x) / 2;
    const float dy = g.outlet_y - (g.far_y + g.near_y) / 2;
    if (std::abs(det) > 1e-9f)
        spin.payout_turn =
            spool_payout_turn((g.ez_y * dx - g.ez_x * dy) / det, (g.ey_x * dy - g.ey_y * dx) / det);
    spin.load_failed = false;
    return true;
}

/// A premultiplied pixel, 0..1 per channel.
struct Premul {
    float r = 0, g = 0, b = 0, a = 0;
};

/// @p buf sampled between its pixels at (@p x, @p y), in its own pixels.
Premul sample(const lv_draw_buf_t* buf, float x, float y) {
    const float fx = x - 0.5f, fy = y - 0.5f;
    const int32_t x0 = (int32_t)std::floor(fx), y0 = (int32_t)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    Premul out;
    const int32_t w = buf->header.w, h = buf->header.h;
    for (int k = 0; k < 4; ++k) {
        const int32_t px = x0 + (k & 1), py = y0 + (k >> 1);
        if (px < 0 || py < 0 || px >= w || py >= h)
            continue;
        const float wt = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
        const uint8_t* p = buf->data + static_cast<size_t>(py) * buf->header.stride + px * 4;
        const float a = (float)p[3] / 255.0f * wt;
        out.b += (float)p[0] / 255.0f * a;
        out.g += (float)p[1] / 255.0f * a;
        out.r += (float)p[2] / 255.0f * a;
        out.a += a;
    }
    return out;
}

/// @p over laid on @p under.
Premul over(const Premul& top, const Premul& under) {
    const float k = 1 - top.a;
    return {top.r + under.r * k, top.g + under.g * k, top.b + under.b * k, top.a + under.a * k};
}

/// Draws flange @p which of lane @p lane at the spool's current angle into its next frame and
/// shows it. Hidden when the lane shows no spool to turn.
void render_flange(lv_obj_t* art, ArtData* data, int lane, int which) {
    LaneSpin& spin = data->spins[static_cast<size_t>(lane)];
    FlangeSpin& flange = spin.flange[which];
    const LaneImages& images = data->lanes[static_cast<size_t>(lane)];
    if (!flange.image || !flange.face)
        return;
    const UnitArtLaneMark& mark = images.mark;
    const int32_t art_w = lv_obj_get_width(art), art_h = lv_obj_get_height(art);
    if (mark.absent || mark.ghost || mark.dim || art_w <= 0 || art_h <= 0) {
        lv_obj_set_hidden(flange.image, true);
        return;
    }
    const tray::ArtLaneGeom& g = data->geom->lane[lane];
    const bool near = which == kNearFlange;
    const float center_x = near ? g.near_x : g.far_x, center_y = near ? g.near_y : g.far_y;
    const Content c = content_of(data, art_w, art_h);
    float hx = 0, hy = 0;
    flange_half_extent(g, hx, hy);
    const int32_t x0 = std::max<int32_t>(0, (int32_t)std::floor(c.x + (center_x - hx) * c.w) - 1);
    const int32_t y0 = std::max<int32_t>(0, (int32_t)std::floor(c.y + (center_y - hy) * c.w) - 1);
    const int32_t x1 = std::min(art_w, (int32_t)std::ceil(c.x + (center_x + hx) * c.w) + 1);
    const int32_t y1 = std::min(art_h, (int32_t)std::ceil(c.y + (center_y + hy) * c.w) + 1);
    const int32_t w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return;
    const int next = 1 - flange.shown;
    lv_draw_buf_t*& frame = flange.frame[next];
    if (frame && (frame->header.w != (uint32_t)w || frame->header.h != (uint32_t)h)) {
        destroy_buf(frame);
        frame = nullptr;
    }
    if (!frame)
        frame = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (!frame)
        return;
    lv_image_cache_drop(frame);

    // The flange's basis maps (u, v) on its unit disc to the drawing: center + ey * u + ez * v.
    // The vents turn by the face angle, from u toward v when it grows; both flanges turn as one.
    const float det = g.ey_x * g.ez_y - g.ez_x * g.ey_y;
    if (std::abs(det) < 1e-9f)
        return;
    const tray::FlangeTurn turn = tray::flange_turn((float)spin.payout_turn * spin.angle);
    const float k = spin.baked_w; // shares to baked pixels
    const bool filled = images.filled;
    // The far flange stands behind the wound filament, which covers it as far as it reaches.
    const bool wound = !near && filled;
    const float wound_r = tray::wound_radius(g.core, g.wound, (float)images.mask_pct / 100.0f);
    const float px_per_unit = tray::art_lane_px_per_unit(g, c.w);
    // The vents are baked centered on a flange's center.
    const float vents_cx = (float)flange.vents->header.w / 2,
                vents_cy = (float)flange.vents->header.h / 2;
    const float fr = (float)images.filament.red / 255.0f;
    const float fg = (float)images.filament.green / 255.0f;
    const float fb = (float)images.filament.blue / 255.0f;
    for (int32_t y = 0; y < h; ++y) {
        auto* row = static_cast<uint8_t*>(lv_draw_buf_goto_xy(frame, 0, y));
        const float sy = ((float)(y0 + y) + 0.5f - c.y) / c.w;
        for (int32_t x = 0; x < w; ++x) {
            uint8_t* px = row + x * 4;
            const float sx = ((float)(x0 + x) + 0.5f - c.x) / c.w;
            const float fx = sx * k - (float)flange.crop_x, fy = sy * k - (float)flange.crop_y;
            Premul face = sample(flange.face, fx, fy);
            if (wound && face.a > 0.0f) {
                const float open = 1.0f - tray::art_lane_coverage(g, sx, sy, wound_r, px_per_unit);
                face = {face.r * open, face.g * open, face.b * open, face.a * open};
            }
            if (face.a <= 0.0f) {
                std::memset(px, 0, 4);
                continue;
            }
            const float dx = sx - center_x, dy = sy - center_y;
            const float u = (g.ez_y * dx - g.ez_x * dy) / det;
            const float v = (g.ey_x * dy - g.ey_y * dx) / det;
            float ru = 0, rv = 0;
            tray::flange_unturn(turn, u, v, ru, rv);
            const float bx = (g.ey_x * ru + g.ez_x * rv) * k + vents_cx;
            const float by = (g.ey_y * ru + g.ez_y * rv) * k + vents_cy;
            Premul out = face;
            Premul vents = sample(flange.vents, bx, by);
            // Vents show only where the flange does.
            vents = {vents.r * face.a, vents.g * face.a, vents.b * face.a, vents.a * face.a};
            out = over(vents, out);
            if (filled && flange.lit) {
                Premul lit = sample(flange.lit, bx, by);
                lit = {lit.r * fr * face.a, lit.g * fg * face.a, lit.b * fb * face.a,
                       lit.a * face.a};
                out = over(lit, out);
            }
            // Whatever the front layer lays over the flange, glass included, lies over the vents.
            Premul front = sample(flange.front, fx, fy);
            front = {front.r * face.a, front.g * face.a, front.b * face.a, front.a * face.a};
            out = over(front, out);
            const float a = std::min(out.a, 1.0f);
            const float inv = a > 0.0f ? 255.0f / a : 0.0f;
            px[0] = (uint8_t)std::min(255.0f, out.b * inv);
            px[1] = (uint8_t)std::min(255.0f, out.g * inv);
            px[2] = (uint8_t)std::min(255.0f, out.r * inv);
            px[3] = (uint8_t)std::lround(a * 255.0f);
        }
    }
    lv_image_set_src(flange.image, frame);
    lv_obj_set_pos(flange.image, x0, y0);
    lv_obj_set_size(flange.image, w, h);
    lv_obj_set_hidden(flange.image, false);
    lv_obj_invalidate(flange.image);
    flange.shown = next;
}

/// Draws both of lane @p lane's flanges at the spool's current angle.
/// A stopped lane whose layers are freed can no longer redraw its turned flanges, so they hide
/// and the flat image shows the spool at rest.
void render_spin(lv_obj_t* art, ArtData* data, int lane) {
    LaneSpin& spin = data->spins[static_cast<size_t>(lane)];
    if (!spin.flange[kNearFlange].face) {
        for (FlangeSpin& flange : spin.flange)
            if (flange.image)
                lv_obj_set_hidden(flange.image, true);
        spin.angle = 0.0f;
        return;
    }
    render_flange(art, data, lane, kFarFlange);
    render_flange(art, data, lane, kNearFlange);
    data->spins[static_cast<size_t>(lane)].last_frame = lv_tick_get();
}

} // namespace

/// Redraws every lane that has turned, after a change of size, color or marks.
void refresh_spins(lv_obj_t* art, ArtData* data) {
    for (size_t i = 0; i < data->spins.size(); ++i)
        if (data->spins[i].flange[kNearFlange].image)
            render_spin(art, data, static_cast<int>(i));
}

/// Hides every spin image for a snapshot of the layers, returning the ones it hid.
std::vector<lv_obj_t*> hide_spins(ArtData* data) {
    std::vector<lv_obj_t*> hidden;
    for (const LaneSpin& spin : data->spins) {
        for (const FlangeSpin& flange : spin.flange) {
            if (flange.image && !lv_obj_is_hidden(flange.image)) {
                lv_obj_set_hidden(flange.image, true);
                hidden.push_back(flange.image);
            }
        }
    }
    return hidden;
}

void show_spins(const std::vector<lv_obj_t*>& images) {
    for (lv_obj_t* image : images)
        lv_obj_set_hidden(image, false);
}

namespace {

/// The display is asleep: no spool turns until it wakes.
bool g_display_asleep = false;

/// Whether a spool in @p art may turn now: it is on screen and the display is awake.
bool spin_can_run(lv_obj_t* art) {
    return !g_display_asleep && lv_obj_is_visible(art);
}

/// Starts lane @p lane's turning at its requested pace from its current angle, when it should
/// turn and is not already. Never draws, so it is safe while the art is being drawn.
void start_spin_anim(ArtData* data, int lane) {
    LaneSpin& turning = data->spins[static_cast<size_t>(lane)];
    lv_obj_t* image = turning.flange[kNearFlange].image; // the animation's own
    if (turning.spin.direction == 0 || !image || !turning.flange[kNearFlange].face ||
        lv_anim_get(image, spin_anim_cb))
        return;
    // Starts from the current angle, so a change of speed never jumps the vents.
    turning.anim_base = turning.angle;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, image);
    lv_anim_set_values(&anim, 0, kSpinTenthsPerTurn);
    lv_anim_set_duration(&anim,
                         static_cast<uint32_t>(turning.spin.step_ms) * kSpoolSpinPhasesPerTurn);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&anim, spin_anim_cb);
    lv_anim_set_early_apply(&anim, false);
    lv_anim_start(&anim);
}

/// Stops every turning spool in @p art where it stands; each keeps its requested spin.
void pause_spins(ArtData* data) {
    for (const LaneSpin& spin : data->spins)
        if (lv_obj_t* image = spin.flange[kNearFlange].image)
            lv_anim_delete(image, spin_anim_cb);
}

/// Starts every spool in @p art that should turn and is not, when it may turn now.
void resume_spins(lv_obj_t* art, ArtData* data) {
    if (!spin_can_run(art))
        return;
    for (size_t i = 0; i < data->spins.size(); ++i)
        start_spin_anim(data, static_cast<int>(i));
}

} // namespace

/// Every live unit art, so a display sleep or wake reaches each one's spools.
std::vector<lv_obj_t*>& live_arts() {
    static std::vector<lv_obj_t*> arts;
    return arts;
}

/// An art coming into view, scrolled back or under an overlay sliding in, is drawn: its
/// requested spins start again.
void spin_draw_cb(lv_event_t* e) {
    lv_obj_t* art = lv_event_get_target_obj(e);
    if (ArtData* data = data_of(art))
        resume_spins(art, data);
}

namespace {

/// One step of a turning spool, at LVGL's animation tick; redraws at most every kSpinFrameMs.
/// Off screen or with the display asleep the animation stops; the spin stays requested and
/// starts again when the art is drawn or the display wakes.
void spin_anim_cb(void* var, int32_t value) {
    auto* image = static_cast<lv_obj_t*>(var);
    lv_obj_t* art = lv_obj_get_parent(image);
    ArtData* data = art ? data_of(art) : nullptr;
    const int lane = (int)reinterpret_cast<intptr_t>(lv_obj_get_user_data(image));
    if (!data || lane < 0 || lane >= (int)data->spins.size())
        return;
    LaneSpin& spin = data->spins[static_cast<size_t>(lane)];
    if (!spin_can_run(art)) {
        lv_anim_delete(image, spin_anim_cb);
        return;
    }
    spin.angle = std::fmod(spin.anim_base + (float)(spin.spin.direction * value) / 10.0f, 360.0f);
    if (lv_tick_elaps(spin.last_frame) < kSpinFrameMs)
        return;
    render_spin(art, data, lane);
}

} // namespace

} // namespace unit_art_detail

using namespace unit_art_detail;

void unit_art_set_lane_spin(lv_obj_t* art, int lane, const SpoolSpin& spin) {
    ArtData* data = art ? data_of(art) : nullptr;
    if (!data || !data->geom || !data->geom->spins || data->tier != UnitArtTier::Full || lane < 0 ||
        lane >= (int)data->spins.size() || lane >= data->geom->lanes)
        return;
    LaneSpin& turning = data->spins[static_cast<size_t>(lane)];
    if (spin == turning.spin)
        return;
    lv_obj_t*& image = turning.flange[kNearFlange].image; // the animation's own
    if (image)
        lv_anim_delete(image, spin_anim_cb);
    turning.spin = spin;
    spdlog::debug("[UnitArt] lane {} turns {} at {} ms a phase", lane, spin.direction,
                  spin.step_ms);
    if (spin.direction == 0) {
        // A stopped spool holds its angle in its last frame; the layers that drew it go.
        for (FlangeSpin& flange : turning.flange) {
            free_flange_layers(flange);
            lv_draw_buf_t*& spare = flange.frame[1 - flange.shown];
            if (spare)
                destroy_buf(spare); // render_flange frees it the same way on a resize
            spare = nullptr;
        }
        return;
    }
    if (!load_spin_layers(data, lane, turning)) {
        turning.spin = SpoolSpin{};
        return;
    }
    if (!image) {
        // Over the flat image, under the glow and a press; the near flange over the far one.
        for (int which : {kNearFlange, kFarFlange}) {
            lv_obj_t* flange = lv_image_create(art);
            const char* suffix = which == kNearFlange ? "_spin" : "_far_spin";
            lv_obj_set_name(flange, ("lane" + std::to_string(lane) + suffix).c_str());
            lv_obj_set_clickable(flange, false);
            lv_obj_set_user_data(flange, reinterpret_cast<void*>(static_cast<intptr_t>(lane)));
            lv_obj_move_to_index(flange, lv_obj_get_index(data->flat) + 1);
            turning.flange[which].image = flange;
        }
    }
    render_spin(art, data, lane);
    if (spin_can_run(art))
        start_spin_anim(data, lane);
}

void unit_art_set_display_asleep(bool asleep) {
    g_display_asleep = asleep;
    spdlog::debug("[UnitArt] display {}: {} turning spools", asleep ? "asleep" : "awake",
                  asleep ? "pausing" : "resuming");
    for (lv_obj_t* art : live_arts()) {
        if (ArtData* data = data_of(art)) {
            if (asleep)
                pause_spins(data);
            else
                resume_spins(art, data);
        }
    }
}

float unit_art_lane_spin_angle(lv_obj_t* art, int lane) {
    ArtData* data = art ? data_of(art) : nullptr;
    if (!data || lane < 0 || lane >= (int)data->spins.size())
        return 0.0f;
    const LaneSpin& spin = data->spins[static_cast<size_t>(lane)];
    float a = std::fmod((float)spin.payout_turn * spin.angle, 360.0f);
    if (a > 180.0f)
        a -= 360.0f;
    else if (a <= -180.0f)
        a += 360.0f;
    return a;
}

lv_obj_t* unit_art_lane_spin_obj(lv_obj_t* art, int lane, bool far_flange) {
    ArtData* data = art ? data_of(art) : nullptr;
    if (!data || lane < 0 || lane >= (int)data->spins.size())
        return nullptr;
    return data->spins[static_cast<size_t>(lane)]
        .flange[far_flange ? kFarFlange : kNearFlange]
        .image;
}

} // namespace helix::ui
