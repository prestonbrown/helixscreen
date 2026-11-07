// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_start_params.h"

#include "filament_mapper.h"
#include "json_utils.h"
#include "printer_discovery.h"

#include <cctype>

#include "hv/json.hpp"

namespace helix::print_start {

bool takes_tool_slots(const PrinterDiscovery& hw) {
    return hw.server_takes_print_start_tools() || hw.has_zmod_afc();
}

bool server_takes_tool_slots(const nlohmann::json& server_info_result) {
    const nlohmann::json* zmod = json_util::find_member(server_info_result, "zmod");
    const nlohmann::json* features = zmod ? json_util::find_member(*zmod, "features") : nullptr;
    if (!features || !features->is_array()) {
        return false;
    }
    for (const auto& feature : *features) {
        if (feature.is_string() && feature.get_ref<const std::string&>() == "print_start_tools") {
            return true;
        }
    }
    return false;
}

std::optional<int> slot_number_of_lane(std::string_view lane) {
    std::size_t start = lane.size();
    while (start > 0 && std::isdigit(static_cast<unsigned char>(lane[start - 1]))) {
        --start;
    }
    // More digits than any slot count; also keeps the sum below from overflowing.
    if (start == lane.size() || lane.size() - start > 6) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = start; i < lane.size(); ++i) {
        value = value * 10 + (lane[i] - '0');
    }
    if (value < 1) {
        return std::nullopt;
    }
    return value;
}

PrintStartParams tool_slot_params(const std::map<int, int>& slot_numbers) {
    std::string tools;
    int written = 0; // entries in `tools` so far, one per tool from T0
    for (const auto& [tool, slot] : slot_numbers) {
        if (tool < 0 || slot < 1) {
            continue;
        }
        // An empty entry leaves that tool to the firmware's own default.
        for (; written < tool; ++written) {
            tools += ',';
        }
        tools += std::to_string(slot);
        tools += ',';
        ++written;
    }
    if (tools.empty()) {
        return {};
    }
    tools.pop_back();
    return {{"tools", tools}};
}

int mapped_backend(const std::vector<ToolMapping>& mappings) {
    for (const auto& m : mappings) {
        if (!m.is_auto && m.mapped_slot >= 0 && m.tool_index >= 0) {
            return m.mapped_backend < 0 ? 0 : m.mapped_backend;
        }
    }
    return 0;
}

std::map<int, int> full_tool_slots(const std::vector<ToolMapping>& mappings, int backend_idx) {
    std::map<int, int> slots;
    for (const auto& m : mappings) {
        // A slot number names a lane only within its own backend.
        if (m.is_auto || m.mapped_slot < 0 || m.tool_index < 0 ||
            (m.mapped_backend < 0 ? 0 : m.mapped_backend) != backend_idx) {
            continue;
        }
        slots[m.tool_index] = m.mapped_slot;
    }
    return slots;
}

} // namespace helix::print_start
