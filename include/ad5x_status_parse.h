// include/ad5x_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

/**
 * @file ad5x_status_parse.h
 * @brief The AD5X IFS's status objects, gcode responses and files, read into
 *        plain structs.
 *
 * Moonraker sends deltas, so a field a frame may omit is a std::optional and an
 * omitted one reads as nullopt. A parse never decides anything about slots, the
 * head or actions; AmsBackendAd5xIfs applies the result under its lock. Pure:
 * no I/O, no locks, no logging, no AmsState.
 */
namespace helix::ad5x {

constexpr int kNumPorts = 4;
constexpr int kToolMapSize = 16;

/// One slot of a GET_ZCOLOR answer, or of a zmod_color / ifs_materials object.
struct ZColorSlot {
    std::string material;
    std::string hex; // Empty for old-format zmod responses (no /HEX).
};

/// What one GET_ZCOLOR SILENT=1 / IFS_STATUS response, or one native status
/// frame, said about the IFS.
struct ZColorSilentResult {
    bool is_prompt_fallback = false; // Response was an action:prompt dialog
    bool is_old_format = false;      // Slot lines had no /HEX segment
    bool ifs_active = false;         // "IFS: True" in summary line
    bool saw_valid_response = false; // Matched at least one summary or slot line
    // True when a genuine GET_ZCOLOR summary or slot line was parsed (as
    // opposed to ONLY the IFS_STATUS JSON). Proves SILENT actually works on
    // this device, which retires the false prompt-demotion (#981).
    bool saw_silent_content = false;
    // True only when the GET_ZCOLOR "// Extruder: ..." summary line was parsed
    // (the line that feeds extruder_slot). Slot lines also set
    // saw_silent_content but carry no Extruder field, so this distinguishes a
    // definitive head reading (extruder_slot reflects "N" or "None") from a
    // frame that simply lacked the summary line — without it a slot-line-only
    // frame would have extruder_slot == nullopt and falsely clear head state.
    bool saw_extruder_summary = false;
    std::optional<int> current_channel;
    std::optional<int> extruder_slot; // 0-based, absent when "None"
    // Seated/engaged channel from IFS_STATUS "Chan" (1-based, 0 = none).
    // Distinct from current_channel (the stale "(N)" paren form, unused) and
    // from extruder_slot (the live "Extruder:" feed view, which reads "None"
    // while loaded-idle). Chan persists at the physically seated port, so it
    // is the seated-channel authority for active_tool_/current_slot.
    std::optional<int> ifs_chan;
    // IFS_STATUS "State", for the diagnostic log line only.
    std::optional<int> ifs_state;
    // Per-port presence from IFS_STATUS "Ports" (RS-485 silk sensors, 1 entry
    // per port, index 0 = port 1). The sensor-backed presence truth — present
    // whenever IFS_STATUS returns clean JSON, independent of the GET_ZCOLOR
    // SILENT slot lines and of the persisted ffmColor cache. When set, it is
    // the presence authority (apply_zcolor_result), which makes empty-channel
    // resurrection from a stale ffmColor structurally impossible (#981).
    std::optional<std::array<bool, kNumPorts>> ifs_ports;
    std::array<std::optional<ZColorSlot>, kNumPorts> slots;
};

/// Parse a GET_ZCOLOR SILENT=1 response, with the IFS_STATUS JSON line that
/// shares its buffer.
[[nodiscard]] ZColorSilentResult parse_zcolor_silent(const std::vector<std::string>& lines);

/// Read one IFS_STATUS-shaped JSON object (the dict cmd_IFS_STATUS dumps, and
/// the dict zmod_ifs.get_status() returns - same keys by construction) into
/// @p result.
void read_ifs_status_object(const nlohmann::json& obj, ZColorSilentResult& result);

/// Fold a `zmod_color` status object into @p result. Its slots[] carry the
/// same ID/Material/HEX the GET_ZCOLOR SILENT=1 slot lines do. FFMInfo.channel
/// is handed back in @p channel rather than applied.
/// @return true if the object carried usable slot content.
bool read_zmod_color_object(const nlohmann::json& obj, ZColorSilentResult& result,
                            std::optional<int>& channel);

/// Normalize a colour from the standalone module's `ifs_materials.slots`
/// ("#RRGGBB" or "#RGB", with or without the '#') into bare upper-case
/// 6-digit hex ("A03CF7"). Empty when the value is not a colour.
[[nodiscard]] std::string normalize_module_color_hex(const std::string& value);

/// Read one `ifs_materials` status object into @p result. Its `slots` dict
/// (STRING keys - Moonraker serialises dict keys that way) carries the same
/// material/colour data the zmod_color slots array does.
/// @return true if the object carried usable slot content.
bool read_ifs_materials_object(const nlohmann::json& obj, ZColorSilentResult& result);

/// The standalone module's `ifs` status object.
struct IfsModuleObject {
    /// loaded_channels as per-port presence; nullopt when absent or when any
    /// entry is not an integer. Channels outside 1..kNumPorts are ignored.
    std::optional<std::array<bool, kNumPorts>> loaded_ports;
    std::optional<int> active_channel; ///< 1-based, the zmod Chan equivalent
    std::optional<std::string> activity;
    std::optional<std::string> error;
};

[[nodiscard]] IfsModuleObject parse_ifs_module_object(const nlohmann::json& obj);

/// The sensor and extruder objects of one status frame.
struct SensorFrame {
    /// `filament_switch_sensor _ifs_port_sensor_<N>` for ports 1..kNumPorts.
    std::array<std::optional<bool>, kNumPorts> port_detected;
    /// The IFS motion sensor, stock section first, then the zmod namespace.
    std::optional<bool> motion_detected;
    /// The toolhead switch sensor: stock section, zmod namespace, then the
    /// standalone module's `toolhead`.
    std::optional<bool> head_detected;
    bool has_extruder = false; ///< the frame carried an `extruder` object
    std::optional<double> extruder_target;
    std::optional<double> extruder_temperature;
};

/// A sensor object counts only when it carries a boolean filament_detected:
/// Moonraker can return a stock section as an empty {} while the zmod
/// namespace carries the real reading.
[[nodiscard]] SensorFrame parse_sensor_frame(const nlohmann::json& status);

/// The `save_variables.variables` dict.
struct SaveVariables {
    /// "bambufy" or "less_waste", when the frame carries that plugin's
    /// `_colors` or `_tools` row (bambufy checked first).
    std::optional<std::string> prefix;
    /// `bambufy_custom_types` when it is an array: its non-empty strings.
    std::optional<std::vector<std::string>> custom_types;
    /// The standalone module's `ifs_at_hub`; 0 when absent or not an integer.
    int module_at_hub = 0;
    /// The module's `ifs_loaded` (1-based, 0 = none, out of range reads 0);
    /// nullopt when absent or not an integer.
    std::optional<int> module_loaded;

    // The plugin rows below are read under the effective prefix: this frame's
    // detection, else the one the caller already knows.

    /// Both plugins' `_tools` arrays are present and disagree.
    bool tools_conflict = false;
    /// `<prefix>_tools`: index = tool, value = port (1-4, 5 = unmapped).
    std::array<std::optional<int>, kToolMapSize> tools;
    /// lessWaste only: the first of `_colors` / `_types` that is an array
    /// shorter than kToolMapSize, and its length.
    std::optional<std::pair<std::string, size_t>> short_tool_list;
    std::optional<int> current_tool; ///< -1 = none, 0-15
    std::optional<bool> external;    ///< bypass mode
};

[[nodiscard]] SaveVariables parse_save_variables(const nlohmann::json& vars,
                                                 const std::string& known_prefix);

/// The `_IFS_VARS` macro's get_status() dict. Both variables accept a bool or
/// the jinja int form.
struct IfsVarsMacro {
    std::optional<bool> backup;
    std::optional<bool> unlock_after_boot;
};

/// nullopt for the empty dict Klipper returns for a macro that does not exist.
[[nodiscard]] std::optional<IfsVarsMacro> parse_ifs_vars_macro(const nlohmann::json& macro_status);

/// filament.json: per-material {filament_tube_length, filament_ifs_speed}.
struct FilamentEjectTable {
    /// The "default" entry, else 1000/1200.
    std::pair<int, int> default_params{1000, 1200};
    /// Each material's pair, missing fields resolved against default_params.
    std::map<std::string, std::pair<int, int>> materials;
};

/// nullopt when the body is not a JSON object.
[[nodiscard]] std::optional<FilamentEjectTable> parse_filament_json(const std::string& content);

/// One port of Adventurer5M.json's FFMInfo.
struct FfmPort {
    std::string hex;  ///< bare upper-case hex, "808080" when the file has none
    std::string type; ///< material, empty for none or the '?' sentinel
    /// ffmColor was non-empty: the pre-SILENT presence signal.
    bool has_color = false;
};

struct AdventurerFile {
    bool has_ffm = false; ///< the file has an FFMInfo object
    /// Ports 1..kNumPorts at index 0..3; nullopt when the file names neither
    /// ffmColor<N> nor ffmType<N>.
    std::array<std::optional<FfmPort>, kNumPorts> ports;
    /// FFMInfo.channel as written (1-based, 0 = none) when it is an integer.
    std::optional<int> channel;
};

/// nullopt when the body is not JSON.
[[nodiscard]] std::optional<AdventurerFile> parse_adventurer_json(const std::string& content);

/// The NAME tokens of `[zmod_ifs] filament_<NAME>: <TEMP>` lines in a user.cfg
/// body, as written (e.g. "PLA+").
[[nodiscard]] std::vector<std::string> parse_user_cfg_filament_types(const std::string& body);

} // namespace helix::ad5x
