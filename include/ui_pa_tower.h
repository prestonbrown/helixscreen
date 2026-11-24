// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pa_nonlinear_tuning.h"

#include <lvgl.h>
#include <string>
#include <vector>

/**
 * @file ui_pa_tower.h
 * @brief `<pa_tower>`: a pressure advance test tower drawn from the side
 *
 * One widget for both tuners. Banded: the linear K tower, its bands stacked
 * bottom-up at true scale with their K. Continuous: the firmware tower's
 * judged faces seen face-on, one value swept up the height, the spots where
 * the nozzle changes speed as guide lines up the full height and, under each
 * face, a strip of its speeds with where the flaw shows (a bulge at a
 * slow-down, a thin spot at a speed-up).
 *
 * The owner sets a PaTowerModel; the widget draws it and nothing else. A tap
 * on the tower records the height under the finger and sends
 * LV_EVENT_VALUE_CHANGED, so the owner binds
 * `<event_cb trigger="value_changed" callback="..."/>` and reads
 * pa_tower_tap_height().
 */

namespace helix::ui {

/// How the tower is filled.
enum class PaTowerFill : int {
    Preview = 0, ///< what will print, dimmed
    Outline = 1, ///< nothing printed yet (the start runs)
    Live = 2,    ///< printing: filled up to printed_mm
    Full = 3,    ///< printed, ready to judge or pick
    Wall = 4,    ///< a plain printed wall, for the reference card's inset
};

enum class PaBandState : int { Planned = 0, Printing = 1, Printed = 2 };

struct PaTowerBand {
    float z_low = 0.0f;
    float z_high = 0.0f;
    std::string label; ///< the K the band prints or printed at
    PaBandState state = PaBandState::Planned;
};

/// A height line across one face, with an optional one-letter tag.
struct PaTowerMarker {
    int face = 0; ///< index into PaTowerModel::faces
    float z_mm = 0.0f;
    bool accent = false; ///< the second color (pass 2's front)
    std::string tag;
};

struct PaTowerModel {
    bool banded = true;
    PaTowerFill fill = PaTowerFill::Preview;
    float height_mm = helix::pa::kTowerHeightMm;
    /// Live: the top of what has printed.
    float printed_mm = 0.0f;
    /// Live: the label beside the layer line (the value at the nozzle).
    std::string layer_tag;

    // Banded
    float base_mm = 0.0f;
    std::vector<PaTowerBand> bands;
    int selected = -1;
    /// Manual: everything from this height up holds held_label.
    float held_from_mm = -1.0f;
    std::string held_label;

    // Continuous
    std::vector<helix::pa::TowerFace> faces;
    /// Value labels up the right side, every 10 mm (index 1 = 10 mm).
    std::vector<std::string> tick_labels;
    std::vector<PaTowerMarker> markers;
    bool light_right = false;
    bool speed_strip = true;
    /// The height ruler and the value labels; off for a small inset.
    bool scale = true;
};

void register_pa_tower_widget();
void pa_tower_set_model(lv_obj_t* tower, PaTowerModel model);
/// A short flourish when a tower is done: a wash that fades over 0.9 s. Nothing
/// when animations are off.
void pa_tower_flourish(lv_obj_t* tower);
/// The height of the latest tap, in millimeters up from the plate.
float pa_tower_tap_height(lv_obj_t* tower);
/// The face (index into PaTowerModel::faces) the latest tap landed on; 0 for a banded tower.
int pa_tower_tap_face(lv_obj_t* tower);
/// The height a tap at screen @p y would report: the test seam for taps.
float pa_tower_height_at(lv_obj_t* tower, int32_t y);

} // namespace helix::ui
