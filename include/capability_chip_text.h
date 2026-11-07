// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// Text for the Printer Manager's capability chips, composed from a snapshot of
/// what discovery found. Pure: no LVGL, no singletons, so every rule here is
/// testable on its own and the overlay only gathers facts and publishes text.

#include "printer_detector.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace helix {

/// One chip per capability the Printer Manager lists, in display order.
enum class CapChip : uint8_t {
    Kinematics,
    BuildVolume,
    Probe,
    BedMesh,
    Qgl,
    ZTilt,
    ScrewsTilt,
    Accelerometer,
    BeltTension,
    ShakeTune,
    HeatedBed,
    ChamberSensor,
    ChamberHeater,
    ChamberDryer,
    FilterFan,
    BedDrying,
    Fans,
    FanTach,
    FilamentSystem,
    ToolChanger,
    FilamentSensors,
    WidthSensors,
    Retraction,
    PaCal,
    ToolOffsets,
    NozzleClean,
    PurgeLine,
    HeatSoak,
    ExcludeObject,
    LayerPause,
    JobQueue,
    Mcus,
    Power,
    Leds,
    LedEffects,
    Speaker,
    Webcam,
    Spoolman,
    Timelapse,
    HelixMacros,
    HelixPlugin,
    Count
};

inline constexpr size_t kCapChipCount = static_cast<size_t>(CapChip::Count);

/// One filament system the printer runs, as its owning backend names it.
struct FilamentSystemFact {
    std::string name;
    bool is_tool_changer = false;
    /// The count in the backend's own noun ("4 slots", "8 gates"), translated by
    /// the UI that gathers the facts; empty when the system reports no positions.
    std::string slot_count;
};

/// Everything the chip texts read. Gathered on the main thread by the overlay.
struct CapabilityChipFacts {
    std::string kinematics;   ///< raw Klipper string, "" if unknown
    BuildVolume build_volume; ///< zero spans = unknown
    std::string probe_type;   ///< probe_type_to_display_string(), "" if none
    int accel_count = 0;
    int fan_count = 0;  ///< controllable fans besides part cooling
    int head_count = 0; ///< physical toolheads
    bool has_tool_changer = false;
    std::vector<FilamentSystemFact> filament_systems;
    int filament_sensor_count = 0;
    int width_sensor_count = 0;
    std::vector<std::string> mcus; ///< PrinterDiscovery::mcu_list()
    int power_device_count = 0;
    int led_count = 0;
    int led_effect_count = 0;
    int webcam_count = 0;
    std::set<CapChip> forced_on; ///< chips the user turned on that detection did not find
};

/// The chip's text: its label, then " · " and a detail when there is one.
/// Empty when the chip has nothing to say (an unknown build volume, no
/// filament system besides a tool changer, a single toolhead). The ToolChanger
/// chip names a tool changer as one; several heads without one read "N Extruders".
[[nodiscard]] std::string capability_chip_text(CapChip chip, const CapabilityChipFacts& facts);

/// Klipper's kinematics string as people write it ("corexy" -> "CoreXY").
/// Unknown kinematics come back unchanged; "" stays "".
[[nodiscard]] std::string kinematics_display_name(const std::string& kinematics);

} // namespace helix
