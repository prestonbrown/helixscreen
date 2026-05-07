// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_CFS

#include "ams_backend_cfs.h"

#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_error.h"
#include "post_op_cooldown_manager.h"

#include "hv/json.hpp"

#include <spdlog/spdlog.h>

#include <fstream>

namespace helix::printer {

using json = nlohmann::json;

const CfsMaterialDb& CfsMaterialDb::instance()
{
    static CfsMaterialDb db;
    return db;
}

CfsMaterialDb::CfsMaterialDb()
{
    load_database();
}

void CfsMaterialDb::load_database()
{
    for (const auto& path : {"assets/cfs_materials.json",
                              "../assets/cfs_materials.json",
                              "/opt/helixscreen/assets/cfs_materials.json"}) {
        std::ifstream f(path);
        if (!f.is_open())
            continue;

        try {
            auto j = nlohmann::json::parse(f);
            for (auto& [id, entry] : j.items()) {
                CfsMaterialInfo info;
                info.id            = id;
                info.brand         = entry.value("brand", "");
                info.name          = entry.value("name", "");
                info.material_type = entry.value("type", "");
                info.min_temp      = entry.value("min_temp", 0);
                info.max_temp      = entry.value("max_temp", 0);
                materials_[id]     = std::move(info);
            }
            spdlog::info("[AMS CFS] Loaded {} materials from {}", materials_.size(), path);
            return;
        } catch (const std::exception& e) {
            spdlog::warn("[AMS CFS] Failed to parse {}: {}", path, e.what());
        }
    }
    spdlog::warn("[AMS CFS] Material database not found");
}

const CfsMaterialInfo* CfsMaterialDb::lookup(const std::string& id) const
{
    auto it = materials_.find(id);
    return it != materials_.end() ? &it->second : nullptr;
}

std::string CfsMaterialDb::strip_code(const std::string& code)
{
    if (code == "-1" || code == "None" || code.empty())
        return "";
    if (code.size() == 6 && code[0] == '1')
        return code.substr(1);
    return code;
}

uint32_t CfsMaterialDb::parse_color(const std::string& color_str)
{
    if (color_str == "-1" || color_str == "None" || color_str.empty())
        return DEFAULT_COLOR;
    std::string hex = color_str;
    if (hex.size() == 7 && hex[0] == '0')
        hex = hex.substr(1);
    try {
        return static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
        return DEFAULT_COLOR;
    }
}

std::string CfsMaterialDb::slot_to_tnn(int global_index)
{
    if (global_index < 0 || global_index > 15)
        return "";
    int unit   = global_index / 4 + 1;
    int slot   = global_index % 4;
    char letter = 'A' + static_cast<char>(slot);
    return "T" + std::to_string(unit) + letter;
}

int CfsMaterialDb::tnn_to_slot(const std::string& tnn)
{
    if (tnn.size() != 3 || tnn[0] != 'T')
        return -1;
    int unit = tnn[1] - '0';
    int slot = tnn[2] - 'A';
    if (unit < 1 || unit > 4 || slot < 0 || slot > 3)
        return -1;
    return (unit - 1) * 4 + slot;
}

// --- CFS Error Decoder ---

namespace {

/// Format the [unit, slot] payload observed in `key849` (and likely all
/// SLOT-level CFS errors) into a human locator. Klipper emits unit as an
/// integer (1-based) and slot as a single uppercase letter ("A"..."D").
/// Returns "" when the payload doesn't match the expected shape — caller
/// then displays the un-augmented message rather than guessing.
///
/// Real-world sample (telemetry 2026-05-05):
///     !! {"code":"key849","values":[1,"B"]}
///   → " in unit 1 slot B"
std::string format_unit_slot(const nlohmann::json& values) {
    if (!values.is_array() || values.size() < 2) return "";
    if (!values[0].is_number_integer()) return "";
    if (!values[1].is_string()) return "";
    int unit = values[0].get<int>();
    std::string slot = values[1].get<std::string>();
    if (unit < 1 || slot.size() != 1) return "";
    char c = slot[0];
    if (c < 'A' || c > 'D') return "";
    return " in unit " + std::to_string(unit) + " slot " + slot;
}

/// Format unit-level errors where values is `[unit]` or `[unit, ...]`.
/// We just grab the leading int and ignore tails. Empty string if the
/// shape doesn't match.
std::string format_unit_only(const nlohmann::json& values) {
    if (!values.is_array() || values.empty()) return "";
    if (!values[0].is_number_integer()) return "";
    int unit = values[0].get<int>();
    if (unit < 1) return "";
    return " on unit " + std::to_string(unit);
}

} // namespace

struct CfsErrorEntry {
    const char* message;
    const char* hint;
    AmsAlertLevel level;
    /// Optional formatter that stringifies the `values` array into a
    /// human-readable locator (" in unit 1 slot B"). Caller appends the
    /// result to the friendly message. nullptr = no per-error format
    /// known yet (no regression — message displays unchanged).
    std::string (*format_values)(const nlohmann::json&) = nullptr;
};

// Format-callback aliases keep the table tidy. Only key849 has been
// confirmed against real telemetry (`[1,"B"]`); the other SLOT/UNIT
// codes are wired up to the same formatters on the assumption that
// Creality uses a consistent shape, but they'll degrade gracefully
// (no extra locator displayed) if the assumption is wrong.
static auto* const fmt_unit_slot = &format_unit_slot;
static auto* const fmt_unit_only = &format_unit_only;

static const std::unordered_map<std::string, CfsErrorEntry> CFS_ERROR_TABLE = {
    // Klipper-internal errors (not CFS-specific) that we frequently surface to
    // users. Despite living in the CFS table for now, these are general — the
    // table predates the broader use case. TODO: rename to KlipperErrorTable.
    {"key298", {"MCU bridge daemon is shut down", "Tap Firmware Restart to recover — on K2 this also bounces the rpi MCU bridge", AmsAlertLevel::SYSTEM, nullptr}},
    {"key585", {"Move out of range", "The requested position is outside the printer's bounds", AmsAlertLevel::SYSTEM, nullptr}},

    {"key831", {"Lost connection to CFS unit", "Check the RS-485 cable between printer and CFS", AmsAlertLevel::SYSTEM, nullptr}},
    {"key834", {"Invalid parameters sent to CFS", "This may indicate a firmware bug — try restarting", AmsAlertLevel::SYSTEM, nullptr}},
    {"key835", {"Filament jammed at CFS connector", "Open the CFS lid, check the PTFE tube connection for the stuck slot", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key836", {"Filament jammed between CFS and sensor", "Check the Bowden tube for kinks or debris", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key837", {"Filament jammed before extruder gear", "Check for tangles on the spool and clear the filament path to the printhead", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key838", {"Filament reached extruder but won't feed", "Check for a clog in the hotend or a worn drive gear", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key840", {"CFS unit state error", "A unit reported an unexpected state — check its current operation", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key841", {"Filament cutter stuck", "The cutter blade didn't return — check for filament wrapped around the cutting mechanism", AmsAlertLevel::SYSTEM, nullptr}},
    {"key843", {"Can't read filament RFID tag", "Re-seat the spool in the slot, ensure the RFID label faces the reader", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key844", {"PTFE tube connection loose", "Re-seat the Bowden tube connector on the CFS unit", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key845", {"Nozzle clog detected", "Run a cold pull or replace the nozzle", AmsAlertLevel::SYSTEM, nullptr}},
    {"key847", {"Empty spool — filament wound around hub", "Remove the empty spool and clear wound filament from the CFS hub", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key848", {"Filament snapped inside CFS", "Open the CFS unit and remove the broken filament from the slot", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key849", {"Retract failed — filament stuck in connector", "Manually pull the filament back through the connector", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key850", {"Retract error — multiple connectors triggered", "Check that only one filament path is active", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key851", {"Retract didn't reach buffer empty position", "The filament may not have fully retracted — try again or manually pull", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key852", {"Sensor mismatch — check extruder and CFS sensors", "Extruder and CFS disagree on filament state — inspect both sensors", AmsAlertLevel::SYSTEM, nullptr}},
    {"key853", {"Humidity sensor malfunction", "CFS unit's humidity sensor is not responding — may need service", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key855", {"Filament cutter position error", "The cutter is out of alignment — recalibrate with CALIBRATE_CUT_POS", AmsAlertLevel::SYSTEM, nullptr}},
    {"key856", {"Filament cutter not detected", "Check that the cutter mechanism is properly installed", AmsAlertLevel::SYSTEM, nullptr}},
    {"key857", {"CFS motor overloaded", "A spool may be tangled or the drive gear is jammed", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key858", {"EEPROM error on CFS unit", "CFS unit storage is corrupted — may need firmware reflash", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key859", {"Measuring wheel error", "The filament length sensor is malfunctioning", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key860", {"Buffer tube problem", "Check the buffer unit on the back of the printer", AmsAlertLevel::SYSTEM, nullptr}},
    {"key861", {"RFID reader malfunction (left)", "The left RFID reader in this CFS unit may need service", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key862", {"RFID reader malfunction (right)", "The right RFID reader in this CFS unit may need service", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key863", {"Retract error — filament still detected", "Filament didn't fully retract, may need manual removal", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key864", {"Extrude error — buffer not full", "Filament didn't fill buffer tube during load", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key865", {"Retract error — failed to exit connector", "Filament stuck in connector during unload", AmsAlertLevel::SLOT, fmt_unit_slot}},
};

std::optional<std::pair<const char*, const char*>>
CfsErrorDecoder::lookup_message(const std::string& key_code) {
    auto it = CFS_ERROR_TABLE.find(key_code);
    if (it == CFS_ERROR_TABLE.end())
        return std::nullopt;
    return std::make_pair(it->second.message, it->second.hint);
}

std::optional<std::pair<std::string, std::string>>
CfsErrorDecoder::lookup_message_with_values(const std::string& key_code,
                                            const nlohmann::json& values) {
    auto it = CFS_ERROR_TABLE.find(key_code);
    if (it == CFS_ERROR_TABLE.end())
        return std::nullopt;
    const auto& entry = it->second;
    std::string message = entry.message;
    if (entry.format_values) {
        std::string locator = entry.format_values(values);
        if (!locator.empty()) {
            message += locator;
        }
    }
    return std::make_pair(std::move(message), std::string(entry.hint));
}

std::optional<AmsAlert> CfsErrorDecoder::decode(const std::string& key_code,
                                                 int unit_index, int slot_index)
{
    auto it = CFS_ERROR_TABLE.find(key_code);
    if (it == CFS_ERROR_TABLE.end())
        return std::nullopt;

    const auto& entry = it->second;
    AmsAlert alert;
    alert.message = entry.message;
    alert.hint = entry.hint;
    alert.level = entry.level;
    alert.severity = SlotError::Severity::ERROR;

    // Extract numeric code: "key845" -> "CFS-845"
    if (key_code.size() > 3) {
        alert.error_code = "CFS-" + key_code.substr(3);
    }

    if (entry.level == AmsAlertLevel::UNIT || entry.level == AmsAlertLevel::SLOT) {
        alert.unit_index = unit_index;
    }
    if (entry.level == AmsAlertLevel::SLOT) {
        alert.slot_index = slot_index;
    }

    return alert;
}

// =============================================================================
// AmsBackendCfs — Main CFS backend class
// =============================================================================

AmsBackendCfs::AmsBackendCfs(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    system_info_.type = AmsType::CFS;
    system_info_.type_name = "CFS";
    system_info_.supports_bypass = false;
    system_info_.tip_method = TipMethod::CUT;
    system_info_.supports_purge = true;

    spdlog::debug("[AMS CFS] Backend created");
}

void AmsBackendCfs::on_started() {
    spdlog::info("[AMS CFS] Backend started — querying initial box state");

    // Load persisted per-slot overrides from the shared FilamentSlotOverrideStore
    // BEFORE issuing the initial status query — otherwise the first status
    // callback (libhv background thread) could fire and parse slots before
    // overrides_ is populated, so the first EVENT_STATE_CHANGED frame would
    // miss override data. load_blocking runs on this (main) thread; the
    // Moonraker DB callback fires on the libhv event loop, so the two threads
    // don't interfere. Migration from helix-screen:cfs_slot_overrides to
    // lane_data happens automatically inside load_blocking the first time
    // lane_data is empty (Task 8).
    if (api_) {
        override_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(api_, "cfs");
        // Do the (potentially 5s) MR DB round-trip OUTSIDE the lock, then swap
        // in under mutex_. Holding mutex_ during the swap ensures the parse
        // path sees a coherent map rather than a torn write.
        auto loaded = override_store_->load_blocking();
        const auto loaded_count = loaded.size();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            overrides_ = std::move(loaded);
        }
        spdlog::info("[AMS CFS] Loaded {} slot overrides from filament_slot store",
                     loaded_count);
    }

    // Query initial state explicitly since the subscription response may have
    // arrived before this backend registered its callback
    nlohmann::json objects_to_query = nlohmann::json::object();
    objects_to_query["box"] = nullptr;
    objects_to_query["motor_control"] = nullptr;
    objects_to_query["filament_switch_sensor filament_sensor"] = nullptr;

    nlohmann::json params = {{"objects", objects_to_query}};

    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this](const nlohmann::json& response) {
            if (response.contains("result") && response["result"].contains("status") &&
                response["result"]["status"].is_object()) {
                // Wrap in notify_status_update format for handle_status_update
                nlohmann::json notification = {
                    {"params", nlohmann::json::array({response["result"]["status"]})}};
                handle_status_update(notification);
                spdlog::info("[AMS CFS] Initial state loaded");
            } else {
                spdlog::warn("[AMS CFS] Initial state query returned unexpected format");
            }
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS CFS] Failed to query initial state: {}", err.message);
        });
}

// --- Static parser ---

AmsSystemInfo AmsBackendCfs::parse_box_status(const nlohmann::json& box_json) {
    AmsSystemInfo info;
    info.type = AmsType::CFS;
    info.type_name = "CFS";
    info.tip_method = TipMethod::CUT;
    info.supports_purge = true;
    info.supports_bypass = false;

    // Parse auto_refill → endless spool support
    info.supports_endless_spool = box_json.value("auto_refill", 0) != 0;
    info.supports_tool_mapping = true;

    // Parse filament loaded state (only meaningful when field present)
    info.filament_loaded = box_json.contains("filament") &&
                           box_json["filament"].get<int>() != 0;

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
    // where the last element is a human-readable material name
    std::unordered_map<std::string, std::string> same_material_names;
    if (box_json.contains("same_material") && box_json["same_material"].is_array()) {
        for (const auto& group : box_json["same_material"]) {
            if (group.is_array() && group.size() >= 4 && group[0].is_string() &&
                group[3].is_string()) {
                same_material_names[group[0].get<std::string>()] = group[3].get<std::string>();
            }
        }
    }

    const auto& db = CfsMaterialDb::instance();

    // Loop over T1-T4 units
    for (int n = 1; n <= 4; ++n) {
        std::string key = "T" + std::to_string(n);
        if (!box_json.contains(key) || !box_json[key].is_object()) {
            spdlog::debug("[AMS CFS] {} not present or not an object", key);
            continue;
        }

        const auto& unit_json = box_json[key];
        std::string state = unit_json.value("state", "None");
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

        // Firmware version and serial
        std::string ver = unit_json.value("version", "-1");
        if (ver != "-1" && ver != "None") {
            unit.firmware_version = ver;
        }
        std::string sn = unit_json.value("sn", "-1");
        if (sn != "-1" && sn != "None") {
            unit.serial_number = sn;
        }

        // Environment: temperature and humidity
        std::string temp_str = unit_json.value("temperature", "None");
        std::string humid_str = unit_json.value("dry_and_humidity", "None");
        if (temp_str != "None" && temp_str != "-1" && humid_str != "None" &&
            humid_str != "-1") {
            EnvironmentData env;
            try {
                env.temperature_c = std::stof(temp_str);
            } catch (...) {
                env.temperature_c = 0.0f;
            }
            try {
                env.humidity_pct = std::stof(humid_str);
            } catch (...) {
                env.humidity_pct = 0.0f;
            }
            env.has_humidity = true;
            unit.environment = env;
        }

        // Parse the 4 slots within this unit
        auto color_arr = unit_json.value("color_value", nlohmann::json::array());
        auto material_arr = unit_json.value("material_type", nlohmann::json::array());
        auto remain_arr = unit_json.value("remain_len", nlohmann::json::array());

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
                auto* mat_info = db.lookup(mat_id);
                if (mat_info) {
                    slot.material = mat_info->material_type;
                    slot.brand = mat_info->brand;
                    slot.nozzle_temp_min = mat_info->min_temp;
                    slot.nozzle_temp_max = mat_info->max_temp;
                } else {
                    // Fallback: check same_material for a human-readable name
                    auto it = same_material_names.find(mat_code_raw);
                    if (it != same_material_names.end()) {
                        slot.material = it->second;
                    }
                }
            }

            // Remaining length
            std::string remain_str = "-1";
            if (i < static_cast<int>(remain_arr.size()) && remain_arr[i].is_string()) {
                remain_str = remain_arr[i].get<std::string>();
            }
            if (remain_str != "-1" && remain_str != "None") {
                try {
                    slot.remaining_length_m = std::stof(remain_str);
                } catch (...) {
                    slot.remaining_length_m = 0.0f;
                }
            }

            // Derive status
            if (color_str == "-1" || color_str == "None") {
                slot.status = SlotStatus::EMPTY;
            } else if (slot.remaining_length_m <= 0.0f && remain_str != "-1") {
                slot.status = SlotStatus::EMPTY;
            } else {
                slot.status = SlotStatus::AVAILABLE;
            }

            // CFS slots map 1:1 to tools (slot 0 = tool 0, etc.)
            slot.mapped_tool = slot.global_index;

            unit.slots.push_back(std::move(slot));
        }

        // Parse active filament slot within this unit.
        // T1.filament = "A"/"B"/"C"/"D" when a slot is loaded, "None" otherwise.
        std::string fil_letter = unit_json.value("filament", "None");
        if (fil_letter.size() == 1 && fil_letter[0] >= 'A' && fil_letter[0] <= 'D') {
            int active_local = fil_letter[0] - 'A';
            info.current_slot = (n - 1) * 4 + active_local;
            info.current_tool = 0;
            spdlog::debug("[AMS CFS] Active filament: {} (slot {})", fil_letter,
                          info.current_slot);
        }

        info.units.push_back(std::move(unit));
        info.total_slots += 4;
    }

    return info;
}

// Canonicalize per-slot RFID data into a fingerprint string used by
// check_hardware_event_clear. CFS exposes material_type and color_value as
// per-slot arrays; combining the raw strings (before strip_code / parse_color
// normalize sentinels away) gives a reliable "spool fingerprint". Sentinel
// values "-1" / "None" / empty produce an empty fingerprint string, which the
// helper treats as "no tag / unread".
//
// The fingerprint is intentionally stable across the same spool: CFS rewrites
// both fields from a server-side RFID lookup, so the same physical tag yields
// the same material_type code and the same color_value. A user swapping to a
// different spool changes at least one of those fields. User edits via
// set_slot_info DON'T touch firmware's material_type/color_value (those come
// from the next parse), so a color edit in the UI can't accidentally mimic a
// swap. CFS does NOT expose a dedicated CARD_UID field — this composite is the
// documented surrogate.
static std::string build_cfs_slot_uid(const nlohmann::json& unit_json, int local_index) {
    auto pick = [&](const char* field) -> std::string {
        if (!unit_json.contains(field) || !unit_json[field].is_array())
            return "";
        const auto& arr = unit_json[field];
        if (local_index < 0 || local_index >= static_cast<int>(arr.size()))
            return "";
        if (!arr[local_index].is_string())
            return "";
        auto s = arr[local_index].get<std::string>();
        // Treat sentinels as absent — otherwise a slot empty for two consecutive
        // parses would look like a "stable" UID baseline and the very next real
        // tag read would be flagged as a swap.
        if (s == "-1" || s == "None" || s.empty())
            return "";
        return s;
    };

    std::string mat = pick("material_type");
    std::string color = pick("color_value");
    if (mat.empty() && color.empty())
        return "";
    return mat + "|" + color;
}

// --- handle_status_update ---

void AmsBackendCfs::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update format: {"method": "notify_status_update", "params": [{...}, timestamp]}
    if (!notification.contains("params") || !notification["params"].is_array() ||
        notification["params"].empty()) {
        return;
    }

    const auto& params = notification["params"][0];
    if (!params.is_object()) {
        return;
    }

    bool changed = false;

    if (params.contains("box") && params["box"].is_object()) {
        const auto& box = params["box"];
        spdlog::debug("[AMS CFS] Received box data with {} keys", box.size());

        // Distinguish meaningful updates from noise (e.g., just measuring_wheel).
        // Full updates have "filament"/"map". Unit updates have "T1"/"T2"/etc.
        bool has_top_level = box.contains("filament") || box.contains("map");
        bool has_unit_data = box.contains("T1") || box.contains("T2") ||
                             box.contains("T3") || box.contains("T4");
        bool is_full_update = has_top_level || has_unit_data;

        if (is_full_update) {
            auto new_info = parse_box_status(box);

            // Build observed per-slot RFID fingerprints for every unit present
            // in this notification. Slots that weren't included stay empty
            // (observed_uids stays at default ""), and empty-UID observations
            // are a no-op inside check_hardware_event_clear (no baseline
            // update, no clear) — exactly the behavior we want for incremental
            // updates that only touch a subset of units.
            std::unordered_map<int, std::string> observed_uids;
            for (int n = 1; n <= 4; ++n) {
                std::string key = "T" + std::to_string(n);
                if (!box.contains(key) || !box[key].is_object())
                    continue;
                const auto& unit_json = box[key];
                std::string state = unit_json.value("state", "None");
                if (state == "None" || state == "-1")
                    continue;
                for (int i = 0; i < 4; ++i) {
                    int global_idx = (n - 1) * 4 + i;
                    observed_uids[global_idx] = build_cfs_slot_uid(unit_json, i);
                }
            }

            std::lock_guard<std::mutex> lock(mutex_);

            if (!new_info.units.empty()) {
                system_info_.units = std::move(new_info.units);
                system_info_.total_slots = new_info.total_slots;
                system_info_.supports_endless_spool = new_info.supports_endless_spool;
                system_info_.tool_to_slot_map = std::move(new_info.tool_to_slot_map);
            }

            // Only update filament_loaded when the field was actually present
            if (box.contains("filament")) {
                system_info_.filament_loaded = new_info.filament_loaded;
            }

            // Active slot from T{n}.filament field ("A"/"B"/"C"/"D")
            if (new_info.current_slot >= 0) {
                system_info_.current_slot = new_info.current_slot;
                system_info_.current_tool = new_info.current_tool;
            } else if (box.contains("filament") && !new_info.filament_loaded &&
                       system_info_.action != AmsAction::LOADING) {
                system_info_.current_slot = -1;
                system_info_.current_tool = -1;
            }

            // Override integration convergence point. Firmware-sourced fields
            // are now written to system_info_.units; run the hardware-event
            // check FIRST (so it sees firmware truth, not override-masked
            // data) and apply_overrides AFTER (so the final SlotInfo visible
            // via get_slot_info reflects user edits).
            for (auto& unit : system_info_.units) {
                for (size_t j = 0; j < unit.slots.size(); ++j) {
                    auto& slot = unit.slots[j];
                    int global_idx = unit.first_slot_global_index + static_cast<int>(j);

                    auto uid_it = observed_uids.find(global_idx);
                    const std::string& observed_uid =
                        (uid_it != observed_uids.end()) ? uid_it->second : std::string{};

                    check_hardware_event_clear(slot, global_idx, observed_uid);
                    apply_overrides(slot, global_idx);
                }
            }
        }
        // Partial updates (measuring_wheel, etc.): skip — don't touch state
        changed = true;
    }

    if (params.contains("filament_switch_sensor filament_sensor")) {
        const auto& sensor = params["filament_switch_sensor filament_sensor"];
        if (sensor.contains("filament_detected")) {
            std::lock_guard<std::mutex> lock(mutex_);
            bool detected = sensor["filament_detected"].get<bool>();
            system_info_.filament_loaded = detected;

            // The filament_switch_sensor sits at the toolhead extruder. It
            // trips at the END of CR_BOX_EXTRUDE — long before the load
            // sequence's CR_BOX_WASTE + CR_BOX_FLUSH (~109 mm @ 240 °C, ~3
            // min) actually finishes. Don't flip `action` here: completion
            // semantics live in `dispatch_action_script`'s gcode-script
            // success callback, which fires when Klipper drains the *entire*
            // script. We just mirror the live filament-present flag.
        }
        changed = true;
    }

    if (params.contains("motor_control")) {
        const auto& motor = params["motor_control"];
        if (motor.contains("motor_ready")) {
            std::lock_guard<std::mutex> lock(mutex_);
            motor_ready_ = motor["motor_ready"].get<bool>();
        }
        changed = true;
    }

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

// --- State queries ---

AmsSystemInfo AmsBackendCfs::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendCfs::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (slot) {
        return *slot;
    }
    return SlotInfo{};
}

// --- Path segments ---

PathSegment AmsBackendCfs::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.filament_loaded ? PathSegment::NOZZLE : PathSegment::NONE;
}

PathSegment AmsBackendCfs::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot) {
        return PathSegment::NONE;
    }

    bool is_active = (system_info_.current_slot == slot_index);

    switch (slot->status) {
    case SlotStatus::AVAILABLE:
    case SlotStatus::FROM_BUFFER:
        // Active slot with filament loaded at nozzle = full path
        if (is_active && system_info_.filament_loaded) {
            return PathSegment::NOZZLE;
        }
        // Active slot during loading = show at hub (in transit)
        if (is_active && system_info_.action == AmsAction::LOADING) {
            return PathSegment::HUB;
        }
        return PathSegment::HUB;
    case SlotStatus::LOADED:
        return PathSegment::NOZZLE;
    default:
        return PathSegment::NONE;
    }
}

PathSegment AmsBackendCfs::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& alert : system_info_.alerts) {
        if (alert.level == AmsAlertLevel::SLOT) {
            return PathSegment::LANE;
        }
        if (alert.level == AmsAlertLevel::SYSTEM || alert.level == AmsAlertLevel::UNIT) {
            return PathSegment::HUB;
        }
    }
    return PathSegment::NONE;
}

// --- Operations ---

AmsError AmsBackendCfs::load_filament(int slot_index) {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    auto gcode = load_gcode(slot_index);
    if (gcode.empty()) {
        return AmsErrorHelper::invalid_slot(slot_index, 15);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
        system_info_.current_slot = slot_index;
    }
    return dispatch_action_script(std::move(gcode));
}

AmsError AmsBackendCfs::unload_filament(int) {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::UNLOADING;
    }
    return dispatch_action_script(unload_gcode());
}

AmsError AmsBackendCfs::select_slot(int) {
    return AmsErrorHelper::not_supported("CFS loads directly");
}

AmsError AmsBackendCfs::change_tool(int tool) {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;

    bool needs_unload = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        needs_unload = system_info_.filament_loaded || system_info_.current_slot >= 0;
    }

    // Validate gcode before mutating state
    std::string gcode = needs_unload ? swap_gcode(tool) : load_gcode(tool);
    if (gcode.empty()) {
        return AmsErrorHelper::invalid_slot(tool, 15);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
        system_info_.current_slot = tool;
    }
    return dispatch_action_script(std::move(gcode));
}

AmsError AmsBackendCfs::reset() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    return execute_gcode(reset_gcode());
}

AmsError AmsBackendCfs::recover() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    return execute_gcode(recover_gcode());
}

AmsError AmsBackendCfs::cancel() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    return execute_gcode("CANCEL_PRINT");
}

// --- set_slot_info ---

AmsError AmsBackendCfs::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Find the unit and local slot for this global index
        SlotInfo* target = nullptr;
        for (auto& unit : system_info_.units) {
            int first = unit.first_slot_global_index;
            int last = first + static_cast<int>(unit.slots.size()) - 1;
            if (slot_index >= first && slot_index <= last) {
                int local = slot_index - first;
                target = &unit.slots[local];
                break;
            }
        }

        if (!target) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }

        // Update in-memory slot state so get_slot_info returns the edit
        // immediately — covers every SlotInfo field the caller may have set,
        // including persist=false previews that must survive until the next
        // firmware parse.
        target->color_rgb = info.color_rgb;
        target->color_name = info.color_name;
        target->material = info.material;
        target->brand = info.brand;
        target->spool_name = info.spool_name;
        target->spoolman_id = info.spoolman_id;
        target->spoolman_vendor_id = info.spoolman_vendor_id;
        target->remaining_weight_g = info.remaining_weight_g;
        target->total_weight_g = info.total_weight_g;

        // For persist=true, stage the override into overrides_ so
        // apply_overrides re-applies the new values on every subsequent parse.
        // For persist=false we explicitly do NOT touch overrides_ — preview
        // edits are in-memory only and will be overwritten by the next
        // firmware parse (expected preview contract).
        //
        // NOTE on self-wipe: CFS's hardware-event check is RFID-fingerprint-based
        // (material_type + color_value from firmware). The user cannot set
        // those raw RFID strings through the UI — set_slot_info only touches
        // display-level material/color_rgb fields, never the raw firmware
        // material_type code. So last_rfid_uid_ stays at whatever firmware
        // last reported and user edits don't race against the hardware-event
        // detection. Same logic as Snapmaker.
        if (persist) {
            helix::ams::FilamentSlotOverride ovr;
            ovr.brand = info.brand;
            ovr.spool_name = info.spool_name;
            ovr.spoolman_id = info.spoolman_id;
            ovr.spoolman_vendor_id = info.spoolman_vendor_id;
            ovr.remaining_weight_g = info.remaining_weight_g;
            ovr.total_weight_g = info.total_weight_g;
            ovr.color_rgb = info.color_rgb;
            ovr.color_name = info.color_name;
            ovr.material = info.material;
            // SlotInfo carries the user's edit OR the bound Spoolman spool's
            // filament profile; the material-DB fallback for fields left at 0
            // is applied at emit time inside resolved_temps(). Centralized in
            // the helper so the four AMS backends stay in sync.
            helix::ams::populate_temps_from_slot_info(ovr, info);
            // updated_at left default — save_async stamps a fresh value.
            overrides_[slot_index] = ovr;
        }
    }

    spdlog::info("[AMS CFS] Updated slot {} info (persist={}): {} {}",
                 slot_index, persist, info.material, info.color_name);

    if (persist && override_store_) {
        // Re-read from overrides_ under the lock to pick up the staged copy.
        helix::ams::FilamentSlotOverride ovr_to_save;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = overrides_.find(slot_index);
            if (it != overrides_.end()) {
                ovr_to_save = it->second;
            }
        }
        // Capture by value — save_async's MR callback may fire long after
        // this returns (MR tracker ~60s timeout). Do NOT capture `this`:
        // the backend may outlive its store, but the store will outlive
        // the scheduled save by design.
        const std::string tag = backend_log_tag();
        override_store_->save_async(
            slot_index, ovr_to_save,
            [tag, slot_index](bool success, const std::string& err) {
                if (!success) {
                    spdlog::warn("{} Override persist failed for slot {}: {}", tag, slot_index,
                                 err);
                }
            });
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendCfs::set_tool_mapping(int, int) {
    return AmsErrorHelper::not_supported("CFS tool mapping via BOX_MODIFY_TN");
}

AmsError AmsBackendCfs::enable_bypass() {
    return AmsErrorHelper::not_supported("CFS has no bypass");
}

AmsError AmsBackendCfs::disable_bypass() {
    return AmsErrorHelper::not_supported("CFS has no bypass");
}

// --- GCode helpers ---

namespace {

/// Wrap a CR_BOX_* operation with the K2 stock screen's positioning
/// envelope. Without this, the flush extrudes onto the build plate instead
/// of into the K2's waste port — `BOX_GO_TO_EXTRUDE_POS` parks the toolhead
/// over the waste area and `BOX_MOVE_TO_SAFE_POS` parks it back when the
/// CR_BOX_END_OPT terminator finishes. SAVE/RESTORE_GCODE_STATE preserves
/// the caller's coordinate mode, feedrate, and absolute/relative state.
std::string wrap_with_park(const std::string& body) {
    return "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
           "BOX_GO_TO_EXTRUDE_POS\n" +
           body +
           "\nBOX_MOVE_TO_SAFE_POS\n"
           "RESTORE_GCODE_STATE NAME=helix_cfs_load";
}

} // namespace

std::string AmsBackendCfs::load_gcode(int idx) {
    std::string tnn = CfsMaterialDb::slot_to_tnn(idx);
    if (tnn.empty()) {
        spdlog::error("[AMS CFS] Invalid slot index for load: {}", idx);
        return "";
    }
    // Use CR_BOX_* commands directly — M8200 macro's Jinja2 `params.I|int` is broken
    // on Creality's Klipper fork (always evaluates to 0, loading T1A regardless of I= value).
    // CR_BOX_PRE_OPT is required before extrude — sets CFS to material-change mode.
    // CR_BOX_WASTE must follow CR_BOX_EXTRUDE (purges transition material).
    return wrap_with_park("CR_BOX_PRE_OPT\nCR_BOX_EXTRUDE TNN=" + tnn +
                          "\nCR_BOX_WASTE\nCR_BOX_FLUSH TNN=" + tnn + "\nCR_BOX_END_OPT");
}

std::string AmsBackendCfs::unload_gcode() {
    // Unload doesn't need TNN — operates on currently loaded filament
    return wrap_with_park("CR_BOX_PRE_OPT\nCR_BOX_CUT\nCR_BOX_RETRUDE\nCR_BOX_END_OPT");
}

std::string AmsBackendCfs::swap_gcode(int idx) {
    std::string tnn = CfsMaterialDb::slot_to_tnn(idx);
    if (tnn.empty()) {
        spdlog::error("[AMS CFS] Invalid slot index for swap: {}", idx);
        return "";
    }
    // Full swap: unload current (cut+retract) then load new slot, all in one session
    return wrap_with_park("CR_BOX_PRE_OPT\nCR_BOX_CUT\nCR_BOX_RETRUDE\nCR_BOX_EXTRUDE TNN=" + tnn +
                          "\nCR_BOX_WASTE\nCR_BOX_FLUSH TNN=" + tnn + "\nCR_BOX_END_OPT");
}

AmsError AmsBackendCfs::dispatch_action_script(std::string gcode) {
    if (!api_) {
        return AmsErrorHelper::not_connected("MoonrakerAPI not available");
    }

    auto on_complete = [this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (system_info_.action != AmsAction::IDLE) {
            spdlog::info("[AMS CFS] Action script complete — action {} -> IDLE",
                         static_cast<int>(system_info_.action));
            system_info_.action = AmsAction::IDLE;
            PostOpCooldownManager::instance().schedule();
        }
    };

    auto token = lifetime_.token();
    auto on_error = [this, token](const MoonrakerError& err) {
        if (token.expired()) return;
        // Klipper rejection (key849, busy, etc.) and timeouts both land here.
        // Either way the driver isn't running our script anymore, so flip back
        // to IDLE so the UI doesn't get stuck on a "loading" spinner.
        spdlog::error("[AMS CFS] Action script failed: {}", err.message);
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::IDLE;
    };

    // Homing-then-execute — same pattern as ensure_homed_then but with our
    // own completion callbacks that propagate to action-state cleanup.
    if (!client_) {
        api_->execute_gcode(gcode, std::move(on_complete), std::move(on_error),
                            MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
        return AmsErrorHelper::success();
    }

    auto gcode_copy = std::move(gcode);
    client_->send_jsonrpc(
        "printer.objects.query",
        nlohmann::json{{"objects", nlohmann::json{{"toolhead", nlohmann::json::array({"homed_axes"})}}}},
        [this, token, gcode_copy, on_complete = std::move(on_complete),
         on_error = on_error](const nlohmann::json& response) {
            if (token.expired()) return;

            bool needs_home = true;
            if (response.contains("result") && response["result"].contains("status")) {
                const auto& status = response["result"]["status"];
                if (status.contains("toolhead") &&
                    status["toolhead"].contains("homed_axes") &&
                    status["toolhead"]["homed_axes"].is_string()) {
                    std::string axes = status["toolhead"]["homed_axes"].get<std::string>();
                    needs_home = (axes.find("xyz") == std::string::npos);
                }
            }

            if (needs_home) {
                spdlog::info("[AMS CFS] Not homed, sending G28 before action script");
                api_->execute_gcode(
                    "G28",
                    [this, token, gcode_copy, on_complete, on_error]() {
                        if (token.expired()) return;
                        api_->execute_gcode(gcode_copy, on_complete, on_error,
                                            MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
                    },
                    on_error, MoonrakerAPI::HOMING_TIMEOUT_MS);
            } else {
                api_->execute_gcode(gcode_copy, on_complete, on_error,
                                    MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
            }
        },
        on_error);

    return AmsErrorHelper::success();
}

std::string AmsBackendCfs::reset_gcode() {
    return "BOX_ERROR_CLEAR";
}

std::string AmsBackendCfs::recover_gcode() {
    return "BOX_ERROR_RESUME_PROCESS";
}

// --- Capabilities ---

helix::printer::EndlessSpoolCapabilities AmsBackendCfs::get_endless_spool_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {.supported = true,
            .editable = false,
            .description = system_info_.supports_endless_spool ? "Auto-refill enabled"
                                                               : "Auto-refill disabled"};
}

helix::printer::ToolMappingCapabilities AmsBackendCfs::get_tool_mapping_capabilities() const {
    return {.supported = true, .editable = false, .description = ""};
}

std::vector<helix::printer::DeviceAction> AmsBackendCfs::get_device_actions() const {
    using DA = helix::printer::DeviceAction;
    using AT = helix::printer::ActionType;
    return {
        DA{"refresh_rfid", "Refresh RFID", "", "", "Re-read spool RFID tags and remaining length",
           AT::BUTTON, {}, {}, 0, 100, "", -1, true, ""},
        DA{"toggle_auto_refill", "Toggle Auto-Refill", "", "",
           "Enable/disable automatic backup spool switching",
           AT::TOGGLE, {}, {}, 0, 100, "", -1, true, ""},
        DA{"nozzle_clean", "Clean Nozzle", "", "",
           "Wipe nozzle on silicone cleaning strip",
           AT::BUTTON, {}, {}, 0, 100, "", -1, true, ""},
        DA{"comm_test", "Communication Test", "", "",
           "Test RS-485 link to CFS units",
           AT::BUTTON, {}, {}, 0, 100, "", -1, true, ""},
    };
}

AmsError AmsBackendCfs::execute_device_action(const std::string& action_id,
                                               const std::any& /*value*/) {
    if (action_id == "refresh_rfid") {
        // Re-query box state from Moonraker (the box module polls CFS automatically)
        on_started(); // Triggers printer.objects.query for box state
        return AmsErrorHelper::success();
    }

    if (action_id == "toggle_auto_refill") {
        return execute_gcode("BOX_ENABLE_AUTO_REFILL");
    }

    if (action_id == "nozzle_clean") {
        return execute_gcode("BOX_NOZZLE_CLEAN");
    }

    if (action_id == "comm_test") {
        // Query box state as a connectivity test
        on_started();
        return AmsErrorHelper::success();
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}

// ============================================================================
// Override layering (shared FilamentSlotOverrideStore)
// ============================================================================

void AmsBackendCfs::apply_overrides(SlotInfo& slot, int slot_index) {
    // Every caller of apply_overrides runs under mutex_ (handle_status_update
    // post-parse loop). overrides_ writers also hold mutex_, so the map read
    // here is implicitly lock-protected. Zero-cost hash miss when the slot
    // has no override — safe in the hot parse path.
    auto it = overrides_.find(slot_index);
    if (it == overrides_.end())
        return;
    const auto& o = it->second;
    // Merge policy matches Snapmaker / ACE. Override wins only when the
    // override field carries a real value; defaults fall through to firmware.
    if (!o.brand.empty())
        slot.brand = o.brand;
    if (!o.spool_name.empty())
        slot.spool_name = o.spool_name;
    if (o.spoolman_id > 0)
        slot.spoolman_id = o.spoolman_id;
    if (o.spoolman_vendor_id > 0)
        slot.spoolman_vendor_id = o.spoolman_vendor_id;
    if (o.remaining_weight_g >= 0.0f)
        slot.remaining_weight_g = o.remaining_weight_g;
    if (o.total_weight_g >= 0.0f)
        slot.total_weight_g = o.total_weight_g;
    if (o.color_rgb != 0)
        slot.color_rgb = o.color_rgb;
    if (!o.color_name.empty())
        slot.color_name = o.color_name;
    if (!o.material.empty())
        slot.material = o.material;
}

void AmsBackendCfs::check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                               const std::string& observed_uid) {
    // Empty observed UID = no RFID tag / sentinel material_type / sentinel
    // color_value. Treat as non-signal: don't update the baseline, don't
    // clear. Without this guard every tag-less poll would overwrite a real
    // prior fingerprint and mask a genuine hardware swap on the next good
    // read. Matches Snapmaker's empty-UID semantics.
    if (observed_uid.empty())
        return;

    auto it = last_rfid_uid_.find(slot_index);
    if (it == last_rfid_uid_.end()) {
        // First observation for this slot — establish baseline. Even if the
        // override was previously saved against a different fingerprint, the
        // first observation is NEVER a swap signal. apply_overrides still
        // runs after us and the override wins.
        last_rfid_uid_[slot_index] = observed_uid;
        spdlog::debug("{} Slot {} baseline RFID fingerprint: {}", backend_log_tag(),
                      slot_index, observed_uid);
        return;
    }
    if (it->second == observed_uid)
        return; // unchanged — no swap signal

    // Fingerprint changed. Record the new value as the baseline FIRST so a
    // failed clear_async doesn't make us re-fire on every subsequent poll.
    const std::string old_uid = it->second;
    it->second = observed_uid;

    auto ovr_it = overrides_.find(slot_index);
    if (ovr_it == overrides_.end()) {
        spdlog::debug("{} Slot {} RFID fingerprint changed {} -> {} (no override to clear)",
                      backend_log_tag(), slot_index, old_uid, observed_uid);
        return;
    }

    spdlog::info("{} Slot {} RFID fingerprint changed {} -> {}, clearing override "
                 "(physical spool swap detected)",
                 backend_log_tag(), slot_index, old_uid, observed_uid);

    // Delegate erase + field reset + clear_async to the shared helper so
    // hardware-event clears and user-initiated clears share one field-reset
    // policy. Caller already holds mutex_.
    (void)ovr_it;
    clear_override_locked(slot_index, slot);
}

void AmsBackendCfs::clear_override_locked(int slot_index, SlotInfo& slot) {
    // Caller must hold mutex_. Erases the in-memory override, resets STRICTLY
    // override-exclusive fields on the live SlotInfo so the cleared state is
    // visible in the very next get_slot_info() read (apply_overrides is a
    // no-op for this slot afterwards).
    //
    // CFS field policy: brand / color_name / total_weight_g come from the
    // RFID material database (CfsMaterialInfo lookup in parse_box_status) —
    // the parse has already written firmware truth for the current spool, so
    // we must NOT re-zero those fields. The override's copies disappear with
    // the erase; firmware's copies stay. Matches Snapmaker policy.
    overrides_.erase(slot_index);

    slot.spool_name.clear();
    slot.spoolman_id = 0;
    slot.spoolman_vendor_id = 0;
    slot.remaining_weight_g = -1.0f;

    if (override_store_) {
        // Capture by value — clear_async's Moonraker callback may fire after
        // this returns (MR tracker ~60s) and potentially after the backend
        // itself is gone. Same rationale as save_async.
        const std::string tag = backend_log_tag();
        override_store_->clear_async(
            slot_index, [tag, slot_index](bool ok, std::string err) {
                if (!ok) {
                    spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
                }
            });
    }
}

void AmsBackendCfs::clear_slot_override(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            spdlog::warn("{} clear_slot_override: no slot entry for global index {}",
                         backend_log_tag(), slot_index);
            return;
        }
        spdlog::info("{} Slot {} override cleared by user request", backend_log_tag(), slot_index);
        clear_override_locked(slot_index, *slot);
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
}

} // namespace helix::printer

#endif // HELIX_HAS_CFS
