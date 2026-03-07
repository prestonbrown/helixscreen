// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_database.h"

#include <algorithm>
#include <any>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file ams_types.h
 * @brief Data structures for multi-filament/AMS system support
 *
 * Supports both Happy Hare (MMU) and AFC-Klipper-Add-On systems.
 * These structures are platform-agnostic - backends translate from
 * their specific APIs to these common types.
 *
 * @note Thread Safety: These structures are NOT thread-safe. The AmsState
 * class provides thread-safe access through LVGL subjects. Direct mutation
 * of these structures should only occur in the backend layer.
 */

/// Default color for slots without filament info (medium gray)
constexpr uint32_t AMS_DEFAULT_SLOT_COLOR = 0x808080;

/**
 * @brief Type of AMS system detected
 *
 * Supports both filament-switching systems (MMU/AMS) and physical tool changers.
 * Tool changers differ in that each "slot" is a complete toolhead with its own
 * extruder, rather than a filament path to a shared toolhead.
 */
enum class AmsType {
    NONE = 0,        ///< No AMS detected
    HAPPY_HARE = 1,  ///< Happy Hare MMU (mmu object in Moonraker)
    AFC = 2,         ///< AFC-Klipper-Add-On (afc object, lane_data database)
    VALGACE = 3,     ///< AnyCubic ACE Pro via ValgACE Klipper driver
    TOOL_CHANGER = 4 ///< Physical tool changer (viesturz/klipper-toolchanger)
};

/**
 * @brief Get string name for AMS type
 * @param type The AMS type enum value
 * @return Human-readable string for the type
 */
inline const char* ams_type_to_string(AmsType type) {
    switch (type) {
    case AmsType::HAPPY_HARE:
        return "Happy Hare";
    case AmsType::AFC:
        return "AFC";
    case AmsType::VALGACE:
        return "ACE Pro";
    case AmsType::TOOL_CHANGER:
        return "Tool Changer";
    default:
        return "None";
    }
}

/**
 * @brief Parse AMS type from string (for Moonraker responses)
 * @param str String to parse (case-insensitive)
 * @return Matching AmsType or NONE if not recognized
 */
inline AmsType ams_type_from_string(std::string_view str) {
    // Simple comparison - backends will use their own detection
    if (str == "mmu" || str == "happy_hare" || str == "Happy Hare") {
        return AmsType::HAPPY_HARE;
    }
    if (str == "afc" || str == "AFC") {
        return AmsType::AFC;
    }
    if (str == "valgace" || str == "ValgACE" || str == "ace" || str == "ACE Pro") {
        return AmsType::VALGACE;
    }
    if (str == "toolchanger" || str == "tool_changer" || str == "Tool Changer") {
        return AmsType::TOOL_CHANGER;
    }
    return AmsType::NONE;
}

/**
 * @brief Check if AMS type is a physical tool changer
 *
 * Tool changers have fundamentally different behavior than filament systems:
 * - Each "slot" is a complete toolhead with its own extruder
 * - Path topology is PARALLEL (not converging to a single nozzle)
 * - "Loading" means mounting the tool, not feeding filament
 *
 * @param type The AMS type to check
 * @return true if this is a physical tool changer
 */
inline bool is_tool_changer(AmsType type) {
    return type == AmsType::TOOL_CHANGER;
}

/**
 * @brief Check if AMS type is a filament-switching system
 *
 * Filament systems route multiple filaments to a single toolhead:
 * - Happy Hare, AFC, ValgACE all fall into this category
 * - Path topology is LINEAR or HUB (converging to single nozzle)
 *
 * @param type The AMS type to check
 * @return true if this is a filament-switching system
 */
inline bool is_filament_system(AmsType type) {
    return type == AmsType::HAPPY_HARE || type == AmsType::AFC || type == AmsType::VALGACE;
}

/**
 * @brief Slot/Lane status
 *
 * Our internal status representation. Use conversion functions to
 * translate from Happy Hare's gate_status values (-1, 0, 1, 2).
 */
enum class SlotStatus {
    UNKNOWN = 0,     ///< Status not known
    EMPTY = 1,       ///< No filament in slot
    AVAILABLE = 2,   ///< Filament available, not loaded
    LOADED = 3,      ///< Filament loaded to extruder
    FROM_BUFFER = 4, ///< Filament available from buffer
    BLOCKED = 5      ///< Slot blocked/jammed
};

/**
 * @brief Get string name for slot status
 * @param status The slot status enum value
 * @return Human-readable string for the status
 */
inline const char* slot_status_to_string(SlotStatus status) {
    switch (status) {
    case SlotStatus::EMPTY:
        return "Empty";
    case SlotStatus::AVAILABLE:
        return "Available";
    case SlotStatus::LOADED:
        return "Loaded";
    case SlotStatus::FROM_BUFFER:
        return "From Buffer";
    case SlotStatus::BLOCKED:
        return "Blocked";
    default:
        return "Unknown";
    }
}

/**
 * @brief Convert Happy Hare gate_status integer to SlotStatus enum
 *
 * Happy Hare uses: -1 = unknown, 0 = empty, 1 = available, 2 = from buffer
 * The "loaded" state is determined by comparing with current_slot, not from
 * gate_status directly.
 *
 * @param hh_status Happy Hare gate_status value (-1, 0, 1, or 2)
 * @return Corresponding SlotStatus enum value
 */
inline SlotStatus slot_status_from_happy_hare(int hh_status) {
    switch (hh_status) {
    case -1:
        return SlotStatus::UNKNOWN;
    case 0:
        return SlotStatus::EMPTY;
    case 1:
        return SlotStatus::AVAILABLE;
    case 2:
        return SlotStatus::FROM_BUFFER;
    default:
        return SlotStatus::UNKNOWN;
    }
}

/**
 * @brief Convert SlotStatus enum to Happy Hare gate_status integer
 * @param status Our SlotStatus enum value
 * @return Happy Hare gate_status value (-1, 0, 1, or 2)
 */
inline int slot_status_to_happy_hare(SlotStatus status) {
    switch (status) {
    case SlotStatus::UNKNOWN:
        return -1;
    case SlotStatus::EMPTY:
        return 0;
    case SlotStatus::AVAILABLE:
        return 1;
    case SlotStatus::FROM_BUFFER:
        return 2;
    // LOADED and BLOCKED don't have direct HH equivalents
    case SlotStatus::LOADED:
        return 1; // Treat as available
    case SlotStatus::BLOCKED:
        return -1; // Treat as unknown
    default:
        return -1;
    }
}

/**
 * @brief Current AMS action/operation
 *
 * Maps to Happy Hare's action strings:
 * "Idle", "Loading", "Unloading", "Forming Tip", "Cutting", "Heating", etc.
 */
enum class AmsAction {
    IDLE = 0,        ///< No operation in progress
    LOADING = 1,     ///< Loading filament to extruder
    UNLOADING = 2,   ///< Unloading filament from extruder
    SELECTING = 3,   ///< Selecting tool/slot
    RESETTING = 4,   ///< Resetting system (MMU_HOME for HH, AFC_RESET for AFC)
    FORMING_TIP = 5, ///< Forming filament tip (legacy, some systems still use)
    HEATING = 6,     ///< Heating for operation
    CHECKING = 7,    ///< Internal sensor verification (not shown in UI)
    PAUSED = 8,      ///< Operation paused (requires attention)
    ERROR = 9,       ///< Error state
    CUTTING = 10,    ///< Cutting filament before retraction (modern AMS)
    PURGING = 11     ///< Purging old filament color after load
};

/**
 * @brief Get string name for AMS action
 * @param action The action enum value
 * @return Human-readable string for the action
 */
inline const char* ams_action_to_string(AmsAction action) {
    switch (action) {
    case AmsAction::IDLE:
        return "Idle";
    case AmsAction::LOADING:
        return "Loading";
    case AmsAction::UNLOADING:
        return "Unloading";
    case AmsAction::SELECTING:
        return "Selecting";
    case AmsAction::RESETTING:
        return "Resetting";
    case AmsAction::FORMING_TIP:
        return "Forming Tip";
    case AmsAction::CUTTING:
        return "Cutting";
    case AmsAction::HEATING:
        return "Heating";
    case AmsAction::CHECKING:
        return "Checking";
    case AmsAction::PAUSED:
        return "Paused";
    case AmsAction::ERROR:
        return "Error";
    case AmsAction::PURGING:
        return "Purging";
    default:
        return "Unknown";
    }
}

/**
 * @brief Parse AMS action from Happy Hare action string
 * @param action_str Action string from printer.mmu.action
 * @return Corresponding AmsAction enum value
 */
inline AmsAction ams_action_from_string(std::string_view action_str) {
    if (action_str == "Idle")
        return AmsAction::IDLE;
    if (action_str == "Loading")
        return AmsAction::LOADING;
    if (action_str == "Unloading")
        return AmsAction::UNLOADING;
    if (action_str == "Selecting")
        return AmsAction::SELECTING;
    if (action_str == "Homing" || action_str == "Resetting")
        return AmsAction::RESETTING;
    if (action_str == "Cutting" || action_str == "Cutting Tip" || action_str == "Cutting Filament")
        return AmsAction::CUTTING;
    if (action_str == "Loading Ext")
        return AmsAction::LOADING;
    if (action_str == "Exiting Ext")
        return AmsAction::UNLOADING;
    if (action_str == "Forming Tip")
        return AmsAction::FORMING_TIP;
    if (action_str == "Heating")
        return AmsAction::HEATING;
    if (action_str == "Checking")
        return AmsAction::CHECKING;
    if (action_str == "Purging")
        return AmsAction::PURGING;
    // Happy Hare uses "Paused" for attention-required states
    if (action_str.find("Pause") != std::string_view::npos)
        return AmsAction::PAUSED;
    if (action_str.find("Error") != std::string_view::npos)
        return AmsAction::ERROR;
    return AmsAction::IDLE;
}

// ============================================================================
// Tip Handling Method
// ============================================================================

/**
 * @brief How the AMS handles filament tip during unload
 *
 * Different systems use different methods to prepare filament for retraction:
 * - CUT: Physical cutter severs filament cleanly (Happy Hare with cutter, AFC)
 * - TIP_FORM: Heat+retract sequence forms a tapered tip (Bambu AMS, some HH configs)
 * - NONE: System doesn't actively manage tip (manual, or no retraction support)
 */
enum class TipMethod {
    NONE = 0,    ///< No active tip handling
    CUT = 1,     ///< Physical filament cutter
    TIP_FORM = 2 ///< Heat and retract to form tapered tip
};

/**
 * @brief Get string name for tip method
 * @param method The tip method enum value
 * @return Human-readable string for the method
 */
inline const char* tip_method_to_string(TipMethod method) {
    switch (method) {
    case TipMethod::NONE:
        return "None";
    case TipMethod::CUT:
        return "Cutter";
    case TipMethod::TIP_FORM:
        return "Tip Forming";
    }
    return "Unknown";
}

/**
 * @brief Get user-friendly step label for tip handling
 * @param method The tip method enum value
 * @return Label suitable for step progress display
 */
inline const char* tip_method_step_label(TipMethod method) {
    switch (method) {
    case TipMethod::CUT:
        return "Cut & retract";
    case TipMethod::TIP_FORM:
        return "Form tip & retract";
    case TipMethod::NONE:
    default:
        return "Retract";
    }
}

// ============================================================================
// Filament Path Visualization Types
// ============================================================================

/**
 * @brief Path topology - affects visual rendering of the filament path
 *
 * Different multi-material systems have different physical topologies:
 * - LINEAR: Selector picks one input from multiple gates (Happy Hare ERCF)
 * - HUB: Multiple lanes merge into a common hub/merger (AFC Box Turtle)
 * - PARALLEL: Each input has its own independent path to a separate toolhead
 *             (physical tool changers like StealthChanger/TapChanger)
 */
enum class PathTopology {
    LINEAR = 0,  ///< Happy Hare: selector picks one input
    HUB = 1,     ///< AFC: merger combines inputs through hub
    PARALLEL = 2 ///< Tool Changer: each slot is a separate toolhead
};

/**
 * @brief Get string name for path topology
 * @param topology The topology enum value
 * @return Human-readable string for the topology
 */
inline const char* path_topology_to_string(PathTopology topology) {
    switch (topology) {
    case PathTopology::LINEAR:
        return "Linear (Selector)";
    case PathTopology::HUB:
        return "Hub (Merger)";
    case PathTopology::PARALLEL:
        return "Parallel (Tool Changer)";
    default:
        return "Unknown";
    }
}

/**
 * @brief Unified path segments (AFC-inspired naming)
 *
 * Both Happy Hare and AFC map to these same logical segments. The path
 * canvas widget draws them differently based on PathTopology.
 *
 * Physical filament path (top to bottom in UI):
 *   SPOOL → PREP → LANE → HUB → OUTPUT → TOOLHEAD → NOZZLE
 *
 * Happy Hare mapping:
 *   SPOOL=Gate storage, PREP=Gate sensor, LANE=Gate-to-selector,
 *   HUB=Selector, OUTPUT=Bowden tube, TOOLHEAD=Extruder sensor, NOZZLE=Loaded
 *
 * AFC mapping:
 *   SPOOL=Lane spool, PREP=Prep sensor, LANE=Lane tube,
 *   HUB=Hub/Merger, OUTPUT=Output tube, TOOLHEAD=Toolhead sensor, NOZZLE=Loaded
 */
enum class PathSegment {
    NONE = 0,     ///< No segment / idle / filament not present
    SPOOL = 1,    ///< At spool (filament storage area)
    PREP = 2,     ///< At entry sensor (prep/gate sensor)
    LANE = 3,     ///< In lane/gate-to-router segment
    HUB = 4,      ///< At router (selector or hub/merger)
    OUTPUT = 5,   ///< In output tube (bowden or hub output)
    TOOLHEAD = 6, ///< At toolhead sensor
    NOZZLE = 7    ///< Fully loaded in nozzle
};

/// Number of path segments for iteration (NONE through NOZZLE)
constexpr int PATH_SEGMENT_COUNT = 8;

/**
 * @brief Get string name for path segment
 * @param segment The segment enum value
 * @return Human-readable string for the segment
 */
inline const char* path_segment_to_string(PathSegment segment) {
    switch (segment) {
    case PathSegment::NONE:
        return "None";
    case PathSegment::SPOOL:
        return "Spool";
    case PathSegment::PREP:
        return "Prep Sensor";
    case PathSegment::LANE:
        return "Lane";
    case PathSegment::HUB:
        return "Hub/Selector";
    case PathSegment::OUTPUT:
        return "Output Tube";
    case PathSegment::TOOLHEAD:
        return "Toolhead";
    case PathSegment::NOZZLE:
        return "Nozzle";
    default:
        return "Unknown";
    }
}

/**
 * @brief Convert Happy Hare filament_pos to unified PathSegment
 *
 * Happy Hare filament_pos values:
 *   0 = unloaded (at spool)
 *   1 = homed at gate
 *   2 = in gate
 *   3 = in bowden
 *   4 = end of bowden
 *   5 = homed at extruder
 *   6 = extruder entry
 *   7 = in extruder
 *   8 = fully loaded
 *
 * @param filament_pos Happy Hare filament_pos value
 * @return Corresponding PathSegment
 */
inline PathSegment path_segment_from_happy_hare_pos(int filament_pos) {
    switch (filament_pos) {
    case 0:
        return PathSegment::SPOOL;
    case 1:
    case 2:
        return PathSegment::PREP; // Gate area
    case 3:
        return PathSegment::LANE; // Moving through
    case 4:
        return PathSegment::HUB; // At selector
    case 5:
        return PathSegment::OUTPUT; // In bowden
    case 6:
        return PathSegment::TOOLHEAD; // At extruder
    case 7:
    case 8:
    case 9: // v4: in-nozzle positions
    case 10:
        return PathSegment::NOZZLE; // Loaded
    default:
        return PathSegment::NONE;
    }
}

/**
 * @brief Infer PathSegment from AFC sensor states
 *
 * AFC uses binary sensor states to determine filament position.
 * Logic: filament is at or past the last sensor that detects it.
 *
 * @param prep_sensor Prep sensor triggered (filament at lane entry)
 * @param hub_sensor Hub sensor triggered (filament in hub)
 * @param toolhead_sensor Toolhead sensor triggered (filament at extruder)
 * @return Inferred PathSegment based on sensor states
 */
inline PathSegment path_segment_from_afc_sensors(bool prep_sensor, bool hub_sensor,
                                                 bool toolhead_sensor) {
    if (toolhead_sensor)
        return PathSegment::NOZZLE;
    if (hub_sensor)
        return PathSegment::TOOLHEAD; // Past hub, approaching toolhead
    if (prep_sensor)
        return PathSegment::HUB; // Past prep, approaching hub
    return PathSegment::SPOOL;   // Not yet at prep
}

/**
 * @brief Per-slot error state
 *
 * Populated by backends when a slot/lane enters an error condition.
 * AFC populates from per-lane status; Happy Hare maps system-level
 * errors to the active gate.
 */
struct SlotError {
    std::string message;                                     ///< Human-readable error description
    enum Severity { INFO, WARNING, ERROR } severity = ERROR; ///< Error severity level
};

/**
 * @brief Buffer health data for AFC buffer fault detection
 *
 * Populated from AFC_buffer status objects. Only applicable to AFC
 * systems with TurtleNeck buffer hardware. Other backends leave
 * buffer_health as nullopt on SlotInfo.
 */
struct BufferHealth {
    bool fault_detection_enabled = false; ///< Whether buffer fault detection is active
    float distance_to_fault = -1.0f;     ///< Distance to fault in mm (-1 = not tracking/null)
    float error_sensitivity = 0.0f;      ///< AFC sensitivity (1-10), 0 = not reported
    std::string state;                    ///< Buffer state (e.g., "Advancing", "Trailing")

    /// Compute fault threshold from error_sensitivity: (11 - sensitivity) * 10 mm
    /// Returns 60mm fallback when sensitivity is 0 (not reported)
    /// Clamps sensitivity to 10 max to ensure threshold >= 10mm
    float fault_threshold() const {
        if (error_sensitivity <= 0.0f)
            return 60.0f;
        float clamped = std::min(error_sensitivity, 10.0f);
        return (11.0f - clamped) * 10.0f;
    }

    /// Map distance_to_fault to danger percentage (0=safe, 100=fault imminent)
    /// Negative or above-threshold distances are treated as safe (0)
    int danger_value() const {
        float max_dist = fault_threshold();
        if (distance_to_fault < 0 || distance_to_fault > max_dist)
            return 0;
        int v = 100 - static_cast<int>((distance_to_fault / max_dist) * 100.0f);
        return std::clamp(v, 0, 100);
    }

    /// Whether the danger level warrants a warning indicator
    bool is_warning() const { return danger_value() > 75; }
};

/**
 * @brief Information about a single slot/lane
 *
 * This represents one filament slot in an AMS unit.
 * Happy Hare calls these "gates" internally, AFC calls them "lanes".
 */
struct SlotInfo {
    int slot_index = -1;   ///< Slot/lane number (0-based within unit)
    int global_index = -1; ///< Global index across all units
    SlotStatus status = SlotStatus::UNKNOWN;

    // Filament information
    std::string color_name;                      ///< Named color (e.g., "Red", "Blue")
    uint32_t color_rgb = AMS_DEFAULT_SLOT_COLOR; ///< RGB color for UI (0xRRGGBB)
    std::string multi_color_hexes;               ///< Comma-separated hex codes for multi-color
                                                 ///< (e.g., "#D4AF37,#C0C0C0,#B87333")
    std::string material;                        ///< Material type (e.g., "PLA", "PETG", "ABS")
    std::string brand;                           ///< Brand name (e.g., "Polymaker", "eSUN")

    // Temperature recommendations (from Spoolman or manual entry)
    int nozzle_temp_min = 0; ///< Minimum nozzle temp (°C)
    int nozzle_temp_max = 0; ///< Maximum nozzle temp (°C)
    int bed_temp = 0;        ///< Recommended bed temp (°C)

    // Tool mapping
    int mapped_tool = -1; ///< Which tool this slot maps to (-1=none)

    // Spoolman integration
    int spoolman_id = 0;           ///< Spoolman spool ID (0=not tracked)
    int spoolman_filament_id = 0;  ///< Spoolman filament definition ID (0=unknown)
    int spoolman_vendor_id = 0;    ///< Spoolman vendor ID (0=unknown)
    std::string spool_name;        ///< Spool name from Spoolman
    float remaining_weight_g = -1; ///< Remaining filament weight in grams (-1=unknown)
    float total_weight_g = -1;     ///< Total spool weight in grams (-1=unknown)

    // Endless spool support (Happy Hare)
    int endless_spool_group = -1; ///< Endless spool group (-1=not grouped)

    // Error state
    std::optional<SlotError> error; ///< Per-slot error state (nullopt = no error)

    /**
     * @brief Get remaining percentage
     * @return 0-100 or -1 if unknown
     */
    [[nodiscard]] float get_remaining_percent() const {
        if (remaining_weight_g < 0 || total_weight_g <= 0)
            return -1;
        return (remaining_weight_g / total_weight_g) * 100.0f;
    }

    /**
     * @brief Check if this slot has filament data configured
     * @return true if material or custom color is set
     */
    [[nodiscard]] bool has_filament_info() const {
        return !material.empty() || color_rgb != AMS_DEFAULT_SLOT_COLOR;
    }

    /**
     * @brief Check if this is a multi-color filament
     * @return true if multi_color_hexes contains color data
     */
    [[nodiscard]] bool is_multi_color() const {
        return !multi_color_hexes.empty();
    }

    /**
     * @brief Check if filament is present in this slot
     * @return true for AVAILABLE, LOADED, FROM_BUFFER, BLOCKED; false for EMPTY, UNKNOWN
     */
    [[nodiscard]] bool is_present() const {
        return status != SlotStatus::EMPTY && status != SlotStatus::UNKNOWN;
    }
};

/**
 * @brief Information about an AMS unit
 *
 * Supports multi-unit configurations (e.g., 2x Box Turtles = 16 slots).
 * Most setups have a single unit with 4-8 slots.
 */
struct AmsUnit {
    int unit_index = 0;              ///< Unit number (0-based)
    std::string name;                ///< Internal name for matching (e.g., "Box_Turtle Turtle_1")
    std::string display_name;        ///< Pretty name for UI (e.g., "Turtle 1") — empty = use name
    int slot_count = 0;              ///< Number of slots on this unit
    int first_slot_global_index = 0; ///< Global index of first slot

    std::vector<SlotInfo> slots; ///< Slot information

    // Unit-level status
    bool connected = false;       ///< Unit communication status
    std::string firmware_version; ///< Firmware version if available

    // Sensors (Happy Hare)
    bool has_encoder = false;         ///< Has filament encoder
    bool has_toolhead_sensor = false; ///< Has toolhead filament sensor
    bool has_slot_sensors = false;    ///< Has per-slot sensors

    // Hub/combiner sensor (AFC Box Turtle, Night Owl, etc.)
    bool has_hub_sensor = false;       ///< Unit has a hub/combiner sensor
    bool hub_sensor_triggered = false; ///< Filament detected at this unit's hub

    // Buffer health (AFC TurtleNeck — one buffer per unit, sits between hub and toolhead)
    std::optional<BufferHealth> buffer_health; ///< Buffer fault state (nullopt = no buffer data)

    // Per-unit topology (for mixed-topology setups like Box Turtle + OpenAMS)
    PathTopology topology = PathTopology::HUB; ///< Filament path topology for this unit

    /// Physical tool label for HUB units (e.g., 4 for extruder4/T4, 5 for extruder5/T5).
    /// -1 means "use min(mapped_tool) from lanes" (the default/PARALLEL behavior).
    int hub_tool_label = -1;

    /**
     * @brief Check if any slot in this unit has an error
     * @return true if at least one slot has error.has_value()
     */
    [[nodiscard]] bool has_any_error() const {
        return std::any_of(slots.begin(), slots.end(),
                           [](const SlotInfo& s) { return s.error.has_value(); });
    }

    /**
     * @brief Get slot by local index (within this unit)
     * @param local_index Index within this unit (0 to slot_count-1)
     * @return Pointer to slot info or nullptr if out of range
     */
    [[nodiscard]] const SlotInfo* get_slot(int local_index) const {
        if (local_index < 0 || local_index >= static_cast<int>(slots.size())) {
            return nullptr;
        }
        return &slots[local_index];
    }

    /**
     * @brief Get mutable slot by local index (within this unit)
     * @param local_index Index within this unit (0 to slot_count-1)
     * @return Pointer to slot info or nullptr if out of range
     */
    [[nodiscard]] SlotInfo* get_slot(int local_index) {
        if (local_index < 0 || local_index >= static_cast<int>(slots.size())) {
            return nullptr;
        }
        return &slots[local_index];
    }
};

// ============================================================================
// Spoolman Integration Mode (Happy Hare v4)
// ============================================================================

/**
 * @brief Spoolman integration mode for Happy Hare v4
 *
 * Happy Hare v4 supports 4 Spoolman integration modes:
 * - OFF: No Spoolman integration
 * - READONLY: Reads spool data but doesn't update
 * - PUSH: Pushes gate changes to Spoolman (sets active spool)
 * - PULL: Pulls spool assignments from Spoolman (Spoolman is source of truth)
 */
enum class SpoolmanMode { OFF = 0, READONLY = 1, PUSH = 2, PULL = 3 };

/**
 * @brief Convert SpoolmanMode to display string
 */
inline const char* spoolman_mode_to_string(SpoolmanMode mode) {
    switch (mode) {
    case SpoolmanMode::OFF:
        return "Off";
    case SpoolmanMode::READONLY:
        return "Read Only";
    case SpoolmanMode::PUSH:
        return "Push";
    case SpoolmanMode::PULL:
        return "Pull";
    default:
        return "Unknown";
    }
}

/**
 * @brief Parse SpoolmanMode from Happy Hare string value
 * @param str String from printer.mmu.spoolman_support
 * @return Corresponding SpoolmanMode
 */
inline SpoolmanMode spoolman_mode_from_string(std::string_view str) {
    if (str == "off" || str == "Off")
        return SpoolmanMode::OFF;
    if (str == "readonly" || str == "Read Only")
        return SpoolmanMode::READONLY;
    if (str == "push" || str == "Push")
        return SpoolmanMode::PUSH;
    if (str == "pull" || str == "Pull")
        return SpoolmanMode::PULL;
    return SpoolmanMode::OFF;
}

/// Encoder-based clog detection (Happy Hare mmu.encoder.*)
struct EncoderClogInfo {
    bool enabled = false;
    int flow_rate = -1;          // 0-100% (-1=unavailable)
    int detection_mode = 0;      // 0=unknown, 1=manual, 2=auto
    float desired_headroom = 0;  // target headroom (mm)
    float detection_length = 0;  // total detection distance (mm)
    float headroom = 0;          // current headroom (mm)
    float min_headroom = 0;      // minimum headroom reached (mm)

    /// Get clog percentage: 0=full headroom, 100=clogged
    [[nodiscard]] int get_clog_pct() const {
        if (detection_length <= 0) return 0;
        float used = detection_length - headroom;
        int pct = static_cast<int>((used / detection_length) * 100.0f + 0.5f);
        return std::clamp(pct, 0, 100);
    }

    /// Warning: min_headroom has dipped below desired_headroom
    [[nodiscard]] bool is_warning() const {
        return desired_headroom > 0 && min_headroom < desired_headroom;
    }
};

/// Flowguard clog/tangle detection (Happy Hare mmu.flowguard.*)
struct FlowguardInfo {
    bool enabled = false;
    bool active = false;
    std::string trigger;      // "CLOG", "TANGLE", or ""
    float level = 0;          // -1.0 (tangle) to +1.0 (clog)
    float max_clog = 0;
    float max_tangle = 0;     // negative value
};

/**
 * @brief Complete AMS system state
 *
 * This is the top-level structure containing all AMS information.
 */
struct AmsSystemInfo {
    AmsType type = AmsType::NONE;
    std::string type_name; ///< "Happy Hare", "AFC", etc.
    std::string version;   ///< System version string

    // Current state
    int current_tool = -1;              ///< Active tool (-1=none, -2=bypass for HH)
    int current_slot = -1;              ///< Active slot (-1=none, -2=bypass for HH)
    int pending_target_slot = -1;       ///< Target slot during tool change (-1=none)
    int current_toolchange = -1;        ///< Current tool change number (-1=none yet, 0-based)
    int number_of_toolchanges = 0;      ///< Total expected tool changes this print
    bool filament_loaded = false;       ///< Filament at extruder
    AmsAction action = AmsAction::IDLE; ///< Current operation
    std::string operation_detail;       ///< Detailed operation string

    // Units
    std::vector<AmsUnit> units; ///< All AMS units
    int total_slots = 0;        ///< Sum of all slots across units

    // Capability flags
    bool supports_endless_spool = false;
    bool supports_tool_mapping = false;
    bool supports_bypass = false;            ///< Has bypass selector position
    bool has_hardware_bypass_sensor = false; ///< true=auto-detect sensor, false=virtual/manual
    TipMethod tip_method = TipMethod::CUT;   ///< How filament tip is handled during unload
    bool supports_purge = false;             ///< Has purge capability after load

    // Happy Hare v4 extended status fields
    SpoolmanMode spoolman_mode = SpoolmanMode::OFF; ///< Spoolman integration mode
    int pending_spool_id = -1;                      ///< Pending spool assignment (v4)
    std::string espooler_state;                     ///< eSpooler state: "rewind"/"assist"/""
    std::string sync_feedback_state; ///< Sync feedback: "compressed"/"tension"/"neutral"/"disabled"
    bool sync_drive = false;         ///< Gear synced to extruder motor
    int clog_detection = 0;          ///< Clog detection: 0=off, 1=manual, 2=auto
    int encoder_flow_rate = -1;      ///< Encoder flow rate (-1=unavailable)
    EncoderClogInfo encoder_info;    ///< Encoder-based clog detection state
    FlowguardInfo flowguard_info;    ///< Flowguard clog/tangle detection state
    float sync_feedback_flow_rate = -1;    ///< Sync feedback flow rate
    float sync_feedback_bias = -2;         ///< Modelled bias [-1.0,1.0], -2=unavailable
    float sync_feedback_bias_raw = -2;     ///< Raw sensor bias [-1.0,1.0], -2=unavailable
    float toolchange_purge_volume = 0;     ///< Slicer purge volume for toolchanges

    // Tool-to-slot mapping (Happy Hare uses "gate" internally)
    std::vector<int> tool_to_slot_map; ///< tool_to_slot_map[tool] = slot

    /**
     * @brief Get slot by global index (across all units)
     * @param global_index Global slot index (0 to total_slots-1)
     * @return Pointer to slot info or nullptr if out of range
     */
    [[nodiscard]] const SlotInfo* get_slot_global(int global_index) const {
        for (const auto& unit : units) {
            if (global_index >= unit.first_slot_global_index &&
                global_index < unit.first_slot_global_index + unit.slot_count) {
                int local_idx = global_index - unit.first_slot_global_index;
                return unit.get_slot(local_idx);
            }
        }
        return nullptr;
    }

    /**
     * @brief Get mutable slot by global index (across all units)
     * @param global_index Global slot index (0 to total_slots-1)
     * @return Pointer to slot info or nullptr if out of range
     */
    [[nodiscard]] SlotInfo* get_slot_global(int global_index) {
        for (auto& unit : units) {
            if (global_index >= unit.first_slot_global_index &&
                global_index < unit.first_slot_global_index + unit.slot_count) {
                int local_idx = global_index - unit.first_slot_global_index;
                return unit.get_slot(local_idx);
            }
        }
        return nullptr;
    }

    /**
     * @brief Get the currently active slot info
     * @return Pointer to active slot or nullptr if none selected
     */
    [[nodiscard]] const SlotInfo* get_active_slot() const {
        if (current_slot < 0)
            return nullptr;
        return get_slot_global(current_slot);
    }

    /**
     * @brief Check if system is available and connected
     * @return true if AMS type is detected and has at least one unit
     */
    [[nodiscard]] bool is_available() const {
        return type != AmsType::NONE && !units.empty();
    }

    /**
     * @brief Check if an operation is in progress
     * @return true if actively loading, unloading, etc.
     */
    [[nodiscard]] bool is_busy() const {
        return action != AmsAction::IDLE && action != AmsAction::ERROR;
    }

    // === Multi-unit helpers ===

    /**
     * @brief Check if this is a multi-unit setup (2+ physical units)
     * @return true if more than one AmsUnit exists
     */
    [[nodiscard]] bool is_multi_unit() const {
        return units.size() > 1;
    }

    /**
     * @brief Get number of physical units
     * @return Number of AmsUnit entries
     */
    [[nodiscard]] int unit_count() const {
        return static_cast<int>(units.size());
    }

    /**
     * @brief Get the unit that contains a given global slot index
     * @param global_index Global slot index (0 to total_slots-1)
     * @return Pointer to containing AmsUnit or nullptr if out of range
     */
    [[nodiscard]] const AmsUnit* get_unit_for_slot(int global_index) const {
        for (const auto& unit : units) {
            if (global_index >= unit.first_slot_global_index &&
                global_index < unit.first_slot_global_index + unit.slot_count) {
                return &unit;
            }
        }
        return nullptr;
    }

    /**
     * @brief Get mutable unit that contains a given global slot index
     * @param global_index Global slot index (0 to total_slots-1)
     * @return Pointer to containing AmsUnit or nullptr if out of range
     */
    [[nodiscard]] AmsUnit* get_unit_for_slot(int global_index) {
        for (auto& unit : units) {
            if (global_index >= unit.first_slot_global_index &&
                global_index < unit.first_slot_global_index + unit.slot_count) {
                return &unit;
            }
        }
        return nullptr;
    }

    /**
     * @brief Get unit by index
     * @param unit_index Unit index (0 to unit_count()-1)
     * @return Pointer to AmsUnit or nullptr if out of range
     */
    [[nodiscard]] const AmsUnit* get_unit(int unit_index) const {
        if (unit_index < 0 || unit_index >= static_cast<int>(units.size())) {
            return nullptr;
        }
        return &units[unit_index];
    }

    /**
     * @brief Get the unit index that contains the currently active slot
     * @return Unit index (0-based) or -1 if no active slot
     */
    [[nodiscard]] int get_active_unit_index() const {
        if (current_slot < 0)
            return -1;
        const auto* unit = get_unit_for_slot(current_slot);
        if (!unit)
            return -1;
        return unit->unit_index;
    }
};

/**
 * @brief Filament requirement from G-code analysis
 *
 * Used for print preview to show which colors are needed.
 */
struct FilamentRequirement {
    int tool_index = -1;                         ///< Tool number from G-code (T0, T1, etc.)
    uint32_t color_rgb = AMS_DEFAULT_SLOT_COLOR; ///< Color hint from slicer
    std::string material;                        ///< Material hint from slicer (if available)
    int mapped_slot = -1;                        ///< Which slot is mapped to this tool

    /**
     * @brief Check if this requirement is satisfied by a slot
     * @return true if a slot is mapped to this tool
     */
    [[nodiscard]] bool is_satisfied() const {
        return mapped_slot >= 0;
    }
};

/**
 * @brief Print color requirements summary
 */
struct PrintColorInfo {
    std::vector<FilamentRequirement> requirements;
    int initial_tool = 0;       ///< First tool used in print
    bool all_satisfied = false; ///< All requirements have mapped slots
};

// ============================================================================
// Dryer Types (for AMS systems with integrated drying)
// ============================================================================

/**
 * @brief Preset drying profile
 *
 * Standard drying profiles for common filament materials.
 * Can be overridden via helixconfig.json "dryer_presets" array.
 */
struct DryingPreset {
    std::string name;       ///< Preset name (e.g., "PLA", "PETG", "ABS")
    float temp_c = 45.0f;   ///< Target temperature in Celsius
    int duration_min = 240; ///< Drying duration in minutes
    int fan_pct = 50;       ///< Fan speed percentage (0-100)

    /**
     * @brief Create a drying preset
     * @param n Preset name
     * @param t Temperature in Celsius
     * @param d Duration in minutes
     * @param f Fan speed percentage
     */
    DryingPreset(std::string n, float t, int d, int f = 50)
        : name(std::move(n)), temp_c(t), duration_min(d), fan_pct(f) {}
    DryingPreset() = default;
};

/**
 * @brief Dryer capability and state information
 *
 * Not all AMS systems have integrated dryers. Currently only ACE Pro (ValgACE)
 * has dryer support. This struct provides a generic interface that other
 * backends can implement when dryer hardware becomes available.
 */
struct DryerInfo {
    bool supported = false;           ///< Does this AMS have a dryer?
    bool active = false;              ///< Currently drying?
    bool allows_during_print = false; ///< Can run while printing? (backend capability)

    // Current state
    float current_temp_c = 0.0f; ///< Current chamber temperature
    float target_temp_c = 0.0f;  ///< Target temperature (0 = off)
    int duration_min = 0;        ///< Total drying duration set
    int remaining_min = 0;       ///< Minutes remaining
    int fan_pct = 0;             ///< Current fan speed (0-100)

    // Hardware capabilities
    float min_temp_c = 35.0f;          ///< Minimum settable temperature
    float max_temp_c = 70.0f;          ///< Maximum settable temperature
    int max_duration_min = 720;        ///< Maximum drying time (12h default)
    bool supports_fan_control = false; ///< Can fan speed be set independently?

    /**
     * @brief Get progress as percentage
     * @return 0-100 percentage, or -1 if not drying
     */
    [[nodiscard]] int get_progress_pct() const {
        if (!active || duration_min <= 0)
            return -1;
        int elapsed = duration_min - remaining_min;
        // Clamp to valid range (handles firmware reporting remaining > duration)
        if (elapsed < 0)
            elapsed = 0;
        if (elapsed > duration_min)
            elapsed = duration_min;
        return (elapsed * 100) / duration_min;
    }

    /**
     * @brief Check if dryer is at target temperature
     * @param tolerance_c Temperature tolerance in Celsius (default 2°C)
     * @return true if within tolerance of target
     */
    [[nodiscard]] bool is_at_temp(float tolerance_c = 2.0f) const {
        if (target_temp_c <= 0)
            return false;
        return std::abs(current_temp_c - target_temp_c) <= tolerance_c;
    }
};

/**
 * @brief Get default drying presets
 *
 * Returns presets derived from the filament database, one per compatibility group.
 * Uses filament::get_drying_presets_by_group() as the single source of truth.
 * These can be overridden via helixconfig.json "dryer_presets" array.
 *
 * @return Vector of default DryingPreset structs
 */
inline std::vector<DryingPreset> get_default_drying_presets() {
    constexpr int DEFAULT_FAN_PCT = 50;

    std::vector<DryingPreset> result;
    for (const auto& fp : filament::get_drying_presets_by_group()) {
        result.emplace_back(fp.name, static_cast<float>(fp.temp_c), fp.time_min, DEFAULT_FAN_PCT);
    }
    return result;
}

// ============================================================================
// Endless Spool Types
// ============================================================================

namespace helix::printer {

/**
 * @brief Capabilities for endless spool feature
 *
 * Describes whether endless spool is supported and whether the UI can modify
 * the configuration. Different backends have different capabilities:
 * - AFC: Fully editable, per-slot backup configuration
 * - Happy Hare: Read-only, group-based (configured via mmu_vars.cfg)
 * - Mock: Configurable for testing both modes
 */
struct EndlessSpoolCapabilities {
    bool supported = false; ///< Does backend support endless spool?
    bool editable = false;  ///< Can UI modify configuration?
    std::string
        description; ///< Human-readable description (e.g., "Per-slot backup", "Group-based")
};

/**
 * @brief Configuration for a single slot's endless spool backup
 *
 * Represents which slot will be used as a backup when the primary slot runs out.
 * This provides a unified view regardless of backend (AFC's runout_lane or
 * Happy Hare's endless_spool_groups).
 */
struct EndlessSpoolConfig {
    int slot_index = 0;   ///< Slot this config applies to
    int backup_slot = -1; ///< Backup slot index (-1 = no backup)
};

/**
 * @brief Capabilities for tool mapping feature
 *
 * Describes whether tool mapping is supported and whether the UI can modify
 * the configuration. Different backends have different capabilities:
 * - AFC: Fully editable, per-lane tool assignment via SET_MAP
 * - Happy Hare: Fully editable, tool-to-gate mapping via MMU_TTG_MAP
 * - Mock: Configurable for testing both modes
 * - ValgACE: Not supported (1:1 fixed mapping)
 * - ToolChanger: Not supported (tools ARE slots)
 */
struct ToolMappingCapabilities {
    bool supported = false;  ///< Does this backend support tool mapping?
    bool editable = false;   ///< Can the UI modify the mapping?
    std::string description; ///< UI hint text (e.g., "Per-lane tool assignment via SET_MAP")
};

/**
 * @brief Action type for dynamic device controls
 */
enum class ActionType {
    BUTTON,   ///< Simple action button
    TOGGLE,   ///< On/off toggle switch
    SLIDER,   ///< Value slider with min/max
    DROPDOWN, ///< Selection from options list
    INFO      ///< Read-only information display
};

/**
 * @brief Convert ActionType to string for display/debug
 */
inline const char* action_type_to_string(ActionType type) {
    switch (type) {
    case ActionType::BUTTON:
        return "Button";
    case ActionType::TOGGLE:
        return "Toggle";
    case ActionType::SLIDER:
        return "Slider";
    case ActionType::DROPDOWN:
        return "Dropdown";
    case ActionType::INFO:
        return "Info";
    default:
        return "Unknown";
    }
}

/**
 * @brief Section metadata for UI rendering
 *
 * Groups related device actions together in the UI.
 */
struct DeviceSection {
    std::string id;          ///< Section identifier (e.g., "calibration")
    std::string label;       ///< Display label (e.g., "Calibration")
    int display_order;       ///< Sort order (0 = first)
    std::string description; ///< Short description for settings row
};

/**
 * @brief Represents a single device-specific action
 *
 * Backends populate these to expose unique features without hardcoding in UI.
 */
struct DeviceAction {
    std::string id;                   ///< Unique action ID (e.g., "afc_calibration")
    std::string label;                ///< Display label
    std::string icon;                 ///< Icon name
    std::string section;              ///< Section ID this action belongs to
    std::string description;          ///< Optional tooltip/hint text
    ActionType type;                  ///< Control type
    std::any current_value;           ///< Current value (for toggles/sliders/dropdowns)
    std::vector<std::string> options; ///< Options for dropdown type
    float min_value = 0;              ///< Min value for slider type
    float max_value = 100;            ///< Max value for slider type
    std::string unit;                 ///< Display unit (e.g., "mm", "%")
    int slot_index = -1;              ///< If action is per-slot (-1 = system-wide)
    bool enabled = true;              ///< Whether action is currently available
    std::string disable_reason;       ///< Why disabled (if applicable)
};

} // namespace helix::printer
