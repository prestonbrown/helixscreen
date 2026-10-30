// include/cfs_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "hv/json.hpp"

/**
 * @file cfs_status_parse.h
 * @brief A stock CFS `box` frame and the small status objects around it, read
 *        into plain structs.
 *
 * Moonraker sends deltas, so every parsed sibling-object field is a
 * std::optional and an omitted or null one reads as nullopt. No AmsState. One
 * exception to purity: parse_stock_box resolves material codes through
 * FilamentCatalog::load_codes_cached, which reads the catalog file once behind its
 * own cache mutex, and it logs at debug level.
 */
namespace helix::printer {

/// Static CFS utility functions (code stripping, color parsing, TNN
/// addressing). Material codes resolve through FilamentCatalog::load_codes("cfs").
class CfsMaterialDb {
  public:
    /// Strip the firmware material_type brand-prefix digit (6-char code =
    /// prefix + 5-char catalog id): "101001" -> "01001" (K2, Creality prefix),
    /// "000003" -> "00003" (K1, Generic prefix), sentinels -> ""
    static std::string strip_code(const std::string& code);

    /// Parse CFS color: "0RRGGBB" -> 0xRRGGBB, sentinels -> 0x808080
    static uint32_t parse_color(const std::string& color_str);

    /// Global slot index -> TNN name: 0 -> "T1A", 4 -> "T2A"
    static std::string slot_to_tnn(int global_index);

    /// TNN name -> global slot index: "T1A" -> 0, "T2A" -> 4, invalid -> -1
    static int tnn_to_slot(const std::string& tnn);

    /// Default color for unknown/sentinel slots
    static constexpr uint32_t kDefaultColor = 0x808080;
};

} // namespace helix::printer

namespace helix::cfs {

/// Bays per CFS box. Both firmwares number a bay (box address - 1) * 4 + local.
constexpr int kBaysPerBox = 4;

/// What kind of `box` frame this is. Full frames carry `filament`/`map`/
/// `auto_refill`/`state` or a per-unit `T1`..`T4`; anything else (a bare
/// measuring_wheel tick) changes no state.
struct BoxFrameShape {
    bool has_top_level = false; ///< `filament`, `map`, `auto_refill` or `state`
    bool has_unit_data = false; ///< `T1`..`T4`
};

struct FilamentSensorDelta {
    /// Null until the sensor takes its first reading: no reading, not "no
    /// filament".
    std::optional<bool> filament_detected;
};

struct ExtruderTempDelta {
    std::optional<double> target_c;
    std::optional<double> temperature_c;
};

struct MotorControlDelta {
    std::optional<bool> motor_ready;
};

[[nodiscard]] BoxFrameShape classify_box_frame(const nlohmann::json& box);

/// Each returns nullopt when the frame carries no such object, and a delta with
/// every field it did not state left unset when it does.
[[nodiscard]] std::optional<FilamentSensorDelta>
parse_filament_sensor(const nlohmann::json& params);
[[nodiscard]] std::optional<ExtruderTempDelta> parse_extruder(const nlohmann::json& params);
[[nodiscard]] std::optional<MotorControlDelta> parse_motor_control(const nlohmann::json& params);

/// Whether printer.cfg's saved RS-485 address table names a unit. The K2 keeps
/// it in the SAVE_CONFIG block as `[auto_addr] mb_addr_table_uniids`, one entry
/// per address (`[0]` for an empty one, the unit's uuid otherwise), and it is
/// readable from configfile.settings before the bus discovery that confirms
/// it. Accepts the table parsed or as its config string; false when the
/// section or option is absent or unreadable.
[[nodiscard]] bool has_registered_unit(const nlohmann::json& configfile_settings);

/// True when a per-slot `vender` string carries no occupancy signal. The box
/// reports one of these for an empty bay; `"none"` (lowercase) is also the
/// synthetic default when the array is short. A REAL vendor name or the
/// present-but-unresolved `"unknown"` marker are NOT sentinels: both mean a
/// spool is seated.
[[nodiscard]] bool is_vender_sentinel(const std::string& v);

/// True when a raw per-slot `material_type` string carries no RFID payload.
/// Adds `"unknown"` to the vender sentinel set: for `material_type` it means
/// the reader has nothing for this bay, whereas for `vender` it means a tag IS
/// seated but its vendor did not resolve (prestonbrown/helixscreen#1077).
///
/// Deliberately NOT applied to `color_value`: that field is user-writable
/// (BOX_MODIFY_TN_DATA PART=color_value, from our identity push or the stock
/// LCD) and reads a real color on untagged bays, so it cannot tell a tagged bay
/// from an untagged one. `material_type` is user-writable too, but only ever
/// with a code the firmware itself reported, so a non-sentinel value means a
/// tag was read or the user labeled the bay.
[[nodiscard]] bool is_material_code_sentinel(const std::string& v);

/// The brand a stock bay's `vender` entry names, for a bay whose material
/// code the catalog does not resolve. A tagged bay's entry is the tag's
/// whole 40-character record, which names its supplier rather than being
/// a brand: Creality's code reads as Creality and any other as no brand.
/// `"unknown"` (a seated spool the reader did not resolve) reads as
/// Creality, a sentinel as no brand, and anything else is the box's own
/// vendor name.
[[nodiscard]] std::string brand_from_vender(const std::string& vender);

/// The Spoolman spool a stock bay's `vender` tag record names, or nullopt.
/// Tag writers such as CFTag put the id in the record's serial field
/// (chars 29-34) as six zero-padded decimal digits. Creality writes its
/// factory serial 000001 there, so 1 names no spool, and neither does 0,
/// a serial that is not all digits, or anything but a whole record.
[[nodiscard]] std::optional<int> tag_record_spoolman_id(std::string_view vender);

/// Order @p info's units by address and stand an absent unit in for every
/// address below the highest one that reported nothing, so a bay's global
/// index stays its firmware slot number whichever boxes are on the bus, and
/// state keyed by it stays on that bay. total_slots becomes the index span.
void reserve_address_gaps(AmsSystemInfo& info);

/// No tool routes through an absent unit's bay: the box is not there to feed it.
void unmap_absent_bays(AmsSystemInfo& info);

/// A stock (`T1`..`T4`) `box` object as the whole system it describes: units,
/// bays, tool map, auto-refill groups and the seated lane.
///
/// `own_labels` maps global slot index -> the material_type code THIS app
/// wrote to that bay. Firmware echoes it back forever, so without it a user's
/// own label reads as RFID tag payload and suppresses the untagged presence
/// fallback. nullptr discounts nothing.
[[nodiscard]] AmsSystemInfo
parse_stock_box(const nlohmann::json& box,
                const std::unordered_map<int, std::string>* own_labels = nullptr);

} // namespace helix::cfs
