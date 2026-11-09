// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_motion.h"

#include <algorithm>

namespace helix {

uint32_t manual_motion_duration_ms(int length_mm, int speed_mm_s) {
    if (length_mm <= 0 || speed_mm_s <= 0)
        return kMinManualMotionMs;
    const uint64_t ms =
        static_cast<uint64_t>(length_mm) * 1000u / static_cast<uint64_t>(speed_mm_s);
    return static_cast<uint32_t>(std::clamp<uint64_t>(ms, kMinManualMotionMs, kMaxManualMotionMs));
}

FilamentMotion filament_motion_from_int(int value) {
    switch (value) {
    case static_cast<int>(FilamentMotion::Extrude):
        return FilamentMotion::Extrude;
    case static_cast<int>(FilamentMotion::Retract):
        return FilamentMotion::Retract;
    default:
        return FilamentMotion::Idle;
    }
}

} // namespace helix
