// include/snapmaker_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snapmaker_print_preferences.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

/**
 * @file snapmaker_status_parse.h
 * @brief The U1's Moonraker status objects, read into plain structs.
 *
 * Moonraker sends deltas: a frame names only the fields that changed, so every
 * parsed field is a std::optional and an omitted one reads as nullopt. A parse
 * never resets state; the backend overlays what a frame carries onto what it
 * holds. Pure: no I/O, no locks, no AmsState.
 */
namespace helix {

/// What one status frame said about one extruder object.
struct ExtruderDelta {
    std::optional<std::string> state; ///< "PARKED", "ACTIVE", "ACTIVATING"
    std::optional<bool> park_pin;
    std::optional<bool> active_pin;
    std::optional<bool> activating_move;
    std::array<std::optional<float>, 3> extruder_offset;
    std::optional<int> switch_count;
    std::optional<int> retry_count;
    std::optional<int> error_count;
    /// The fitted nozzle, which the firmware keeps outside the config.
    std::optional<double> nozzle_diameter;         ///< mm
    std::optional<std::string> nozzle_volume_type; ///< "standard" | "high_flow"
};

/// Per-extruder tool state from Snapmaker custom Klipper fields
struct ExtruderToolState {
    std::string state;                                ///< e.g., "PARKED", "ACTIVE", "ACTIVATING"
    bool park_pin = false;                            ///< Tool is in park position
    bool active_pin = false;                          ///< Tool is in active position
    bool activating_move = false;                     ///< Tool change move in progress
    std::array<float, 3> extruder_offset = {0, 0, 0}; ///< XYZ offset
    int switch_count = 0;                             ///< Total tool changes for this extruder
    int retry_count = 0;                              ///< Tool change retries
    int error_count = 0;                              ///< Tool change errors
    /// The fitted nozzle as the head reports it; unset until it has.
    std::optional<double> nozzle_diameter;
    std::optional<std::string> nozzle_volume_type;

    /// Overlays the fields @p delta carries; an omitted field keeps its value.
    void apply(const ExtruderDelta& delta);
};

/// RFID tag data parsed from filament_detect info
struct SnapmakerRfidInfo {
    std::string main_type;         ///< e.g., "PLA", "PETG"
    std::string sub_type;          ///< e.g., "SnapSpeed", "Basic"
    std::string manufacturer;      ///< e.g., "Polymaker"
    std::string vendor;            ///< e.g., "Snapmaker"
    uint32_t color_rgb = 0x808080; ///< RGB color (ARGB masked to 0x00FFFFFF)
    int hotend_min_temp = 0;
    int hotend_max_temp = 0;
    int bed_temp = 0;
    int weight_g = 0; ///< Spool weight in grams
    /// A multi-colour spool's colours, COLOR_NUMS of RGB_1..RGB_5, as
    /// "#RRGGBB,#RRGGBB". Empty when the tag states one colour.
    std::string multi_color_hexes;
    /// Canonical string form of CARD_UID (e.g. "144,32,196,2"). Empty when no
    /// tag is present, the RFID reader is disabled, or the field is missing.
    /// Used by the override system as the hardware-event signal: a change
    /// means the physical spool was swapped.
    std::string uid;
    /// Whether CARD_UID names a real card: a nonzero integer, or an array with a
    /// nonzero byte. The firmware reports a scalar 0 or an all-zero array for no
    /// card (reader off, untagged spool, an endpoint write). nullopt when the
    /// entry carries no readable CARD_UID.
    std::optional<bool> has_card;
};

namespace snapmaker {

constexpr int kToolCount = 4;

/// The `filament_detect` object: the tag reader's per-channel answer.
struct FilamentDetectDelta {
    /// `info[i]`: the whole tag record for channel i, present when the frame
    /// carried an object for it. A record is a reading, replaced as a whole.
    std::array<std::optional<SnapmakerRfidInfo>, kToolCount> info;
    /// `state[i]`: the entrance/tag reader's raw state for channel i.
    std::array<std::optional<int>, kToolCount> state;
};

/// One lane's entry in a `filament_feed left` / `filament_feed right` object.
struct FeedChannelDelta {
    int lane = 0;
    std::optional<bool> filament_detected; ///< the port sensor
    std::optional<std::string> channel_state;
    std::optional<std::string> channel_action_state;
    std::optional<std::string> channel_error;
    std::optional<bool> module_exist;
    std::optional<bool> disable_auto;
};

/// The `print_task_config` object: the task manager's record of the print and
/// of what each head is configured with.
struct PrintTaskConfigDelta {
    PrintPreferences preferences;
    /// extruder_map_table: logical tool -> physical head. A non-integer or
    /// out-of-range head is -1 ("no opinion") rather than clamped: substituting
    /// head 0 would be identity read as truth.
    std::optional<std::vector<int>> extruder_map_table;
    /// extruders_used: the heads the current task uses.
    std::optional<std::vector<bool>> extruders_used;
    std::array<std::optional<bool>, kToolCount> filament_exist;
    std::array<std::optional<std::string>, kToolCount> filament_type;
    std::array<std::optional<std::string>, kToolCount> filament_vendor;
    /// The product line inside the type ("Basic", "SnapSpeed").
    std::array<std::optional<std::string>, kToolCount> filament_sub_type;
    /// Whether the head's entry came from a Snapmaker RFID spool.
    std::array<std::optional<bool>, kToolCount> filament_official;
    /// Hex RGBA, as the firmware spells it; see rgb_from_rgba_hex.
    std::array<std::optional<std::string>, kToolCount> filament_color_rgba;
};

/// The RGB a `filament_color_rgba` entry names (the lane-colour grammar: bare or
/// `#`-prefixed hex, with or without an alpha byte), or nullopt when it names
/// none.
[[nodiscard]] std::optional<uint32_t> rgb_from_rgba_hex(const std::string& hex);

/// One per-tool runout sensor object: `filament_motion_sensor e{N}_filament`
/// (the Snapmaker default) or the generic `filament_switch_sensor` form.
struct ToolheadSensorDelta {
    int tool = 0;
    std::string object; ///< the status key, for logs
    std::optional<bool> enabled;
    std::optional<bool> filament_detected;
};

/// Everything the backend reads from one status frame, parsed up front.
struct StatusDelta {
    /// extruder, extruder1 .. extruder3; nullopt when the frame has no such
    /// object.
    std::array<std::optional<ExtruderDelta>, kToolCount> extruders;
    /// toolhead.extruder, the carriage's authority on the picked tool.
    std::optional<std::string> toolhead_extruder;
    /// nullopt when the frame has no filament_detect object.
    std::optional<FilamentDetectDelta> filament_detect;
    /// Every lane entry the frame carried, in the order the backend applies
    /// them: `filament_feed left` then `filament_feed right`, each by lane.
    std::vector<FeedChannelDelta> feed_channels;
    /// The batch macro's `doing` save-variable; nullopt when the frame has no
    /// such object or no boolean in it.
    std::optional<bool> batch_doing;
    /// nullopt when the frame has no print_task_config object.
    std::optional<PrintTaskConfigDelta> print_task_config;
    /// The per-tool runout sensors the frame carried, in status-object order.
    std::vector<ToolheadSensorDelta> toolhead_sensors;
};

[[nodiscard]] ExtruderDelta parse_extruder_delta(const nlohmann::json& extruder);

[[nodiscard]] SnapmakerRfidInfo parse_rfid_info(const nlohmann::json& json);

[[nodiscard]] FilamentDetectDelta parse_filament_detect(const nlohmann::json& detect);

[[nodiscard]] std::vector<ToolheadSensorDelta> parse_toolhead_sensors(const nlohmann::json& status);

[[nodiscard]] PrintTaskConfigDelta parse_print_task_config(const nlohmann::json& status);

[[nodiscard]] std::vector<FeedChannelDelta> parse_feed_channels(const nlohmann::json& status);

/// @p batch_macro_object is the status key the AUTO_FEEDING_BATCH macro
/// publishes under ("gcode_macro " plus its config-case name), empty when the
/// firmware has no such macro.
[[nodiscard]] StatusDelta parse_status(const nlohmann::json& status,
                                       const std::string& batch_macro_object = {});

} // namespace snapmaker
} // namespace helix
