// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_CFS

#include "ams_backend_cfs.h"

#include "ui_temperature_utils.h"

#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_error.h"
#include "post_op_cooldown_manager.h"
#include "printer_detector.h"

#include <spdlog/spdlog.h>

#include <fstream>

#include "hv/json.hpp"

namespace helix::printer {

using json = nlohmann::json;

const CfsMaterialDb& CfsMaterialDb::instance() {
    static CfsMaterialDb db;
    return db;
}

CfsMaterialDb::CfsMaterialDb() {
    load_database();
}

void CfsMaterialDb::load_database() {
    for (const auto& path : {"assets/cfs_materials.json", "../assets/cfs_materials.json",
                             "/opt/helixscreen/assets/cfs_materials.json"}) {
        std::ifstream f(path);
        if (!f.is_open())
            continue;

        try {
            auto j = nlohmann::json::parse(f);
            for (auto& [id, entry] : j.items()) {
                CfsMaterialInfo info;
                info.id = id;
                info.brand = entry.value("brand", "");
                info.name = entry.value("name", "");
                info.material_type = entry.value("type", "");
                info.min_temp = entry.value("min_temp", 0);
                info.max_temp = entry.value("max_temp", 0);
                materials_[id] = std::move(info);
            }
            spdlog::info("[AMS CFS] Loaded {} materials from {}", materials_.size(), path);
            return;
        } catch (const std::exception& e) {
            spdlog::warn("[AMS CFS] Failed to parse {}: {}", path, e.what());
        }
    }
    spdlog::warn("[AMS CFS] Material database not found");
}

const CfsMaterialInfo* CfsMaterialDb::lookup(const std::string& id) const {
    auto it = materials_.find(id);
    return it != materials_.end() ? &it->second : nullptr;
}

std::string CfsMaterialDb::strip_code(const std::string& code) {
    if (code == "-1" || code == "None" || code.empty())
        return "";
    if (code.size() == 6 && code[0] == '1')
        return code.substr(1);
    return code;
}

uint32_t CfsMaterialDb::parse_color(const std::string& color_str) {
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

std::string CfsMaterialDb::slot_to_tnn(int global_index) {
    if (global_index < 0 || global_index > 15)
        return "";
    int unit = global_index / 4 + 1;
    int slot = global_index % 4;
    char letter = 'A' + static_cast<char>(slot);
    return "T" + std::to_string(unit) + letter;
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
    if (!values.is_array() || values.size() < 2)
        return "";
    if (!values[0].is_number_integer())
        return "";
    if (!values[1].is_string())
        return "";
    int unit = values[0].get<int>();
    std::string slot = values[1].get<std::string>();
    if (unit < 1 || slot.size() != 1)
        return "";
    char c = slot[0];
    if (c < 'A' || c > 'D')
        return "";
    return " in unit " + std::to_string(unit) + " slot " + slot;
}

/// Format unit-level errors where values is `[unit]` or `[unit, ...]`.
/// We just grab the leading int and ignore tails. Empty string if the
/// shape doesn't match.
std::string format_unit_only(const nlohmann::json& values) {
    if (!values.is_array() || values.empty())
        return "";
    if (!values[0].is_number_integer())
        return "";
    int unit = values[0].get<int>();
    if (unit < 1)
        return "";
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
    {"key111",
     {"Pre-heat the extruder first",
      "Filament can't be loaded below the minimum extrude temperature. Set a hotend target (e.g. "
      "220°C for PLA, 240°C for PETG), wait for it to reach temperature, then try again",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key298",
     {"MCU bridge daemon is shut down",
      "Tap Firmware Restart to recover — on K2 this also bounces the rpi MCU bridge",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key585",
     {"Move out of range", "The requested position is outside the printer's bounds",
      AmsAlertLevel::SYSTEM, nullptr}},

    // Motor controller init errors (motor_control_wrapper.so). Typically fire
    // during CFS bring-up. Users can't fix these directly — power cycle is the
    // standard remedy. Surfacing them avoids the raw chinglish from Creality
    // ("Motor set pin restore io status error").
    {"key800",
     {"Motor controller error",
      "A motor IO pin couldn't be restored. Power-cycle the printer to recover",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key801",
     {"Z motor calibration failed",
      "Stall detection setup failed for the Z motor — power-cycle to retry", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key802",
     {"Extruder motor calibration failed",
      "Stall detection setup failed for the extruder — power-cycle to retry", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key803",
     {"Motor parameter setup failed",
      "A motor's parameters couldn't be written. Power-cycle the printer to retry",
      AmsAlertLevel::SYSTEM, nullptr}},

    {"key831",
     {"Lost connection to CFS unit", "Check the RS-485 cable between printer and CFS",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key834",
     {"Invalid parameters sent to CFS", "This may indicate a firmware bug — try restarting",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key835",
     {"Filament jammed at CFS connector",
      "Open the CFS lid, check the PTFE tube connection for the stuck slot", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key836",
     {"Filament jammed between CFS and sensor", "Check the Bowden tube for kinks or debris",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key837",
     {"Filament jammed before extruder gear",
      "Check for tangles on the spool and clear the filament path to the printhead",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key838",
     {"Filament reached extruder but won't feed",
      "Check for a clog in the hotend or a worn drive gear", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key839",
     {"No filament detected at CFS extrude position",
      "The selected slot may be empty or the filament didn't reach the CFS extruder",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key840",
     {"CFS unit state error", "A unit reported an unexpected state — check its current operation",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key841",
     {"Filament cutter stuck",
      "The cutter blade didn't return — check for filament wrapped around the cutting mechanism",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key843",
     {"Can't read filament RFID tag",
      "Re-seat the spool in the slot, ensure the RFID label faces the reader", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key844",
     {"PTFE tube connection loose", "Re-seat the Bowden tube connector on the CFS unit",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key845",
     {"Nozzle clog detected", "Run a cold pull or replace the nozzle", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key846",
     {"Empty print detected — feed rate too slow",
      "CFS feed rate fell below extruder demand. The spool may be empty or jammed",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key847",
     {"Empty spool — filament wound around hub",
      "Remove the empty spool and clear wound filament from the CFS hub", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key848",
     {"Filament snapped inside CFS",
      "Open the CFS unit and remove the broken filament from the slot", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key849",
     {"Retract failed — filament stuck in connector",
      "Manually pull the filament back through the connector", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key850",
     {"Retract error — multiple connectors triggered",
      "Check that only one filament path is active", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key851",
     {"Retract didn't reach buffer empty position",
      "The filament may not have fully retracted — try again or manually pull", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key852",
     {"Sensor mismatch — check extruder and CFS sensors",
      "Extruder and CFS disagree on filament state — inspect both sensors", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key853",
     {"Humidity sensor malfunction",
      "CFS unit's humidity sensor is not responding — may need service", AmsAlertLevel::UNIT,
      fmt_unit_only}},
    {"key854",
     {"Cutter blade didn't sever filament",
      "Filament is still present after the cut — the blade may be dull or misaligned",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key855",
     {"Filament cutter position error",
      "The cutter is out of alignment — recalibrate with CALIBRATE_CUT_POS", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key856",
     {"Filament cutter not detected", "Check that the cutter mechanism is properly installed",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key857",
     {"CFS motor overloaded", "A spool may be tangled or the drive gear is jammed",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key858",
     {"EEPROM error on CFS unit", "CFS unit storage is corrupted — may need firmware reflash",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key859",
     {"Measuring wheel error", "The filament length sensor is malfunctioning", AmsAlertLevel::UNIT,
      fmt_unit_only}},
    {"key860",
     {"Buffer tube problem", "Check the buffer unit on the back of the printer",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key861",
     {"RFID reader malfunction (left)", "The left RFID reader in this CFS unit may need service",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key862",
     {"RFID reader malfunction (right)", "The right RFID reader in this CFS unit may need service",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key863",
     {"Retract error — filament still detected",
      "Filament didn't fully retract, may need manual removal", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key864",
     {"Extrude error — buffer not full", "Filament didn't fill buffer tube during load",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key865",
     {"Retract error — failed to exit connector", "Filament stuck in connector during unload",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    // key866 is a second "no cutter" variant emitted from the motor driver path
    // (key856 comes from the box driver). Same root cause, same remedy.
    {"key866",
     {"Filament cutter not detected", "Check that the cutter mechanism is properly installed",
      AmsAlertLevel::SYSTEM, nullptr}},
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

std::optional<AmsAlert> CfsErrorDecoder::decode(const std::string& key_code, int unit_index,
                                                int slot_index) {
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

    // Latch K1 vs K2 macro dialect once. PrinterDetector reads the resolved
    // printer type straight from Config, which is the only source guaranteed
    // populated this early: the CFS backend is constructed in the discovery
    // callback (init_subsystems_from_hardware) BEFORE auto_detect_and_save runs.
    // Reading from Config here is therefore deliberate — do not move this to a
    // capability snapshot that is populated later in the discovery sequence, or
    // a saved-type K1/Hi relaunch would mis-route to the K2 dialect. #968.
    //
    // The Creality Hi ships the non-prefixed BOX_* dialect in its box.cfg
    // (BOX_CUT_MATERIAL / BOX_EXTRUDE_MATERIAL / BOX_RETRUDE_MATERIAL /
    // BOX_GO_TO_EXTRUDE_POS / BOX_SAVE_FAN / BOX_RESTORE_FAN), i.e. the K1
    // family — NOT the K2 CR_BOX_* primitives. Route it to the K1 variant so
    // load/unload/swap dispatch resolves to macros the firmware actually
    // defines.
    macro_variant_ = (PrinterDetector::is_creality_k1() || PrinterDetector::is_creality_hi())
                         ? CfsMacroVariant::K1
                         : CfsMacroVariant::K2;

    spdlog::info("[AMS CFS] Backend created — macro variant: {}",
                 macro_variant_ == CfsMacroVariant::K1 ? "K1 (BOX_*)" : "K2 (CR_BOX_*)");
}

// --- Sensor Ownership (#1054) ---

bool AmsBackendCfs::owns_filament_sensor(const std::string& bare_name,
                                         const helix::PrinterDiscovery& discovery) {
    (void)discovery; // CFS owns a single fixed name; no discovery needed.
    // K2 CFS exposes one filament_switch_sensor at the toolhead with the bare
    // name "filament_sensor". Conventional elsewhere, so only claimed when CFS
    // is the detected backend.
    return bare_name == "filament_sensor";
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
        spdlog::info("[AMS CFS] Loaded {} slot overrides from filament_slot store", loaded_count);
    }

    // Query initial state explicitly since the subscription response may have
    // arrived before this backend registered its callback
    nlohmann::json objects_to_query = nlohmann::json::object();
    objects_to_query["box"] = nullptr;
    objects_to_query["motor_control"] = nullptr;
    objects_to_query["filament_switch_sensor filament_sensor"] = nullptr;
    // Phase synthesis (Task #2) needs target rises to detect purge phase
    objects_to_query["extruder"] = nlohmann::json::array({"target", "temperature"});

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

    // box.filament is a stale active-lane SELECTION index, NOT a "filament
    // loaded" truth — it retains a lane number even when nothing is loaded,
    // producing phantom "loaded from lane N". The authoritative loaded signal
    // is the toolhead sensor (filament_switch_sensor filament_sensor), handled
    // in handle_status_update. Leave filament_loaded false here.
    info.filament_loaded = false;

    // Runout signal: box.filament_useup == 1 means no filament at the box gate.
    // Raw firmware value here; ams_state gates display on print-paused state to
    // avoid false positives at pre-load print start (see filament_useup decode).
    info.filament_runout = box_json.value("filament_useup", 0) != 0;

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
        if (temp_str != "None" && temp_str != "-1" && humid_str != "None" && humid_str != "-1") {
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
        auto vender_arr = unit_json.value("vender", nlohmann::json::array());

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

            std::string vender_str = "none";
            if (i < static_cast<int>(vender_arr.size()) && vender_arr[i].is_string()) {
                vender_str = vender_arr[i].get<std::string>();
            }

            // Brand fallback: the CFS material-DB lookup above populates
            // slot.brand for known Creality material codes. For RFID spools
            // whose code isn't in our DB, fall back to the box's own per-slot
            // "vender" string when it carries a real value (hardware reports
            // the sentinel "unknown"/"-1" when no RFID vendor data is present).
            if (slot.brand.empty() && !vender_str.empty() && vender_str != "unknown" &&
                vender_str != "-1" && vender_str != "None" && vender_str != "none") {
                slot.brand = vender_str;
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

            // Derive status. color_value is latched RFID data: it reads the
            // sentinel "-1"/"None"/"unknown" for a removed spool or a
            // physically-present untagged (3rd-party) spool. Treat all three as
            // "no RFID presence" so a stale/untagged color doesn't fake an
            // AVAILABLE bay. A user override later promotes assigned bays back
            // to AVAILABLE (see apply_overrides).
            if (color_str == "-1" || color_str == "None" || color_str == "unknown") {
                slot.status = SlotStatus::EMPTY;
            } else if (slot.remaining_length_m <= 0.0f && remain_str != "-1") {
                slot.status = SlotStatus::EMPTY;
            } else {
                slot.status = SlotStatus::AVAILABLE;
            }

            if (vender_str == "none" || vender_str == "None" || vender_str == "-1" ||
                vender_str.empty()) {
                slot.status = SlotStatus::EMPTY;

                slot.brand.clear();
                slot.material.clear();
                slot.color_name.clear();
                slot.spool_name.clear();
                slot.color_rgb = CfsMaterialDb::parse_color("-1");
                slot.spoolman_id = 0;
                slot.spoolman_vendor_id = 0;
                slot.remaining_length_m = 0.0f;
                slot.remaining_weight_g = -1.0f;
                slot.total_weight_g = -1.0f;
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
            // CFS slots map 1:1 to tools (slot.mapped_tool = slot.global_index above);
            // current_tool must mirror current_slot so the print-status color dot
            // labels the correct tool index.
            info.current_tool = info.current_slot;
            spdlog::debug("[AMS CFS] Active filament: {} (slot {}, tool {})", fil_letter,
                          info.current_slot, info.current_tool);
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

        // Log box.filament_useup transitions with box-local context. Decoded
        // (2026-06-18, live K2 runout->reload): runout / path-empty signal —
        // 1 when no filament at the box gate (pre-load, runout), 0 when loaded
        // and feeding. Reads only the notification's own fields — no
        // system_info_ access, so no lock needed here. No UI effect yet.
        if (box.contains("filament_useup") && box["filament_useup"].is_number()) {
            int useup = box["filament_useup"].get<int>();
            if (useup != last_filament_useup_) {
                spdlog::debug("[AMS CFS] filament_useup {} -> {} (box.filament={}, "
                              "auto_refill={}, enable={})",
                              last_filament_useup_, useup, box.value("filament", -1),
                              box.value("auto_refill", -1), box.value("enable", -1));
                last_filament_useup_ = useup;
            }
        }

        // Distinguish meaningful updates from noise (e.g., just measuring_wheel).
        // Full updates have "filament"/"map". Unit updates have "T1"/"T2"/etc.
        bool has_top_level = box.contains("filament") || box.contains("map");
        bool has_unit_data =
            box.contains("T1") || box.contains("T2") || box.contains("T3") || box.contains("T4");
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

            // Deliberately do NOT touch filament_loaded here. box.filament is a
            // selection index, not a loaded flag (see parse_box_status). The
            // toolhead-sensor branch below is the sole writer of
            // filament_loaded — a box update lacking the sensor param must not
            // clobber the sensor-derived value.

            // Update runout flag only when the field was actually present
            if (box.contains("filament_useup")) {
                system_info_.filament_runout = new_info.filament_runout;
            }

            // Active slot from T{n}.filament field ("A"/"B"/"C"/"D"). When the
            // notification carried per-unit lane data but no unit reports an
            // active lane (current_slot < 0) and we're not mid-load, clear the
            // active selection. Driven solely by the per-unit T{n}.filament
            // letter — not box.filament, which is a stale selection index, not
            // a loaded flag. Gate on has_unit_data so a partial top-level-only
            // update (e.g. box:{filament:N} during a tool change) can't clobber
            // a still-valid active slot.
            if (new_info.current_slot >= 0) {
                system_info_.current_slot = new_info.current_slot;
                system_info_.current_tool = new_info.current_tool;
            } else if (has_unit_data && system_info_.action != AmsAction::LOADING) {
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
                    // Mirror firmware-truth color/material into lane_data so
                    // OrcaSlicer's MoonrakerPrinterAgent sees the spool. Runs
                    // BEFORE apply_overrides so the values reflect firmware,
                    // not the override-masked view. FillUnsetOnly: CFS user
                    // edits don't reach firmware, so we must not let firmware
                    // overwrite them — see mirror_firmware_to_lane_data docs.
                    helix::ams::mirror_firmware_to_lane_data(
                        override_store_.get(), overrides_, global_idx, slot.color_rgb,
                        slot.material, slot.status == SlotStatus::AVAILABLE,
                        helix::ams::MirrorPolicy::FillUnsetOnly, backend_log_tag());
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

            // Drive phase synthesis off the transition (Task #2).
            if (detected != last_filament_detected_) {
                on_filament_transition_locked(detected);
            }
            last_filament_detected_ = detected;
        }
        changed = true;
    }

    if (params.contains("extruder")) {
        const auto& extr = params["extruder"];
        bool action_transitioned = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (extr.contains("target") && extr["target"].is_number()) {
                last_extruder_target_deci_ =
                    helix::units::to_decidegrees(extr["target"].get<double>());
            }
            if (extr.contains("temperature") && extr["temperature"].is_number()) {
                last_extruder_temp_deci_ =
                    helix::units::to_decidegrees(extr["temperature"].get<double>());
            }
            AmsAction before = system_info_.action;
            on_extruder_temp_change_locked(last_extruder_temp_deci_, last_extruder_target_deci_);
            action_transitioned = (system_info_.action != before);
        }
        // Extruder telemetry is high-frequency; only emit when action
        // actually moved, otherwise we'd thrash the UI on every temp tick.
        if (action_transitioned) {
            changed = true;
        }
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
    if (err.result != AmsResult::SUCCESS)
        return err;
    auto gcode = load_gcode(slot_index, macro_variant_);
    if (gcode.empty()) {
        return AmsErrorHelper::invalid_slot(slot_index, 15);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
        system_info_.current_slot = slot_index;
        begin_phase_tracking();
        apply_synthesized_action_locked();
    }
    return dispatch_action_script(std::move(gcode));
}

AmsError AmsBackendCfs::unload_filament(int) {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS)
        return err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::UNLOADING;
        begin_phase_tracking();
        apply_synthesized_action_locked();
    }
    return dispatch_action_script(unload_gcode(macro_variant_));
}

AmsError AmsBackendCfs::select_slot(int) {
    return AmsErrorHelper::not_supported("CFS loads directly");
}

AmsError AmsBackendCfs::change_tool(int tool) {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS)
        return err;

    bool needs_unload = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Cut-first decision centralized in needs_unload_before_load(): on K1 CFS
        // current_slot reports a *preloaded* (cassette-staged) slot with the
        // nozzle still empty, so the K1 override keys on filament_loaded only
        // (avoids the "hallucinated cut on an empty nozzle" the reporter saw,
        // #968). K2 retains the filament_loaded || current_slot >= 0 behavior.
        needs_unload = needs_unload_before_load(system_info_);
    }

    // Validate gcode before mutating state
    std::string gcode =
        needs_unload ? swap_gcode(tool, macro_variant_) : load_gcode(tool, macro_variant_);
    if (gcode.empty()) {
        return AmsErrorHelper::invalid_slot(tool, 15);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
        system_info_.current_slot = tool;
        begin_phase_tracking();
        apply_synthesized_action_locked();
    }
    return dispatch_action_script(std::move(gcode));
}

AmsError AmsBackendCfs::reset() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS)
        return err;
    return execute_gcode(reset_gcode());
}

AmsError AmsBackendCfs::recover() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS)
        return err;
    return execute_gcode(recover_gcode());
}

AmsError AmsBackendCfs::cancel() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS)
        return err;
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
            ovr.color_set = true; // a user-edit always records a color, even pure black (#000000)
            ovr.color_name = info.color_name;
            ovr.material = info.material;
            // User-lock: CFS uses FillUnsetOnly so the locks are
            // belt-and-suspenders here, but they keep the on-disk schema
            // consistent across backends and protect against a future
            // policy change. See #965 for the motivating bug.
            ovr.user_locked_color = true;
            ovr.user_locked_material = !info.material.empty();
            // SlotInfo carries the user's edit OR the bound Spoolman spool's
            // filament profile; the material-DB fallback for fields left at 0
            // is applied at emit time inside resolved_temps(). Centralized in
            // the helper so the four AMS backends stay in sync.
            helix::ams::populate_temps_from_slot_info(ovr, info);
            // updated_at left default — save_async stamps a fresh value.
            overrides_[slot_index] = ovr;
        }
    }

    spdlog::info("[AMS CFS] Updated slot {} info (persist={}): {} {}", slot_index, persist,
                 info.material, info.color_name);

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
            slot_index, ovr_to_save, [tag, slot_index](bool success, const std::string& err) {
                if (!success) {
                    spdlog::warn("{} Override persist failed for slot {}: {}", tag, slot_index,
                                 err);
                }
            });

        // Push the user's color back to firmware so the K2's stock LCD and
        // any other Moonraker-DB readers see the same value. Color-only —
        // material codes are CFS-internal and we don't have a complete
        // reverse-map. See push_slot_color_to_firmware docs for the discovered
        // BOX_MODIFY_TN_DATA gcode syntax.
        push_slot_color_to_firmware(slot_index, info.color_rgb);
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendCfs::push_slot_color_to_firmware(int global_index, uint32_t color_rgb) {
    // Validate slot index BEFORE formatting the gcode — invalid args trigger
    // an unhandled TypeError in box_wrapper that Klipper escalates to
    // invoke_shutdown. Better to silently no-op than to crash the printer.
    //
    // No color-value validation here on purpose: pure black (0x000000) is a
    // legitimate user choice and we don't want to silently drop it. The
    // caller (set_slot_info, which sets color_set=true on the override) is
    // responsible for only invoking this when a real color was chosen.
    constexpr int kCfsMaxSlots = 16; // 4 units × 4 slots
    if (global_index < 0 || global_index >= kCfsMaxSlots) {
        spdlog::debug("{} push_slot_color_to_firmware: skipping invalid slot {}", backend_log_tag(),
                      global_index);
        return;
    }

    const int unit = (global_index / 4) + 1;           // 1..4
    const char slot_letter = 'A' + (global_index % 4); // A..D
    char data[16];
    // CFS color_value format observed in tn_data.json: 7 hex chars, leading
    // nibble appears to be alpha (always 0 in stock data). RGB is the trailing
    // 6 hex digits. Match the firmware's own "0RRGGBB" format exactly.
    std::snprintf(data, sizeof(data), "0%06X", color_rgb & 0xFFFFFFu);

    char gcode[96];
    std::snprintf(gcode, sizeof(gcode),
                  "BOX_MODIFY_TN_DATA ADDR=%d NUM=%c PART=color_value DATA=%s", unit, slot_letter,
                  data);

    auto err = execute_gcode(gcode);
    if (err.result != AmsResult::SUCCESS) {
        // Non-fatal — the override is already in lane_data so user data isn't
        // lost; only the firmware-side LCD won't reflect this edit.
        spdlog::warn("{} push_slot_color_to_firmware: gcode dispatch failed for slot {}: {}",
                     backend_log_tag(), global_index, err.technical_msg);
    }
}

AmsError AmsBackendCfs::set_tool_mapping(int tool_number, int slot_index) {
    // CFS exposes per-print tool→slot remap via the BOX_MODIFY_TN gcode (format
    // observed in box_wrapper.cpython-39.so: "BOX_MODIFY_TN %s=%s"). Both sides
    // use the TNN notation (T1A..T4D) that matches the box.map JSON keys/values
    // returned by Moonraker.
    //
    // Example: set_tool_mapping(0, 5) sends "BOX_MODIFY_TN T1A=T2B" — when the
    // slicer emits T0/T1A, the CFS routes from physical slot T2B (index 5).
    constexpr int kCfsMaxSlots = 16; // 4 units × 4 slots
    if (tool_number < 0 || tool_number >= kCfsMaxSlots) {
        return AmsError(AmsResult::INVALID_TOOL,
                        "Tool " + std::to_string(tool_number) + " out of range",
                        "Invalid tool number", "");
    }
    if (slot_index < 0 || slot_index >= kCfsMaxSlots) {
        return AmsErrorHelper::invalid_slot(slot_index, kCfsMaxSlots - 1);
    }

    std::string tool_tnn = CfsMaterialDb::slot_to_tnn(tool_number);
    std::string slot_tnn = CfsMaterialDb::slot_to_tnn(slot_index);
    if (tool_tnn.empty() || slot_tnn.empty()) {
        return AmsError(AmsResult::INVALID_TOOL,
                        "Failed to encode TNN for tool=" + std::to_string(tool_number) +
                            " slot=" + std::to_string(slot_index),
                        "Invalid tool/slot", "");
    }

    // Optimistic local update so get_tool_mapping() reflects the new mapping
    // immediately for UI/restore-snapshot reads. The next box-status websocket
    // update will re-confirm (or correct, if Klipper rejected the BOX_MODIFY_TN
    // for some reason) — firmware remains the source of truth.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tool_number < static_cast<int>(system_info_.tool_to_slot_map.size())) {
            system_info_.tool_to_slot_map[tool_number] = slot_index;
        }
    }

    std::string cmd = "BOX_MODIFY_TN " + tool_tnn + "=" + slot_tnn;
    spdlog::info("[AMS CFS] Remapping tool {} -> slot {} ({})", tool_tnn, slot_tnn, cmd);
    if (macro_variant_ == CfsMacroVariant::K1) {
        // #968 Phase 5 — the reporter confirmed BOX_MODIFY_TN no-ops on the K1
        // official CFS upgrade firmware (the box's "current TN" state it sets is
        // ignored), and we have no confirmed working alternative. We still emit
        // it because it is harmless (no key61 "Unknown command"), but in-print
        // tool->slot remap may simply not take effect on K1. The optimistic
        // local update above keeps the UI consistent regardless.
        // TODO(#968): confirm a working K1 remap path on hardware (console test)
        // before advertising remap capability on K1 in get_tool_mapping_capabilities.
        spdlog::info("[AMS CFS] K1 firmware: in-print tool remap (BOX_MODIFY_TN) may be "
                     "unsupported; emitting anyway (harmless). Verify on hardware (#968).");
    }
    return execute_gcode(cmd);
}

AmsError AmsBackendCfs::enable_bypass() {
    return AmsErrorHelper::not_supported("CFS has no bypass");
}

AmsError AmsBackendCfs::disable_bypass() {
    return AmsErrorHelper::not_supported("CFS has no bypass");
}

// --- GCode helpers ---

namespace {

/// Wrap a CR_BOX_* operation with the K2 macro envelope.
///
/// Sequence (better-than-stock — see notes below):
///
///   1. SAVE_GCODE_STATE        — preserve caller coordinate mode + feedrate
///   2. BOX_SAVE_FAN            — suppress part-cooling during the op
///   3. BOX_GO_TO_EXTRUDE_POS   — toolhead over waste chute (X=133/Y=378)
///   4. BOX_MODE_WAIT           — wait for box state machine to settle
///                                before the active phase
///   5. <body>                  — CR_BOX_PRE_OPT, EXTRUDE/CUT/RETRUDE/etc.
///   6. BOX_NOZZLE_CLEAN        — (load/swap only) wipe on silicone pad
///                                at X=160-170, Y=374
///   7. BOX_RESTORE_FAN         — restore part-cooling state
///   8. BOX_MOVE_TO_SAFE_POS    — park at safe_pos (X=225/Y=345)
///   9. RESTORE_GCODE_STATE
///
/// Stock K2 BOX_LOAD_MATERIAL_HEATING calls BOX_SET_TEMP, which targets
/// Tn_extrude_temp (220°C). We deliberately omit it: if a user has PETG
/// loaded at 240°C and taps Load, BOX_SET_TEMP would cool the hotend
/// mid-op and the subsequent flush would underextrude. A cold extruder
/// instead surfaces a friendly "Extrude below minimum temp — pre-heat
/// first" modal via the key111 entry in CFS_ERROR_TABLE.
///
/// Klipper macros have no try/finally — if the body raises, steps 7-9
/// are skipped. Best-effort unwind (independent BOX_RESTORE_FAN +
/// RESTORE_GCODE_STATE scripts) lives in `dispatch_action_script`'s
/// on_error path.
///
/// `wipe_after` toggles step 6. Load and swap end with the nozzle full
/// of fresh filament — wipe required (observed K2 Plus 2026-05-23: T1
/// load drip-trailed across the bed). Unload ends with the nozzle empty
/// post-cut — wipe omitted to avoid pushing dribble back into the hotend.
std::string wrap_with_park_k2(const std::string& body, bool wipe_after) {
    std::string post_body = "\n";
    if (wipe_after) {
        post_body += "BOX_NOZZLE_CLEAN\n";
    }
    post_body += "BOX_RESTORE_FAN\n"
                 "BOX_MOVE_TO_SAFE_POS\n"
                 "RESTORE_GCODE_STATE NAME=helix_cfs_load";
    return "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
           "BOX_SAVE_FAN\n"
           "BOX_GO_TO_EXTRUDE_POS\n"
           "BOX_MODE_WAIT\n" +
           body + post_body;
}

/// Wrap a BOX_* step body with the K1 official CFS macro envelope (#968).
///
/// The K1 firmware does not expose BOX_SAVE_FAN / BOX_RESTORE_FAN /
/// BOX_MODE_WAIT (verified absent in the public K1-Max box.cfg dump at
/// DieDutchman/K1-Max-KAMP-CFS-Fix, and from the #968 reporter's gcode/help
/// output). Every K1 load/swap/unload sequence shares the same envelope:
///
///   SAVE_GCODE_STATE NAME=helix_cfs_load
///   BOX_ERROR_CLEAR
///   BOX_CHECK_MATERIAL
///   <body>                       — the variant-specific BOX_* step list
///   BOX_MOVE_TO_SAFE_POS         — park
///   RESTORE_GCODE_STATE NAME=helix_cfs_load
///
/// `body` carries everything between CHECK_MATERIAL and the safe-park tail,
/// including each builder's own positioning (BOX_GO_TO_EXTRUDE_POS) and wipe
/// (BOX_NOZZLE_CLEAN) — load wipes after the feed, swap wipes before it, and
/// unload has neither, so those steps live in the body rather than the
/// envelope. The emitted string is byte-identical to the prior inline literals.
std::string wrap_with_envelope_k1(const std::string& body) {
    return "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
           "BOX_ERROR_CLEAR\n"
           "BOX_CHECK_MATERIAL\n" +
           body +
           "\nBOX_MOVE_TO_SAFE_POS\n"
           "RESTORE_GCODE_STATE NAME=helix_cfs_load";
}

std::string wrap_with_park(CfsMacroVariant variant, const std::string& body, bool wipe_after) {
    // Only the K2 (CR_BOX_*) builders route through here — the K1 builders own
    // their wipe/positioning explicitly and call wrap_with_envelope_k1 directly.
    // The K1 branch (envelope has no wipe knob) is kept for completeness/safety.
    return variant == CfsMacroVariant::K1 ? wrap_with_envelope_k1(body)
                                          : wrap_with_park_k2(body, wipe_after);
}

} // namespace

std::string AmsBackendCfs::load_gcode(int idx, CfsMacroVariant variant) {
    std::string tnn = CfsMaterialDb::slot_to_tnn(idx);
    if (tnn.empty()) {
        spdlog::error("[AMS CFS] Invalid slot index for load: {}", idx);
        return "";
    }
    if (variant == CfsMacroVariant::K1) {
        // K1 official CFS upgrade firmware — fresh load, nozzle empty. Mirrors
        // the firmware orchestrator BOX_LOAD_MATERIAL_WITHOUT_MATERIAL step
        // list with explicit TNN= (the box's bare-macro "current TN" set via
        // BOX_MODIFY_TN no-ops on K1, per the reporter):
        //   ERROR_CLEAR → CHECK_MATERIAL → EXTRUDE → EXTRUDER_EXTRUDE → FLUSH
        // BOX_EXTRUDER_EXTRUDE is the root-cause fix for "no auto-extrude after
        // load" (#968): the firmware's WITHOUT_MATERIAL chain drives the main
        // extruder after the cassette feed, and we previously omitted it.
        // Homing is handled upstream by dispatch_action_script; do NOT add
        // IF_NEED_HOME here. Envelope (wrap_with_envelope_k1) adds ERROR_CLEAR /
        // CHECK_MATERIAL / MOVE_TO_SAFE_POS; the body carries positioning, the
        // feed steps, and the post-feed wipe. All commands confirmed in box.cfg.
        return wrap_with_envelope_k1("BOX_GO_TO_EXTRUDE_POS\n"
                                     "BOX_EXTRUDE_MATERIAL TNN=" +
                                     tnn + "\nBOX_EXTRUDER_EXTRUDE TNN=" + tnn +
                                     "\nBOX_MATERIAL_FLUSH TNN=" + tnn + "\nBOX_NOZZLE_CLEAN");
    }
    // Use CR_BOX_* commands directly — M8200 macro's Jinja2 `params.I|int` is broken
    // on Creality's Klipper fork (always evaluates to 0, loading T1A regardless of I= value).
    // CR_BOX_PRE_OPT is required before extrude — sets CFS to material-change mode.
    // CR_BOX_WASTE must follow CR_BOX_EXTRUDE (purges transition material).
    // wipe_after=true: nozzle is full of fresh filament; wipe before parking.
    return wrap_with_park(variant,
                          "CR_BOX_PRE_OPT\nCR_BOX_EXTRUDE TNN=" + tnn +
                              "\nCR_BOX_WASTE\nCR_BOX_FLUSH TNN=" + tnn + "\nCR_BOX_END_OPT",
                          /*wipe_after=*/true);
}

std::string AmsBackendCfs::unload_gcode(CfsMacroVariant variant) {
    if (variant == CfsMacroVariant::K1) {
        // K1: mirror the firmware BOX_QUIT_MATERIAL step list:
        //   ERROR_CLEAR → CHECK_MATERIAL → CUT → RETRUDE → safe park.
        // BOX_CUT_MATERIAL handles the cut; BOX_RETRUDE_MATERIAL is the no-TNN
        // retract primitive (operates on the currently-loaded slot tracked by
        // the box driver) — same one BOX_QUIT_MATERIAL uses in box.cfg. Nozzle
        // is empty after the cut+retract, so no wipe. Homing handled upstream.
        return wrap_with_envelope_k1("BOX_CUT_MATERIAL\n"
                                     "BOX_RETRUDE_MATERIAL");
    }
    // Unload ends with CR_BOX_RETRUDE — the nozzle is empty (cut + retracted),
    // so no wipe needed. Skipping the wipe also avoids the wipe macro pushing
    // anything back into the hotend during a "filament-out" state.
    // BOX_MODE_WAIT before CR_BOX_RETRUDE mirrors the stock
    // BOX_QUIT_MATERIAL_RETRUDE_MATERIAL sequence (state-machine settle
    // after the cut).
    return wrap_with_park(variant,
                          "CR_BOX_PRE_OPT\nCR_BOX_CUT\nBOX_MODE_WAIT\n"
                          "CR_BOX_RETRUDE\nCR_BOX_END_OPT",
                          /*wipe_after=*/false);
}

std::string AmsBackendCfs::swap_gcode(int idx, CfsMacroVariant variant) {
    std::string tnn = CfsMaterialDb::slot_to_tnn(idx);
    if (tnn.empty()) {
        spdlog::error("[AMS CFS] Invalid slot index for swap: {}", idx);
        return "";
    }
    if (variant == CfsMacroVariant::K1) {
        // K1: nozzle loaded — mirror the firmware BOX_LOAD_MATERIAL_WITH_MATERIAL
        // step list with explicit TNN=:
        //   ERROR_CLEAR → CHECK_MATERIAL → CUT → RETRUDE → GO_TO_EXTRUDE_POS →
        //   NOZZLE_CLEAN → EXTRUDE → EXTRUDER_EXTRUDE → FLUSH → safe park.
        // No BOX_MODE_WAIT on K1 firmware. BOX_EXTRUDER_EXTRUDE between EXTRUDE
        // and FLUSH is the same auto-extrude fix as the fresh-load path (#968).
        // Wipe (NOZZLE_CLEAN) precedes the new feed here exactly as the firmware
        // WITH_MATERIAL chain does. Homing handled upstream.
        return wrap_with_envelope_k1("BOX_CUT_MATERIAL\n"
                                     "BOX_RETRUDE_MATERIAL\n"
                                     "BOX_GO_TO_EXTRUDE_POS\n"
                                     "BOX_NOZZLE_CLEAN\n"
                                     "BOX_EXTRUDE_MATERIAL TNN=" +
                                     tnn + "\nBOX_EXTRUDER_EXTRUDE TNN=" + tnn +
                                     "\nBOX_MATERIAL_FLUSH TNN=" + tnn);
    }
    // Full swap: unload current (cut+retract) then load new slot, all in one
    // session. BOX_MODE_WAIT interposed after CR_BOX_CUT (let the cutter
    // recover) and before CR_BOX_EXTRUDE (new slot's state-machine handoff).
    // Ends with flush of the NEW filament so wipe is required, same as load.
    return wrap_with_park(variant,
                          "CR_BOX_PRE_OPT\nCR_BOX_CUT\nBOX_MODE_WAIT\n"
                          "CR_BOX_RETRUDE\nBOX_MODE_WAIT\n"
                          "CR_BOX_EXTRUDE TNN=" +
                              tnn + "\nCR_BOX_WASTE\nCR_BOX_FLUSH TNN=" + tnn + "\nCR_BOX_END_OPT",
                          /*wipe_after=*/true);
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
            end_phase_tracking();
            PostOpCooldownManager::instance().schedule();
        }
    };

    auto token = lifetime_.token();
    auto on_error = [this, token](const MoonrakerError& err) {
        // L081 Mechanism C: marshal member writes (system_info_) to main.
        token.defer("AmsBackendCfs::action_err", [this, err]() {
            // Klipper rejection (key849, busy, etc.) and timeouts both land here.
            // Either way the driver isn't running our script anymore, so flip back
            // to IDLE so the UI doesn't get stuck on a "loading" spinner.
            spdlog::error("[AMS CFS] Action script failed: {}", err.message);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                system_info_.action = AmsAction::IDLE;
                end_phase_tracking();
            }

            // Best-effort unwind. The wrap_with_park envelope emits
            // BOX_SAVE_FAN + SAVE_GCODE_STATE upfront and relies on the
            // tail of the script to restore them — but Klipper macros
            // have no try/finally, so if the body raised mid-sequence
            // (key849, key851, min_extrude_temp, etc.) the restore lines
            // never ran. Fire each cleanup as a separate gcode_script
            // with empty callbacks so one failing (e.g. RESTORE_GCODE_STATE
            // when no save exists because SAVE never ran) doesn't block
            // the other. Worst case: cleanup is a no-op.
            if (api_) {
                // BOX_RESTORE_FAN only exists on K2 firmware (and only the K2
                // envelope ran BOX_SAVE_FAN). The K1 official CFS upgrade
                // firmware lacks the macro and never saved fan state, so emitting
                // it there just returns key61 — skip it on K1. (#968)
                if (macro_variant_ != CfsMacroVariant::K1) {
                    api_->execute_gcode(
                        "BOX_RESTORE_FAN", []() {}, [](const MoonrakerError&) {},
                        MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
                }
                api_->execute_gcode(
                    "RESTORE_GCODE_STATE NAME=helix_cfs_load", []() {},
                    [](const MoonrakerError&) {}, MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
            }
        });
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
        nlohmann::json{
            {"objects", nlohmann::json{{"toolhead", nlohmann::json::array({"homed_axes"})}}}},
        [this, token, gcode_copy, on_complete = std::move(on_complete),
         on_error = on_error](const nlohmann::json& response) {
            // L081 Mechanism C: defer member access (api_->execute_gcode dispatch)
            // to main thread. The inner G28-completion lambda is itself invoked
            // on a bg thread, so it also routes its api_ call through token.defer.
            token.defer("AmsBackendCfs::homing_query_apply", [this, token, gcode_copy, on_complete,
                                                              on_error, response]() {
                bool needs_home = true;
                if (response.contains("result") && response["result"].contains("status")) {
                    const auto& status = response["result"]["status"];
                    if (status.contains("toolhead") && status["toolhead"].contains("homed_axes") &&
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
                            // L081 Mechanism C: defer member access (api_) to main.
                            token.defer("AmsBackendCfs::g28_done", [this, gcode_copy, on_complete,
                                                                    on_error]() {
                                api_->execute_gcode(gcode_copy, on_complete, on_error,
                                                    MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
                            });
                        },
                        on_error, MoonrakerAPI::HOMING_TIMEOUT_MS);
                } else {
                    api_->execute_gcode(gcode_copy, on_complete, on_error,
                                        MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
                }
            });
        },
        on_error);

    return AmsErrorHelper::success();
}

// ============================================================================
// Sub-phase synthesis (Task #2)
//
// CFS macros decompose into Heat → Cut → Retract → Feed → Purge but the
// backend sets system_info_.action once at dispatch and leaves it until the
// gcode-script completion callback. We synthesize per-phase transitions from
// physical signals so the UI's step indicator advances correctly:
//
//   - filament_switch_sensor edge true→false  ⇒ cut just completed
//   - filament_switch_sensor edge false→true  ⇒ new filament reached extruder
//   - extruder.target rising >10°C above the post-heat baseline ⇒ purge phase
//
// Heat anchoring lives in the UI (AmsOperationSidebar) and overrides any
// action we publish here while current_temp < target - 5°C.
// ============================================================================

void AmsBackendCfs::begin_phase_tracking() {
    phase_tracker_ = PhaseTracker{};
    phase_tracker_.active = true;
    phase_tracker_.started_with_filament = system_info_.filament_loaded;
    // Don't latch baseline_target until heating completes — the macro raises
    // target as its first act, and that rise is not a purge signal.
}

void AmsBackendCfs::end_phase_tracking() {
    phase_tracker_ = PhaseTracker{};
}

void AmsBackendCfs::on_filament_transition_locked(bool new_detected) {
    if (!phase_tracker_.active) {
        spdlog::debug("[AMS CFS] Phase: filament {} -> {} (no active op)", last_filament_detected_,
                      new_detected);
        return;
    }
    if (last_filament_detected_ && !new_detected) {
        phase_tracker_.seen_filament_drop = true;
        spdlog::info("[AMS CFS] Phase: filament drop (cut completed)");
    } else if (!last_filament_detected_ && new_detected && phase_tracker_.seen_filament_drop) {
        phase_tracker_.seen_filament_rise = true;
        spdlog::info("[AMS CFS] Phase: filament rise after drop (swap feed complete)");
    } else if (!last_filament_detected_ && new_detected && !phase_tracker_.started_with_filament) {
        // Fresh-load case: filament arrives without a prior drop. Treat the
        // rise as the start of the post-feed phase.
        phase_tracker_.seen_filament_rise = true;
        spdlog::info("[AMS CFS] Phase: filament rise (fresh feed complete)");
    }
    // Promote a pending purge target (latched earlier when the target rose
    // before feed completed) once the rise actually happens.
    if (phase_tracker_.seen_filament_rise && phase_tracker_.pending_purge_target &&
        !phase_tracker_.seen_purge_signal) {
        phase_tracker_.seen_purge_signal = true;
        spdlog::info("[AMS CFS] Phase: purge promoted (pending target jump + feed complete)");
    }
    apply_synthesized_action_locked();
}

void AmsBackendCfs::on_extruder_temp_change_locked(int new_temp_deci, int new_target_deci) {
    if (!phase_tracker_.active)
        return;

    if (new_target_deci > 0 && new_temp_deci >= (new_target_deci - 50 /* 5°C deci */)) {
        if (!phase_tracker_.reached_target_once) {
            phase_tracker_.reached_target_once = true;
            phase_tracker_.baseline_target_deci = new_target_deci;
            spdlog::debug("[AMS CFS] Phase: reached target {}°C, baseline latched",
                          helix::ui::temperature::deci_to_degrees(new_target_deci));
        }
    }

    // Purge detection: target rises >10°C above the post-heat baseline. The
    // K2 macro raises target multiple times — a 220→235 "operational" jump
    // very early (before cut), then 235→240 (flush_max_temp) at purge.
    // Macro variants may also do a single 220→240 jump well before feed.
    // Latch the target-jump observation in pending_purge_target regardless
    // of filament state, then promote to seen_purge_signal once the feed
    // physically completes (seen_filament_rise = true). UNLOAD has no rise,
    // so promotion never happens and purge correctly stays off.
    if (phase_tracker_.reached_target_once && phase_tracker_.baseline_target_deci > 0 &&
        new_target_deci > phase_tracker_.baseline_target_deci + 100 /* 10°C deci */ &&
        !phase_tracker_.pending_purge_target) {
        phase_tracker_.pending_purge_target = true;
        spdlog::debug(
            "[AMS CFS] Phase: purge target jump latched (target {}°C > baseline {}°C + 10)",
            helix::ui::temperature::deci_to_degrees(new_target_deci),
            helix::ui::temperature::deci_to_degrees(phase_tracker_.baseline_target_deci));
    }
    if (phase_tracker_.pending_purge_target && phase_tracker_.seen_filament_rise &&
        !phase_tracker_.seen_purge_signal) {
        phase_tracker_.seen_purge_signal = true;
        spdlog::info("[AMS CFS] Phase: purge promoted (target jump + feed complete)");
    }

    apply_synthesized_action_locked();
}

void AmsBackendCfs::apply_synthesized_action_locked() {
    if (!phase_tracker_.active)
        return;

    AmsAction synth;
    if (phase_tracker_.seen_purge_signal) {
        synth = AmsAction::PURGING;
    } else if (phase_tracker_.started_with_filament && !phase_tracker_.seen_filament_drop) {
        // Swap or unload, before the cutter trips — Cut/Tip phase.
        synth = AmsAction::CUTTING;
    } else if (phase_tracker_.seen_filament_drop && !phase_tracker_.seen_filament_rise) {
        // After cut, before new filament arrives (or whole unload retract).
        synth = AmsAction::UNLOADING;
    } else {
        // Fresh-load feed, post-feed swap, or post-purge tail.
        synth = AmsAction::LOADING;
    }

    if (system_info_.action != synth && system_info_.action != AmsAction::IDLE) {
        spdlog::info("[AMS CFS] Phase synth: {} -> {}", ams_action_to_string(system_info_.action),
                     ams_action_to_string(synth));
        system_info_.action = synth;
    }
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
    return {.supported = true,
            .editable = true,
            .description = "Tool reassignment via BOX_MODIFY_TN"}; // i18n: do not translate
}

std::vector<int> AmsBackendCfs::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

std::vector<helix::printer::DeviceAction> AmsBackendCfs::get_device_actions() const {
    using DA = helix::printer::DeviceAction;
    using AT = helix::printer::ActionType;
    return {
        DA{"refresh_rfid",
           "Refresh RFID",
           "",
           "",
           "Re-read spool RFID tags and remaining length",
           AT::BUTTON,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
        DA{"toggle_auto_refill",
           "Toggle Auto-Refill",
           "",
           "",
           "Enable/disable automatic backup spool switching",
           AT::TOGGLE,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
        DA{"nozzle_clean",
           "Clean Nozzle",
           "",
           "",
           "Wipe nozzle on silicone cleaning strip",
           AT::BUTTON,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
        DA{"comm_test",
           "Communication Test",
           "",
           "",
           "Test RS-485 link to CFS units",
           AT::BUTTON,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
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
    if (o.color_set)
        slot.color_rgb = o.color_rgb;
    if (!o.color_name.empty())
        slot.color_name = o.color_name;
    if (!o.material.empty())
        slot.material = o.material;

    // Trust the user's assignment for presence. Untagged 3rd-party spools
    // always read RFID -1, so firmware reports the bay EMPTY even though a
    // spool is physically present. If the override carries a real assignment,
    // the user has told us a spool is in this bay — promote it to AVAILABLE.
    // const bool real_assignment = o.spoolman_id > 0 || !o.material.empty() || !o.brand.empty() ||
    //                              !o.spool_name.empty() || o.color_set;
    // if (real_assignment && slot.status == SlotStatus::EMPTY) {
    //     slot.status = SlotStatus::AVAILABLE;
    // }
}

void AmsBackendCfs::check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                               const std::string& observed_uid) {
    if (slot.status == SlotStatus::EMPTY) {
        auto ovr_it = overrides_.find(slot_index);
        if (ovr_it != overrides_.end()) {
            spdlog::info("{} Slot {} is physically empty, clearing stale override",
                         backend_log_tag(), slot_index);
            clear_override_locked(slot_index, slot);
        }
        last_rfid_uid_.erase(slot_index);
        return;
    }

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
        spdlog::debug("{} Slot {} baseline RFID fingerprint: {}", backend_log_tag(), slot_index,
                      observed_uid);
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
        override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
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
