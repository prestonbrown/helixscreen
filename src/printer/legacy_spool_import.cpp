// SPDX-License-Identifier: GPL-3.0-or-later

#include "legacy_spool_import.h"

#include "core/text_io.h"
#include "filament_op_dispatch.h"
#include "json_utils.h"

namespace helix::ams::legacy {

namespace {
constexpr const char* kPrinters = "printers";
} // namespace

std::map<int, int> tool_spools_from_set(const nlohmann::json& set) {
    std::map<int, int> out;
    if (!set.is_object()) {
        return out;
    }
    for (const auto& [key, entry] : set.items()) {
        const auto tool = helix::text_io::parse_leading<int>(key);
        if (!tool || *tool < 0 || !entry.is_object()) {
            continue;
        }
        const int spool = helix::json_util::safe_int(entry, "spoolman_id", 0);
        if (spool > 0) {
            out[*tool] = spool;
        }
    }
    return out;
}

std::map<int, int> tool_spools_from_file(const nlohmann::json& file,
                                         const std::string& printer_id) {
    if (!file.is_object()) {
        return {};
    }
    const auto printers = file.find(kPrinters);
    if (printers == file.end() || !printers->is_object()) {
        return tool_spools_from_set(file);
    }
    const auto mine = printers->find(printer_id);
    return mine == printers->end() ? std::map<int, int>{} : tool_spools_from_set(*mine);
}

nlohmann::json set_keeping_tools(const nlohmann::json& set, const std::map<int, int>& owed) {
    nlohmann::json kept = nlohmann::json::object();
    if (!set.is_object()) {
        return kept;
    }
    for (const auto& [key, entry] : set.items()) {
        const auto tool = helix::text_io::parse_leading<int>(key);
        if (tool && owed.count(*tool) != 0) {
            kept[key] = entry;
        }
    }
    return kept;
}

std::optional<nlohmann::json> file_keeping_tools(const nlohmann::json& file,
                                                 const std::string& printer_id,
                                                 const std::map<int, int>& owed) {
    if (!file.is_object()) {
        return std::nullopt;
    }
    const auto printers = file.find(kPrinters);
    if (printers == file.end() || !printers->is_object()) {
        nlohmann::json kept = set_keeping_tools(file, owed);
        if (tool_spools_from_set(kept).empty()) {
            return std::nullopt;
        }
        return kept;
    }
    nlohmann::json rest = file;
    if (printers->contains(printer_id)) {
        nlohmann::json kept = set_keeping_tools((*printers)[printer_id], owed);
        if (tool_spools_from_set(kept).empty()) {
            rest[kPrinters].erase(printer_id);
        } else {
            rest[kPrinters][printer_id] = std::move(kept);
        }
    }
    for (const auto& [id, set] : rest[kPrinters].items()) {
        if (!tool_spools_from_set(set).empty()) {
            return rest;
        }
    }
    return std::nullopt;
}

std::optional<SlotInfo> external_spool_from_settings(const nlohmann::json& block) {
    if (!block.is_object()) {
        return std::nullopt;
    }
    const bool assigned = helix::json_util::safe_bool(block, "assigned", false);
    if (!assigned && !block.contains("color_rgb")) {
        return std::nullopt;
    }
    SlotInfo info;
    info.slot_index = helix::ui::kExternalSpoolSlot;
    info.global_index = helix::ui::kExternalSpoolSlot;
    info.color_rgb = static_cast<uint32_t>(
        helix::json_util::safe_int(block, "color_rgb", static_cast<int>(kAmsDefaultSlotColor)));
    info.material = helix::json_util::safe_string(block, "material");
    info.brand = helix::json_util::safe_string(block, "brand");
    info.catalog_id = helix::json_util::safe_string(block, "catalog_id");
    info.product_name = helix::json_util::safe_string(block, "product_name");
    info.nozzle_temp_min = helix::json_util::safe_int(block, "nozzle_temp_min", 0);
    info.nozzle_temp_max = helix::json_util::safe_int(block, "nozzle_temp_max", 0);
    info.bed_temp = helix::json_util::safe_int(block, "bed_temp", 0);
    info.spoolman_id = helix::json_util::safe_int(block, "spoolman_id", 0);
    info.spool_name = helix::json_util::safe_string(block, "spool_name");
    info.remaining_weight_g = helix::json_util::safe_float(block, "remaining_weight_g", -1.0f);
    info.total_weight_g = helix::json_util::safe_float(block, "total_weight_g", -1.0f);
    info.status = SlotStatus::AVAILABLE;
    return info;
}

} // namespace helix::ams::legacy
