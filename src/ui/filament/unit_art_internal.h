// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The unit art widget's own state, shared by its drawing (ui_unit_art.cpp) and its turning
// spools (unit_art_spin.cpp). Nothing outside those two files includes it.

#include "ui_unit_art.h"

#include "lv_draw_buf_guard.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"
#include "spool_spin.h"
#include "unit_art_geometry.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace helix::ui::unit_art_detail {

struct LaneImages {
    lv_obj_t* color;
    lv_obj_t* shade;
    lv_draw_buf_t* mask = nullptr; ///< the wound filament's outline at mask_fill, A8
    int mask_pct = 100;
    lv_obj_t* body = nullptr; ///< the lane's spool stack less its filament, when baked so
    lv_obj_t* glow = nullptr; ///< its spool's soft light, just behind it; hidden unless marked
    lv_obj_t* dim = nullptr;  ///< its spool's silhouette in the card's color; hidden unless marked
    lv_obj_t* rim = nullptr;  ///< a dashed ring round its spool's near flange: a ghost's mark
    lv_color_t filament = {}; ///< the live filament color the tint takes
    bool filled = false;      ///< the lane holds filament, so its tint and shading show
    UnitArtLaneMark mark;
};

/// One flange of a turning spool, redrawn over the flat image with its vents rotated in the
/// flange's plane and the front layer laid back over them. Its baked face is cut by what stands
/// in front of it, so the redrawn flange shows only where the flat image shows it.
struct FlangeSpin {
    lv_obj_t* image = nullptr; // WIDGET_PTR_OK: child of this widget, gone with it
    /// Drawn in turn, so the buffer a draw may still read is never the one being written.
    lv_draw_buf_t* frame[2] = {nullptr, nullptr};
    int shown = 0;
    /// The lane's baked face, cropped to the flange, and the drawing's vents (and, on the near
    /// flange, lit vents), centered on a flange, all at their baked size.
    lv_draw_buf_t* face = nullptr;
    lv_draw_buf_t* vents = nullptr;
    lv_draw_buf_t* lit = nullptr;
    lv_draw_buf_t* front = nullptr; ///< the front layer, cropped as the face is
    int32_t crop_x = 0, crop_y = 0; ///< the crop's corner in baked pixels
};

constexpr int kNearFlange = 0;
constexpr int kFarFlange = 1;

/// A lane's turning spool: both its flanges, turned together.
struct LaneSpin {
    FlangeSpin flange[2];
    float baked_w = 0.0f; ///< the baked layers' width in pixels
    bool load_failed = false;
    SpoolSpin spin;
    float angle = 0.0f;      ///< degrees turned, toward paying out
    int payout_turn = 1;     ///< the spool's sense of paying out (spool_payout_turn)
    float anim_base = 0.0f;  ///< the angle the running animation started from
    uint32_t last_frame = 0; ///< tick of the last frame drawn
};

struct ArtData {
    UnitArtTier tier = UnitArtTier::Thumb;
    const char* built_id = nullptr;
    int built_lanes = 0;
    std::vector<LaneImages> lanes;
    lv_obj_t* layers = nullptr; // WIDGET_PTR_OK: child of this widget, gone with it
    lv_obj_t* flat = nullptr;   // WIDGET_PTR_OK: child of this widget, gone with it
    lv_draw_buf_t* flat_buf = nullptr;
    uint64_t flat_key = 0;
    lv_obj_t* front = nullptr; // WIDGET_PTR_OK: child of this widget, gone with it
    lv_obj_t* glow = nullptr;  // WIDGET_PTR_OK: child of this widget, gone with it
    lv_draw_buf_t* glow_buf = nullptr;
    uint64_t glow_key = 0;
    /// Rings round a pressed lane's flanges, drawn over the flat image so a press never
    /// rebuilds it.
    lv_obj_t* press = nullptr; // WIDGET_PTR_OK: child of this widget, gone with it

    const tray::UnitArtGeom* geom = nullptr; ///< where the drawing's spools stand
    std::string base;                        ///< asset path of the layers, less "_<layer>.png"
    std::vector<LaneSpin> spins;             ///< per lane, its turning spool
    int32_t mask_w = 0, mask_h = 0;
    uint64_t lane_key = 0; ///< the lanes' colors and fills the flat image was last built for

    const tray::CarriageTrack* track = nullptr;
    lv_obj_t* carriage = nullptr;      // WIDGET_PTR_OK: child of this widget, gone with it
    lv_obj_t* carriage_flat = nullptr; // WIDGET_PTR_OK: child of this widget, gone with it
    lv_draw_buf_t* carriage_buf = nullptr;
    int32_t carriage_w = 0, carriage_h = 0;
    int carriage_gate = 0;
    bool carriage_placed = false;
};

inline ArtData* data_of(lv_obj_t* art) {
    return static_cast<ArtData*>(lv_obj_get_user_data(art));
}

/// Frees a buffer no widget shows any more, once any draw still reading it has finished.
inline void destroy_buf(lv_draw_buf_t* buf) {
    lv_image_cache_drop(buf);
    helix::safe_draw_buf_destroy(buf, "unitart");
}

inline void destroy_buf_cb(lv_event_t* e) {
    destroy_buf(static_cast<lv_draw_buf_t*>(lv_event_get_user_data(e)));
}

/// Frees @p buf when @p holder, a widget that may still draw it, is deleted.
inline void free_with(lv_obj_t* holder, lv_draw_buf_t*& buf) {
    if (!buf)
        return;
    if (holder) {
        // DECLARATIVE_OK: a condemned widget's buffer lives exactly as long as it does.
        lv_obj_add_event_cb(holder, destroy_buf_cb, LV_EVENT_DELETE, buf);
    } else {
        destroy_buf(buf);
    }
    buf = nullptr;
}

struct Content {
    float x, y, w;
};

inline Content content_of(const ArtData* data, int32_t w, int32_t h) {
    const float aspect = data->geom ? data->geom->aspect : 1.0f;
    const float dw = std::min((float)w, (float)h * aspect);
    return {((float)w - dw) / 2, ((float)h - dw / aspect) / 2, dw};
}

/// Frees everything of a lane's spin, its animation included.
void free_spin(LaneSpin& spin);

/// Redraws every lane that has turned, after a change of size, color or marks.
void refresh_spins(lv_obj_t* art, ArtData* data);

/// Hides every spin image for a snapshot of the layers, returning the ones it hid.
std::vector<lv_obj_t*> hide_spins(ArtData* data);
void show_spins(const std::vector<lv_obj_t*>& images);

/// Every live unit art, so a display sleep or wake reaches each one's spools.
std::vector<lv_obj_t*>& live_arts();

/// The art's draw hook: an art being drawn restarts the spins asked of it.
void spin_draw_cb(lv_event_t* e);

} // namespace helix::ui::unit_art_detail
