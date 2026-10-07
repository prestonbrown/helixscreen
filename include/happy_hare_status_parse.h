// include/happy_hare_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_translation.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hv/json.hpp"

/**
 * @file happy_hare_status_parse.h
 * @brief Happy Hare's `printer.mmu` status object, read into plain structs.
 *
 * Moonraker sends deltas, so every parsed field is a std::optional and an
 * omitted one reads as nullopt: a parse never resets state, the backend overlays
 * what a frame carries onto what it holds. A per-gate array is a
 * std::vector of optionals sized as the frame sent it. Pure: no locks, no
 * AmsState.
 */
namespace helix::happy_hare {

template <typename T> using GateArray = std::optional<std::vector<std::optional<T>>>;

/// The selector/filament state: where the MMU is and what it is doing.
struct MmuCoreDelta {
    std::optional<int> gate; ///< -1 = none, -2 = bypass
    std::optional<int> tool;
    std::optional<bool> filament_loaded; ///< from `filament`: "Loaded" / "Unloaded"
    std::optional<std::string> reason_for_pause;
    std::optional<std::string> action;
    std::optional<int> filament_pos;
    std::optional<int> bowden_progress; ///< clamped to [-1, 100]
    std::optional<bool> has_bypass;
};

/// How many gates there are and how they split across units.
struct MmuTopologyDelta {
    std::optional<int> num_units; ///< at least 1
    /// Per-unit gate counts, from `unit_gate_counts` when it names any, else
    /// from `num_gates` (a "6,4" string, an integer or an array).
    std::optional<std::vector<int>> gate_counts;
    std::optional<int> active_unit;
    std::optional<std::vector<int>> ttg_map; ///< tool -> gate
};

/// What the MMU says about each gate's spool, one array per key.
struct GateIdentityDelta {
    GateArray<int> gate_status; ///< -1 unknown, 0 empty, 1 available, 2 from buffer
    /// gate_color_rgb: 0xRRGGBB integers, or [r, g, b] floats in 0..1.
    GateArray<uint32_t> color_rgb;
    GateArray<ams::ColorReading> color; ///< gate_color hex strings
    GateArray<std::string> material;
    GateArray<int> spool_id;
    GateArray<int> temperature;
    GateArray<std::string> name;          ///< gate_name
    GateArray<std::string> filament_name; ///< gate_filament_name (EMU)
    GateArray<int> endless_spool_group;
};

struct EncoderDelta {
    std::optional<int> flow_rate;
    std::optional<float> desired_headroom;
    std::optional<float> detection_length;
    std::optional<float> headroom;
    std::optional<float> min_headroom;
};

struct FlowguardDelta {
    std::optional<bool> enabled;
    std::optional<bool> active;
    std::optional<std::string> trigger;
    std::optional<float> level;
    std::optional<float> max_clog;
    std::optional<float> max_tangle;
    std::optional<int> encoder_mode;
    /// The object carries buffer FlowGuard's own readings (level, trigger,
    /// max_clog). An encoder-only v4 unit publishes just {active, enabled,
    /// encoder_mode}: the encoder's clog detection, not buffer FlowGuard.
    bool buffer_data = false;
};

/// The v4 extended status: eSpooler, sync feedback, clog detection, counters.
struct MmuTelemetryDelta {
    std::optional<std::string> espooler_active; ///< v3; v4 keeps it as a deprecated alias
    /// v4 `espooler`: one operation per gate ('', off, rewind, assist, print).
    /// A non-string entry reads as ''.
    std::optional<std::vector<std::string>> espooler;
    std::optional<std::string> sync_feedback_state;
    std::optional<float> sync_feedback_bias; ///< sync_feedback_bias_modelled
    std::optional<float> sync_feedback_bias_raw;
    std::optional<bool> sync_drive;
    std::optional<int> clog_detection_enabled;
    std::optional<EncoderDelta> encoder;
    std::optional<FlowguardDelta> flowguard;
    std::optional<std::string> led_exit_effect; ///< leds.unit0.exit_effect
    std::optional<float> sync_feedback_flow_rate;
    std::optional<float> toolchange_purge_volume;
    /// num_toolchanges as a 0-based index of the current change, -1 for none.
    std::optional<int> current_toolchange;
    /// slicer_tool_map.total_toolchanges; 0 when the object is present without
    /// one.
    std::optional<int> number_of_toolchanges;
    std::optional<SpoolmanMode> spoolman_mode;
    std::optional<int> pending_spool_id;
    /// Fields published as JSON null. v4 sends these for a selected unit that
    /// has no buffer (sync feedback, flowguard) or no encoder; v3 never does.
    bool sync_feedback_bias_null = false;
    bool sync_feedback_bias_raw_null = false;
    bool flowguard_null = false;
    bool encoder_null = false;
    /// `tangle_prevention` was in the frame (null or not): only v4 publishes it.
    bool v4_marker = false;
};

/// `sensors`: the pre-gate sensor readings.
struct MmuSensorsDelta {
    /// Per-gate entries in object order: gate index and whether the sensor is
    /// triggered (null and non-booleans read as not triggered). v3 names them
    /// `mmu_pre_gate_N`; v4 names them `mmu_entry_N`, and lists them only while
    /// no gate is selected.
    std::vector<std::pair<int, bool>> pre_gate;
    /// The aggregate `mmu_pre_gate` of EMU boxes, which only knows the active
    /// gate.
    std::optional<bool> aggregate_pre_gate;
    /// Whether the toolhead / extruder-entry sensors are fitted and enabled:
    /// the dict carries their key whenever they are fitted, null when disabled.
    bool has_toolhead_sensor = false;
    bool has_extruder_sensor = false;
};

struct DryingObjectDelta {
    std::optional<bool> active;
    std::optional<float> current_temp_c;
    std::optional<float> target_temp_c;
    std::optional<int> remaining_min;
    std::optional<int> duration_min;
    std::optional<int> fan_pct;
};

/// `drying_state`: an object (KMS) or one string per gate (EMU).
struct DryingDelta {
    std::optional<DryingObjectDelta> object;
    std::optional<std::vector<std::string>> per_gate;
};

struct MmuStatusDelta {
    MmuCoreDelta core;
    MmuTopologyDelta topology;
    GateIdentityDelta identity;
    MmuTelemetryDelta telemetry;
    std::optional<MmuSensorsDelta> sensors;
    std::optional<DryingDelta> drying;
    /// The endless-spool ENABLE bit, from `endless_spool_enabled` or its older
    /// spelling `endless_spool`.
    std::optional<bool> endless_spool_enabled;
};

/// What the connect-time `mmu_machine` / `configfile.settings` pair says about
/// how an install is laid out.
///
/// Happy Hare 3 keeps every tunable on `[mmu]`. Happy Hare 4 has no `[mmu]`: it
/// splits the tunables across `[mmu_parameters]` (machine-wide), one
/// `[mmu_unit_parameters <unit>]` per unit and `[mmu_toolhead <name>]`, and
/// publishes `happy_hare_version` on `mmu_machine`, which v3 never does.
struct MachineLayout {
    std::string version;             ///< happy_hare_version; empty when neither source names one
    double version_number = 0;       ///< the version as a number (3.42, 4.0); 0 when unknown
    bool v4 = false;                 ///< version 4 or later: the split layout
    int num_units = 1;               ///< mmu_machine.num_units, v4 only
    std::string unit_params_section; ///< "mmu_unit_parameters <unit 0>", v4 only
    std::string toolhead_section;    ///< "mmu_toolhead <name>" unit 0 uses, v4 only
    /// Whether any unit has a bypass, from mmu_machine.unit_N.has_bypass. v4
    /// only: v4 publishes printer.mmu.has_bypass as a constant true.
    std::optional<bool> has_bypass;
};

/// One unit's machine fields. v3 and v4 both publish them on the live
/// `mmu_machine` object as `unit_0`, `unit_1`, ...; an older v3 has them only on
/// configfile's `[mmu_machine]`, read as a single unit.
struct MachineUnit {
    std::string display_name;                     ///< v4 `display_name`; empty when not published
    std::string selector_type;                    ///< e.g. "VirtualSelector" (Type B)
    int first_gate = -1;                          ///< -1 when not published
    int num_gates = 0;                            ///< 0 when not published
    std::string filament_heater;                  ///< shared enclosure heater
    std::string environment_sensor;               ///< shared enclosure sensor
    std::vector<std::string> filament_heaters;    ///< one per gate of THIS unit
    std::vector<std::string> environment_sensors; ///< one per gate of THIS unit
    std::optional<bool> has_bypass;
    bool filament_always_gripped = false;
    std::optional<bool> filament_buffer;
    /// From the unit's configfile `[mmu_unit <name>] encoder`; nullopt when the
    /// install does not say (v3)
    std::optional<bool> has_encoder;
};

/// A per-unit capability v4 checks before it accepts a command or a
/// MMU_TEST_CONFIG parameter.
enum class UnitFeature {
    Servo,          ///< MMU_SERVO
    SelectorSpeed,  ///< selector_move_speed
    Encoder,        ///< encoder calibration, gate calibration, encoder clog mode
    SyncToExtruder, ///< sync_to_extruder (v4: not on an always-gripped unit)
    FilamentBuffer, ///< gear_from_filament_buffer_speed (v4)
};

/// Whether @p unit has @p feature. v3 knows only Type A from Type B, so there
/// every selector feature means "not a VirtualSelector" and the v4-only guards
/// always pass. An unknown selector type passes.
[[nodiscard]] bool unit_supports(const MachineUnit& unit, UnitFeature feature, bool v4);

/// Every unit in order. Lists come as a JSON array or a comma-separated string.
[[nodiscard]] std::vector<MachineUnit> read_machine_units(const nlohmann::json& settings,
                                                          const nlohmann::json& live_mmu_machine);

/// Enclosure heaters or environment sensors across every unit: one shared name
/// when every unit uses the same one, else one entry per gate in global gate
/// order ("" for a gate with none). A single unit keeps its own form.
struct UnitObjects {
    std::string shared;
    std::vector<std::string> per_gate;
};
enum class UnitObjectKind { Heater, EnvironmentSensor };
[[nodiscard]] UnitObjects collect_unit_objects(const std::vector<MachineUnit>& units,
                                               UnitObjectKind kind);

/// @param settings         configfile.settings (may be empty)
/// @param live_mmu_machine the live `mmu_machine` status object (may be empty)
[[nodiscard]] MachineLayout read_machine_layout(const nlohmann::json& settings,
                                                const nlohmann::json& live_mmu_machine);

/// The name the install @p layout describes accepts for the tunable @p key,
/// which callers spell the way HelixScreen does. Clog detection is named three
/// ways: ENABLE_CLOG_DETECTION / MMU_CALIBRATION_CLOG_LENGTH before 3.42,
/// FLOWGUARD_ENCODER_MODE / FLOWGUARD_ENCODER_MAX_MOTION from 3.42 on. Empty
/// when that version has no such parameter.
[[nodiscard]] std::string_view param_name(std::string_view key, const MachineLayout& layout);

/// Whether v4 keeps tunable @p key (v3 spelling) per unit, so a multi-unit
/// MMU_TEST_CONFIG setting it needs UNIT=.
[[nodiscard]] bool param_is_per_unit(std::string_view key);

/// The configfile value of the tunable @p key (v3 spelling) from whichever
/// section @p layout keeps it in, or nullptr.
[[nodiscard]] const nlohmann::json* find_config_param(const nlohmann::json& settings,
                                                      const MachineLayout& layout,
                                                      std::string_view key);

/// A configfile number: Klipper reports parsed settings as numbers, and
/// hand-written settings carry numeric strings.
[[nodiscard]] std::optional<float> read_config_number(const nlohmann::json* v);

/// One gate's `filament_switch_sensor mmu_entry_<gate>` object. v4's
/// printer.mmu.sensors covers only the selected gate, so these Klipper objects
/// are the per-gate source there. Fields are nullopt when the frame omits them.
struct EntrySensorReading {
    int gate = -1; ///< global gate index
    std::optional<bool> detected;
    std::optional<bool> enabled;
};

/// Every `filament_switch_sensor mmu_entry_<N>` object in a status notification's
/// params, in object order.
[[nodiscard]] std::vector<EntrySensorReading>
parse_entry_sensor_objects(const nlohmann::json& params);

[[nodiscard]] MmuCoreDelta parse_core(const nlohmann::json& mmu);
[[nodiscard]] MmuTopologyDelta parse_topology(const nlohmann::json& mmu);
[[nodiscard]] GateIdentityDelta parse_gate_identity(const nlohmann::json& mmu);
[[nodiscard]] MmuTelemetryDelta parse_telemetry(const nlohmann::json& mmu);
[[nodiscard]] MmuStatusDelta parse_mmu_status(const nlohmann::json& mmu);

} // namespace helix::happy_hare
