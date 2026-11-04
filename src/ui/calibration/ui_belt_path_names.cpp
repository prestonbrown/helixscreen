// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_belt_path_names.h"

#include "lvgl.h"
#include "settings_manager.h"

namespace helix::ui {

namespace {

using helix::calibration::BeltLayout;
using helix::calibration::BeltPath;

enum class Side { LEFT, RIGHT, UPPER, LOWER };

/// Path A's side under a set layout; Path B takes the opposite one.
Side side_of(BeltPath path, BeltLayout layout) {
    const bool a = path == BeltPath::PATH_A;
    switch (layout) {
    case BeltLayout::A_LEFT_B_RIGHT:
        return a ? Side::LEFT : Side::RIGHT;
    case BeltLayout::A_RIGHT_B_LEFT:
        return a ? Side::RIGHT : Side::LEFT;
    case BeltLayout::A_UPPER_B_LOWER:
        return a ? Side::UPPER : Side::LOWER;
    case BeltLayout::A_LOWER_B_UPPER:
    case BeltLayout::UNSET:
        break;
    }
    return a ? Side::LOWER : Side::UPPER;
}

} // namespace

BeltPathName belt_path_name(BeltPath path, BeltLayout layout) {
    if (layout == BeltLayout::UNSET) {
        const char* name = path == BeltPath::PATH_A ? lv_tr("Path A") : lv_tr("Path B");
        return {name, name};
    }
    switch (side_of(path, layout)) {
    case Side::LEFT:
        return {lv_tr("Left belt"), lv_tr("the left belt")};
    case Side::RIGHT:
        return {lv_tr("Right belt"), lv_tr("the right belt")};
    case Side::UPPER:
        return {lv_tr("Upper belt"), lv_tr("the upper belt")};
    case Side::LOWER:
        break;
    }
    return {lv_tr("Lower belt"), lv_tr("the lower belt")};
}

BeltPath belt_path_in_slot(int slot, BeltLayout layout) {
    const bool b_first = layout == BeltLayout::A_RIGHT_B_LEFT;
    return (slot == 0) != b_first ? BeltPath::PATH_A : BeltPath::PATH_B;
}

BeltPath belt_path_in_slot(int slot) {
    return belt_path_in_slot(slot, helix::calibration::belt_layout_from_int(
                                       SettingsManager::instance().get_belt_layout()));
}

BeltPathName belt_path_name(BeltPath path) {
    return belt_path_name(path, helix::calibration::belt_layout_from_int(
                                    SettingsManager::instance().get_belt_layout()));
}

} // namespace helix::ui
