// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace helix::printer {

/// Unified per-slot sensor state. Replaces AFC's LaneSensors and
/// Happy Hare's GateSensorState with a single struct usable by all backends.
struct SlotSensors {
    // AFC binary sensors
    bool prep = false;
    bool load = false;
    bool loaded_to_hub = false;

    // Happy Hare pre-gate sensor
    bool has_pre_gate_sensor = false;
    bool pre_gate_triggered = false;

    // AFC selector sensor. Only units with a physical selector (HTLF,
    // QuattroBox) publish `selector`; has_selector records whether the field
    // was ever seen so a Box Turtle isn't rendered as "selector clear".
    bool has_selector = false;
    bool selector = false;

    // AFC buffer/readiness
    std::string buffer_status;
    std::string filament_status;
    float dist_hub = 0.0f;

    /// Hex colour AFC drives the lane's status LED to. Firmware emits it as the
    /// second half of the same `get_filament_status()` split that produces
    /// filament_status, so it is that string's severity colour straight from
    /// the firmware rather than one we re-derive.
    std::string filament_status_led;

    /// Comma-separated names of the homing endstops configured on this lane
    /// (e.g. "load,hub,tool_start,tool_end,buffer_advance,buffer_trailing").
    /// Absent on lanes with no `_endstops` — a per-lane capability list.
    std::string endstops;
};

/// A single slot in the registry. Owns all per-slot state.
struct SlotEntry {
    int global_index = -1;
    int unit_index = -1;
    std::string backend_name; // "lane4" (AFC), "0" (HH) — for G-code

    SlotInfo info;
    SlotSensors sensors;
    int endless_spool_backup = -1;
};

/// Unit metadata in the registry.
struct RegistryUnit {
    std::string name;
    int first_slot = 0;
    int slot_count = 0;
};

/// Single source of truth for all slot-indexed state.
///
/// NOT thread-safe — callers must hold their own mutex.
/// No LVGL or Moonraker dependencies.
class SlotRegistry {
  public:
    // === Initialization ===
    void initialize(const std::string& unit_name, const std::vector<std::string>& slot_names);
    void
    initialize_units(const std::vector<std::pair<std::string, std::vector<std::string>>>& units);

    // === Reorganization (atomic) ===
    void
    reorganize(const std::vector<std::pair<std::string, std::vector<std::string>>>& unit_slot_map);

    // === Slot access ===
    int slot_count() const;
    bool is_valid_index(int global_index) const;
    const SlotEntry* get(int global_index) const;
    SlotEntry* get_mut(int global_index);
    const SlotEntry* find_by_name(const std::string& backend_name) const;
    SlotEntry* find_by_name_mut(const std::string& backend_name);
    int index_of(const std::string& backend_name) const;
    std::string name_of(int global_index) const;

    // === Unit access ===
    int unit_count() const;
    const RegistryUnit& unit(int unit_index) const;
    std::pair<int, int> unit_slot_range(int unit_index) const;
    int unit_for_slot(int global_index) const;

    // === Tool mapping ===

    /**
     * @brief Who wrote a tool mapping — us, or the printer.
     *
     * The registry is written from two directions that look identical once
     * stored: a backend's set_tool_mapping() updates it OPTIMISTICALLY before
     * the gcode is even sent, and the subscription parser updates it from what
     * the firmware actually reports. A reader comparing tool_map() against an
     * expected value therefore cannot tell "the printer confirmed this" from
     * "we asked for this and may have been refused" — which is exactly how a
     * restore that Klipper rejected looked successful (#1270).
     *
     * Firmware writes bump firmware_mapping_generation(); optimistic ones do
     * not. A caller that needs proof records the generation before sending and
     * waits for it to advance.
     */
    enum class MappingSource {
        Optimistic, ///< Our own intent, not yet confirmed by the printer
        Firmware,   ///< Parsed from what the printer reported
    };

    int tool_for_slot(int global_index) const;
    int slot_for_tool(int tool_number) const;
    void set_tool_mapping(int global_index, int tool_number,
                          MappingSource source = MappingSource::Optimistic);

    /**
     * @brief Monotonic count of FIRMWARE-sourced mapping writes.
     *
     * Advances only when the printer tells us a mapping, never when we write
     * our own intent. Never resets except with the registry itself, so a
     * caller can hold a value across an async round trip and compare.
     */
    [[nodiscard]] uint64_t firmware_mapping_generation() const;
    /// Clear a slot's tool mapping (mark unmapped). Also clears the reverse map
    /// entry if it still points at this slot. set_tool_mapping() rejects negative
    /// tool numbers, so this is the primitive for resetting to unmapped.
    void clear_tool_mapping(int global_index);
    /// Bulk replace of the whole forward map. Same source semantics as
    /// set_tool_mapping(): Happy Hare's ttg_map arrives this way, as one
    /// authoritative array per subscription update rather than per-slot deltas.
    void set_tool_map(const std::vector<int>& tool_to_slot,
                      MappingSource source = MappingSource::Optimistic);
    /// Forward map: tool_map()[tool] = global slot index, -1 for an unmapped
    /// tool. This is the exact vector build_system_info() copies into
    /// AmsSystemInfo::tool_to_slot_map; exposed on its own so a caller that only
    /// needs the mapping doesn't have to build (and copy) a whole snapshot.
    const std::vector<int>& tool_map() const;

    // === Endless spool ===
    int backup_for_slot(int global_index) const;
    void set_backup(int global_index, int backup_slot);
    /// Every slot's backup edge in one vector: `backup_edges()[slot]` is that
    /// slot's backup, or -1. Feed it to
    /// helix::printer::endless_spool_config_from_edges() - that pair is what
    /// replaced the identical build-a-config loop AFC and the mock each carried.
    std::vector<int> backup_edges() const;

    // === Snapshot ===
    AmsSystemInfo build_system_info() const;

    // === Lifecycle ===
    bool is_initialized() const;
    void clear();

  private:
    std::vector<SlotEntry> slots_;
    std::unordered_map<std::string, int> name_to_index_;
    std::vector<int> tool_to_slot_;
    std::vector<RegistryUnit> units_;
    bool initialized_ = false;
    uint64_t firmware_mapping_generation_ = 0;

    void rebuild_reverse_maps();
};

} // namespace helix::printer
