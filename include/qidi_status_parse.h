// include/qidi_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hv/json.hpp"

/**
 * @file qidi_status_parse.h
 * @brief The QIDI Box's save_variables, status objects and filament table,
 *        read into plain structs.
 *
 * Moonraker sends deltas, so a field a frame may omit is a std::optional and an
 * omitted one reads as nullopt. A parse decides nothing about slots or actions;
 * AmsBackendQidi applies the result under its lock. Pure: no I/O, no locks, no
 * logging, no AmsState.
 */
namespace helix::qidi {

constexpr int kSlotsPerBox = 4;
constexpr int kMaxBoxes = 4;
constexpr int kMaxSlots = kSlotsPerBox * kMaxBoxes;

[[nodiscard]] bool is_all_digits(std::string_view s);

/// A filament_slot<N> string product id ("QFA90", "QFL30"): two to four
/// capital letters then two or three digits, surrounding whitespace trimmed.
/// Digits alone are a table row id, not a product id.
[[nodiscard]] std::optional<std::string> read_filament_code(const nlohmann::json& val);

/// What one frame's save_variables said about one slot's identity keys.
struct SlotIdentity {
    /// filament_slot<N> as a product id; when set, filament_id is not.
    std::optional<std::string> filament_code;
    /// filament_slot<N> as a palette row id (0 = unset).
    std::optional<int> filament_id;
    /// color_slot<N> was a colour list; color_rgb is its first entry's RGB,
    /// empty for an empty list or one whose first entry is not a colour.
    bool color_list = false;
    std::optional<std::uint32_t> color_rgb;
    /// color_slot<N> as a palette row id (0 = unset), when it is not a list.
    std::optional<int> color_id;
    std::optional<int> vendor_id;
};

/// The `save_variables.variables` dict.
struct SaveVariables {
    /// box_count when it is an integer in 0..kMaxBoxes.
    std::optional<int> box_count;
    /// enable_box when it is an integer.
    std::optional<bool> enable_box;
    /// value_t<T> = "slot<M>": (T, M) in tool order, for T and M inside the
    /// effective slot count.
    std::vector<std::pair<int, int>> tool_slots;
    /// slot<N>: the Box's integer state word.
    std::array<std::optional<int>, kMaxSlots> slot_state;
    /// The frame carried is_tool_change or any slot<N> key, of any type.
    bool has_slot_or_action_key = false;
    /// is_tool_change is a non-zero integer.
    bool tool_change = false;
    /// last_load_slot: -1 for "slot-1" (nothing loaded), else the slot;
    /// nullopt when absent, not a string, or naming no slot.
    std::optional<int> last_load_slot;
    std::array<SlotIdentity, kMaxSlots> ids;
    /// Identity keys whose value is no id at all (negative, or not an
    /// integer); the previously stated id stands.
    std::vector<std::string> ignored_id_keys;
};

/// @p known_slot_count is the slot count before this frame; a valid box_count
/// in the frame overrides it.
[[nodiscard]] SaveVariables parse_save_variables(const nlohmann::json& variables,
                                                 int known_slot_count);

/// What one multi_color_controller frame said about one slot:
/// `box.box<B>_filament_info_<I>_<field>`.
struct ControllerSlotDelta {
    /// filament_id as a product id, empty when it is not one.
    std::optional<std::string> filament_id;
    std::optional<std::string> filament_type;
    std::optional<std::string> vendor;
    std::optional<int> min_temp;
    std::optional<int> max_temp;
};

/// Keyed by global slot. Empty when the object has no `box` dict.
[[nodiscard]] std::map<int, ControllerSlotDelta>
parse_controller_status(const nlohmann::json& controller);

/// `box<N>` in an object key -> unit index N-1, for N in 1..kMaxBoxes.
[[nodiscard]] std::optional<int> parse_box_unit_index(const std::string& key);

/// box_extras.box_drying_state: (unit index, end_time epoch; 0 when absent),
/// in key order. nullopt when the frame has no box_drying_state object, which
/// is also what says the firmware owns no drying timer.
[[nodiscard]] std::optional<std::vector<std::pair<int, std::int64_t>>>
parse_box_drying_state(const nlohmann::json& box_extras);

/// The parts of printer.configfile.settings the Box reads.
struct ConfigSettings {
    /// The settable dryer ceiling, from heater_generic heater_box<N>.max_temp or
    /// box_config box<N>.target_max_temp_heater_generic (the last one wins).
    std::optional<float> dryer_max_temp;
    /// A [multi_color_controller] section, bare or instanced (#1083).
    bool has_multi_color = false;
    /// [force_move] enable_force_move.
    bool force_move = false;
};

[[nodiscard]] ConfigSettings parse_config_settings(const nlohmann::json& settings);

/// One box's environment readings from a status frame; the highest of several
/// sources wins.
struct BoxReading {
    std::optional<float> temp;
    std::optional<float> humidity;
    std::optional<float> target;
};

/// Temperature comes from `heater_generic heater_box<N>` and `aht20_f
/// heater_box<N>` (the `temperature_sensor heater_temp_*_box<N>` thermistors
/// mirror the heater element and are excluded); target from the heater only;
/// humidity from any of the three.
[[nodiscard]] std::array<BoxReading, kMaxBoxes>
parse_heater_status(const nlohmann::json& notification);

/// One `[fila<N>]` section of officiall_filas_list.cfg.
struct FilaProfile {
    std::string name; ///< Human label from `filament` (e.g. "PLA Rapido")
    std::string type; ///< Material class from `type` (e.g. "PLA", "ABS-GF")
    int nozzle_min = 0;
    int nozzle_max = 0;
    int box_min = 0;
    int box_max = 0;
};

struct FilasList {
    std::map<int, FilaProfile> profiles; ///< [fila<N>], N > 0
    std::map<int, std::uint32_t> colors; ///< [colordict] id -> 0xRRGGBB
    std::map<int, std::string> vendors;  ///< [vendor_list] id -> name
};

/// A minimal ConfigParser-compatible read of officiall_filas_list.cfg.
[[nodiscard]] FilasList parse_filas_list(const std::string& content);

} // namespace helix::qidi
