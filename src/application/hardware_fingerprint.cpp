// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hardware_fingerprint.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace helix {

size_t compute_hardware_fingerprint(const PrinterDiscovery& hw) {
    size_t h = 0;
    auto combine = [&h](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        h ^= std::hash<T>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
    };
    auto combine_vec = [&](const std::vector<std::string>& v) {
        // Sort for order-independence — Klipper's object-list iteration order
        // isn't stable across reconnects, but the hardware shape is.
        std::vector<std::string> sorted(v.begin(), v.end());
        std::sort(sorted.begin(), sorted.end());
        combine(sorted.size());
        for (const auto& x : sorted) {
            combine(x);
        }
    };
    auto combine_set = [&](const std::unordered_set<std::string>& s) {
        std::vector<std::string> v(s.begin(), s.end());
        std::sort(v.begin(), v.end());
        combine(v.size());
        for (const auto& x : v) {
            combine(x);
        }
    };

    // Hardware-name vectors (component-level identity).
    combine_vec(hw.heaters());
    combine_vec(hw.fans());
    combine_vec(hw.sensors());
    combine_vec(hw.leds());
    combine_vec(hw.steppers());
    combine_vec(hw.filament_sensor_names());
    combine_vec(hw.afc_lane_names());
    combine_vec(hw.afc_hub_names());
    combine_vec(hw.afc_unit_object_names());
    combine_vec(hw.afc_buffer_names());
    combine_vec(hw.tool_names());
    combine_vec(hw.mmu_encoder_names());
    combine_vec(hw.mmu_servo_names());
    combine_vec(hw.ace_object_names());
    combine_vec(hw.width_sensor_objects());
    combine_vec(hw.led_effects());
    combine_vec(hw.led_macros());

    // Macro sets (a printer's user-facing macro surface).
    combine_set(hw.macros());
    combine_set(hw.helix_macros());

    // Identity strings (printer-level identity, not per-reconnect state).
    combine(hw.hostname());
    combine(hw.mcu());
    combine(hw.kinematics());
    combine(static_cast<int>(hw.mmu_type()));
    combine(hw.qidi_box_slot_count());

    // Capability flags — these are derived from the object list, but include
    // them explicitly so adding a new capability flag in PrinterDiscovery
    // doesn't silently bypass fingerprint changes.
    combine(hw.has_qgl());
    combine(hw.has_z_tilt());
    combine(hw.has_bed_mesh());
    combine(hw.has_probe());
    combine(hw.has_heater_bed());
    combine(hw.has_mmu());
    combine(hw.has_snapmaker());
    combine(hw.has_tool_changer());
    combine(hw.has_medusahc());
    combine(hw.has_chamber_heater());
    combine(hw.has_chamber_sensor());
    combine(hw.chamber_heater_name());
    combine(hw.chamber_heater_object_name());
    combine(hw.chamber_cooling_fan_name());
    combine(hw.has_led());
    combine(hw.has_led_effects());
    combine(hw.has_accelerometer());
    combine(hw.has_firmware_retraction());
    combine(hw.has_timelapse());
    combine(hw.has_exclude_object());
    combine(hw.has_screws_tilt());
    combine(hw.has_klippain_shaketune());
    combine(hw.has_speaker());
    combine(hw.has_fan_feedback());
    combine(hw.is_kalico());

    return h;
}

} // namespace helix
