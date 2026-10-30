// include/ace_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ace_slot_parse.h"
#include "ams_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

/**
 * @file ace_status_parse.h
 * @brief The ACE backend's status objects and REST bridge responses, read into
 *        plain structs.
 *
 * Two producers: the Klipper object (`ace`, `filament_hub`, the ACEPRO
 * driver's `ace_instance_N` and manager) and the REST bridge's
 * `/server/ace/{info,status,slots}`. Notify frames carry only the fields that
 * changed, so every field a frame may omit is a std::optional and an omitted
 * one reads as nullopt. Pure: no I/O, no locks, no AmsState.
 */
namespace helix::ace {

/// An ACE Pro or ACE 2 has four slots; the ACEPRO driver numbers tools four per unit.
inline constexpr int kSlotsPerUnit = 4;
/// Far beyond any daisy chain; bounds what a malformed `ace_instance_N` name
/// can allocate.
inline constexpr int kMaxUnits = 16;
/// A `slots` array longer than this is refused whole as malformed.
inline constexpr std::size_t kMaxSlotsPerObject = 16;

/// The driver's per-slot `park_state`. "parked" is a strand the driver left
/// staged short of the hub: after a sensor-verified unload or smart load, but
/// also after a fixed-length toolhead-only unload, which need not reach the
/// hub. "loaded" and "unknown" (also a missing field, and any string this
/// backend does not know) carry no staging claim.
enum class ParkState { Unknown, Parked, Loaded };

/// What one slot entry states. Every producer sends the `slots` array whole,
/// so an entry describes its whole bay; the optionals are fields a driver may
/// not send at all.
struct SlotDelta {
    /// `status` as the driver spelled it.
    std::optional<std::string> status_word;
    /// status_word through slot_status_from_string.
    std::optional<SlotStatus> status;
    /// A readable colour; see parse_slot_color.
    std::optional<uint32_t> color;
    /// See read_slot_material.
    std::optional<std::string> material;
    /// The driver tool the slot feeds; -1 when the entry names none.
    int tool = -1;
    TagRead tag_read = TagRead::Silent;
    ParkState park_state = ParkState::Unknown;
    /// `temp_min` / `temp_max`, a number or a numeric string.
    std::optional<int> temp_min;
    std::optional<int> temp_max;
};

/// One slot entry. A non-object entry still occupies its bay's index, so the
/// array reader keeps it as nullopt.
[[nodiscard]] SlotDelta read_slot(const nlohmann::json& slot_json);

/// An object's `slots` array.
struct SlotArray {
    /// One entry per array element, nullopt where the element is not an
    /// object. nullopt when `slots` is absent, not an array, or refused.
    std::optional<std::vector<std::optional<SlotDelta>>> entries;
    /// The length of an array refused for exceeding kMaxSlotsPerObject; 0
    /// otherwise.
    std::size_t refused_size = 0;
};

[[nodiscard]] SlotArray read_slots(const nlohmann::json& data);

/// The manager's two path sensors: `rdm_sensor` at the hub, `toolhead_sensor`
/// at the extruder.
struct PathSensors {
    std::optional<bool> rdm;
    std::optional<bool> toolhead;

    [[nodiscard]] bool stated() const {
        return rdm.has_value() || toolhead.has_value();
    }
};

[[nodiscard]] PathSensors read_path_sensors(const nlohmann::json& data);

/// One Klipper status object: a ValgACE `ace`, a native `filament_hub`, an
/// ACEPRO `ace_instance_N`, or the ACEPRO manager.
struct ObjectDelta {
    std::optional<std::string> model;
    std::optional<std::string> protocol;
    std::optional<std::string> firmware;
    /// The unit's `status` word through ams_action_from_string.
    std::optional<AmsAction> action;
    SlotArray slots;
    /// The first `slots` entry stating "loaded", the ValgACE seat. Read from
    /// the raw array, so a refused array still names it.
    std::optional<std::size_t> first_loaded_entry;
    /// The ACEPRO unit link `connection_state` states; nullopt for a state this
    /// reader does not know.
    std::optional<bool> linked;
    DryerDelta dryer;
    /// An ACE 2's humidity; an ACE Pro states null.
    std::optional<float> humidity;
    std::optional<int> loaded_slot;
    /// Native GoKlipper's `current_filament` ("<unitId>-<localIndex>"): the
    /// local index, when the string names one that is not negative.
    std::optional<int> current_filament_slot;
    /// The ACEPRO manager's seat: a global tool index, -1 for nothing loaded.
    std::optional<int> current_index;
    PathSensors sensors;
    std::optional<int> target_index;
    std::optional<bool> ace_pro_enabled;
    std::optional<int> feed_assist_slot;
    std::optional<bool> endless_spool_enabled;
    std::optional<std::string> endless_spool_match_mode;
};

[[nodiscard]] ObjectDelta parse_ace_object(const nlohmann::json& data);

/// The REST bridge's `/server/ace/info`.
struct InfoResponse {
    bool names_model = false;
    std::optional<std::string> version;
    /// `slot_count`, when it is in [0, kMaxSlotsPerObject].
    std::optional<int> slot_count;
    /// An integer `slot_count` out of that range: the response is malformed.
    std::optional<int> refused_slot_count;
};

[[nodiscard]] InfoResponse parse_info_response(const nlohmann::json& data);

/// The REST bridge's `/server/ace/status`: one instance's status plus the
/// ACEPRO bridge's envelope.
struct StatusResponse {
    std::optional<std::string> model;
    std::optional<std::string> firmware;
    std::optional<int> loaded_slot;
    /// The response carries an `ace_manager` key: an ACEPRO bridge.
    bool names_manager = false;
    /// `instance_index`, when not negative.
    std::optional<int> instance_index;
    /// `ace_manager` is an object, so its target index is read whole.
    bool manager_object = false;
    std::optional<int> manager_current_index;
    PathSensors manager_sensors;
    std::optional<int> manager_target_index;
    /// The bridge's `action`, else the unit's `status`; nullopt when the chosen
    /// field is empty or neither is a string.
    std::optional<AmsAction> action;
    /// The response has no `action` key, so `action` came from the unit's
    /// status, which stays "ready" through a toolchange.
    bool action_from_unit_status = false;
    DryerDelta dryer;
};

[[nodiscard]] StatusResponse parse_status_response(const nlohmann::json& data);

} // namespace helix::ace
