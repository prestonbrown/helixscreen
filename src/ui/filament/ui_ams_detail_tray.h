// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_detail.h"

#include "ams_tray_projection.h"
#include "ams_tray_shell_draw.h"
#include "lvgl/lvgl.h"

// ============================================================================
// Dry-box unit: box, spools on its floor, glass lid
// ============================================================================
// One camera (ams_tray_projection.h) for the spools, their box and the lid.
// The geometry is computed by ams_detail_update_tray() relative to the
// slot_container and drawn by two callbacks: the inside faces behind the
// spools (slot_grid DRAW_MAIN), the front, side and glass in front of them
// (slot_tray DRAW_POST).

namespace helix::ui::ams_tray {

namespace tray = helix::ui::tray;

// Opacities of the faces and glass, dark theme first.
struct TrayOpacities {
    lv_opa_t front, edge, shell, glass, cap, glass_edge;
    float sheen;
};
constexpr TrayOpacities kDarkOpa{140, 140, 204, 15, 36, 153, 0.26f};
constexpr TrayOpacities kLightOpa{140, 153, 204, 13, 26, 140, 0.60f};

struct TrayColors {
    lv_color_t front, wall, floor, back, side, edge, shell, glass, glass_edge;
};

/// Everything the two draw callbacks need, relative to slot_container's origin.
/// One detail view is shown at a time (AmsPanel and AmsOverviewPanel are
/// singletons and each shows one unit), so one copy serves both.
struct TrayState {
    bool valid = false;
    lv_obj_t* container = nullptr; // slot_container the geometry is relative to
    tray::TrayBox box{};
    tray::LidMode lid = tray::LidMode::None;
    float lid_h = 0;
    float lane_half = 0;
    int lane_count = 0;
    float lane_x[kAmsDetailMaxSlots] = {}; // front-plane slot centres
    TrayColors color{};
    TrayOpacities opa = kDarkOpa;
    const tray::UnitShell* shell = nullptr; // nullptr draws the generic box
    float spool_size = 0;
};

extern TrayState s_tray;

void tray_back_draw_cb(lv_event_t* e);
void tray_front_draw_cb(lv_event_t* e);

} // namespace helix::ui::ams_tray
