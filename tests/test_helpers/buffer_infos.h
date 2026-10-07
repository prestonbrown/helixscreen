// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <initializer_list>

namespace helix::test {

/// One four-slot unit per entry of @p pressures, each with a filament pressure
/// sensor at that reading against @p set_point (-1 = none published), as
/// OpenAMS reports them. sync_feedback_bias is derived the way the backends do.
inline AmsSystemInfo fps_units(std::initializer_list<float> pressures, float set_point = 0.5f,
                               int current_slot = -1) {
    AmsSystemInfo info;
    int u = 0;
    for (float pressure : pressures) {
        AmsUnit unit;
        unit.unit_index = u;
        unit.first_slot_global_index = u * 4;
        unit.slot_count = 4;
        BufferHealth fps;
        fps.fps_value = fps.smoothed_fps = pressure;
        fps.fps_set_point = set_point;
        fps.fps_reported = true;
        unit.buffer_health = fps;
        info.units.push_back(unit);
        ++u;
    }
    info.total_slots = u * 4;
    info.current_slot = current_slot;
    info.sync_feedback_bias = info.pressure_sensor_bias();
    return info;
}

} // namespace helix::test
