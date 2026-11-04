// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "bed_plate_gesture.h"

#include <cstdlib>
#include <utility>

namespace helix {

namespace {
/// Both set and naming the same XY.
bool same_xy(const std::optional<AxisTarget>& a, const std::optional<AxisTarget>& b) {
    return a && b && a->x == b->x && a->y == b->y;
}
} // namespace

BedPlateGesture::BedPlateGesture(Hooks hooks) : hooks_(std::move(hooks)) {}

bool BedPlateGesture::press(int x, int y) {
    pressing_ = false;
    dragging_ = false;
    drag_target_.reset();
    if (!hooks_.can_move || !hooks_.can_move()) {
        return false;
    }
    pressing_ = true;
    failed_ = false;
    press_x_ = x;
    press_y_ = y;
    sent_target_.reset();
    lift_.reset();
    return true;
}

bool BedPlateGesture::pressing(int x, int y) {
    if (!pressing_) {
        return false;
    }
    if (!dragging_ && std::abs(x - press_x_) <= kPlateDragThresholdPx &&
        std::abs(y - press_y_) <= kPlateDragThresholdPx) {
        return false;
    }
    dragging_ = true;
    // PRESSING repeats on every input read while the finger rests.
    auto target = hooks_.target_at ? hooks_.target_at(x, y) : std::nullopt;
    if (!target || same_xy(target, drag_target_)) {
        return false;
    }
    drag_target_ = target;
    // The head follows the finger, coalesced latest-wins by the owner's sender.
    send_target(*target);
    return true;
}

BedPlateGesture::Release BedPlateGesture::release(int x, int y) {
    Release out;
    if (!pressing_) {
        return out;
    }
    out.was_drag = dragging_;
    if (dragging_) {
        // A drag that already streamed the point it ended on sends nothing more.
        auto target = hooks_.target_at ? hooks_.target_at(x, y) : std::nullopt;
        if (target && !same_xy(target, sent_target_)) {
            send_target(*target);
        }
    } else if (hooks_.target_at) {
        out.tap_target = hooks_.target_at(press_x_, press_y_);
    }
    pressing_ = false;
    dragging_ = false;
    drag_target_.reset();
    return out;
}

void BedPlateGesture::lost() {
    pressing_ = false;
    dragging_ = false;
    drag_target_.reset();
}

bool BedPlateGesture::send_once(const AxisTarget& target) {
    if (!hooks_.can_move || !hooks_.can_move() || !hooks_.lift) {
        return false;
    }
    const PlateLift lift = hooks_.lift();
    if (lift.kind == PlateLift::Kind::Refuse) {
        if (hooks_.refused) {
            hooks_.refused();
        }
        return false;
    }
    AxisTarget lifted = target;
    if (lift.kind == PlateLift::Kind::Lift) {
        lifted.z = lift.z;
    }
    if (hooks_.send) {
        hooks_.send(lifted);
    }
    return true;
}

void BedPlateGesture::mark_failed() {
    failed_ = true;
}

void BedPlateGesture::send_target(const AxisTarget& target) {
    if (failed_) {
        return;
    }
    if (!hooks_.can_move || !hooks_.can_move()) {
        failed_ = true;
        return;
    }
    // The lift is decided once, at the gesture's first move, and stamped on every later
    // one: a sample replacing a pending target replaces it wholesale, so a lift carried
    // by the first alone could be dropped. A repeated move to the same height is no
    // motion, never a descent.
    if (!lift_) {
        lift_ = hooks_.lift ? hooks_.lift() : PlateLift{};
        if (lift_->kind == PlateLift::Kind::Refuse) {
            failed_ = true;
            if (hooks_.refused) {
                hooks_.refused();
            }
            return;
        }
    }
    AxisTarget lifted = target;
    if (lift_->kind == PlateLift::Kind::Lift) {
        lifted.z = lift_->z;
    }
    sent_target_ = target;
    if (hooks_.send) {
        hooks_.send(lifted);
    }
}

} // namespace helix
