// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What ui_ams_detail.cpp and ui_ams_detail_labels.cpp share: the unit view's label, tick and
// lane-number layout over a drawn unit, and the small helpers both lay the unit out with.

#include "ui_ams_slot_layout.h"
#include "ui_widget_ref.h"

#include "ams_tray_projection.h"
#include "lvgl/lvgl.h"

#include <cstdint>

namespace helix {
struct AmsSystemInfo;
}

namespace helix::ui::ams_detail_layout {

// Defined in ui_ams_detail.cpp.

/// Centre of the slot's spool graphic, in coordinates relative to @p origin.
bool spool_centre(lv_obj_t* slot, lv_point_t origin, float& x, float& y, int32_t& size);
/// Clears @p obj's translate, so it is measured where its layout put it.
void clear_translate(lv_obj_t* obj);
/// Whether the shown unit stands in a physical tray.
bool unit_has_physical_tray();
/// Whether the unit view draws a tray (3D spools) under its spools.
bool draws_tray();
/// The unit a view shows: the whole-backend view of a one-unit system counts as that unit.
int view_unit(const helix::AmsSystemInfo& info, int unit_index);

// Defined in ui_ams_detail_labels.cpp.

/// The unit ams_detail_create_slots() lays out, which a spool size cap is kept for.
void set_layout_unit(int unit_index);
/// The rows the labels over a drawn unit's @p count slots take, estimated at the slots' pitch
/// in @p layout across @p slot_area, to reserve their height.
int over_lane_label_rows(const helix::ui::WidgetRef slot_widgets[], int count,
                         const AmsSlotLayout& layout, lv_obj_t* slot_area);
/// How far below where lane @p lane's tube leaves a drawn unit's bottom edge its sensor band
/// and number stand.
int32_t art_lane_band_drop(int lane);
/// Marks each lane of the unit view's art as its slot shows it.
void refresh_unit_view_marks();
/// How the lane numbers under the art stand, their left edges at @p left.
helix::ui::AmsLaneBadgeLayout art_badge_layout(const helix::ui::WidgetRef slots[],
                                               const float left[], int n);
/// Lays the labels over the unit out from s_tray; returns the highest top any reaches,
/// INT32_MAX with none.
int32_t place_unit_labels();
/// Over the art each lane's tool badge stands just right of its label, off the drawing.
void place_tool_badges();
/// A lane's spool changed what it shows: the labels and the art's marks follow.
void unit_view_lane_changed(void* user);

} // namespace helix::ui::ams_detail_layout
