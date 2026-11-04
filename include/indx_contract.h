// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bondtech INDX: the one place that knows its macro contract.
//
// INDX publishes no tool-changer status object. The tool on the head, the
// latch, the tool count and the per-tool settings live in save_variables and
// in variables of Bondtech's upstream macros (BondtechAB/INDX, macros/*.cfg).
// Those macros are the contract: this module reads only the variables listed
// in docs/devel/FILAMENT_BACKEND_INDX.md and sends only the commands named
// there. A printer that lacks part of it is reported as not recognized, never
// guessed at.
//
// Pure functions, no I/O: AmsBackendIndx owns the state and the sends.

#include "json_fwd.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace helix {
class PrinterDiscovery;
}

namespace helix::indx {

/// Upstream ships T0..T3 and tells users to add more; the README's own
/// ceiling is ten tools.
inline constexpr int kMaxTools = 10;

/// One entry of `gcode_macro LOAD_FILAMENT.filament_presets`.
struct FilamentPreset {
    float temp_c = 0.0f;
    float density = 0.0f;
    float heat_capacity = 0.0f;

    // Spelled out: the ESP32 image builds as C++17, which has no defaulted comparisons.
    bool operator==(const FilamentPreset& o) const {
        return temp_c == o.temp_c && density == o.density && heat_capacity == o.heat_capacity;
    }
    bool operator!=(const FilamentPreset& o) const {
        return !(*this == o);
    }
};

/// What one status frame said. Every field is nullopt (or empty) when the
/// frame did not carry it: Moonraker republishes only what changed, so absence
/// is no news, never a clearing.
struct Reading {
    std::optional<int> active_tool;        ///< save_variables active_tool; -1 = empty head
    std::optional<int> seat_suspect;       ///< save_variables seat_suspect; -1 = clear
    std::optional<int> toolchange_count;   ///< save_variables toolchange_count
    std::optional<int> toolhead_state;     ///< _TOOLCHANGE_VARS.toolhead_state: 0 locked, 2 open
    std::optional<std::string> speed_mode; ///< _TOOLCHANGE_VARS.mode
    std::optional<int> tool_count;         ///< TOOL_POSITIONS.tool_count
    std::optional<std::set<int>> no_heat_tools; ///< TOOL_POSITIONS.no_heat_tools
    std::optional<bool> docks_calibrated;       ///< TOOL_POSITIONS.docks_calibrated
    /// LOAD_FILAMENT.filament_presets, keyed as the printer writes them.
    std::optional<std::map<std::string, FilamentPreset>> presets;
    /// T<n>.spool_id, where the T<n> macro declares one (not upstream; the
    /// variable exists only on macro sets that track a spool per tool).
    std::map<int, int> tool_spool_ids;

    bool operator==(const Reading& o) const {
        return active_tool == o.active_tool && seat_suspect == o.seat_suspect &&
               toolchange_count == o.toolchange_count && toolhead_state == o.toolhead_state &&
               speed_mode == o.speed_mode && tool_count == o.tool_count &&
               no_heat_tools == o.no_heat_tools && docks_calibrated == o.docks_calibrated &&
               presets == o.presets && tool_spool_ids == o.tool_spool_ids;
    }
    bool operator!=(const Reading& o) const {
        return !(*this == o);
    }
};

/// Pull a reading out of a status frame. nullopt when the frame held nothing
/// this module reads.
[[nodiscard]] std::optional<Reading> read_status(const nlohmann::json& status);

/// Fold @p update into @p into: every field the update carried replaces the
/// held one, every field it did not carry is kept.
void merge(Reading& into, const Reading& update);

/// Whether discovery found the INDX host module: `indx` in
/// printer.objects.list (mainline indx_klipper) or an `[indx]` section in
/// configfile.config (Kalico).
[[nodiscard]] bool present(const PrinterDiscovery& hw);

/// The macro surface a printer exposes, checked against the contract.
struct Contract {
    /// Everything required is there; nothing is sent to the printer otherwise.
    bool recognized = false;
    /// Required objects that are absent, in contract order, for logs and the
    /// error text.
    std::vector<std::string> missing;
    /// LOAD_FILAMENT is there to load a tool.
    bool has_load_filament = false;
    /// MODE_STEALTH, MODE_NORMAL and MODE_SPORT are all there.
    bool has_speed_modes = false;
    /// TC_CYCLE_ALL is there.
    bool has_cycle_all = false;
    /// MANUAL_TOOL_REMOVE and MANUAL_TOOL_SEAT are both there.
    bool has_manual_tool_recovery = false;
    /// MANUAL_TOOL_SEAT is there.
    bool has_manual_tool_seat = false;
    /// SET_TOOL_FILAMENT is there to update a tool's heater model.
    bool has_set_tool_filament = false;
};

/// The missing objects as one comma-separated line, for logs and the error.
[[nodiscard]] std::string missing_text(const Contract& contract);

/// Check the discovered objects against the contract.
[[nodiscard]] Contract check_contract(const PrinterDiscovery& hw);

/// Whether the printer's own tool-change macros switch the Spoolman active
/// spool: a T<n> or CHANGE_TOOL template calling SET_ACTIVE_SPOOL. Reads the
/// MacroParamCache, which discovery fills from configfile.
[[nodiscard]] bool macros_set_active_spool(const PrinterDiscovery& hw);

/// `variable_tool_count` from the TOOL_POSITIONS config (MacroParamCache), so
/// the slots exist before the first status frame. 0 when absent.
[[nodiscard]] int config_tool_count();

/// Status objects to subscribe, with field allowlists. Empty unless @p hw is
/// an INDX printer.
[[nodiscard]] nlohmann::json required_status_objects(const PrinterDiscovery& hw);

/// The swap command for @p tool: `T<n>` when that macro exists, so a user's
/// wrapper runs exactly as for a slicer change, else `CHANGE_TOOL TOOL=<n>`.
[[nodiscard]] std::string change_tool_gcode(const PrinterDiscovery& hw, int tool);

/// The park command.
[[nodiscard]] std::string park_gcode();

/// `LOAD_FILAMENT TOOL=<n> TYPE=<preset>` when @p material names a preset
/// (helix::match_material: case, then filament family), with the preset key as
/// the printer wrote it; else `LOAD_FILAMENT TOOL=<n> TEMP=<temp_c>`, which is
/// also the answer for a material that is not safe on a gcode line.
[[nodiscard]] std::string load_gcode(int tool, const std::string& material, int temp_c,
                                     const std::map<std::string, FilamentPreset>& presets);

/// Why the nozzle may not be heated, or empty when it may. INDX's own
/// M104/M109 refuse the same two cases; this is what a direct
/// SET_HEATER_TEMPERATURE would otherwise bypass.
[[nodiscard]] std::string heat_blocked_reason(int active_tool, const std::set<int>& no_heat_tools);

/// Why a tool change or park may not be sent now, or empty when it may.
[[nodiscard]] std::string motion_blocked_reason(const Contract& contract, const Reading& state);

/// "Tool 4 (T3)": the app's full tool name, safe off the main thread.
[[nodiscard]] std::string tool_text(int tool);

/// A heater calibration runs the full fit with the nozzle heating to 200C and
/// back; the rpc must outlive it.
inline constexpr uint32_t kCalibrationTimeoutMs = 1800000;

/// Tool-change speed, the upstream MODE_* macros.
enum class SpeedMode { Stealth, Normal, Sport };
inline constexpr std::array<SpeedMode, 3> kSpeedModes = {SpeedMode::Stealth, SpeedMode::Normal,
                                                         SpeedMode::Sport};

/// The dropdown option for @p mode, untranslated ("Stealth").
[[nodiscard]] const char* speed_mode_option(SpeedMode mode);

/// `_TOOLCHANGE_VARS.mode` or a dropdown option, in any case; nullopt for a
/// mode this module does not know.
[[nodiscard]] std::optional<SpeedMode> parse_speed_mode(std::string_view text);

/// `MODE_STEALTH`, `MODE_NORMAL` or `MODE_SPORT`.
[[nodiscard]] std::string speed_mode_gcode(SpeedMode mode);

/// Pick up and park every tool in turn; the head ends empty.
[[nodiscard]] std::string cycle_all_gcode();

/// Open the latch so the tool on the head can be taken off by hand.
[[nodiscard]] std::string manual_remove_gcode();

/// Lock the latch onto @p tool, held on the head by hand, and record it as mounted.
[[nodiscard]] std::string manual_seat_gcode(int tool);

/// The two fits of the induction heater's safety model.
enum class HeaterCalibration { Heater, Fan };

/// `INDX_CALIBRATE` or `INDX_FAN_CALIBRATE`.
[[nodiscard]] std::string calibration_gcode(HeaterCalibration which);

/// Why @p which may not start now, or empty when it may: the contract and a
/// half-seated tool first (uncalibrated docks do not matter: nothing docks),
/// then a mounted tool with a hotend, then for the heater fit a nozzle target
/// of zero.
[[nodiscard]] std::string calibration_blocked_reason(const Contract& contract, const Reading& state,
                                                     HeaterCalibration which,
                                                     double nozzle_target_c);

/// The preset key @p material names, spelled as the printer wrote it
/// (helix::match_material: case, then filament family); nullopt for no match
/// or a material that is not safe on a gcode line.
[[nodiscard]] std::optional<std::string>
preset_for_material(const std::string& material,
                    const std::map<std::string, FilamentPreset>& presets);

/// `SET_TOOL_FILAMENT TOOL=<n> DENSITY=<d> HEAT_CAPACITY=<hc>`.
[[nodiscard]] std::string set_tool_filament_gcode(int tool, const FilamentPreset& preset);

/// The guided flow that seats a tool held on the head by hand.
inline constexpr const char* kSeatByHandFlowId = "indx_seat_by_hand";

/// An INDX with a recognized contract whose macros can seat a tool by hand
/// (MANUAL_TOOL_SEAT): the one answer for the Tool Offsets entry and the
/// seat_by_hand device action.
[[nodiscard]] bool supports_seat_by_hand(const PrinterDiscovery& hw);

/// Confirm text before MANUAL_TOOL_SEAT for @p tool.
[[nodiscard]] std::string seat_confirm_message(int tool);

/// Confirm text before MANUAL_TOOL_REMOVE with @p tool on the head.
[[nodiscard]] std::string remove_confirm_message(int tool);

} // namespace helix::indx
