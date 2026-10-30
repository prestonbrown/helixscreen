// src/printer/cfs_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cfs_status_parse.h"

#include "ams_status_json.h"
#include "ams_tool_map_sync.h"
#include "filament_catalog.h"
#include "json_utils.h"
#include "lane_translation.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace helix::printer {

std::string CfsMaterialDb::strip_code(const std::string& code) {
    if (code == "-1" || code == "None" || code.empty())
        return "";
    // Firmware reports material_type as a 6-char code: one prefix digit
    // (brand class — '1' Creality on K2, '0' Generic on K1) + the 5-char
    // catalog id. The K1 half of the rule comes from the #968 reporter dumps
    // (tn_data.json "000003" resolves to catalog id "00003" = Generic PETG,
    // "000001" = Generic PLA); the original K2-only '1' strip silently failed
    // to resolve every K1 code. Catalog ids are always 5 chars, so any 6-char
    // report is prefixed by construction.
    if (code.size() == 6)
        return code.substr(1);
    return code;
}

uint32_t CfsMaterialDb::parse_color(const std::string& color_str) {
    if (color_str == "-1" || color_str == "None")
        return kDefaultColor;
    // Creality's leading-zero "0RRGGBB" is this firmware's own spelling and is
    // owned here; what is left is ordinary hex, which helix::ams::read_lane_color
    // reads by the same grammar as every other lane-shaped producer. A value
    // carrying anything but a colour is refused whole rather than read up to its
    // first bad character, so a trailing-garbage field renders as "no colour"
    // instead of a plausible wrong one.
    std::string hex = color_str;
    if (hex.size() == 7 && hex[0] == '0')
        hex = hex.substr(1);
    const auto reading = helix::ams::read_lane_color(hex);
    return reading.kind == helix::ams::ColorReadingKind::Observed ? reading.rgb : kDefaultColor;
}

std::string CfsMaterialDb::slot_to_tnn(int global_index) {
    if (global_index < 0 || global_index > 15)
        return "";
    int unit = global_index / 4 + 1;
    int slot = global_index % 4;
    char letter = 'A' + static_cast<char>(slot);
    return "T" + std::to_string(unit) + letter; // DISPLAY_NUMBERING_OK: CFS address
}

int CfsMaterialDb::tnn_to_slot(const std::string& tnn) {
    if (tnn.size() != 3 || tnn[0] != 'T')
        return -1;
    int unit = tnn[1] - '0';
    int slot = tnn[2] - 'A';
    if (unit < 1 || unit > 4 || slot < 0 || slot > 3)
        return -1;
    return (unit - 1) * 4 + slot;
}

} // namespace helix::printer

namespace helix::cfs {

using printer::CfsMaterialDb;
using printer::FilamentCatalog;

namespace {

/// A tagged bay's `vender` entry: the tag's whole record, 40 alphanumeric
/// characters (printer-research/CREALITY_CFS_K2_INTERNALS.md § "RFID tags").
constexpr std::size_t kTagRecordLength = 40;
/// The supplier field, chars 6-9 of the record.
constexpr std::size_t kTagSupplierOffset = 5;
constexpr std::size_t kTagSupplierLength = 4;
constexpr std::string_view kCrealitySupplierCode = "0276";
/// The serial field, chars 29-34 of the record.
constexpr std::size_t kTagSerialOffset = 28;
constexpr std::size_t kTagSerialLength = 6;
/// Creality's factory serial, which every stock spool carries.
constexpr int kFactoryTagSerial = 1;

bool is_tag_record(std::string_view v) {
    return v.size() == kTagRecordLength &&
           std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
}

} // namespace

BoxFrameShape classify_box_frame(const nlohmann::json& box) {
    BoxFrameShape shape;
    shape.has_top_level = box.contains("filament") || box.contains("map") ||
                          box.contains("auto_refill") || box.contains("state");
    shape.has_unit_data =
        box.contains("T1") || box.contains("T2") || box.contains("T3") || box.contains("T4");
    return shape;
}

std::optional<FilamentSensorDelta> parse_filament_sensor(const nlohmann::json& params) {
    const auto it = params.find("filament_switch_sensor filament_sensor");
    if (it == params.end()) {
        return std::nullopt;
    }
    FilamentSensorDelta d;
    if (it->is_object()) {
        d.filament_detected = ams::read_field<bool>(*it, "filament_detected");
    }
    return d;
}

std::optional<ExtruderTempDelta> parse_extruder(const nlohmann::json& params) {
    const auto it = params.find("extruder");
    if (it == params.end()) {
        return std::nullopt;
    }
    ExtruderTempDelta d;
    if (it->is_object()) {
        d.target_c = ams::read_field<double>(*it, "target");
        d.temperature_c = ams::read_field<double>(*it, "temperature");
    }
    return d;
}

std::optional<MotorControlDelta> parse_motor_control(const nlohmann::json& params) {
    const auto it = params.find("motor_control");
    if (it == params.end()) {
        return std::nullopt;
    }
    MotorControlDelta d;
    if (it->is_object()) {
        d.motor_ready = ams::read_field<bool>(*it, "motor_ready");
    }
    return d;
}

bool has_registered_unit(const nlohmann::json& configfile_settings) {
    const nlohmann::json* section = json_util::find_member(configfile_settings, "auto_addr");
    const nlohmann::json* option =
        section ? json_util::find_member(*section, "mb_addr_table_uniids") : nullptr;
    if (!option) {
        return false;
    }
    const nlohmann::json table =
        option->is_string()
            ? nlohmann::json::parse(option->get_ref<const std::string&>(), nullptr, false)
            : *option;
    if (!table.is_array()) {
        return false;
    }
    for (const auto& entry : table) {
        if (!entry.is_array()) {
            continue;
        }
        for (const auto& byte : entry) {
            if (json_util::as_int64(byte) != 0) {
                return true;
            }
        }
    }
    return false;
}

bool is_vender_sentinel(const std::string& v) {
    return v.empty() || v == "none" || v == "None" || v == "-1";
}

bool is_material_code_sentinel(const std::string& v) {
    return v.empty() || v == "none" || v == "None" || v == "-1" || v == "unknown";
}

std::string brand_from_vender(const std::string& vender) {
    if (is_vender_sentinel(vender)) {
        return {};
    }
    const bool record = is_tag_record(vender);
    const bool creality =
        vender == "unknown" ||
        (record && std::string_view(vender).substr(kTagSupplierOffset, kTagSupplierLength) ==
                       kCrealitySupplierCode);
    if (creality) {
        return "Creality"; // i18n: do not translate - brand name
    }
    return record ? std::string{} : vender;
}

std::optional<int> tag_record_spoolman_id(std::string_view vender) {
    if (!is_tag_record(vender)) {
        return std::nullopt;
    }
    const std::string_view serial = vender.substr(kTagSerialOffset, kTagSerialLength);
    if (!std::all_of(serial.begin(), serial.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return std::nullopt;
    }
    int id = 0;
    for (const char c : serial) {
        id = id * 10 + (c - '0');
    }
    if (id <= kFactoryTagSerial) {
        return std::nullopt;
    }
    return id;
}

void reserve_address_gaps(AmsSystemInfo& info) {
    std::sort(info.units.begin(), info.units.end(), [](const AmsUnit& a, const AmsUnit& b) {
        return a.first_slot_global_index < b.first_slot_global_index;
    });
    std::vector<AmsUnit> units;
    for (auto& unit : info.units) {
        for (int address = static_cast<int>(units.size()); address < unit.unit_index; ++address) {
            AmsUnit gap;
            gap.unit_index = address;
            gap.name =
                "T" + std::to_string(address + 1); // DISPLAY_NUMBERING_OK: unit name is its box key
            gap.display_name = "CFS Unit " + std::to_string(address + 1);
            gap.slot_count = kBaysPerBox;
            gap.first_slot_global_index = address * kBaysPerBox;
            gap.topology = PathTopology::HUB;
            gap.absent = true;
            for (int local = 0; local < kBaysPerBox; ++local) {
                SlotInfo bay;
                bay.slot_index = local;
                bay.global_index = gap.first_slot_global_index + local;
                bay.status = SlotStatus::EMPTY;
                gap.slots.push_back(std::move(bay));
            }
            units.push_back(std::move(gap));
        }
        units.push_back(std::move(unit));
    }
    info.units = std::move(units);
    info.total_slots = info.units.empty() ? 0
                                          : info.units.back().first_slot_global_index +
                                                info.units.back().slot_count;
}

void unmap_absent_bays(AmsSystemInfo& info) {
    for (auto& unit : info.units) {
        if (!unit.absent) {
            continue;
        }
        for (auto& bay : unit.slots) {
            if (bay.mapped_tool >= 0 &&
                bay.mapped_tool < static_cast<int>(info.tool_to_slot_map.size())) {
                info.tool_to_slot_map[static_cast<size_t>(bay.mapped_tool)] = -1;
            }
            bay.mapped_tool = -1;
        }
    }
}

AmsSystemInfo parse_stock_box(const nlohmann::json& box_json,
                              const std::unordered_map<int, std::string>* own_labels) {
    AmsSystemInfo info;
    info.type = AmsType::CFS;
    info.type_name = "CFS";
    info.tip_method = TipMethod::CUT;
    info.supports_purge = true;
    info.supports_bypass = false;

    // Parse auto_refill → endless spool support.
    //
    // Top-level box fields (auto_refill, filament, enable, filament_useup) are
    // documented as JSON ints, unlike the per-unit scalars further down, which are
    // documented as strings (CREALITY_CFS_K2_INTERNALS.md § "Moonraker Object: box").
    // safe_int is therefore belt-and-braces here rather than a known-shape fix:
    // .value("auto_refill", 0) still throws type_error.302 on a null or a string,
    // and that throw escapes parse_box_status and drops the entire box frame.
    //
    // Test for an explicit 1 rather than != 0 because safe_int parses numeric
    // strings: were the box to ever report the "-1" sentinel here (the documented
    // absence marker everywhere else in this object), != 0 would read it as TRUE.
    info.endless_spool_enabled = helix::json_util::safe_int(box_json, "auto_refill", 0) == 1;

    // box.filament is a stale active-lane SELECTION index, NOT a "filament
    // loaded" truth — it retains a lane number even when nothing is loaded,
    // producing phantom "loaded from lane N". The authoritative loaded signal
    // is the toolhead sensor (filament_switch_sensor filament_sensor), handled
    // in handle_status. Leave filament_loaded false here.
    info.filament_loaded = false;

    // Runout signal: box.filament_useup == 1 means no filament at the box gate.
    // Raw firmware value here; ams_state gates display on print-paused state to
    // avoid false positives at pre-load print start (see filament_useup decode).
    // safe_int + "== 1" for the same reasons as auto_refill above: a null or
    // wrong-typed value must not throw, and a "-1" must not read as a runout.
    // "== 1" also matches this field's decode exactly, per the line above.
    info.filament_runout = helix::json_util::safe_int(box_json, "filament_useup", 0) == 1;

    // Parse tool mapping from "map" object
    if (box_json.contains("map") && box_json["map"].is_object()) {
        // Find maximum tool index to size the mapping vector
        int max_tool = -1;
        for (auto& [tnn_key, tnn_val] : box_json["map"].items()) {
            int slot = CfsMaterialDb::tnn_to_slot(tnn_key);
            if (slot >= 0 && slot > max_tool) {
                max_tool = slot;
            }
        }
        if (max_tool >= 0) {
            info.tool_to_slot_map.resize(max_tool + 1, -1);
            for (auto& [tnn_key, tnn_val] : box_json["map"].items()) {
                // The map value is a "TnnA" string; anything else (null for an
                // unmapped tool, or a number) would make get<std::string>()
                // throw type_error.302 out of parse_box_status and drop the
                // whole box frame. Skip the entry instead — same is_string()
                // discipline as the same_material group parse below.
                if (!tnn_val.is_string()) {
                    continue;
                }
                int src = CfsMaterialDb::tnn_to_slot(tnn_key);
                int dst = CfsMaterialDb::tnn_to_slot(tnn_val.get<std::string>());
                if (src >= 0 && dst >= 0 && src < static_cast<int>(info.tool_to_slot_map.size())) {
                    info.tool_to_slot_map[src] = dst;
                }
            }
        }
    }

    // Build same_material lookup: material_type code -> material name string
    // "same_material" contains groups like ["101001", "0000000", ["T1A"], "PLA"]
    // where the last element is a human-readable material name. group[2] (the
    // lane list) feeds the auto-refill capability: get_endless_spool_
    // capabilities() answers from these ordinals whether any two lanes
    // actually group (#1391). Presence of the array is recorded separately
    // because "no data" (the flat dialect never sends the field) must stay
    // distinguishable from "parsed and nothing groups".
    std::unordered_map<std::string, std::string> same_material_names;
    if (box_json.contains("same_material") && box_json["same_material"].is_array()) {
        info.endless_spool_groups_reported = true;
        // One entry per addressable bay (T1A..T4D, tnn_to_slot's domain), not
        // one per CONNECTED unit: a box whose T1 dropped must not lose T2's
        // groups to a shorter vector.
        info.endless_spool_group_ids.assign(16, -1);
        int group_ordinal = 0;
        for (const auto& group : box_json["same_material"]) {
            if (group.is_array() && group.size() >= 4 && group[0].is_string() &&
                group[3].is_string()) {
                same_material_names[group[0].get<std::string>()] = group[3].get<std::string>();
            }
            // Lane list, with the same per-entry is_string discipline as every
            // other index into this array: one malformed group must skip its
            // entry, not throw the whole box frame away.
            if (group.is_array() && group.size() >= 3 && group[2].is_array()) {
                for (const auto& lane : group[2]) {
                    if (!lane.is_string()) {
                        continue;
                    }
                    int slot = CfsMaterialDb::tnn_to_slot(lane.get<std::string>());
                    if (slot >= 0 && slot < static_cast<int>(info.endless_spool_group_ids.size())) {
                        info.endless_spool_group_ids[static_cast<size_t>(slot)] = group_ordinal;
                    }
                }
            }
            ++group_ordinal;
        }
    }

    // Shared snapshot of ONLY the cfs-coded slice. This runs on every full box
    // update — any `filament`/`map`/`T1..T4` key, so once per spool move, not
    // once per session — and load_codes() re-parses the whole 100 KB catalog
    // (~872 kB of transient heap) to keep 73 products. Cached and invalidated by
    // user-overlay writes; the snapshot stays alive for this scope even if
    // another thread retires it mid-parse.
    const auto cfs_catalog_snapshot = FilamentCatalog::load_codes_cached("cfs");
    const FilamentCatalog& cfs_catalog = *cfs_catalog_snapshot;

    // Loop over T1-T4 units
    for (int n = 1; n <= 4; ++n) {
        std::string key = "T" + std::to_string(n); // DISPLAY_NUMBERING_OK: box object key
        if (!box_json.contains(key) || !box_json[key].is_object()) {
            spdlog::debug("[AMS CFS] {} not present or not an object", key);
            continue;
        }

        const auto& unit_json = box_json[key];
        // safe_string, not .value(): the per-unit scalars below are documented as
        // strings (CREALITY_CFS_K2_INTERNALS.md § "Per-Unit Fields"), but .value() with a
        // string default THROWS type_error.302 on a JSON null or a numeric value,
        // and that throw escapes parse_box_status and drops the whole box frame.
        // safe_string returns the supplied default for null/wrong-type WITHOUT
        // stringifying, so a numeric -1 yields the literal default rather than the
        // string "-1" — the sentinel comparisons below stay exact either way.
        std::string state = helix::json_util::safe_string(unit_json, "state", "None");
        if (state == "None" || state == "-1") {
            spdlog::debug("[AMS CFS] {} disconnected (state={})", key, state);
            continue; // Disconnected unit
        }

        spdlog::debug("[AMS CFS] {} connected (state={})", key, state);

        AmsUnit unit;
        unit.unit_index = n - 1;
        unit.name = key;
        unit.display_name = "CFS Unit " + std::to_string(n);
        unit.slot_count = 4;
        unit.first_slot_global_index = (n - 1) * 4;
        unit.connected = true;
        unit.topology = PathTopology::HUB;
        // Every box feeds the printer's one extruder through the CFS hub, so all
        // of them share toolhead 0; the T<n> numbers are routes into it.
        unit.hub_tool_label = 0;

        // Firmware version and serial
        std::string ver = helix::json_util::safe_string(unit_json, "version", "-1");
        if (ver != "-1" && ver != "None") {
            unit.firmware_version = ver;
        }
        std::string sn = helix::json_util::safe_string(unit_json, "sn", "-1");
        if (sn != "-1" && sn != "None") {
            unit.serial_number = sn;
        }

        // Environment: temperature and humidity. Both are documented as STRINGS
        // ("27", "48"), so safe_string is the faithful read. Note the residual: if
        // a firmware variant ever sent these as numbers, safe_string yields "None"
        // and the unit silently reports no environment data — better than today's
        // throw, which discards every other field on the box too, but it is a
        // silent loss. Switch to safe_float here if a numeric variant is ever seen.
        std::string temp_str = helix::json_util::safe_string(unit_json, "temperature", "None");
        std::string humid_str =
            helix::json_util::safe_string(unit_json, "dry_and_humidity", "None");
        if (temp_str != "None" && temp_str != "-1" && humid_str != "None" && humid_str != "-1") {
            EnvironmentData env;
            env.temperature_c = helix::text_io::parse_leading<float>(temp_str).value_or(0.0f);
            env.humidity_pct = helix::text_io::parse_leading<float>(humid_str).value_or(0.0f);
            env.has_humidity = true;
            unit.environment = env;
        }

        // Parse the 4 slots within this unit.
        //
        // These four keys are array[4] on a connected unit, but a unit that drops
        // out reports the SCALAR sentinel "-1"/"None" for the very same keys
        // (CREALITY_CFS_K2_INTERNALS.md § "Disconnected Units"; the sibling
        // `filament_rack` object is documented with exactly that scalar form).
        // .value(key, json::array()) does not throw on a scalar — it round-trips
        // through get<json> and hands back the string — but a string json has
        // size() == 1, so the `i < size()` bounds check in the loop below passes
        // for i == 0 and then operator[](size_type) throws type_error.305, BEFORE
        // the .is_string() guard on that same line can run. Normalizing a
        // non-array to an empty array makes every slot fall back to its sentinel
        // default instead. Same is_array() discipline as build_cfs_slot_uid's
        // `pick` lambda below.
        auto array_field = [&unit_json](const char* field) -> nlohmann::json {
            auto it = unit_json.find(field);
            if (it == unit_json.end() || !it->is_array())
                return nlohmann::json::array();
            return *it;
        };
        auto color_arr = array_field("color_value");
        auto material_arr = array_field("material_type");
        auto remain_arr = array_field("remain_len");
        auto vender_arr = array_field("vender");

        for (int i = 0; i < 4; ++i) {
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = (n - 1) * 4 + i;

            // Color
            std::string color_str = "-1";
            if (i < static_cast<int>(color_arr.size()) && color_arr[i].is_string()) {
                color_str = color_arr[i].get<std::string>();
            }
            slot.color_rgb = CfsMaterialDb::parse_color(color_str);

            // Material type
            std::string mat_code_raw = "-1";
            if (i < static_cast<int>(material_arr.size()) && material_arr[i].is_string()) {
                mat_code_raw = material_arr[i].get<std::string>();
            }
            std::string mat_id = CfsMaterialDb::strip_code(mat_code_raw);
            if (!mat_id.empty()) {
                const auto* mat_info = cfs_catalog.resolve_code("cfs", mat_id);
                if (mat_info) {
                    slot.material = mat_info->type;
                    slot.brand = mat_info->brand;
                    slot.nozzle_temp_min = mat_info->nozzle_min;
                    slot.nozzle_temp_max = mat_info->nozzle_max;
                } else {
                    // Fallback: check same_material for a human-readable name
                    auto it = same_material_names.find(mat_code_raw);
                    if (it != same_material_names.end()) {
                        slot.material = it->second;
                    }
                }
            }

            // Per-slot vendor string. Unlike color_value/material_type — which
            // are LATCHED RFID data that stay pinned to the last spool after
            // removal and thus fake "ghost" slots — `vender` reads a real vendor
            // name or the present-but-unresolved "unknown" when a spool is
            // seated, and a sentinel ("none"/"None"/"-1") when the bay is empty
            // (verified against K2 Plus hardware; prestonbrown/helixscreen#1077).
            std::string vender_str = "none";
            if (i < static_cast<int>(vender_arr.size()) && vender_arr[i].is_string()) {
                vender_str = vender_arr[i].get<std::string>();
            }
            const bool vender_occupied = !is_vender_sentinel(vender_str);

            // Brand fallback for RFID spools whose material code isn't in our
            // DB (the DB lookup above already sets slot.brand for known codes).
            // Priority for a vendor-occupied bay:
            //   1. a real vendor name reported by the box, else
            //   2. "Creality" when the tag is present but the vendor is
            //      "unknown" — CFS RFID tags are Creality's proprietary
            //      ecosystem, so a present-but-unresolved tag is a Creality
            //      spool. A user override still wins over this.
            if (slot.brand.empty() && vender_occupied) {
                slot.brand = brand_from_vender(vender_str);
            }

            // Remaining length
            std::string remain_str = "-1";
            if (i < static_cast<int>(remain_arr.size()) && remain_arr[i].is_string()) {
                remain_str = remain_arr[i].get<std::string>();
            }
            if (remain_str != "-1" && remain_str != "None") {
                slot.remaining_length_m =
                    helix::text_io::parse_leading<float>(remain_str).value_or(0.0f);
            }

            // Presence, in priority order. Each firmware field is used only
            // where it is actually trustworthy:
            //
            //  1. `vender` is the LIVE occupancy signal for a TAGGED bay — it
            //     reads a real vendor name (or the present-but-unresolved
            //     "unknown") while a tag is seated and drops to a sentinel the
            //     moment the spool is pulled. Non-sentinel => PRESENT.
            //
            //  2. `remain_len` is a FALLBACK that exists solely to cover
            //     UNTAGGED 3rd-party spools: they have no RFID vendor, so
            //     `vender` reads sentinel even though a spool is physically
            //     seated, and the measuring wheel is the only evidence we get
            //     (prestonbrown/helixscreen#1077).
            //
            //     But `remain_len` LATCHES: after a tagged spool is removed the
            //     box keeps reporting its last length forever (observed on K2:
            //     vender "none" alongside remain_len "50"). Applying the
            //     untagged fallback there pins the bay AVAILABLE permanently.
            //
            //     A bay carrying a real latched RFID material code
            //     (`material_type` past its sentinels) is by definition a
            //     TAGGED bay, and the untagged fallback does not apply to it —
            //     for those, `vender` alone decides. So the fallback is
            //     consulted only when no tag was ever read, which is exactly
            //     the untagged case it was written for.
            //
            //     `color_value` is deliberately NOT part of this test even
            //     though it also latches. It is user-writable (the stock LCD
            //     and our own push_slot_identity_to_firmware both write it)
            //     and on real K2 hardware it reads a real color on EVERY bay,
            //     tagged or not — including bays whose material_type is
            //     "unknown". Folding it in would make has_tag_payload
            //     permanently true, silently disabling the untagged fallback
            //     and re-breaking #1077.
            //
            //     `material_type` IS written by the identity push too (#968),
            //     so a non-sentinel code is not PROOF of a tag on its own — it
            //     may be the user's own label echoed back. `own_labels` names
            //     the codes we wrote, and only those; a matching code is
            //     discounted here, because otherwise labeling a bay SUPPRESSES
            //     the very fallback that was keeping it visible. On firmware
            //     whose `vender` stays sentinel for untagged spools — the #1077
            //     population this fallback exists for — that flipped a seated
            //     spool to EMPTY the moment the user named it.
            //
            //     The discount is safe precisely because push_slot_identity_to_
            //     firmware records a code ONLY when the bay's pre-push code was
            //     a sentinel. A genuinely tagged bay is never in the map, so
            //     relabeling one still suppresses the fallback and its latched
            //     remain_len cannot ghost the pulled spool as AVAILABLE. What
            //     is left is the invariant that was broken: labeling a bay does
            //     not change how its presence is read.
            //
            // An override never promotes presence. It supplies IDENTITY only —
            // a bay that reads EMPTY here stays EMPTY, and the retained
            // identity is what ui_ams_slot.cpp ghosts (see apply_resolved_lane,
            // which takes presence from a sensed reading and nothing else).
            const bool remain_present = slot.remaining_length_m > 0.0f;
            bool code_is_own_label = false;
            if (own_labels != nullptr) {
                auto own_it = own_labels->find(slot.global_index);
                code_is_own_label = own_it != own_labels->end() && own_it->second == mat_code_raw;
            }
            const bool has_tag_payload =
                !is_material_code_sentinel(mat_code_raw) && !code_is_own_label;
            const bool untagged_present = !has_tag_payload && remain_present;
            if (vender_occupied || untagged_present) {
                slot.status = SlotStatus::AVAILABLE;
            } else {
                slot.status = SlotStatus::EMPTY;
                // Scrub the latched display fields parse populated so a removed
                // spool's stale color/material doesn't render on the empty bay.
                // This resets only PARSED firmware fields — not the persistent
                // user declaration, which the lane sources and
                // clear_override_locked own.
                slot.brand.clear();
                slot.material.clear();
                slot.color_name.clear();
                slot.color_rgb = CfsMaterialDb::parse_color("-1");
                slot.remaining_length_m = 0.0f;
            }

            // mapped_tool is deliberately NOT written here. box.map, parsed
            // above into info.tool_to_slot_map, is the box's own statement of
            // the mapping, and the single sync_tool_map_from_forward() pass at
            // the end of this function derives every slot's mapped_tool from
            // it. Stamping the identity here as well is what made a
            // BOX_MODIFY_TN remap show the right op-button lane and the wrong
            // lane badge — two writers, one of them ignoring firmware.

            unit.slots.push_back(std::move(slot));
        }

        // Parse active filament slot within this unit.
        // T1.filament = "A"/"B"/"C"/"D" when a slot is loaded, "None" otherwise.
        std::string fil_letter = helix::json_util::safe_string(unit_json, "filament", "None");
        if (fil_letter.size() == 1 && fil_letter[0] >= 'A' && fil_letter[0] <= 'D') {
            int active_local = fil_letter[0] - 'A';
            info.current_slot = (n - 1) * 4 + active_local;
            spdlog::debug("[AMS CFS] Active filament: {} (slot {})", fil_letter, info.current_slot);
        }

        info.units.push_back(std::move(unit));
    }
    reserve_address_gaps(info);

    // Publish both directions from the one source the box actually states —
    // box.map. identity_fallback=true keeps the historical 1:1 default for
    // every tool the payload leaves unmapped (and for the common case of no
    // `map` key at all), so a box that has never been remapped parses exactly
    // as it always did.
    printer::sync_tool_map_from_forward(info, /*identity_fallback=*/true);
    unmap_absent_bays(info);

    // current_tool must name the tool that ROUTES THROUGH the seated lane, not
    // the lane index: with T0 remapped onto lane 2 the print-status color dot
    // has to read T0. Derived from the same pass above, so it cannot disagree
    // with the badge.
    //
    // A seated lane that no tool maps to (possible from a PARTIAL box.map,
    // where identity_fallback deliberately refuses to hand a claimed lane back)
    // falls back to the lane index — the pre-remap answer — rather than
    // publishing -1. -1 is a legitimate value of this field, but only in
    // combination with "nothing loaded", and no consumer renders the
    // seated-yet-toolless pair usefully: ams_current_tool.xml hides the whole
    // indicator on `< 0`, taking the still-valid filament colour swatch with
    // it, and ToolState::set_ams_topology() clamps a negative active_tool to 0,
    // which lights T0 in the tool switcher and the nozzle label while a
    // different lane is physically seated. A stale-but-plausible tool number
    // beats both of those.
    if (info.current_slot >= 0) {
        const auto* seated = info.get_slot_global(info.current_slot);
        const int seated_tool = seated ? seated->mapped_tool : -1;
        info.current_tool = seated_tool >= 0 ? seated_tool : info.current_slot;
    }

    return info;
}

} // namespace helix::cfs
